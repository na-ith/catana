#!/usr/bin/env node
/**
 * Capture and decode Colonist WebSocket frames through Chrome's DevTools Protocol.
 *
 * Requires Node.js 22+ and a Chromium browser started with a remote-debugging
 * port. This tool deliberately records frame payloads only; it does not record
 * request headers, cookies, or authentication headers.
 */

import {
  appendFileSync,
  closeSync,
  mkdirSync,
  openSync,
  writeFileSync,
} from "node:fs";
import { dirname, resolve } from "node:path";
import { pathToFileURL } from "node:url";


const textDecoder = new TextDecoder();
const MAX_SAFE_BIGINT = BigInt(Number.MAX_SAFE_INTEGER);
const MIN_SAFE_BIGINT = BigInt(Number.MIN_SAFE_INTEGER);


class MessagePackDecoder {
  constructor(input) {
    this.bytes = input instanceof Uint8Array ? input : new Uint8Array(input);
    this.view = new DataView(
      this.bytes.buffer,
      this.bytes.byteOffset,
      this.bytes.byteLength,
    );
    this.offset = 0;
  }

  ensure(length) {
    if (this.offset + length > this.bytes.byteLength) {
      throw new RangeError(
        `truncated MessagePack value at byte ${this.offset}: need ${length} more byte(s)`,
      );
    }
  }

  take(length) {
    this.ensure(length);
    const result = this.bytes.subarray(this.offset, this.offset + length);
    this.offset += length;
    return result;
  }

  uint8() {
    this.ensure(1);
    return this.view.getUint8(this.offset++);
  }

  int8() {
    this.ensure(1);
    const value = this.view.getInt8(this.offset);
    this.offset += 1;
    return value;
  }

  uint16() {
    this.ensure(2);
    const value = this.view.getUint16(this.offset);
    this.offset += 2;
    return value;
  }

  int16() {
    this.ensure(2);
    const value = this.view.getInt16(this.offset);
    this.offset += 2;
    return value;
  }

  uint32() {
    this.ensure(4);
    const value = this.view.getUint32(this.offset);
    this.offset += 4;
    return value;
  }

  int32() {
    this.ensure(4);
    const value = this.view.getInt32(this.offset);
    this.offset += 4;
    return value;
  }

  uint64() {
    this.ensure(8);
    const value = this.view.getBigUint64(this.offset);
    this.offset += 8;
    return value <= MAX_SAFE_BIGINT ? Number(value) : value;
  }

  int64() {
    this.ensure(8);
    const value = this.view.getBigInt64(this.offset);
    this.offset += 8;
    return value >= MIN_SAFE_BIGINT && value <= MAX_SAFE_BIGINT
      ? Number(value)
      : value;
  }

  float32() {
    this.ensure(4);
    const value = this.view.getFloat32(this.offset);
    this.offset += 4;
    return value;
  }

  float64() {
    this.ensure(8);
    const value = this.view.getFloat64(this.offset);
    this.offset += 8;
    return value;
  }

  string(length) {
    return textDecoder.decode(this.take(length));
  }

  array(length) {
    return Array.from({ length }, () => this.value());
  }

  map(length) {
    const entries = Array.from({ length }, () => [this.value(), this.value()]);
    const canUseObject = entries.every(([key]) =>
      ["string", "number", "boolean", "bigint"].includes(typeof key));
    if (!canUseObject) return new Map(entries);

    const result = {};
    for (const [key, value] of entries) {
      Object.defineProperty(result, String(key), {
        configurable: true,
        enumerable: true,
        value,
        writable: true,
      });
    }
    return result;
  }

  extension(length) {
    const type = this.int8();
    return { $extension: type, $binary: bytesToBase64(this.take(length)) };
  }

  value() {
    const prefix = this.uint8();
    if (prefix <= 0x7f) return prefix;
    if (prefix >= 0xe0) return prefix - 0x100;
    if (prefix >= 0xa0 && prefix <= 0xbf) return this.string(prefix & 0x1f);
    if (prefix >= 0x90 && prefix <= 0x9f) return this.array(prefix & 0x0f);
    if (prefix >= 0x80 && prefix <= 0x8f) return this.map(prefix & 0x0f);

    switch (prefix) {
      case 0xc0: return null;
      case 0xc1: throw new TypeError("reserved MessagePack prefix 0xc1");
      case 0xc2: return false;
      case 0xc3: return true;
      case 0xc4: return this.take(this.uint8());
      case 0xc5: return this.take(this.uint16());
      case 0xc6: return this.take(this.uint32());
      case 0xc7: return this.extension(this.uint8());
      case 0xc8: return this.extension(this.uint16());
      case 0xc9: return this.extension(this.uint32());
      case 0xca: return this.float32();
      case 0xcb: return this.float64();
      case 0xcc: return this.uint8();
      case 0xcd: return this.uint16();
      case 0xce: return this.uint32();
      case 0xcf: return this.uint64();
      case 0xd0: return this.int8();
      case 0xd1: return this.int16();
      case 0xd2: return this.int32();
      case 0xd3: return this.int64();
      case 0xd4: return this.extension(1);
      case 0xd5: return this.extension(2);
      case 0xd6: return this.extension(4);
      case 0xd7: return this.extension(8);
      case 0xd8: return this.extension(16);
      case 0xd9: return this.string(this.uint8());
      case 0xda: return this.string(this.uint16());
      case 0xdb: return this.string(this.uint32());
      case 0xdc: return this.array(this.uint16());
      case 0xdd: return this.array(this.uint32());
      case 0xde: return this.map(this.uint16());
      case 0xdf: return this.map(this.uint32());
      default: throw new TypeError(`unsupported MessagePack prefix 0x${prefix.toString(16)}`);
    }
  }

  decode() {
    const result = this.value();
    if (this.offset !== this.bytes.byteLength) {
      throw new TypeError(
        `MessagePack value has ${this.bytes.byteLength - this.offset} trailing byte(s)`,
      );
    }
    return result;
  }
}


function bytesToBase64(bytes) {
  return Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength).toString("base64");
}


function decodeMessagePack(bytes) {
  return new MessagePackDecoder(bytes).decode();
}


function decodeText(payload) {
  try {
    return JSON.parse(payload);
  } catch {
    return payload;
  }
}


function decodeColonistFrame(direction, opcode, payloadData) {
  if (opcode === 1) {
    return { format: "text", value: decodeText(payloadData) };
  }
  if (opcode !== 2) {
    return { format: "control", value: payloadData };
  }

  const bytes = new Uint8Array(Buffer.from(payloadData, "base64"));
  if (direction === "received") {
    return { format: "messagepack", value: decodeMessagePack(bytes) };
  }

  // Colonist's outgoing binary envelope is:
  // route (u8), message type (u8), UTF-8 recipient length (u8), recipient,
  // followed by a MessagePack payload. A few router commands use raw bytes.
  if (bytes.length < 3) {
    return { format: "binary", value: bytes };
  }
  const recipientLength = bytes[2];
  const payloadOffset = 3 + recipientLength;
  if (payloadOffset > bytes.length) {
    return { format: "binary", value: bytes };
  }
  const envelope = {
    route: bytes[0],
    messageType: bytes[1],
    recipient: textDecoder.decode(bytes.subarray(3, payloadOffset)),
    data: null,
  };
  const payload = bytes.subarray(payloadOffset);
  if (payload.length === 0) return { format: "colonist-envelope", value: envelope };
  try {
    envelope.data = decodeMessagePack(payload);
  } catch {
    envelope.data = payload;
  }
  return { format: "colonist-envelope", value: envelope };
}


function jsonSafe(value, seen = new Set()) {
  if (typeof value === "bigint") return value.toString();
  if (value instanceof Uint8Array) return { $binary: bytesToBase64(value) };
  if (value instanceof Map) {
    return { $map: Array.from(value, ([key, item]) => [jsonSafe(key, seen), jsonSafe(item, seen)]) };
  }
  if (Array.isArray(value)) return value.map((item) => jsonSafe(item, seen));
  if (value && typeof value === "object") {
    if (seen.has(value)) return "[circular]";
    seen.add(value);
    const result = {};
    for (const [key, item] of Object.entries(value)) result[key] = jsonSafe(item, seen);
    seen.delete(value);
    return result;
  }
  if (typeof value === "number" && !Number.isFinite(value)) return String(value);
  return value;
}


function usage() {
  return `Usage: node tools/capture_colonist_ws.mjs [options]

Options:
  --port PORT       Chrome remote-debugging port (default: 9222)
  --page TEXT       Text which must occur in the page URL (default: colonist.io)
  --socket TEXT     Text which must occur in the WebSocket URL
                    (default: socket.svr.colonist.io)
  --output FILE     JSONL destination (default: timestamped file in current directory)
  --verbose         Print complete decoded messages as they arrive
  --list            List debuggable browser pages and exit
  --help            Show this help

The output contains raw payloads and decoded values, but not cookies or request
headers. Start the recorder before reloading Colonist or joining a game.`;
}


function parseArguments(argv) {
  const timestamp = new Date().toISOString().replaceAll(":", "-").replace(/\.\d{3}Z$/, "Z");
  const options = {
    port: 9222,
    page: "colonist.io",
    socket: "socket.svr.colonist.io",
    output: `colonist-websocket-${timestamp}.jsonl`,
    verbose: false,
    list: false,
    help: false,
  };
  const valued = new Set(["--port", "--page", "--socket", "--output"]);
  for (let index = 0; index < argv.length; ++index) {
    const argument = argv[index];
    if (valued.has(argument)) {
      if (index + 1 >= argv.length) throw new Error(`${argument} requires a value`);
      const key = argument.slice(2);
      options[key] = argv[++index];
    } else if (argument === "--verbose") {
      options.verbose = true;
    } else if (argument === "--list") {
      options.list = true;
    } else if (argument === "--help" || argument === "-h") {
      options.help = true;
    } else {
      throw new Error(`unknown option: ${argument}`);
    }
  }
  options.port = Number(options.port);
  if (!Number.isInteger(options.port) || options.port < 1 || options.port > 65535) {
    throw new Error("--port must be an integer from 1 to 65535");
  }
  return options;
}


function preview(value, limit = 500) {
  const rendered = JSON.stringify(jsonSafe(value));
  if (rendered === undefined) return String(value);
  return rendered.length <= limit ? rendered : `${rendered.slice(0, limit)}…`;
}


async function main(argv = process.argv.slice(2)) {
  let options;
  try {
    options = parseArguments(argv);
  } catch (error) {
    console.error(`capture_colonist_ws: ${error.message}`);
    console.error(usage());
    return 2;
  }
  if (options.help) {
    console.log(usage());
    return 0;
  }

  const endpoint = `http://127.0.0.1:${options.port}/json/list`;
  let targets;
  try {
    const response = await fetch(endpoint);
    if (!response.ok) throw new Error(`${response.status} ${response.statusText}`);
    targets = await response.json();
  } catch (error) {
    console.error(`capture_colonist_ws: cannot reach Chrome at ${endpoint}: ${error.message}`);
    console.error("Start Chrome with --remote-debugging-port and a dedicated --user-data-dir.");
    return 1;
  }

  const pages = targets.filter((target) => target.type === "page");
  if (options.list) {
    for (const page of pages) console.log(`${page.id}\t${page.url}\t${page.title}`);
    return 0;
  }
  const target = pages.find((page) => page.url.includes(options.page));
  if (!target?.webSocketDebuggerUrl) {
    console.error(`capture_colonist_ws: no debuggable page URL contains ${JSON.stringify(options.page)}`);
    console.error(`Run with --list to inspect ${pages.length} available page(s).`);
    return 1;
  }

  const output = resolve(options.output);
  mkdirSync(dirname(output), { recursive: true });
  writeFileSync(output, "", { flag: "wx" });
  const outputHandle = openSync(output, "a");
  const socketUrls = new Map();
  const trackedSockets = new Set();
  let frameCount = 0;
  let decodedCount = 0;
  let finished = false;

  const browserSocket = new WebSocket(target.webSocketDebuggerUrl);
  const finish = (status = 0) => {
    if (finished) return;
    finished = true;
    try { browserSocket.close(); } catch {}
    closeSync(outputHandle);
    console.log(`Captured ${frameCount} frame(s), decoded ${decodedCount}; log: ${output}`);
    process.exitCode = status;
  };

  browserSocket.addEventListener("open", () => {
    browserSocket.send(JSON.stringify({ id: 1, method: "Network.enable" }));
    console.log(`Attached to ${target.url}`);
    console.log(`Waiting for WebSockets containing ${JSON.stringify(options.socket)}.`);
    console.log("Reload Colonist now, then join or resume the game. Press Ctrl-C to stop.");
  });

  browserSocket.addEventListener("message", (event) => {
    const message = JSON.parse(String(event.data));
    if (message.method === "Network.webSocketCreated") {
      const { requestId, url } = message.params;
      socketUrls.set(requestId, url);
      if (url.includes(options.socket)) {
        trackedSockets.add(requestId);
        console.log(`Tracking ${url}`);
      }
      return;
    }

    const directions = {
      "Network.webSocketFrameReceived": "received",
      "Network.webSocketFrameSent": "sent",
    };
    const direction = directions[message.method];
    if (!direction) return;
    const { requestId, response, timestamp } = message.params;
    if (!trackedSockets.has(requestId)) return;

    ++frameCount;
    let decoded;
    let decodeError;
    try {
      decoded = decodeColonistFrame(direction, response.opcode, response.payloadData);
      ++decodedCount;
    } catch (error) {
      decodeError = error instanceof Error ? error.message : String(error);
    }

    const record = jsonSafe({
      recordedAt: new Date().toISOString(),
      direction,
      requestId,
      url: socketUrls.get(requestId),
      monotonicTimestamp: timestamp,
      opcode: response.opcode,
      raw: response.opcode === 2
        ? { encoding: "base64", data: response.payloadData }
        : { encoding: "text", data: response.payloadData },
      decoded,
      decodeError,
    });
    appendFileSync(outputHandle, `${JSON.stringify(record)}\n`);

    const value = decoded?.value;
    const type = direction === "received" ? value?.id : value?.messageType;
    const prefix = direction === "received" ? "←" : "→";
    const label = type === undefined ? decoded?.format ?? "undecoded" : `type ${type}`;
    const detail = decodeError ? `decode error: ${decodeError}` : preview(value);
    if (options.verbose) console.log(`${prefix} ${label}\n${JSON.stringify(jsonSafe(value), null, 2)}`);
    else console.log(`${prefix} ${label}: ${detail}`);
  });

  browserSocket.addEventListener("error", () => {
    console.error("capture_colonist_ws: Chrome debugging connection failed");
    finish(1);
  });
  browserSocket.addEventListener("close", () => finish(process.exitCode ?? 0));
  process.once("SIGINT", () => finish(0));
  process.once("SIGTERM", () => finish(0));
  return new Promise((resolvePromise) => {
    browserSocket.addEventListener("close", () => resolvePromise(process.exitCode ?? 0));
  });
}


export {
  MessagePackDecoder,
  decodeColonistFrame,
  decodeMessagePack,
  jsonSafe,
  parseArguments,
};


const invokedPath = process.argv[1] ? pathToFileURL(resolve(process.argv[1])).href : "";
if (import.meta.url === invokedPath) {
  process.exitCode = await main();
}
