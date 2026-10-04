// Pins: docs/ecosystem.md section 1 — "pinned wasmtime and wabt
// versions", and the ask's "wasmtime (40.0.0) and wabt pinned
// versions, self-verified at startup, refused on mismatch". The
// constants must equal .github/workflows/ci.yml's WASMTIME_VERSION /
// WABT_VERSION; the version parser and every refusal path of the
// startup check are pinned here against injected probes — nothing is
// spawned.

import test from "node:test";
import assert from "node:assert/strict";

import {
  PINNED_WABT,
  PINNED_WASMTIME,
  parseToolVersion,
  verifyPins,
} from "../src/pins.ts";
import type { Probe } from "../src/pins.ts";

test("pins: the versions are the CI pins (never let these drift apart)", () => {
  // .github/workflows/ci.yml: WASMTIME_VERSION: 40.0.0 / WABT_VERSION: 1.0.39
  assert.equal(PINNED_WASMTIME, "40.0.0");
  assert.equal(PINNED_WABT, "1.0.39");
});

test("pins: parseToolVersion reads wasmtime's prefixed shape", () => {
  assert.equal(parseToolVersion("wasmtime 40.0.0\n"), "40.0.0");
});

test("pins: parseToolVersion reads wabt's bare shape", () => {
  assert.equal(parseToolVersion("1.0.39\n"), "1.0.39");
});

test("pins: parseToolVersion survives banners around the version", () => {
  assert.equal(parseToolVersion("wasmtime 41.2.3 (cranelift)"), "41.2.3");
});

test("pins: parseToolVersion returns null on versionless output", () => {
  assert.equal(parseToolVersion(""), null);
  assert.equal(parseToolVersion("no version here\n"), null);
});

test("pins: parseToolVersion takes the FIRST x.y.z token", () => {
  assert.equal(parseToolVersion("1.0.38 extra 9.9.9"), "1.0.38");
});

// A probe that answers per tool. Every fixture must be shaped per
// tool: one canned stdout for both tools parses as a wasmtime version
// on wat2wasm too and mis-flags the wabt pin (the bug this replaces).
function probeTools(versions: Readonly<Record<string, string>>): Probe {
  return async (argv) => {
    const tool = argv[0] ?? "";
    const out = versions[tool];
    return out === undefined ? null : out;
  };
}

test("verifyPins: matching tools verify clean", async () => {
  const v = await verifyPins(
    probeTools({ wasmtime: "wasmtime 40.0.0\n", wat2wasm: "1.0.39\n" }),
  );
  assert.equal(v.ok, true);
  assert.equal(v.reason, "");
});

test("verifyPins: a version mismatch is refused, naming both versions", async () => {
  const v = await verifyPins(
    probeTools({ wasmtime: "wasmtime 41.0.0\n", wat2wasm: "1.0.39\n" }),
  );
  assert.equal(v.ok, false);
  assert.match(v.reason, /wasmtime version 41\.0\.0 != pinned 40\.0\.0/);
  assert.match(v.reason, /ci\.yml/);
});

test("verifyPins: wabt is checked too (wat2wasm carries the wabt version)", async () => {
  // wasmtime matches; wat2wasm drifts — the refusal must name wat2wasm.
  const v = await verifyPins(
    probeTools({ wasmtime: "wasmtime 40.0.0\n", wat2wasm: "1.0.38\n" }),
  );
  assert.equal(v.ok, false);
  assert.match(v.reason, /wat2wasm version 1\.0\.38 != pinned 1\.0\.39/);
});

test("verifyPins: a missing tool is refused, naming the tool and the pins", async () => {
  const v = await verifyPins(probeTools({}));
  assert.equal(v.ok, false);
  assert.match(v.reason, /wasmtime not found/);
  assert.match(v.reason, /40\.0\.0/);
  assert.match(v.reason, /1\.0\.39/);
});

test("verifyPins: versionless output is refused (not silently accepted)", async () => {
  const v = await verifyPins(
    probeTools({ wasmtime: "garbage\n", wat2wasm: "1.0.39\n" }),
  );
  assert.equal(v.ok, false);
  assert.match(v.reason, /no x\.y\.z version/);
});

test("verifyPins: checks in a fixed order — wasmtime first", async () => {
  const seen: string[] = [];
  const probe: Probe = async (argv) => {
    const tool = argv[0];
    if (tool !== undefined) seen.push(tool);
    return tool === "wasmtime" ? "wasmtime 40.0.0\n" : "0.0.0\n";
  };
  const v = await verifyPins(probe);
  assert.deepEqual(seen, ["wasmtime", "wat2wasm"]);
  assert.equal(v.ok, false);
  assert.match(v.reason, /wat2wasm version 0\.0\.0 != pinned 1\.0\.39/);
});
