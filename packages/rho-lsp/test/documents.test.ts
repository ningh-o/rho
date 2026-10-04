// Pins: the document-sync contract — full-text sync only, version
// tracking, and the size cap (ecosystem.md §4: every request
// time-capped and degrading honestly; an oversized document is skipped
// and cleared, never half-served). Change-without-open is protocol
// misuse and is ignored, never fabricated.

import { describe, expect, it } from "vitest";
import { DocumentStore } from "../src/documents.js";

describe("DocumentStore", () => {
  it("opens, changes (full sync), and closes documents", () => {
    const store = new DocumentStore();
    const uri = "file:///w/main.rho";
    store.open(uri, 1, "fn main() {}\n", "/w/main.rho");
    expect(store.get(uri)?.version).toBe(1);
    store.change(uri, 2, "fn main() -> i32 { return 0; }\n");
    expect(store.get(uri)?.version).toBe(2);
    expect(store.get(uri)?.text).toContain("return 0");
    expect(store.close(uri)).toBe(true);
    expect(store.get(uri)).toBeUndefined();
    expect(store.close(uri)).toBe(false);
  });

  it("tracks UTF-8 byte size and flags over-cap documents", () => {
    const store = new DocumentStore(32);
    const uri = "file:///w/big.rho";
    const doc = store.open(uri, 1, "let s = \"中文\";\n");
    expect(doc.bytes).toBe(Buffer.byteLength(doc.text, "utf8"));
    expect(store.overCap(doc)).toBe(false);
    const big = store.change(uri, 2, "x".repeat(40));
    expect(store.overCap(big as NonNullable<typeof big>)).toBe(true);
  });

  it("records sibling .rho documents of the same directory only", () => {
    const store = new DocumentStore();
    const root = store.open("file:///w/main.rho", 1, "use helper;\n", "/w/main.rho");
    store.open("file:///w/helper.rho", 1, "pub fn help() {}\n", "/w/helper.rho");
    store.open("file:///w/other.txt", 1, "not rho", "/w/other.txt");
    store.open("file:///sub/deep.rho", 1, "fn deep() {}\n", "/sub/deep.rho");
    const siblings = store.siblingsOf(root);
    expect(siblings.map((s) => s.path)).toEqual(["/w/helper.rho"]);
  });

  it("ignores a change for a document that was never opened", () => {
    const store = new DocumentStore();
    expect(store.change("file:///nope.rho", 1, "text")).toBeUndefined();
  });
});
