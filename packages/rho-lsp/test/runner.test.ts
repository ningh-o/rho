// Pins: the embedding-face contract — SubprocessRunner must speak the
// canonical two-step the repo's own green legs pin (tests/run-corpus-diff.sh:56-61,
// tests/run-selfhost.sh): (1) `rho build libs/compiler/main.rho -o
// <tmp>/face.wasm --set SRC=<doc> --set MODS=<blocks> [--set FMT=1]`,
// (2) bare `wasmtime face.wasm`. Faces: step-1 failure = toolchain
// trouble (a runner error, NEVER document diagnostics); rc 0 =
// accepted; rc 1 = clean refusal whose stderr IS the document's
// diagnostics; rc > 1 = the compiler died (runner error). Every spawn
// is time-capped (ecosystem.md §4: every request time-capped). The
// MODS block format is pinned byte-for-byte against run-corpus-diff.sh.

import { chmodSync, existsSync, mkdirSync, mkdtempSync, readdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { afterEach, beforeEach, describe, expect, it } from "vitest";
import { SubprocessRunner } from "../src/subprocess-runner.js";
import { assembleMods } from "../src/runner.js";
import type { CheckRequest } from "../src/runner.js";

let work: string;
let cannedDir: string;

const FAKE_RHO = `#!/bin/sh
# fake boot: dump argv, touch the -o target, exit $RHO_FAKE_RC (default 0)
i=0
for a in "$@"; do
  printf '%s' "$a" > "$RHO_FAKE_DIR/arg-$i"
  i=$((i+1))
done
out=""
prev=""
for a in "$@"; do
  [ "$prev" = "-o" ] && out="$a"
  prev="$a"
done
: > "$out"
exit "\${RHO_FAKE_RC:-0}"
`;

const FAKE_WASMTIME = `#!/bin/sh
# fake wasm face: canned stderr rides fd 2 (the compiler's diagnostic
# stream), canned stdout on fd 1; exit the canned rc
cat "$RHO_FAKE_DIR/stderr" 1>&2 2>/dev/null
cat "$RHO_FAKE_DIR/stdout" 2>/dev/null
if [ -f "$RHO_FAKE_DIR/sleep" ]; then
  sleep "$(cat "$RHO_FAKE_DIR/sleep")"
fi
exit "$(cat "$RHO_FAKE_DIR/rc" 2>/dev/null || echo 0)"
`;

beforeEach(() => {
  work = mkdtempSync(join(tmpdir(), "rho-lsp-runner-"));
  cannedDir = join(work, "canned");
  const bin = join(work, "bin");
  const repo = join(work, "repo", "libs", "compiler");
  mkdirSync(cannedDir);
  mkdirSync(bin);
  mkdirSync(repo, { recursive: true });
  for (const f of ["main.rho", "lex.rho", "parse.rho"]) writeFileSync(join(repo, f), `// ${f}\n`);
  writeFileSync(join(bin, "fake-rho"), FAKE_RHO);
  writeFileSync(join(bin, "fake-wasmtime"), FAKE_WASMTIME);
  chmodSync(join(bin, "fake-rho"), 0o755);
  chmodSync(join(bin, "fake-wasmtime"), 0o755);
});

afterEach(() => {
  delete process.env.RHO_FAKE_DIR;
  rmSync(work, { recursive: true, force: true });
});

function makeRunner(deadlineMs = 5000): SubprocessRunner {
  return new SubprocessRunner({
    rhoBinary: join(work, "bin", "fake-rho"),
    repoRoot: join(work, "repo"),
    wasmtime: join(work, "bin", "fake-wasmtime"),
    deadlineMs,
  });
}

function req(overrides: Partial<CheckRequest> = {}): CheckRequest {
  return {
    rootText: "fn main() -> i32 { return 0; }",
    modules: [
      { path: "b.rho", text: "pub fn b() {}\n" },
      { path: "a.rho", text: "pub fn a() {}\n" },
    ],
    docPath: "/doc/main.rho",
    ...overrides,
  };
}

function arg(n: number): string {
  return readFileSync(join(cannedDir, `arg-${n}`), "utf8");
}

describe("step 1: the boot bake", () => {
  it("invokes rho build on libs/compiler/main.rho with SRC and MODS baked", async () => {
    process.env.RHO_FAKE_DIR = cannedDir;
    const runner = makeRunner();
    const r = await runner.check(req(), 5000);
    expect(r.ok).toBe(true);
    expect(arg(0)).toBe("build");
    expect(arg(1)).toBe(join(work, "repo", "libs", "compiler", "main.rho"));
    expect(arg(2)).toBe("-o");
    expect(arg(3)).toMatch(/face\.wasm$/);
    expect(arg(4)).toBe("--set");
    expect(arg(5)).toBe(`SRC=${req().rootText}`);
    expect(arg(6)).toBe("--set");
    // MODS: name-sorted blocks, the run-corpus-diff face, byte for byte
    // (arg(7) is the full --set value: the MODS= prefix rides it)
    expect(arg(7)).toBe("MODS=" + assembleMods([{ path: "a.rho", text: "pub fn a() {}\n" }, { path: "b.rho", text: "pub fn b() {}\n" }]));
    expect(existsSync(join(cannedDir, "arg-8"))).toBe(false); // no FMT face on check
  });

  it("adds FMT=1 for the format face", async () => {
    process.env.RHO_FAKE_DIR = cannedDir;
    const runner = makeRunner();
    await runner.format(req(), 5000);
    expect(arg(8)).toBe("--set");
    expect(arg(9)).toBe("FMT=1");
  });

  it("leaves no build leftovers in the runner's own tree", async () => {
    process.env.RHO_FAKE_DIR = cannedDir;
    const runner = makeRunner();
    await runner.check(req(), 5000);
    // face.wasm lives in a private mkdtemp under the OS tmpdir (created
    // and removed per request); nothing spills into the repo work dir
    const names = readdirSync(work);
    expect(names.filter((n) => n.startsWith("face.wasm"))).toEqual([]);
  });
});

describe("step 2: the wasm face", () => {
  it("rc 0 = accepted; stdout rides WAT (check) or canonical text (fmt)", async () => {
    process.env.RHO_FAKE_DIR = cannedDir;
    writeFileSync(join(cannedDir, "rc"), "0");
    writeFileSync(join(cannedDir, "stdout"), "(module)\n");
    const runner = makeRunner();
    const check = await runner.check(req(), 5000);
    expect(check.ok).toBe(true);
    expect(check.exitCode).toBe(0);
    expect(check.stdout).toBe("(module)\n");
    const fmt = await runner.format(req(), 5000);
    expect(fmt.ok).toBe(true);
    expect(fmt.text).toBe("(module)\n");
  });

  it("rc 1 = clean refusal: stderr becomes the document's parsed diagnostics", async () => {
    process.env.RHO_FAKE_DIR = cannedDir;
    writeFileSync(join(cannedDir, "rc"), "1");
    writeFileSync(join(cannedDir, "stderr"), "check: unknown fn 'mystery'\ncheck: 1 error(s)\n");
    const runner = makeRunner();
    const r = await runner.check(req(), 5000);
    expect(r.ok).toBe(false);
    expect(r.exitCode).toBe(1);
    expect(r.parsed.diags.map((d) => d.message)).toEqual(["check: unknown fn 'mystery'"]);
    // the summary line is the checker's bookkeeping, not a diagnostic
    expect(r.parsed.diags).toHaveLength(1);
  });

  it("rc > 1 = the compiler died mid-run: a runner error, never document diagnostics", async () => {
    process.env.RHO_FAKE_DIR = cannedDir;
    writeFileSync(join(cannedDir, "rc"), "101");
    writeFileSync(join(cannedDir, "stderr"), "panic: index out of bounds\n");
    const runner = makeRunner();
    await expect(runner.check(req(), 5000)).rejects.toMatchObject({ face: "panic" });
  });

  it("step-1 failure = toolchain trouble: a runner error with the build face", async () => {
    process.env.RHO_FAKE_DIR = cannedDir;
    writeFileSync(join(cannedDir, "rc"), "0"); // wasmtime would succeed, but...
    const failing = new SubprocessRunner({
      rhoBinary: join(work, "bin", "missing-rho"),
      repoRoot: join(work, "repo"),
      wasmtime: join(work, "bin", "fake-wasmtime"),
    });
    await expect(failing.check(req(), 5000)).rejects.toMatchObject({ face: "build" });
  });
});

describe("the transport budget (POSIX MAX_ARG_STRLEN)", () => {
  it("an SRC value over the per-argv-element budget refuses the face before any spawn", async () => {
    process.env.RHO_FAKE_DIR = cannedDir;
    const runner = makeRunner();
    // "SRC=" + text must exceed MAX_ARG_BYTES (131072): one argv element
    // over the Linux ceiling would die E2BIG in execve, before the
    // child ever ran — refuse it honestly instead
    const big = "x".repeat(131_073);
    await expect(runner.check(req({ rootText: big }), 5000)).rejects.toMatchObject({ face: "size" });
    expect(existsSync(join(cannedDir, "arg-0"))).toBe(false); // the fake toolchain was never woken
  });

  it("a MODS bundle over the budget refuses the same way", async () => {
    process.env.RHO_FAKE_DIR = cannedDir;
    const runner = makeRunner();
    const fat = "y".repeat(131_073);
    await expect(runner.check(req({ modules: [{ path: "big.rho", text: fat }] }), 5000)).rejects.toMatchObject({
      face: "size",
    });
    expect(existsSync(join(cannedDir, "arg-0"))).toBe(false);
  });
});

describe("the time cap", () => {
  it("aborts a wedged wasm face and reports timedOut", async () => {
    process.env.RHO_FAKE_DIR = cannedDir;
    writeFileSync(join(cannedDir, "rc"), "0");
    writeFileSync(join(cannedDir, "sleep"), "30"); // the wedge
    const runner = makeRunner(100);
    const started = Date.now();
    const r = await runner.check(req(), 100);
    expect(Date.now() - started).toBeLessThan(10_000);
    expect(r.timedOut).toBe(true);
    expect(r.exitCode).toBeNull();
    expect(r.ok).toBe(false);
  }, 20_000);

  it("aborts a wedged build face the same way", async () => {
    process.env.RHO_FAKE_DIR = cannedDir;
    // fake-rho has no sleep hook; point the runner at the sleeping
    // wasmtime script as the "build" binary instead
    const runner = new SubprocessRunner({
      rhoBinary: join(work, "bin", "fake-wasmtime"),
      repoRoot: join(work, "repo"),
      wasmtime: join(work, "bin", "fake-wasmtime"),
      deadlineMs: 100,
    });
    // fake-rho roles: the build step IS the sleeping script here
    writeFileSync(join(cannedDir, "sleep"), "30");
    const r = await runner.check(req(), 100);
    // the sleep script exits with rc from the canned file: the build
    // face reports timedOut before that matters
    expect(r.timedOut).toBe(true);
  }, 20_000);
});

describe("MODS assembly", () => {
  it("is byte-identical to the corpus-diff face", () => {
    expect(assembleMods([])).toBe("");
    expect(assembleMods([{ path: "geom/priority.rho", text: "pub fn p() {}\n" }])).toBe(
      "@MOD@ geom/priority.rho\npub fn p() {}\n\n",
    );
  });

  it("SubprocessRunner.modulesFor: same-directory .rho siblings only, name-sorted", () => {
    const siblings = new Map<string, string>([
      ["/doc/b.rho", "pub fn b() {}\n"],
      ["/doc/a.rho", "pub fn a() {}\n"],
      ["/doc/notes.txt", "nope"],
      ["/elsewhere/c.rho", "nope"],
      ["/doc/main.rho", "the root itself"],
    ]);
    const mods = SubprocessRunner.modulesFor("/doc/main.rho", siblings);
    expect(mods).toEqual([
      { path: "a.rho", text: "pub fn a() {}\n" },
      { path: "b.rho", text: "pub fn b() {}\n" },
    ]);
  });

  it("modulesFor: an untitled document rides no modules", () => {
    expect(SubprocessRunner.modulesFor(undefined, new Map([["/doc/a.rho", "x"]]))).toEqual([]);
  });
});
