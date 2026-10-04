// Pins: ecosystem.md §4's reliability clauses, one by one —
//   * every request time-capped, including the DNF case: a runner that
//     NEVER settles is abandoned by the server's race, the document's
//     diagnostics publish EMPTY, and the timeout is counted;
//   * a hung or failing server degrades to no-LSP, never to wrong
//     diagnostics: consecutive failures latch degraded mode (answers
//     null, publishes nothing, logs once) — an editor must not flap;
//   * an invalid generation pin (version self-validation) runs the
//     server INERT: full protocol, zero answers;
//   * oversized documents are skipped and cleared, never half-served;
//   * the latency ledger is deterministic (median of an even count is
//     the averaged middle; p95 by the ceil index).

import { describe, expect, it } from "vitest";
import { InMemoryConnection } from "../testkit/in-memory-connection.js";
import { FakeRunner, HangingRunner } from "../testkit/fake-runner.js";
import { RhoLspServer } from "../src/server.js";
import { RUNNER_TIMEOUT, raceDeadline } from "../src/runner.js";
import type { GenerationCheck } from "../src/generation.js";

const GEN: GenerationCheck = {
  ok: true,
  manifest: {
    schemaVersion: "rho-lsp.generation/1",
    rhoCommit: "c",
    compilerSources: {},
    artifacts: {},
    tools: { wasmtime: null, wat2wasm: null },
    rebuildIdentical: true,
  },
};

const URI = "file:///w/main.rho";
const QUIET = "fn main() -> i32 { return 0; }\n"; // no mut decls: publishes are empty arrays

function makeServer(runner: FakeRunner | HangingRunner | null, gen: GenerationCheck | null = GEN, opts: Record<string, unknown> = {}) {
  const conn = new InMemoryConnection();
  const server = new RhoLspServer(conn, runner, gen, { debounceMs: 0, requestTimeoutMs: 30, ...opts });
  server.listen();
  return { conn, server };
}

const open = (conn: InMemoryConnection, text: string = QUIET, version = 1): void =>
  conn.send("textDocument/didOpen", { textDocument: { uri: URI, languageId: "rho", version, text } });

describe("the time cap and the DNF path", () => {
  it("a runner that never settles is abandoned at the cap: empty diagnostics, timeout counted", async () => {
    const hanging = new HangingRunner();
    const { conn } = makeServer(hanging);
    open(conn);
    await conn.waitFor(() => conn.publishedFor(URI).length === 1, 3000);
    const pub = conn.lastPublishedFor(URI);
    expect(pub?.diagnostics).toEqual([]); // cleared, never stale, never invented
    const stats = (await conn.callOk("rho/lspStats", {})) as { timeouts: number; failures: number };
    expect(stats.timeouts).toBe(1);
    expect(stats.failures).toBe(1);
    expect(conn.logs.some((l) => l.message.includes("time cap"))).toBe(true);
    // HangingRunner.calls records every invocation; the abandon must
    // not have retried
    expect(hanging.calls).toHaveLength(1);
  }, 5000);

  it("raceDeadline resolves RUNNER_TIMEOUT for a never-settling promise", async () => {
    const started = Date.now();
    const r = await raceDeadline(new Promise<never>(() => undefined), 40);
    expect(r).toBe(RUNNER_TIMEOUT);
    expect(Date.now() - started).toBeGreaterThanOrEqual(30);
    expect(Date.now() - started).toBeLessThan(2000);
  });

  it("a runner error is a failure, not a crash: publish empty and count it", async () => {
    const runner = new FakeRunner();
    runner.script({ kind: "throw", message: "toolchain build failed (rc=1)" });
    const { conn } = makeServer(runner);
    open(conn);
    await conn.waitFor(() => conn.publishedFor(URI).length === 1);
    expect(conn.lastPublishedFor(URI)?.diagnostics).toEqual([]);
    const stats = (await conn.callOk("rho/lspStats", {})) as { failures: number; timeouts: number };
    expect(stats.failures).toBe(1);
    expect(stats.timeouts).toBe(0);
  });
});

describe("the degradation latch (no-LSP, never wrong-LSP)", () => {
  it("two consecutive failures latch degraded mode; the latch holds", async () => {
    const runner = new FakeRunner();
    runner.script(
      { kind: "throw", message: "boom 1" },
      { kind: "throw", message: "boom 2" },
    );
    const { conn, server } = makeServer(runner, GEN, { maxConsecutiveFailures: 2 });
    open(conn);
    await conn.waitFor(() => conn.publishedFor(URI).length === 1);
    conn.send("textDocument/didChange", { textDocument: { uri: URI, version: 2 }, contentChanges: [{ text: QUIET }] });
    await conn.waitFor(() => conn.publishedFor(URI).length === 2);
    expect(server.isDegraded).toBe(true);
    // the server says so, once
    const degrades = conn.logs.filter((l) => l.message.includes("degraded to no-LSP"));
    expect(degrades).toHaveLength(1);
    // degraded: features answer nothing...
    expect(await conn.callOk("textDocument/hover", { textDocument: { uri: URI }, position: { line: 0, character: 4 } })).toBeNull();
    expect(await conn.callOk("textDocument/formatting", { textDocument: { uri: URI }, options: {} })).toBeNull();
    // ...and further opens publish empty WITHOUT waking the runner
    const callsBefore = runner.checkCalls.length;
    open(conn, QUIET, 3);
    await conn.waitFor(() => conn.publishedFor(URI).length === 3);
    expect(runner.checkCalls.length).toBe(callsBefore);
  });

  it("a failure sandwiched between successes does not latch", async () => {
    const runner = new FakeRunner();
    runner.script(
      { kind: "throw", message: "one bad day" },
      { kind: "ok" },
      { kind: "ok" },
      { kind: "ok" },
    );
    const { conn, server } = makeServer(runner, GEN, { maxConsecutiveFailures: 3 });
    open(conn);
    await conn.waitFor(() => conn.publishedFor(URI).length === 1);
    conn.send("textDocument/didChange", { textDocument: { uri: URI, version: 2 }, contentChanges: [{ text: QUIET }] });
    await conn.waitFor(() => conn.publishedFor(URI).length === 2);
    expect(server.isDegraded).toBe(false);
  });
});

describe("inert mode (no runner or an invalid generation)", () => {
  it("a server with no valid pin answers the protocol and serves nothing", async () => {
    const runner = new FakeRunner();
    const { conn } = makeServer(runner, { ok: false, reason: "generation.json not found" });
    const init = (await conn.callOk("initialize", {})) as { rho: { inert: boolean; generation: { error: string } } };
    expect(init.rho.inert).toBe(true);
    expect(init.rho.generation.error).toContain("not found");
    open(conn);
    await conn.waitFor(() => conn.publishedFor(URI).length === 1);
    expect(conn.lastPublishedFor(URI)?.diagnostics).toEqual([]);
    expect(runner.checkCalls).toHaveLength(0); // the compiler was never woken
    expect(await conn.callOk("textDocument/hover", { textDocument: { uri: URI }, position: { line: 0, character: 4 } })).toBeNull();
    expect(await conn.callOk("textDocument/completion", { textDocument: { uri: URI }, position: { line: 0, character: 0 } })).toBeNull();
    expect(await conn.callOk("textDocument/definition", { textDocument: { uri: URI }, position: { line: 0, character: 4 } })).toBeNull();
    expect(await conn.callOk("textDocument/formatting", { textDocument: { uri: URI }, options: {} })).toBeNull();
  });

  it("a server with no runner at all is equally inert", async () => {
    const { conn } = makeServer(null, GEN);
    const init = (await conn.callOk("initialize", {})) as { rho: { inert: boolean; runner: unknown } };
    expect(init.rho.inert).toBe(true);
    expect(init.rho.runner).toBeNull();
    open(conn);
    await conn.waitFor(() => conn.publishedFor(URI).length === 1);
    expect(conn.lastPublishedFor(URI)?.diagnostics).toEqual([]);
  });
});

describe("the document size cap", () => {
  it("skips an oversized document and clears its diagnostics", async () => {
    const runner = new FakeRunner();
    runner.script({ kind: "ok" });
    const { conn } = makeServer(runner, GEN, { maxDocBytes: 8 });
    open(conn, "fn main() { " + "x".repeat(64) + " }\n");
    await conn.waitFor(() => conn.publishedFor(URI).length === 1);
    expect(conn.lastPublishedFor(URI)?.diagnostics).toEqual([]);
    expect(runner.checkCalls).toHaveLength(0);
    expect(conn.logs.some((l) => l.message.includes("validation skipped"))).toBe(true);
  });

  it("a document under the store cap but over the argv transport budget is skipped, never failed", async () => {
    const runner = new FakeRunner();
    runner.script({ kind: "ok" });
    const { conn } = makeServer(runner); // default store cap 512 KiB
    // ~131 KiB of text: under maxDocBytes, but "SRC=<text>" exceeds the
    // POSIX per-argv-element ceiling (Linux MAX_ARG_STRLEN) — on Linux
    // the exec would die E2BIG, and three such deaths must never latch
    // the server degraded for a document property
    const fat = "fn main() { " + "x".repeat(131_073) + " }\n";
    open(conn, fat);
    await conn.waitFor(() => conn.publishedFor(URI).length === 1);
    expect(conn.lastPublishedFor(URI)?.diagnostics).toEqual([]);
    expect(runner.checkCalls).toHaveLength(0); // the runner was never woken
    const stats = (await conn.callOk("rho/lspStats", {})) as { failures: number; timeouts: number; checks: number };
    expect(stats.checks).toBe(0);
    expect(stats.failures).toBe(0);
    expect(stats.timeouts).toBe(0);
    expect(conn.logs.some((l) => l.message.includes("argv budget"))).toBe(true);
  });
});

describe("the latency ledger (deterministic)", () => {
  it("median of an even count is the averaged middle; p95 by the ceil index", async () => {
    const runner = new FakeRunner();
    runner.script(
      { kind: "ok", durationMs: 40 },
      { kind: "ok", durationMs: 10 },
      { kind: "ok", durationMs: 30 },
      { kind: "ok", durationMs: 20 },
    );
    const { conn } = makeServer(runner);
    for (let v = 1; v <= 4; v++) {
      open(conn, QUIET, v);
      await conn.waitFor(() => conn.publishedFor(URI).length === v);
    }
    const stats = (await conn.callOk("rho/lspStats", {})) as {
      checks: number;
      settledRuns: number;
      medianMs: number;
      p95Ms: number;
      maxMs: number;
      degraded: boolean;
      inert: boolean;
    };
    expect(stats.checks).toBe(4);
    expect(stats.settledRuns).toBe(4);
    expect(stats.medianMs).toBe(25); // sorted [10,20,30,40] -> (20+30)/2
    expect(stats.p95Ms).toBe(40); // ceil(0.95*4)=4 -> the 4th sorted value
    expect(stats.maxMs).toBe(40);
    expect(stats.degraded).toBe(false);
    expect(stats.inert).toBe(false);
  });
});
