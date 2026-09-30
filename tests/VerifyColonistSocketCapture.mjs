#!/usr/bin/env node

import assert from "node:assert/strict";
import {
  decodeColonistFrame,
  decodeMessagePack,
  jsonSafe,
  parseArguments,
} from "../tools/capture_colonist_ws.mjs";


function bytes(...values) {
  return new Uint8Array(values.flat());
}


const message = bytes(
  0x82,
  0xa2, 0x69, 0x64, 0x2a,
  0xa4, 0x64, 0x61, 0x74, 0x61,
  0x86,
  0xa7, 0x74, 0x65, 0x72, 0x72, 0x61, 0x69, 0x6e,
  0xa6, 0x66, 0x6f, 0x72, 0x65, 0x73, 0x74,
  0xa5, 0x72, 0x6f, 0x6c, 0x6c, 0x73, 0x92, 0x06, 0x08,
  0xa6, 0x61, 0x63, 0x74, 0x69, 0x76, 0x65, 0xc3,
  0xa7, 0x6d, 0x69, 0x73, 0x73, 0x69, 0x6e, 0x67, 0xc0,
  0xa8, 0x6e, 0x65, 0x67, 0x61, 0x74, 0x69, 0x76, 0x65, 0xfd,
  0xa5, 0x63, 0x6f, 0x75, 0x6e, 0x74, 0xcd, 0x01, 0x2c,
);
assert.deepEqual(decodeMessagePack(message), {
  id: 42,
  data: {
    terrain: "forest",
    rolls: [6, 8],
    active: true,
    missing: null,
    negative: -3,
    count: 300,
  },
});

const incoming = decodeColonistFrame(
  "received",
  2,
  Buffer.from(message).toString("base64"),
);
assert.equal(incoming.format, "messagepack");
assert.equal(incoming.value.id, 42);
assert.equal(incoming.value.data.terrain, "forest");

const outgoingPayload = bytes(
  0x81, 0xa4, 0x6d, 0x6f, 0x76, 0x65, 0xa4, 0x72, 0x6f, 0x61, 0x64,
);
const outgoing = decodeColonistFrame(
  "sent",
  2,
  Buffer.from(bytes(3, 17, 2, 0x70, 0x31, ...outgoingPayload)).toString("base64"),
);
assert.deepEqual(outgoing, {
  format: "colonist-envelope",
  value: { route: 3, messageType: 17, recipient: "p1", data: { move: "road" } },
});

const binary = decodeMessagePack(bytes(0x81, 0xa4, 0x62, 0x6c, 0x6f, 0x62, 0xc4, 3, 0, 255, 127));
assert.deepEqual(jsonSafe(binary), { blob: { $binary: "AP9/" } });

const largeInteger = decodeMessagePack(bytes(0xcf, 0, 0x20, 0, 0, 0, 0, 0, 0));
assert.equal(largeInteger, 9007199254740992n);
assert.equal(jsonSafe(largeInteger), "9007199254740992");

assert.deepEqual(parseArguments(["--port", "9333", "--verbose", "--output", "capture.jsonl"]), {
  port: 9333,
  page: "colonist.io",
  socket: "socket.svr.colonist.io",
  output: "capture.jsonl",
  verbose: true,
  list: false,
  help: false,
});

assert.throws(() => decodeMessagePack(bytes(0x92, 1)), /truncated MessagePack/);
console.log("Colonist socket capture decoder checks passed");
