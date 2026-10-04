// generation.ts — version self-validation. The ecosystem version policy
// (docs/ecosystem.md) is absolute: every package pins the exact
// compiler generation it was built from; a release never retags the
// compiler. artifacts/generation.json is that pin, written by
// tools/build-check.mjs, and this module is the server-side half of the
// contract: at startup the server verifies the pin against the world
// it is about to serve and GOES INERT on any mismatch — a wrong-
// generation or tampered toolchain answers nothing rather than risk
// wrong diagnostics (ecosystem.md §4: a degraded server degrades to
// no-LSP, never to wrong answers).
//
// The pin carries: the rho commit, per-file SHA-256 of the compiler
// sources (libs/compiler/*.rho) the check face is built from, SHA-256
// of the two canary artifacts the chain produced, the pinned tool
// versions, and whether the double-build determinism leg held. No
// timestamps, no volatile fields: two clean runs of build-check.mjs
// produce byte-identical JSON (the determinism law reaches this file).
//
// Pins: test/generation.test.ts.

import { createHash } from "node:crypto";
import { readdir, readFile, stat } from "node:fs/promises";
import { join } from "node:path";

export const GENERATION_SCHEMA = "rho-lsp.generation/1";

export interface GenerationManifest {
  schemaVersion: typeof GENERATION_SCHEMA;
  rhoCommit: string;
  /** libs/compiler/*.rho file name -> SHA-256 (source the face is built from). */
  compilerSources: Record<string, string>;
  /** Canary artifacts -> SHA-256 (stock mirror + fmt self face). */
  artifacts: Record<string, string>;
  tools: {
    /** Versions as the tools themselves report them; null = not found at build time. */
    wasmtime: string | null;
    wat2wasm: string | null;
  };
  /** The double-build byte-compare held when the generation was made. */
  rebuildIdentical: boolean;
}

export type GenerationCheck =
  | { ok: true; manifest: GenerationManifest }
  | { ok: false; reason: string };

export function sha256Hex(data: Buffer | string): string {
  return createHash("sha256").update(data).digest("hex");
}

/** Parse and shape-check a manifest (no filesystem access). */
export function parseGeneration(text: string): GenerationCheck {
  let value: unknown;
  try {
    value = JSON.parse(text);
  } catch {
    return { ok: false, reason: "generation.json is not valid JSON" };
  }
  if (value === null || typeof value !== "object") return { ok: false, reason: "generation.json is not an object" };
  const m = value as Record<string, unknown>;
  if (m["schemaVersion"] !== GENERATION_SCHEMA) {
    return { ok: false, reason: `unsupported schemaVersion ${JSON.stringify(m["schemaVersion"])}` };
  }
  if (typeof m["rhoCommit"] !== "string" || m["rhoCommit"] === "") {
    return { ok: false, reason: "rhoCommit missing" };
  }
  if (typeof m["rebuildIdentical"] !== "boolean") {
    return { ok: false, reason: "rebuildIdentical missing" };
  }
  if (!m["rebuildIdentical"]) {
    return { ok: false, reason: "the generation's own determinism leg failed (rebuildIdentical=false)" };
  }
  for (const key of ["compilerSources", "artifacts", "tools"] as const) {
    if (m[key] === null || typeof m[key] !== "object") return { ok: false, reason: `${key} missing` };
  }
  const tools = m["tools"] as Record<string, unknown>;
  for (const t of ["wasmtime", "wat2wasm"] as const) {
    if (tools[t] !== null && typeof tools[t] !== "string") return { ok: false, reason: `tools.${t} malformed` };
  }
  return { ok: true, manifest: m as unknown as GenerationManifest };
}

export interface VerifyPaths {
  /** artifacts/generation.json */
  manifestPath: string;
  /** Directory holding the canary artifacts named by the manifest. */
  artifactsDir: string;
  /** Directory holding libs/compiler/*.rho. */
  compilerDir: string;
}

/**
 * Full verification: manifest parses, the pinned compiler sources hash
 * to exactly what is on disk, and the canary artifacts re-hash to
 * their pins. Any mismatch is a wrong-generation alarm.
 */
export async function verifyGeneration(paths: VerifyPaths): Promise<GenerationCheck> {
  let manifestText: string;
  try {
    manifestText = await readFile(paths.manifestPath, "utf8");
  } catch {
    return { ok: false, reason: "generation.json not found — run tools/build-check.mjs first" };
  }
  const parsed = parseGeneration(manifestText);
  if (!parsed.ok) return parsed;
  const manifest = parsed.manifest;

  for (const [name, pinned] of Object.entries(manifest.compilerSources)) {
    const p = join(paths.compilerDir, name);
    try {
      const text = await readFile(p, "utf8");
      if (sha256Hex(text) !== pinned) return { ok: false, reason: `compiler source ${name} does not match the pinned generation` };
    } catch {
      return { ok: false, reason: `compiler source ${name} not found under ${paths.compilerDir}` };
    }
  }
  // and nothing extra may sit in libs/compiler: a .rho source the pin
  // does not know about is a generation the server cannot name
  try {
    const onDisk = await readdir(paths.compilerDir);
    for (const entry of onDisk) {
      if (entry.endsWith(".rho") && !(entry in manifest.compilerSources)) {
        return { ok: false, reason: `compiler source ${entry} is not part of the pinned generation` };
      }
    }
  } catch {
    return { ok: false, reason: `compiler source directory ${paths.compilerDir} not found` };
  }
  for (const [artifact, pinned] of Object.entries(manifest.artifacts)) {
    const p = join(paths.artifactsDir, artifact);
    try {
      await stat(p);
      const data = await readFile(p);
      if (sha256Hex(data) !== pinned) return { ok: false, reason: `artifact ${artifact} does not match the pinned hash (stale or tampered build)` };
    } catch {
      return { ok: false, reason: `artifact ${artifact} not found — run tools/build-check.mjs first` };
    }
  }
  return { ok: true, manifest };
}
