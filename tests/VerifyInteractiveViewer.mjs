// Optional browser integration check. Requires Playwright and an installed Chrome.
// CATAN_PLAYWRIGHT_MODULE can point at an existing Playwright module; no install
// or browser download is performed by this script.
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';

const {chromium} = await import(process.env.CATAN_PLAYWRIGHT_MODULE || 'playwright');
const perspective = process.env.CATAN_TEST_PERSPECTIVE;
const cli = spawn(process.argv[2] || './build/catan_cli', ['42','--visualizer','--mcts-threads','1',
  ...(perspective ? ['--perspective', perspective] : [])],
  {env:{...process.env,BROWSER:'/usr/bin/true'},stdio:['pipe','pipe','pipe']});
let output='', waiting=null, terminalError='';
cli.stdout.on('data',chunk=>{output+=chunk;deliver();});
cli.stderr.on('data',chunk=>{terminalError+=chunk;});
cli.on('exit',code=>{if(waiting){waiting.reject(new Error(`CLI exited ${code}: ${output} ${terminalError}`));waiting=null;}});
function deliver() {
  if(waiting&&output.endsWith('> ')) {
    const pending=waiting;waiting=null;const result=output;output='';pending.resolve(result);
  }
}
function prompt() {
  return new Promise((resolve,reject)=>{
    const timer=setTimeout(()=>{waiting=null;reject(new Error('CLI prompt timeout: '+output+terminalError));},10000);
    waiting={resolve:value=>{clearTimeout(timer);resolve(value);},reject:error=>{clearTimeout(timer);reject(error);}};deliver();
  });
}
let browser;
const errors=[];
try {
  const first=await prompt(),url=first.match(/Live visualizer: (http:\/\/127\.0\.0\.1:\d+\/)/)[1];
  browser=await chromium.launch({headless:true,executablePath:process.env.CATAN_CHROME_PATH || '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome'});
  const page=await browser.newPage({viewport:{width:1440,height:1100}});
  page.on('pageerror',error=>errors.push(error.message));
  await page.goto(url);
  try { await page.locator('.legal-target').first().waitFor({timeout:5000}); }
  catch(error) { throw new Error(`Board startup failed: ${await page.locator('#status').textContent()}`,{cause:error}); }
  assert.equal(await page.locator('#board [data-layer="layout"] .terrain').count(),19);
  await page.evaluate(()=>{window.originalLayout=document.querySelector('[data-layer="layout"]');window.originalTargets=document.querySelector('[data-layer="targets"]');});
  if(perspective!=='p2') {
    await page.waitForFunction(()=>document.querySelector('.analysis-row'));
    await page.locator('.analysis-row').first().click();
    const selected=await page.locator('#selected-label').textContent();
    await page.waitForTimeout(450);
    assert.equal(await page.locator('#selected-label').textContent(),selected);
    await page.locator('#cancel').click();
  } else {
    assert(await page.locator('#tree-p1').isDisabled());
    assert(!await page.locator('#tree-p2').isDisabled());
  }
  assert(await page.evaluate(()=>window.originalLayout===document.querySelector('[data-layer="layout"]')&&window.originalTargets===document.querySelector('[data-layer="targets"]')));
  await page.locator('.legal-target').first().hover();
  assert.equal(await page.locator('[data-layer="preview"] polygon').count(),1);
  await page.screenshot({path:'/tmp/catan-viewer-desktop.png',fullPage:true});
  await page.locator('.legal-target').first().click();
  const recorded=await prompt();assert.match(recorded,/Viewer selected.*settlement/);
  await page.waitForFunction(()=>document.querySelector('#phase').textContent==='initial road');
  assert.equal(await page.locator('[data-tool="road"]').getAttribute('aria-pressed'),'true');
  await page.locator('.legal-target').first().press('Enter');
  assert.match(await prompt(),/Viewer selected.*road/);
  await page.waitForFunction(()=>document.querySelector('#phase').textContent==='initial settlement');
  await page.locator('#rewind').click();
  assert.match(await prompt(),/Rewound one action/);
  await page.waitForFunction(()=>document.querySelector('#phase').textContent==='initial road');
  assert(await page.evaluate(()=>window.originalLayout===document.querySelector('[data-layer="layout"]')));
  const enabledTree=perspective==='p2'?'tree-p2':'tree-p1';
  await page.locator('#'+enabledTree).uncheck();await prompt();
  await page.waitForFunction(id=>!document.getElementById(id).disabled&&!document.querySelector('body').classList.contains('busy'),enabledTree);
  const base=await (await fetch(url+'state.json')).json();

  // Startup must recover after a rendering failure without a new position
  // version, and an old engine must explain the mismatch rather than get stuck.
  const startupPage=await browser.newPage();
  let startupMode='broken';
  const initialLayout={topology:base.topology,hexes:base.board.hexes,ports:base.board.ports};
  await startupPage.route('**/updates?*',async route=>{
    const position=structuredClone(base);
    if(startupMode==='broken')position.players[0].resources=null;
    if(startupMode==='legacy') {
      delete position.board.robber_hex;
      position.transformations=position.transformations.map(({index,description,chance_probability})=>({index,description,chance_probability}));
    }
    await route.fulfill({json:{position,layout:initialLayout,
      revisions:{position:startupMode==='legacy'?'legacy':'retry-same-position',layout:'startup-layout'},
      analysis:{position_version:base.controls.position_version,mcts:null}}});
  });
  await startupPage.goto(url);
  await startupPage.waitForFunction(()=>document.querySelector('#status').classList.contains('error'));
  startupMode='valid';
  await startupPage.locator('.legal-target').first().waitFor({timeout:5000});
  assert(!await startupPage.locator('#board-hint').textContent().then(text=>text.includes('Loading board')));
  startupMode='legacy';
  await startupPage.waitForFunction(()=>document.querySelector('#board-hint').textContent.includes('Rebuild it'));
  assert.match(await startupPage.locator('#status').textContent(),/older viewer interface/);
  await startupPage.close();

  // UI forms cover every action family using typed fixtures. Commands are
  // intercepted here; the earlier real game verifies the application boundary.
  let fixture=structuredClone(base),fixtureVersion=1000,captured=[],fixtureAnalysis=null,fixtureExtras=[];
  await page.route('**/updates?*',async route=>{
    const query=new URL(route.request().url()).searchParams,revision='fixture-'+fixtureVersion;
    const data={revisions:{position:revision,catalog:revision,analysis:revision}};
    if(query.get('position')!==revision)data.position=fixture;
    if(query.get('catalog')!==revision)data.catalog={position_version:fixtureVersion,moves:fixtureExtras};
    if(query.get('analysis')!==revision)data.analysis={position_version:fixtureVersion,mcts:fixtureAnalysis};
    await route.fulfill({json:data});
  });
  await page.route('**/command',async route=>{
    captured.push(route.request().postDataJSON());
    await route.fulfill({json:{accepted:true,request_id:1,position_version:fixtureVersion,message:'Fixture accepted'}});
  });
  function move(type,params={},description=type.replaceAll('_',' ')) {
    return {id:type+JSON.stringify(params),type,description,chance_probability:null,highlight_targets:[],...params};
  }
  async function show(actions,phase='main',extra={}) {
    fixtureVersion++;fixture=structuredClone(base);fixture.controls.position_version=fixtureVersion;
    fixture.controls.discard_input_player=null;fixture.turn.phase=phase;fixture.turn.decision_player=0;
    fixture.transformations=actions;fixture.possible_winners=[];fixtureExtras=[];fixtureAnalysis=null;
    Object.assign(fixture,extra);
    await page.waitForFunction(version=>document.querySelector('#board').dataset.positionVersion===String(version),fixtureVersion);
  }
  async function commit(expected) {
    await page.locator('#commit').click();
    await page.waitForFunction(()=>!document.body.classList.contains('busy'));
    assert.equal(captured.at(-1).move_id,expected.id);
    assert.equal(captured.at(-1).type,'move');
    assert.equal(captured.at(-1).position_version,fixtureVersion);
  }
  for(const [type,label,phase] of [['begin_roll','Roll dice','await roll'],['buy_development','Buy development card','main'],
    ['play_knight','Play knight','main'],['play_road_building','Play road building','main'],
    ['play_year_of_plenty','Play year of plenty','main'],['end_turn','End turn','main'],
    ['draw_unknown_development','Draw unknown development card','development draw']]) {
    const action=move(type,{},label.toLowerCase());await show([action],phase);
    await page.getByRole('button',{name:label,exact:true}).click();await commit(action);
  }
  for(const [type,params,label,phase] of [['roll_dice',{sum:8,chance_probability:.2},'8','dice roll'],
    ['draw_development',{card:'knight',chance_probability:.5},'knight','development draw'],
    ['steal',{resource:'ore',chance_probability:.3},'Ore','robber steal'],
    ['choose_resource',{resource:'grain'},'Grain','year of plenty'],['play_monopoly',{resource:'wool'},'Wool','main']]) {
    const action=move(type,params);await show([action],phase);
    await page.getByRole('button',{name:label,exact:true}).click();await commit(action);
    if(type==='roll_dice') {
      await page.getByRole('button',{name:'Sample random outcome'}).click();
      await page.waitForFunction(()=>!document.body.classList.contains('busy'));
      assert.equal(captured.at(-1).type,'random');
    }
  }
  const trade=move('trade',{give:'grain',receive:'ore',give_amount:3});
  await show([trade]);await page.locator('[data-give="grain"]').click();
  assert.equal(await page.locator('[data-receive]:enabled').count(),1);
  await page.locator('[data-receive="ore"]').click();await commit(trade);
  const batch=move('trade_batch',{trades:[{give:'grain',receive:'ore'},{give:'lumber',receive:'brick'}]});
  fixtureExtras=[batch];fixtureAnalysis={perspective:0,iterations:100,workers:1,maximum_depth:4,unique_root_children:1,
    actions:[{id:batch.id,index:0,visits:20,win_probability:.6,recommended:true}]};
  fixtureVersion++;fixture.controls.position_version=fixtureVersion;
  await page.waitForFunction(version=>document.querySelector('#board').dataset.positionVersion===String(version),fixtureVersion);
  await page.locator('.analysis-row').first().click();await commit(batch);
  const discard=move('discard',{cards:[1,0,2,0,0]});
  const turn={...base.turn,phase:'discarding',decision_player:0,pending_discards:[3,0]};
  const players=structuredClone(base.players);players[0].resources={lumber:2,brick:0,wool:3,grain:0,ore:0};
  await show([discard],'discarding',{turn,players});
  await page.locator('#discard-lumber').fill('1');await page.locator('#discard-wool').fill('2');
  await page.getByRole('button',{name:'Select discard',exact:true}).click();await commit(discard);
  for(const [type,params,phase,tool] of [['build_settlement',{vertex:0},'main','settlement'],
    ['build_road',{edge:0},'main','road'],['build_city',{vertex:0},'main','city'],['move_robber',{hex:1},'move robber','robber']]) {
    const action=move(type,params);await show([action],phase);
    if(phase==='main')await page.locator(`[data-tool="${tool}"]`).click();
    await page.locator('.legal-target').first().click();await page.waitForFunction(()=>!document.body.classList.contains('busy'));
    assert.equal(captured.at(-1).move_id,action.id);
  }
  const resign=move('resign');await show([resign]);
  await page.getByRole('button',{name:'Resign…'}).click();await commit(resign);
  await show([],'main',{possible_winners:[1]});
  await page.getByRole('button',{name:'Record P2 victory'}).click();
  await page.waitForFunction(()=>!document.body.classList.contains('busy'));
  assert.equal(captured.at(-1).type,'winner');assert.equal(captured.at(-1).player,1);
  await page.setViewportSize({width:390,height:844});
  await page.screenshot({path:'/tmp/catan-viewer-mobile.png',fullPage:true});
  assert(await page.evaluate(()=>document.documentElement.scrollWidth<=window.innerWidth));
  assert.deepEqual(errors,[]);
  console.log('Real board commits, keyboard input, rewind, stable DOM, all move forms, and mobile layout passed');
} finally {
  await browser?.close();
  if(cli.exitCode===null){cli.stdin.write('q\n');await new Promise(resolve=>{cli.once('exit',resolve);setTimeout(()=>{cli.kill();resolve();},3000).unref();});}
}
