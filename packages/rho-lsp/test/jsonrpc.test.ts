// Pins: the transport law — a frame's Content-Length counts UTF-8
// BYTES, never string length (ecosystem.md §4: deterministic,
// byte-exact behavior extends to the wire); partial reads must parse;
// a malformed header must resynchronize, never guess. Tested with CJK
// and emoji payloads whose string length differs from byte length.

import { describe, expect, it } from "vitest";
import { FrameReader, writeFrame } from "../src/jsonrpc.js";

function collect(): { frames: string[]; reader: FrameReader } {
  const frames: string[] = [];
  return { frames, reader: new FrameReader((payload) => frames.push(payload)) };
}

describe("frame writing", () => {
  it("Content-Length is the UTF-8 byte length, not the string length", () => {
    const msg = { jsonrpc: "2.0", method: "x", params: { s: "中文😀" } };
    const buf = writeFrame(msg);
    const header = buf.subarray(0, buf.indexOf(13)).toString("utf8");
    const byteLen = Buffer.byteLength(JSON.stringify(msg), "utf8");
    expect(header).toBe(`Content-Length: ${byteLen}`);
    // string length would have undercounted (2 CJK + 1 emoji = 5 units
    // -> 11 bytes): the wire length must exceed it
    expect(JSON.stringify(msg).length).toBeLessThan(byteLen);
  });
});

describe("frame reading", () => {
  it("parses one frame", () => {
    const { frames, reader } = collect();
    const msg = { jsonrpc: "2.0", id: 1, result: null };
    reader.feed(writeFrame(msg));
    expect(frames).toEqual([JSON.stringify(msg)]);
  });

  it("parses two frames arriving in one chunk", () => {
    const { frames, reader } = collect();
    const a = { jsonrpc: "2.0", method: "a" };
    const b = { jsonrpc: "2.0", method: "b" };
    reader.feed(Buffer.concat([writeFrame(a), writeFrame(b)]));
    expect(frames).toEqual([JSON.stringify(a), JSON.stringify(b)]);
  });

  it("parses a frame split across arbitrary chunk boundaries", () => {
    const { frames, reader } = collect();
    const msg = { jsonrpc: "2.0", id: 7, result: { v: "中文😀" } };
    const whole = writeFrame(msg);
    reader.feed(whole.subarray(0, 3));
    expect(frames).toEqual([]);
    reader.feed(whole.subarray(3, 20));
    expect(frames).toEqual([]);
    reader.feed(whole.subarray(20));
    expect(frames).toEqual([JSON.stringify(msg)]);
  });

  it("keeps leftover bytes buffered until their frame completes", () => {
    const { frames, reader } = collect();
    const a = { jsonrpc: "2.0", method: "a" };
    const b = { jsonrpc: "2.0", method: "b" };
    reader.feed(writeFrame(a));
    const bBytes = writeFrame(b);
    reader.feed(bBytes.subarray(0, bBytes.length - 2));
    expect(frames).toEqual([JSON.stringify(a)]);
    expect(reader.pending).toBe(true);
    reader.feed(bBytes.subarray(bBytes.length - 2));
    expect(frames).toEqual([JSON.stringify(a), JSON.stringify(b)]);
  });

  it("drops a header block without Content-Length and resynchronizes", () => {
    const { frames, reader } = collect();
    reader.feed(Buffer.from("garbage: no length\r\n\r\n"));
    expect(frames).toEqual([]);
    const msg = { jsonrpc: "2.0", id: 2, result: null };
    reader.feed(writeFrame(msg));
    expect(frames).toEqual([JSON.stringify(msg)]);
  });
});
