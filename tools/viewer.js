'use strict';

const $ = id => document.getElementById(id);
const resources = ['lumber', 'brick', 'wool', 'grain', 'ore'];
const cardNames = ['knight', 'road building', 'year of plenty', 'monopoly', 'victory point'];
const resourceColours = {lumber:'#468153', brick:'#b86049', wool:'#83a957', grain:'#ba973e', ore:'#768691'};
const terrainColours = {forest:'#4f855a', hills:'#b9684e', pasture:'#a2bf70', fields:'#dfc168', mountains:'#8b999c', desert:'#d9c695'};
const playerColours = ['#ee796c', '#72b2ef'];
const boardTools = {
  settlement: {label:'⌂ Settlement', types:['place_settlement', 'build_settlement'], cost:'1 lumber · 1 brick · 1 wool · 1 grain'},
  road: {label:'━ Road', types:['place_road', 'build_road'], cost:'1 lumber · 1 brick'},
  city: {label:'▥ City', types:['build_city'], cost:'2 grain · 3 ore'},
  robber: {label:'♟ Robber', types:['move_robber'], cost:'Choose a highlighted hex'},
};
let game = null, layout = null, analysis = null, extras = [];
let revisions = {}, moves = new Map(), selectedId = null, tool = null, tradeGive = null;
let pending = false, connected = false, analysisHeld = false, analysisLimit = 80;
let syncPromise = null;
let renderedPositionVersion = null;
let renderedEventIds = null;
const eventRows = new Map();
let points = new Map(), edges = new Map(), hexes = new Map(), centres = new Map();
let piecesLayer, targetsLayer, previewLayer, highlightsLayer;
const analysisRows = new Map();

function element(tag, className = '', content = null) {
  const node = document.createElement(tag);
  node.className = className;
  if (content !== null) node.textContent = content;
  return node;
}
function svg(tag, attributes = {}, content = null) {
  const node = document.createElementNS('http://www.w3.org/2000/svg', tag);
  for (const [key, value] of Object.entries(attributes)) node.setAttribute(key, String(value));
  if (content !== null) node.textContent = content;
  return node;
}
function button(label, onClick, className = '') {
  const node = element('button', className, label);
  node.type = 'button';
  node.addEventListener('click', () => { if (!pending) onClick(); });
  return node;
}
function setText(node, value) { if (node.textContent !== String(value)) node.textContent = value; }
function status(message, error = false) { setText($('status'), message); $('status').classList.toggle('error', error); }
function actor() { return game.controls.discard_input_player ?? game.turn.decision_player; }
function legal(type) { return (game?.transformations || []).filter(move => move.type === type); }
function coordinateTransform(vertices) {
  const raw = vertices.map(v => ({id:v.id, x:v.x, y:v.y * Math.sqrt(3)}));
  const cx = raw.reduce((sum, v) => sum + v.x, 0) / raw.length;
  const cy = raw.reduce((sum, v) => sum + v.y, 0) / raw.length;
  const angle = -Math.PI / 6;
  const rotated = raw.map(v => ({id:v.id, x:cx+(v.x-cx)*Math.cos(angle)-(v.y-cy)*Math.sin(angle),
    y:cy+(v.x-cx)*Math.sin(angle)+(v.y-cy)*Math.cos(angle)}));
  const minX = Math.min(...rotated.map(v => v.x)), maxX = Math.max(...rotated.map(v => v.x));
  const minY = Math.min(...rotated.map(v => v.y)), maxY = Math.max(...rotated.map(v => v.y));
  const scale = Math.min(705/(maxX-minX), 645/(maxY-minY));
  return new Map(rotated.map(v => [v.id, [35+(790-(maxX-minX)*scale)/2+(v.x-minX)*scale,
    55+(720-(maxY-minY)*scale)/2+(v.y-minY)*scale]]));
}
function hexPoints(id) { return hexes.get(id).vertices.map(v => points.get(v).join(',')).join(' '); }
function roadLine(id, attributes = {}) {
  const [a,b] = edges.get(id).vertices.map(v => points.get(v));
  return svg('line', {x1:a[0], y1:a[1], x2:b[0], y2:b[1], ...attributes});
}
function buildingShape(vertex, city, colour) {
  const [x,y] = points.get(vertex);
  const offsets = city ? [[-20,15],[-20,-8],[-8,-20],[3,-8],[21,-8],[21,15]]
    : [[-15,13],[-15,-5],[0,-18],[15,-5],[15,13]];
  return svg('polygon', {points:offsets.map(([dx,dy]) => `${x+dx},${y+dy}`).join(' '),
    fill:colour, stroke:'#23323a', 'stroke-width':3, 'stroke-linejoin':'round'});
}
function robberShape(id, colour = '#23323a') {
  const [x,y] = centres.get(id);
  const group = svg('g');
  group.append(svg('circle', {cx:x, cy:y+34, r:8, fill:colour, stroke:'#f7edd5', 'stroke-width':2}),
    svg('path', {d:`M ${x-11} ${y+44} Q ${x} ${y+35} ${x+11} ${y+44} L ${x+15} ${y+62} L ${x-15} ${y+62} Z`,
      fill:colour, stroke:'#f7edd5', 'stroke-width':2}));
  return group;
}
function buildBoard() {
  points = coordinateTransform(layout.topology.vertices);
  edges = new Map(layout.topology.edges.map(edge => [edge.id, edge]));
  hexes = new Map(layout.topology.hexes.map(hex => [hex.id, hex]));
  centres = new Map(layout.topology.hexes.map(hex => {
    const corners = hex.vertices.map(v => points.get(v));
    return [hex.id, [corners.reduce((sum,p) => sum+p[0],0)/6, corners.reduce((sum,p) => sum+p[1],0)/6]];
  }));
  const base = svg('g', {'data-layer':'layout'}), debug = svg('g', {class:'debug-layer'});
  base.append(svg('rect', {x:14, y:30, width:832, height:750, rx:32, fill:'#46778b'}));
  const allPoints = [...points.values()], cx = allPoints.reduce((s,p)=>s+p[0],0)/allPoints.length,
    cy = allPoints.reduce((s,p)=>s+p[1],0)/allPoints.length;
  for (const port of layout.ports) {
    const [a,b] = edges.get(port.edge).vertices.map(v => points.get(v));
    const mx=(a[0]+b[0])/2, my=(a[1]+b[1])/2, length=Math.hypot(mx-cx,my-cy);
    const px=mx+(mx-cx)/length*33, py=my+(my-cy)/length*33;
    for (const p of [a,b]) base.append(svg('line',{x1:p[0],y1:p[1],x2:px,y2:py,stroke:'#eedbae','stroke-width':5}));
    base.append(svg('rect',{x:px-29,y:py-13,width:58,height:26,rx:7,fill:'#f5e7c4',stroke:'#435b60'}),
      svg('text',{x:px,y:py+4,'text-anchor':'middle',class:'port-label'}, port.type.replace(/lumber|brick|wool|grain|ore/g, r=>r[0].toUpperCase())));
  }
  for (const tile of layout.hexes) {
    const [x,y] = centres.get(tile.id);
    base.append(svg('polygon',{points:hexPoints(tile.id),fill:terrainColours[tile.terrain],class:'terrain'}));
    if (tile.number) {
      base.append(svg('circle',{cx:x,cy:y,r:24,class:'token'}),
        svg('text',{x,y:y+5,'text-anchor':'middle',class:'number'+([6,8].includes(tile.number)?' hot':'')},tile.number),
        svg('text',{x,y:y+17,'text-anchor':'middle',class:'pips'},'•'.repeat(6-Math.abs(7-tile.number))));
    }
    debug.append(svg('text',{x:x+31,y:y+5,class:'debug-label'},`H${tile.id}`));
  }
  for (const [id,[x,y]] of points) debug.append(svg('text',{x:x+7,y:y-9,class:'debug-label'},`V${id}`));
  for (const [id,edge] of edges) {
    const [a,b]=edge.vertices.map(v=>points.get(v));
    debug.append(svg('text',{x:(a[0]+b[0])/2,y:(a[1]+b[1])/2-6,class:'debug-label'},`E${id}`));
  }
  piecesLayer=svg('g',{'data-layer':'pieces'}); targetsLayer=svg('g',{'data-layer':'targets'});
  highlightsLayer=svg('g',{'data-layer':'highlights'}); previewLayer=svg('g',{class:'preview','data-layer':'preview'});
  $('board').replaceChildren(base,piecesLayer,highlightsLayer,targetsLayer,previewLayer,debug);
}
function renderPieces() {
  const nodes=[];
  game.board.roads.forEach((road,id)=>{
    if (road) nodes.push(roadLine(id,{stroke:'#22323a','stroke-width':15,'stroke-linecap':'round'}),
      roadLine(id,{stroke:playerColours[road.owner],'stroke-width':10,'stroke-linecap':'round'}));
  });
  game.board.buildings.forEach((building,id)=>{
    if (building) nodes.push(buildingShape(id,building.type==='city',playerColours[building.owner]));
  });
  nodes.push(robberShape(game.board.robber_hex));
  piecesLayer.replaceChildren(...nodes);
}
function moveLabel(move) {
  if (!move) return 'Move';
  switch(move.type) {
    case 'place_settlement': case 'build_settlement': return `Settlement · V${move.vertex}`;
    case 'place_road': case 'build_road': return `Road · E${move.edge}`;
    case 'build_city': return `City · V${move.vertex}`;
    case 'move_robber': return `Robber · H${move.hex}`;
    case 'roll_dice': return `Roll ${move.sum}`;
    case 'trade': return `${move.give_amount || ''} ${move.give} → 1 ${move.receive}`.trim();
    case 'trade_batch': return `Trade batch · ${move.trades.length} exchanges`;
    case 'discard': return 'Discard '+move.cards.map((n,i)=>n?`${n} ${resources[i]}`:'').filter(Boolean).join(', ');
    case 'draw_development': return `Draw ${move.card}`;
    case 'steal': return `Steal ${move.resource}`;
    case 'choose_resource': return `Take ${move.resource}`;
    case 'play_monopoly': return `Monopoly · ${move.resource}`;
    default: return move.description[0].toUpperCase()+move.description.slice(1);
  }
}
function rebuildMoves() {
  moves = new Map((game.transformations || []).map(move=>[move.id,move]));
  for (const move of extras) if (!moves.has(move.id)) moves.set(move.id,move);
}
function toolMoves() { return game.transformations.filter(move=>boardTools[tool]?.types.includes(move.type)); }
function renderTools() {
  $('tools').replaceChildren(...Object.entries(boardTools).map(([key,config])=>{
    const available=game.transformations.some(move=>config.types.includes(move.type));
    const node=button(config.label,()=>{
      tool=tool===key?null:key; selectedId=null; renderTools(); renderTargets(); renderSelection();
    },tool===key?'active':'');
    node.disabled=!available; node.setAttribute('aria-pressed',String(tool===key)); node.dataset.tool=key;
    node.title=available ? config.cost : 'No legal '+key+' move in this position';
    return node;
  }));
  setText($('board-hint'),tool ? `Hover to preview; click a highlighted ${tool==='road'?'edge':tool==='robber'?'hex':'intersection'} to commit.`
    : 'Select a build tool to see legal locations.');
}
function showPreview(move) {
  previewLayer.replaceChildren();
  if (!move) return;
  if (move.vertex !== undefined) previewLayer.append(buildingShape(move.vertex,move.type==='build_city',playerColours[actor()]));
  if (move.edge !== undefined) previewLayer.append(roadLine(move.edge,{stroke:playerColours[actor()],'stroke-width':12,'stroke-linecap':'round'}));
  if (move.hex !== undefined) previewLayer.append(robberShape(move.hex,playerColours[actor()]));
}
function renderTargets() {
  showPreview(moves.get(selectedId));
  targetsLayer.replaceChildren(...toolMoves().map(move=>{
    const target=svg('g',{class:'legal-target',role:'button',tabindex:0,'aria-label':moveLabel(move),'data-move-id':move.id});
    if (move.vertex!==undefined) { const [x,y]=points.get(move.vertex); target.append(svg('circle',{cx:x,cy:y,r:16})); }
    else if (move.edge!==undefined) {
      const [a,b]=edges.get(move.edge).vertices.map(vertex=>points.get(vertex));
      const length=Math.hypot(b[0]-a[0],b[1]-a[1]),angle=Math.atan2(b[1]-a[1],b[0]-a[0])*180/Math.PI;
      // A rectangle gives horizontal and vertical roads a real hit area, as
      // well as a nonzero accessibility/focus box (SVG line bounds omit stroke).
      target.append(roadLine(move.edge),svg('rect',{x:0,y:-12,width:length,height:24,fill:'transparent',
        transform:`translate(${a[0]} ${a[1]}) rotate(${angle})`,'pointer-events':'all'}));
    }
    else target.append(svg('polygon',{points:hexPoints(move.hex)}));
    const preview=()=>{showPreview(move);setText($('board-hint'),`${moveLabel(move)} — click to commit`);};
    const clear=()=>{showPreview(moves.get(selectedId));renderToolHint();};
    target.addEventListener('pointerenter',preview); target.addEventListener('pointerleave',clear);
    target.addEventListener('focus',preview); target.addEventListener('blur',clear);
    target.addEventListener('click',()=>commitMove(move.id));
    target.addEventListener('keydown',event=>{if (event.key==='Enter'||event.key===' ') {event.preventDefault();commitMove(move.id);}});
    return target;
  }));
}
function renderToolHint() {
  setText($('board-hint'),tool ? `Hover to preview; click a highlighted ${tool==='road'?'edge':tool==='robber'?'hex':'intersection'} to commit.`
    : 'Select a build tool to see legal locations.');
}
function highlightMove(move) {
  document.querySelectorAll('.is-highlighted').forEach(node=>node.classList.remove('is-highlighted'));
  highlightsLayer.replaceChildren();
  if (!move) return;
  if (move.vertex!==undefined) {const [x,y]=points.get(move.vertex);highlightsLayer.append(svg('circle',{cx:x,cy:y,r:27,class:'board-highlight'}));}
  if (move.edge!==undefined) highlightsLayer.append(roadLine(move.edge,{class:'board-highlight road'}));
  if (move.hex!==undefined) highlightsLayer.append(svg('polygon',{points:hexPoints(move.hex),class:'board-highlight'}));
  const targets=new Set(move.highlight_targets || []);
  document.querySelectorAll('[data-highlight-id]').forEach(node=>{
    if(targets.has(node.dataset.highlightId)) node.classList.add('is-highlighted');
  });
}
function selectMove(id) {
  if(pending || !moves.has(id)) return;
  const options=game.transformations.filter(move=>move.type!=='resign');
  if(options.length===1 && options[0].id===id) {commitMove(id);return;}
  selectedId=id; renderSelection(); highlightMove(moves.get(id)); showPreview(moves.get(id));
  for(const [key,row] of analysisRows) row.classList.toggle('selected',key===id);
}
function renderSelection() {
  const move=moves.get(selectedId);
  $('selection').hidden=!move;
  if(!move) {highlightMove(null);return;}
  setText($('selected-label'),moveLabel(move));
  setText($('selected-detail'),move.type==='trade_batch'
    ? move.trades.map((trade,i)=>`${i+1}. ${trade.give} → ${trade.receive}`).join(' · ')
    : move.type==='resign' ? 'Concede the game to the other player.'
    : move.chance_probability != null ? `Observed outcome · ${(move.chance_probability*100).toFixed(1)}% chance`
    : 'Commit to apply this move to the current position.');
  setText($('commit'),move.type==='resign'?'Confirm resignation':game.information&&actor()!==game.information.observer?'Record move':'Commit move');
  $('commit').disabled=pending;
}
function resourceButton(resource,onClick,label=null) {
  const node=button(label || resource[0].toUpperCase()+resource.slice(1),onClick,'resource-button');
  node.style.setProperty('--resource-color',resourceColours[resource]);return node;
}
function group(title,nodes) {
  const section=element('section');section.append(element('h3','',title));
  const grid=element('div','button-grid');grid.append(...nodes);section.append(grid);return section;
}
function renderActions() {
  const observer=game.information?.observer, opponent=observer!==undefined&&observer!==actor();
  setText($('actor'),`P${actor()+1}`);$('actor').style.color=playerColours[actor()];
  setText($('action-title'),game.turn.winner!=null?'Game finished':opponent?'Record opponent move':'Choose a move');
  setText($('action-hint'),game.turn.winner!=null?`Player ${game.turn.winner+1} won. You can rewind to continue.`
    : game.turn.phase==='discarding' ? `Select P${actor()+1}'s discard. Both hands change together when all choices are recorded.`
    : opponent ? 'Enter the move you observed. Opponent development draws stay unknown.'
    : game.turn.phase==='development draw'&&observer!==undefined ? 'Record the development card you received.'
    : 'Use the board for building and robber moves. Other moves are available here.');
  const nodes=[], simpleTypes=['begin_roll','buy_development','play_knight','play_road_building','play_year_of_plenty','end_turn','draw_unknown_development'];
  const simple=game.transformations.filter(move=>simpleTypes.includes(move.type));
  if(simple.length) nodes.push(group('Actions',simple.map(move=>button(moveLabel(move),()=>selectMove(move.id)))));
  const rolls=legal('roll_dice');
  if(rolls.length) nodes.push(group('Record dice total',rolls.map(move=>{
    const node=button(String(move.sum),()=>selectMove(move.id));node.title=`${(move.chance_probability*100).toFixed(1)}% chance`;return node;
  })));
  for(const [type,title] of [['draw_development','Record development card'],['steal','Record stolen resource'],
    ['choose_resource','Year of plenty · choose resource'],['play_monopoly','Play monopoly · choose resource']]) {
    const choices=legal(type);
    if(choices.length) nodes.push(group(title,choices.map(move=>move.resource
      ? resourceButton(move.resource,()=>selectMove(move.id)) : button(move.card,()=>selectMove(move.id)))));
  }
  const trades=legal('trade');
  if(trades.length) {
    if(!trades.some(move=>move.give===tradeGive)) tradeGive=null;
    const section=element('section');
    const giveButtons=resources.map(resource=>{
      const available=trades.filter(move=>move.give===resource);
      const node=resourceButton(resource,()=>{tradeGive=resource;selectedId=null;renderActions();renderSelection();},
        `${available[0]?.give_amount || '–'} ${resource}`);
      node.disabled=!available.length;node.setAttribute('aria-pressed',String(tradeGive===resource));node.dataset.give=resource;return node;
    });
    const receiveButtons=resources.map(resource=>{
      const move=trades.find(candidate=>candidate.give===tradeGive&&candidate.receive===resource);
      const node=resourceButton(resource,()=>selectMove(move.id),`1 ${resource}`);
      node.disabled=!move;node.classList.toggle('legal-choice',Boolean(move));node.dataset.receive=resource;return node;
    });
    section.append(group('Bank trade · give',giveButtons),group('Receive · legal trades highlighted',receiveButtons));nodes.push(section);
  }
  const discards=legal('discard');
  if(discards.length) nodes.push(discardControls(discards));
  const chances=game.transformations.some(move=>move.chance_probability!=null);
  if(chances&&!legal('draw_unknown_development').length) nodes.push(group('Debug simulation',[
    button('Sample random outcome',()=>sendCommand('random'))]));
  const winners=(game.possible_winners || []).map(player=>button(`Record P${player+1} victory`,()=>sendCommand('winner',{player})));
  if(winners.length) nodes.push(group('Observed victory',winners));
  const resign=legal('resign')[0];
  if(resign) nodes.push(group('Game', [button('Resign…',()=>selectMove(resign.id),'danger')]));
  $('actions').replaceChildren(...nodes);
}
function discardControls(choices) {
  const section=element('section');section.append(element('h3','',`Discard ${game.turn.pending_discards[actor()]} cards`));
  const grid=element('div','discard-grid'),inputs=[];
  const selected=choices.find(move=>move.id===selectedId)?.cards || [0,0,0,0,0];
  const summary=element('p','discard-total'),confirm=button('Select discard',()=>selectMove(confirm.dataset.moveId));
  const update=()=>{
    const counts=inputs.map(input=>input.value===''?NaN:Number(input.value));
    const count=counts.reduce((sum,n)=>sum+(Number.isFinite(n)?n:0),0);
    const choice=choices.find(move=>move.cards.every((n,i)=>n===counts[i]));
    summary.textContent=`${count} / ${game.turn.pending_discards[actor()]} selected`;
    confirm.disabled=!choice;confirm.dataset.moveId=choice?.id || '';
  };
  resources.forEach((resource,i)=>{
    const row=element('div','discard-row'),label=element('label','',resource),input=element('input');
    const maximum=game.players[actor()].resources[resource];
    row.style.setProperty('--resource-color',resourceColours[resource]);
    input.type='number';input.min=0;input.max=maximum;input.step=1;input.value=selected[i];input.id='discard-'+resource;
    label.htmlFor=input.id;inputs.push(input);input.addEventListener('input',update);
    row.append(label,button('−',()=>{input.value=Math.max(0,Number(input.value)-1);update();}),input,
      button('+',()=>{input.value=Math.min(maximum,Number(input.value)+1);update();}),element('small','muted',`/ ${maximum}`));
    grid.append(row);
  });
  update();section.append(grid,summary,confirm);return section;
}
function renderPlayers() {
  $('players').replaceChildren(...game.players.map(player=>{
    const panel=element('section','player'),heading=element('div','player-heading');
    panel.style.setProperty('--player-color',playerColours[player.id]);
    const hidden=player.development_cards===null;
    heading.append(element('strong','',`Player ${player.id+1}${player.id===actor()?' · to move':''}`),
      element('span','',hidden?`${player.public_vp} public VP`:`${player.total_vp} VP · ${player.public_vp} public`));
    const hand=element('div','resources');
    for(const resource of resources) {
      const node=element('div','resource');node.dataset.highlightId=`resource-${player.id}-${resource}`;
      node.style.setProperty('--resource-color',resourceColours[resource]);
      node.append(element('span','resource-count',player.resources[resource]),element('small','',resource));hand.append(node);
    }
    const cards=element('div','cards');
    for(const card of cardNames) {
      const count=hidden?game.information.expected_opponent_cards[card].toFixed(2):player.development_cards[card];
      const node=element('span','devcard',`${card} ${count}`);node.dataset.highlightId=`devcard-${player.id}-${card.replaceAll(' ','-')}`;
      node.title=hidden?'Expected count of hidden cards':`${player.bought_this_turn[card]} bought this turn`;cards.append(node);
    }
    const note=hidden?`${game.information.opponent_card_count} unknown development cards · counts are estimates`
      : `${player.played_knights} knights played · newly bought cards cannot be played this turn`;
    panel.append(heading,hand,cards,element('p','card-note',note));return panel;
  }));
  const bank=element('p','',resources.map(resource=>`${resource} ${game.bank[resource]}`).join(' · '));bank.dataset.highlightId='bank';
  const deck=element('p','',`Development deck: ${game.information?.deck_count ?? Object.values(game.development_deck).reduce((a,b)=>a+b,0)} cards${game.information?' · composition estimated':''}`);
  deck.dataset.highlightId='development-deck';
  const awards=game.awards,owner=id=>id===null?'unclaimed':`P${id+1}`;
  const awardText=element('p','',`Longest road: ${owner(awards.longest_road)} · lengths ${awards.road_lengths.join(' / ')}. Largest army: ${owner(awards.largest_army)}.`);
  const dice=element('p','',`Balanced dice: ${game.dice.cards_left} cards left · recent rolls ${game.dice.recent_rolls.join(', ') || 'none'} · sevens by player ${game.dice.sevens_by_player.join(' / ')}`);
  dice.dataset.highlightId='dice';
  const details=[bank,deck,awardText,dice];
  const composition=game.information?.expected_deck_cards ?? game.development_deck;
  for(const card of cardNames) {
    const node=element('span','devcard',`${card}: ${game.information?composition[card].toFixed(2):composition[card]}`);
    node.dataset.highlightId='development-deck-card-'+card.replaceAll(' ','-');details.push(node);
  }
  $('game-details').replaceChildren(...details);
}
function renderEvents() {
  const events=game.events || [], ids=events.map(event=>event.id).join(',');
  if(ids===renderedEventIds) return;
  const list=$('events'), atBottom=list.scrollHeight-list.scrollTop-list.clientHeight<24;
  const retained=new Set(events.map(event=>event.id));
  for(const [id,row] of eventRows) if(!retained.has(id)) {row.remove();eventRows.delete(id);}
  for(const event of events) {
    if(eventRows.has(event.id)) continue;
    const row=element('li','event');row.dataset.eventId=event.id;
    row.append(element('strong','event-message',event.message));
    for(const detail of event.details || []) row.append(element('div','event-detail',detail));
    eventRows.set(event.id,row);list.append(row);
  }
  $('events-empty').hidden=events.length>0;
  list.hidden=events.length===0;
  setText($('events-title'),events[0]?.type==='roll_dice'?'This turn':'Game events');
  if(atBottom) list.scrollTop=list.scrollHeight;
  renderedEventIds=ids;
}
function renderPosition(position) {
  // Commit the rendered version only after all controls have been constructed.
  // A failed initial render must remain eligible for retry on the next update.
  const changed=position.controls.position_version!==renderedPositionVersion;
  game=position;
  renderEvents();
  $('board').dataset.positionVersion=game.controls.position_version;
  if(changed) {selectedId=null;extras=[];analysis=null;analysisLimit=80;analysisRows.clear();$('analysis').replaceChildren();}
  rebuildMoves();
  if(changed) {
    const phaseTool={'initial settlement':'settlement','initial road':'road','free roads':'road','move robber':'robber'}[game.turn.phase];
    tool=phaseTool || (boardTools[tool]&&toolMoves().length?tool:null);
    renderPieces();renderPlayers();renderActions();renderTools();renderTargets();renderSelection();
    renderedPositionVersion=game.controls.position_version;
  }
  setText($('turn-summary'),`Turn ${game.turn.number} · ${game.turn.winner!=null?`P${game.turn.winner+1} wins`:`P${actor()+1} to move`}`);
  setText($('perspective'),game.information?`Player ${game.information.observer+1} perspective`:'Full game · both hands visible');
  setText($('phase'),game.turn.phase);
  $('rewind').disabled=pending||!game.controls.can_rewind;
  ['tree-p1','tree-p2'].forEach((id,p)=>{
    $(id).checked=game.controls.tree_running[p];$(id).disabled=pending||(game.information&&game.information.observer!==p);
  });
}
function renderAnalysis() {
  if(!analysis) {
    setText($('analysis-summary'),game?.turn.winner!=null?'Game finished.':game?.information&&actor()!==game.information.observer?'Analysis is available on your turn.':'Waiting for search…');
    $('analysis').replaceChildren();analysisRows.clear();$('analysis-more').hidden=true;return;
  }
  setText($('analysis-summary'),`P${analysis.perspective+1} · ${analysis.iterations.toLocaleString()} iterations · ${analysis.workers} workers · depth ${analysis.maximum_depth} · ${analysis.unique_root_children} root nodes`);
  const ranked=analysis.actions.filter(result=>moves.has(result.id)).sort((a,b)=>b.win_probability-a.win_probability||b.visits-a.visits||a.index-b.index);
  const visible=ranked.slice(0,analysisLimit),ids=new Set(visible.map(result=>result.id));
  const list=$('analysis');
  for(const [id,row] of analysisRows) if(!ids.has(id)&&!analysisHeld) {row.remove();analysisRows.delete(id);}
  const displayed=analysisHeld ? analysis.actions.filter(result=>analysisRows.has(result.id)) : visible;
  for(const [position,result] of displayed.entries()) {
    const move=moves.get(result.id);let row=analysisRows.get(result.id);
    if(!row) {
      row=button('',()=>selectMove(result.id),'analysis-row');row.dataset.moveId=result.id;
      row.append(element('span','move-name',moveLabel(move)),element('span','move-score'),element('span','move-visits'));
      row.title=move.description;
      row.addEventListener('pointerenter',()=>highlightMove(moves.get(result.id)));
      row.addEventListener('pointerleave',()=>highlightMove(moves.get(selectedId)));
      row.addEventListener('focus',()=>highlightMove(moves.get(result.id)));
      row.addEventListener('blur',()=>highlightMove(moves.get(selectedId)));
      analysisRows.set(result.id,row);
    }
    row.classList.toggle('recommended',result.recommended);row.classList.toggle('selected',result.id===selectedId);
    setText(row.children[1],(result.win_probability*100).toFixed(1)+'%');
    setText(row.children[2],result.visits.toLocaleString());
    if(!analysisHeld && list.children[position]!==row) list.insertBefore(row,list.children[position] || null);
  }
  $('analysis-more').hidden=analysisLimit>=ranked.length;
  setText($('analysis-more'),`Show more moves (${Math.min(analysisLimit,ranked.length)} / ${ranked.length})`);
}
async function commitMove(id) {
  if(pending||!moves.has(id)) return;
  await sendCommand('move',{move_id:id});
}
async function sendCommand(type,extra={}) {
  if(pending||!game) return;
  pending=true;document.body.classList.add('busy');$('commit').disabled=true;$('rewind').disabled=true;
  status('Applying…');
  try {
    const response=await fetch('/command',{method:'POST',headers:{'Content-Type':'application/json'},
      body:JSON.stringify({type,position_version:game.controls.position_version,...extra})});
    const body=await response.text();let reply;
    try {reply=JSON.parse(body);} catch {throw new Error(body);}
    if(!response.ok||!reply.accepted) throw new Error(reply.message || 'The move was rejected.');
    selectedId=null;showPreview(null);highlightMove(null);
    if(syncPromise) await syncPromise;
    await syncUpdates();
    status(reply.message);
  } catch(error) {status(error.message,true);}
  finally {
    pending=false;document.body.classList.remove('busy');renderSelection();
    $('rewind').disabled=!game.controls.can_rewind;
    ['tree-p1','tree-p2'].forEach((id,p)=>{$(id).disabled=Boolean(game.information&&game.information.observer!==p);});
  }
}
async function fetchUpdate() {
  try {
    const query=new URLSearchParams(revisions);
    const response=await fetch('/updates?'+query,{cache:'no-store'});
    if(!response.ok) throw new Error(await response.text());
    const update=await response.json();
    if(update.position && (!update.position.controls || !Number.isInteger(update.position.board?.robber_hex) ||
      !Array.isArray(update.position.transformations) ||
      update.position.transformations.some(move=>typeof move.id!=='string'||typeof move.type!=='string'))) {
      const message='The Catan executable uses an older viewer interface. Rebuild it and restart the visualizer.';
      setText($('board-hint'),message);
      throw new Error(message);
    }
    if(update.layout && revisions.layout!==update.revisions.layout) {layout=update.layout;buildBoard();renderedPositionVersion=null;}
    if(update.position) renderPosition(update.position);
    if(update.catalog && update.catalog.position_version===game.controls.position_version) {
      extras=update.catalog.moves;rebuildMoves();
    }
    if(update.analysis && update.analysis.position_version===game.controls.position_version) analysis=update.analysis.mcts;
    if(update.position||update.catalog||update.analysis) renderAnalysis();
    revisions={...revisions,...update.revisions};
    if(!connected) {connected=true;status('Connected');if(renderedPositionVersion!==null)renderToolHint();}
  } catch(error) {
    connected=false;
    if(renderedPositionVersion===null) setText($('board-hint'),'Unable to load board · '+error.message);
    status('Viewer disconnected · '+error.message,true);
  }
}
function syncUpdates() {
  if(syncPromise) return syncPromise;
  syncPromise=fetchUpdate().finally(()=>{syncPromise=null;});
  return syncPromise;
}
async function refresh() {
  await syncUpdates();
  // A single request at a time; no overlapping refreshes on slow connections.
  window.setTimeout(refresh,document.hidden?1000:200);
}
$('rewind').addEventListener('click',()=>sendCommand('rewind'));
$('commit').addEventListener('click',()=>commitMove(selectedId));
$('cancel').addEventListener('click',()=>{if(!pending){selectedId=null;renderSelection();showPreview(null);renderAnalysis();}});
$('debug-ids').addEventListener('change',event=>$('board').classList.toggle('show-ids',event.target.checked));
['tree-p1','tree-p2'].forEach((id,player)=>$(id).addEventListener('change',event=>sendCommand('tree',{player,enabled:event.target.checked})));
$('analysis').addEventListener('pointerenter',()=>{analysisHeld=true;});
$('analysis').addEventListener('pointerleave',()=>{analysisHeld=$('analysis').contains(document.activeElement);if(!analysisHeld)renderAnalysis();});
$('analysis').addEventListener('focusin',()=>{analysisHeld=true;});
$('analysis').addEventListener('focusout',()=>window.setTimeout(()=>{analysisHeld=$('analysis').matches(':hover')||$('analysis').contains(document.activeElement);if(!analysisHeld)renderAnalysis();},0));
$('analysis-more').addEventListener('click',()=>{analysisLimit+=80;renderAnalysis();});
refresh();
