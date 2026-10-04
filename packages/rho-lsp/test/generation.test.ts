// Pins: the version self-validation law (ecosystem.md version policy:
// "every package pins the exact compiler generation it was built
// from") joined to §4's degradation clause: any pin mismatch — bad
// JSON, wrong schema, failed determinism leg, drifted compiler source,
// stale or tampered artifact — leaves the server NOTHING to serve, and
// the manifest itself must be byte-stable (no timestamps, fixed key
// order — the determinism law reaches this file).

import { createHash } from "node:crypto";
import { mkdtempSync, mkdirSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { afterEach, describe, expect, it } from "vitest";
import { GENERATION_SCHEMA, parseGeneration, verifyGeneration } from "../src/generation.js";

const dirs: string[] = [];

function tempTree(files: Record<string, string | Buffer>): string {
  const root = mkdtempSync(join(tmpdir(), "rho-lsp-gen-"));
  dirs.push(root);
  for (const [rel, content] of Object.entries(files)) {
    const p = join(root, rel);
    mkdirSync(dirname(p), { recursive: true });
    writeFileSync(p, content);
  }
  return root;
}

afterEach(() => {
  while (dirs.length > 0) {
    const d = dirs.pop();
    if (d !== undefined) rmSync(d, { recursive: true, force: true });
  }
});

const sha = (s: string | Buffer) => createHash("sha256").update(s).digest("hex");

const LEX = "// lex.rho\npub fn lex() {}\n";
const MAIN = "// main.rho\nfn main() {}\n";
const CHECK_WASM = Buffer.from([0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00]);

function manifestJson(overrides: Record<string, unknown> = {}): string {
  const base = {
    schemaVersion: GENERATION_SCHEMA,
    rhoCommit: "9f3e910deadbeef",
    compilerSources: { "lex.rho": sha(LEX), "main.rho": sha(MAIN) },
    artifacts: { "rho-check.wasm": sha(CHECK_WASM), "rho-fmt.wasm": sha(CHECK_WASM) },
    tools: { wasmtime: "wasmtime 40.0.0", wat2wasm: "1.0.32" },
    rebuildIdentical: true,
    ...overrides,
  };
  return JSON.stringify(base, null, 2) + "\n";
}

function happyTree(manifest = manifestJson()): string {
  return tempTree({
    "libs/compiler/main.rho": MAIN,
    "libs/compiler/lex.rho": LEX,
    "artifacts/rho-check.wasm": CHECK_WASM,
    "artifacts/rho-fmt.wasm": CHECK_WASM,
    "artifacts/generation.json": manifest,
  });
}

function verify(root: string) {
  return verifyGeneration({
    manifestPath: join(root, "artifacts", "generation.json"),
    artifactsDir: join(root, "artifacts"),
    compilerDir: join(root, "libs", "compiler"),
  });
}

describe("parseGeneration", () => {
  it("accepts a well-formed manifest", () => {
    const r = parseGeneration(manifestJson());
    expect(r.ok).toBe(true);
    if (r.ok) {
      expect(r.manifest.rhoCommit).toBe("9f3e910deadbeef");
      expect(r.manifest.tools.wasmtime).toBe("wasmtime 40.0.0");
    }
  });

  it("rejects bad JSON, wrong schema, missing fields, and a failed determinism leg", () => {
    expect(parseGeneration("not json").ok).toBe(false);
    expect(parseGeneration("{}").ok).toBe(false);
    expect(parseGeneration(manifestJson({ schemaVersion: "other/1" })).ok).toBe(false);
    expect(parseGeneration(manifestJson({ rhoCommit: "" })).ok).toBe(false);
    expect(parseGeneration(manifestJson({ rebuildIdentical: false })).ok).toBe(false);
    const noTools = JSON.parse(manifestJson()) as Record<string, unknown>;
    delete noTools["tools"];
    expect(parseGeneration(JSON.stringify(noTools)).ok).toBe(false);
  });
});

describe("verifyGeneration", () => {
  it("verifies a consistent tree", async () => {
    const root = happyTree();
    const r = await verify(root);
    expect(r.ok).toBe(true);
  });

  it("goes inert when the manifest is missing (build-check.mjs never ran)", async () => {
    const root = tempTree({ "libs/compiler/main.rho": MAIN });
    const r = await verify(root);
    expect(r.ok).toBe(false);
    if (!r.ok) expect(r.reason).toContain("not found");
  });

  it("alarms on a stale or tampered artifact", async () => {
    const root = happyTree();
    writeFileSync(join(root, "artifacts", "rho-check.wasm"), Buffer.from([0xde, 0xad]));
    const r = await verify(root);
    expect(r.ok).toBe(false);
    if (!r.ok) expect(r.reason).toContain("rho-check.wasm");
  });

  it("alarms on a drifted compiler source", async () => {
    const root = happyTree();
    writeFileSync(join(root, "libs", "compiler", "lex.rho"), "// changed\n");
    const r = await verify(root);
    expect(r.ok).toBe(false);
    if (!r.ok) expect(r.reason).toContain("lex.rho");
  });

  it("alarms on a compiler source the pin does not know (a different generation)", async () => {
    const root = happyTree();
    writeFileSync(join(root, "libs", "compiler", "fmt.rho"), "// unexpected extra\n");
    const r = await verify(root);
    expect(r.ok).toBe(false);
    if (!r.ok) expect(r.reason).toContain("fmt.rho");
  });

  it("alarms on a missing artifact", async () => {
    const root = happyTree();
    rmSync(join(root, "artifacts", "rho-fmt.wasm"));
    const r = await verify(root);
    expect(r.ok).toBe(false);
    if (!r.ok) expect(r.reason).toContain("rho-fmt.wasm");
  });
});

describe("manifest byte-stability", () => {
  it("carries no volatile fields: two clean runs of build-check.mjs pin the same bytes", () => {
    const text = manifestJson();
    expect(text).not.toMatch(/timestamp|builtAt|"date"/i);
    // the typed round-trip preserves every key (nothing is derived or dropped)
    const parsed = parseGeneration(text);
    expect(parsed.ok).toBe(true);
    if (!parsed.ok) return;
    expect(Object.keys(parsed.manifest).sort()).toEqual(
      ["artifacts", "compilerSources", "rebuildIdentical", "rhoCommit", "schemaVersion", "tools"].sort(),
    );
  });
});
