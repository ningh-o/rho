// pins.ts — the pinned wasmtime and wabt versions, and the startup
// self-check that refuses to run against anything else.
//
// Law: docs/ecosystem.md section 1 — "pinned wasmtime and wabt
// versions". The pins live here AND in .github/workflows/ci.yml
// (WASMTIME_VERSION / WABT_VERSION); a trend line measured across tool
// generations is not a trend line, so the harness probes the installed
// tools at startup and refuses (exit 2) on any mismatch or on a
// version it cannot parse.

export const PINNED_WASMTIME = "40.0.0";
export const PINNED_WABT = "1.0.39";

export const PIN_NOTE =
  "the same pins as .github/workflows/ci.yml (WASMTIME_VERSION / WABT_VERSION)";

// Probes a tool: returns its stdout for the given arguments, or null
// when the tool cannot be found or executed at all. Injected by the
// tests; the CLI wires it to spawnToolVersionProbe below.
export type Probe = (argv: readonly string[]) => Promise<string | null>;

// The first x.y.z token in a tool's --version output. wasmtime prints
// "wasmtime 40.0.0"; the wabt tools print "1.0.39". One tolerant
// parser covers both shapes: the exact shapes were not probed by
// executing the tools this wave (static delivery — no programs run),
// so the parser accepts either and the owner's first run confirms it.
export function parseToolVersion(stdout: string): string | null {
  const m = /\d+\.\d+\.\d+/.exec(stdout);
  if (m === null) return null;
  return m[0] ?? null;
}

export interface Verification {
  readonly ok: boolean;
  // A one-line refusal reason; empty when ok. Printed to stderr by the
  // CLI on the refusal path (never part of the deterministic report).
  readonly reason: string;
}

// Checks both pinned tools in a fixed order (wasmtime, then wat2wasm)
// and returns the first refusal, or ok. Refusals name the tool, both
// versions, and the pin's home so the fix is one lookup away.
export async function verifyPins(probe: Probe): Promise<Verification> {
  const checks: ReadonlyArray<readonly [tool: string, pinned: string]> = [
    ["wasmtime", PINNED_WASMTIME],
    ["wat2wasm", PINNED_WABT],
  ];
  for (const [tool, pinned] of checks) {
    const out = await probe([tool, "--version"]);
    if (out === null) {
      return {
        ok: false,
        reason:
          `${tool} not found or not executable — bench pins wasmtime ` +
          `${PINNED_WASMTIME} and wat2wasm (wabt) ${PINNED_WABT} (${PIN_NOTE})`,
      };
    }
    const got = parseToolVersion(out);
    if (got === null) {
      return {
        ok: false,
        reason:
          `${tool} --version printed no x.y.z version ` +
          `(got: ${JSON.stringify(out.slice(0, 80))}) — pins: ` +
          `wasmtime ${PINNED_WASMTIME}, wat2wasm ${PINNED_WABT} (${PIN_NOTE})`,
      };
    }
    if (got !== pinned) {
      return {
        ok: false,
        reason:
          `${tool} version ${got} != pinned ${pinned} ` +
          `(${PIN_NOTE}) — install the pinned version and retry`,
      };
    }
  }
  return { ok: true, reason: "" };
}
