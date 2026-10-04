// Pins: the wire-visible behavior contract of the whole server
// (ecosystem.md §4's acceptance face, on an in-memory transport):
// capabilities and serverInfo at initialize; diagnostics published
// with VERBATIM compiler messages and converted positions, hints
// appended after; latest-wins on superseded document states; full-sync
// didChange; didClose clears; formatting = one whole-document TextEdit
// or nothing (fmt never half-formats); hover/definition/completion
// from the scanner; the LSP exit law; deterministic output — the same
// scenario twice produces identical publishDiagnostics arrays.

import { describe, expect, it } from "vitest";
import { InMemoryConnection } from "../testkit/in-memory-connection.js";
import { FakeRunner } from "../testkit/fake-runner.js";
import { RhoLspServer } from "../src/server.js";
import type { GenerationCheck } from "../src/generation.js";
import type { CheckRequest, CheckResult, FormatResult, Runner, RunnerInfo } from "../src/runner.js";

const GEN: GenerationCheck = {
  ok: true,
  manifest: {
    schemaVersion: "rho-lsp.generation/1",
    rhoCommit: "9f3e910deadbeef",
    compilerSources: {},
    artifacts: {},
    tools: { wasmtime: "wasmtime 40.0.0", wat2wasm: "wat2wasm 1.0" },
    rebuildIdentical: true,
  },
};

const URI = "file:///w/main.rho";
const GOOD = "fn main() -> i32 { return 0; }\n";
const tick = () => new Promise((r) => setTimeout(r, 0));

function makeServer(runner: Runner, gen: GenerationCheck | null = GEN, opts: Record<string, unknown> = {}): { conn: InMemoryConnection; server: RhoLspServer } {
  const conn = new InMemoryConnection();
  const server = new RhoLspServer(conn, runner, gen, {
    debounceMs: 0,
    requestTimeoutMs: 1000,
    ...opts,
  });
  server.listen();
  return { conn, server };
}

describe("initialize", () => {
  it("advertises the capability set, serverInfo, and the pinned generation", async () => {
    const { conn } = makeServer(new FakeRunner());
    const init = (await conn.callOk("initialize", { rootUri: null })) as {
      capabilities: Record<string, unknown>;
      serverInfo: { name: string; version: string };
      rho: { inert: boolean; generation: { rhoCommit: string } };
    };
    expect(init.capabilities.textDocumentSync).toEqual({ openClose: true, change: 1 });
    expect(init.capabilities.hoverProvider).toBe(true);
    expect(init.capabilities.definitionProvider).toBe(true);
    expect(init.capabilities.completionProvider).toEqual({ triggerCharacters: ["."], resolveProvider: false });
    expect(init.capabilities.documentFormattingProvider).toBe(true);
    expect(init.serverInfo.name).toBe("rho-lsp");
    expect(init.rho.inert).toBe(false);
    expect(init.rho.generation.rhoCommit).toBe("9f3e910deadbeef");
  });

  it("honors initializationOptions.mutHints as the §18 capability switch", async () => {
    const runner = new FakeRunner();
    runner.script({ kind: "refused", stderr: "check: unknown name 'y'\n" });
    const { conn } = makeServer(runner);
    await conn.call("initialize", { initializationOptions: { mutHints: false } });
    conn.send("textDocument/didOpen", {
      textDocument: { uri: URI, languageId: "rho", version: 1, text: "fn main() {\n  let mut x = 1;\n  use_y(y);\n}\n" },
    });
    await conn.waitFor(() => conn.publishedFor(URI).length > 0);
    const diag = conn.lastPublishedFor(URI);
    // the compiler's diagnostic rides; the mut hint does not
    expect(diag?.diagnostics.map((d) => d.code)).toEqual(["rho-check"]);
  });
});

describe("the check pipeline", () => {
  it("publishes compiler diagnostics verbatim, then mut hints", async () => {
    const runner = new FakeRunner();
    runner.script({ kind: "refused", stderr: "check: unknown name 'y'\ncheck: 1 error(s)\n" });
    const { conn } = makeServer(runner);
    conn.send("textDocument/didOpen", {
      textDocument: {
        uri: URI,
        languageId: "rho",
        version: 1,
        text: 'fn main() {\n  let mut x = 1;\n  printf("{}", y);\n}\n',
      },
    });
    await conn.waitFor(() => conn.publishedFor(URI).length > 0);
    const pub = conn.lastPublishedFor(URI);
    expect(pub?.version).toBe(1);
    expect(pub?.diagnostics).toHaveLength(2); // the compiler line + the hint; the summary is excluded
    const compilerDiag = pub?.diagnostics[0];
    expect(compilerDiag?.message).toBe("check: unknown name 'y'"); // verbatim
    expect(compilerDiag?.severity).toBe(1);
    expect(compilerDiag?.source).toBe("rho");
    expect(compilerDiag?.range).toEqual({ start: { line: 0, character: 0 }, end: { line: 0, character: 0 } });
    const hint = pub?.diagnostics[1];
    expect(hint?.code).toBe("mut-never-written");
    expect(hint?.severity).toBe(4); // hint, never an error (§18)
  });

  it("maps a positioned diagnostic through the byte-column law (CJK fixture)", async () => {
    const runner = new FakeRunner();
    runner.script({ kind: "refused", stderr: "main.rho:2:20: error: unknown name 'y'\n" });
    const { conn } = makeServer(runner);
    const text = 'fn main() {\n  let s: string = "中文"; let b: i32 = y;\n}\n';
    conn.send("textDocument/didOpen", { textDocument: { uri: URI, languageId: "rho", version: 1, text } });
    await conn.waitFor(() => conn.publishedFor(URI).length > 0);
    const d = conn.lastPublishedFor(URI)?.diagnostics[0];
    // byte col 20 is the first byte of 中 on that line (3 bytes, 1
    // UTF-16 unit): the position lands at the character's start
    expect(d?.range.start).toEqual({ line: 1, character: 19 });
    expect(d?.range.end).toEqual(d?.range.start); // start points only: zero-width ranges
  });

  it("a positioned line naming a FOREIGN file demotes to file-level, never mapped onto this text", async () => {
    const runner = new FakeRunner();
    runner.script({ kind: "refused", stderr: "helper.rho:2:5: error: unknown name 'q'\n" });
    const { conn } = makeServer(runner);
    conn.send("textDocument/didOpen", { textDocument: { uri: URI, languageId: "rho", version: 1, text: GOOD } });
    await conn.waitFor(() => conn.publishedFor(URI).length > 0);
    const d = conn.lastPublishedFor(URI)?.diagnostics[0];
    // helper.rho is a different document: its coordinates must not
    // drive ranges over main.rho's text — the compiler's verbatim line
    // degrades to a file-level diagnostic instead (degrade, never
    // wrong)
    expect(d?.message).toBe("helper.rho:2:5: error: unknown name 'q'");
    expect(d?.range).toEqual({ start: { line: 0, character: 0 }, end: { line: 0, character: 0 } });
    expect(d?.code).toBe("rho-check");
    expect(d?.severity).toBe(1);
  });

  it("a clean check publishes an empty diagnostics array", async () => {
    const runner = new FakeRunner();
    runner.script({ kind: "ok" });
    const { conn } = makeServer(runner);
    conn.send("textDocument/didOpen", { textDocument: { uri: URI, languageId: "rho", version: 1, text: GOOD } });
    await conn.waitFor(() => conn.publishedFor(URI).length > 0);
    expect(conn.lastPublishedFor(URI)?.diagnostics).toEqual([]);
  });

  it("unrecognized compiler output is logged, never published", async () => {
    const runner = new FakeRunner();
    runner.script({ kind: "refused", stderr: "rho: cannot open missing/mod.rho\n" });
    const { conn } = makeServer(runner);
    conn.send("textDocument/didOpen", { textDocument: { uri: URI, languageId: "rho", version: 1, text: GOOD } });
    await conn.waitFor(() => conn.logs.length > 0);
    expect(conn.lastPublishedFor(URI)?.diagnostics).toEqual([]);
    expect(conn.logs.some((l) => l.message.includes("cannot open missing/mod.rho"))).toBe(true);
  });
});

describe("document lifecycle", () => {
  it("didChange re-checks with the new full text; didClose clears", async () => {
    const runner = new FakeRunner();
    runner.script({ kind: "ok" }, { kind: "refused", stderr: "check: unknown name 'z'\n" });
    const { conn } = makeServer(runner);
    conn.send("textDocument/didOpen", { textDocument: { uri: URI, languageId: "rho", version: 1, text: GOOD } });
    await conn.waitFor(() => conn.publishedFor(URI).length === 1);
    conn.send("textDocument/didChange", {
      textDocument: { uri: URI, version: 2 },
      contentChanges: [{ text: "fn main() { z; }\n" }],
    });
    await conn.waitFor(() => conn.publishedFor(URI).length === 2);
    const second = conn.lastPublishedFor(URI);
    expect(second?.version).toBe(2);
    expect(second?.diagnostics[0]?.message).toBe("check: unknown name 'z'");
    conn.send("textDocument/didClose", { textDocument: { uri: URI } });
    const cleared = conn.lastPublishedFor(URI);
    expect(cleared?.diagnostics).toEqual([]);
    expect(cleared?.version).toBeNull();
  });

  it("drops a superseded check's result (latest-wins)", async () => {
    const runner = new TwoPhaseRunner();
    const { conn } = makeServer(runner);
    conn.send("textDocument/didOpen", { textDocument: { uri: URI, languageId: "rho", version: 1, text: "v1\n" } });
    await tick();
    expect(runner.calls).toBe(1);
    conn.send("textDocument/didChange", { textDocument: { uri: URI, version: 2 }, contentChanges: [{ text: "v2\n" }] });
    await tick();
    expect(runner.calls).toBe(2);
    // the v1 check settles first: its answer is stale and must be dropped
    runner.settle(0);
    runner.settle(1);
    await conn.waitFor(() => conn.publishedFor(URI).length === 1);
    const pubs = conn.publishedFor(URI);
    expect(pubs[0]?.version).toBe(2);
  });

  it("a didChange that early-exits (degraded flip) still invalidates a check already in flight", async () => {
    const runner = new TwoPhaseRunner();
    const { conn, server } = makeServer(runner, GEN, { maxConsecutiveFailures: 1 });
    conn.send("textDocument/didOpen", { textDocument: { uri: URI, version: 1, text: "v1\n" } });
    await tick();
    expect(runner.calls).toBe(1); // the v1 check is parked mid-flight
    // flip the latch while the check is parked (a failing format counts)
    await conn.call("textDocument/formatting", { textDocument: { uri: URI }, options: {} });
    expect(server.isDegraded).toBe(true);
    conn.send("textDocument/didChange", { textDocument: { uri: URI, version: 2 }, contentChanges: [{ text: "v2\n" }] });
    await conn.waitFor(() => conn.publishedFor(URI).length === 1); // the early-exit's publishEmpty
    runner.settle(0);
    await tick();
    // the parked v1 answer must NOT surface now that the server is
    // degraded: the early exit bumped the token, so the latest-wins
    // gate drops it ("degraded publishes nothing" holds in the window)
    expect(conn.publishedFor(URI)).toHaveLength(1);
    expect(conn.lastPublishedFor(URI)?.version).toBe(2);
    expect(conn.lastPublishedFor(URI)?.diagnostics).toEqual([]);
  });

  it("a didChange that early-exits (over cap) still invalidates a check already in flight", async () => {
    const runner = new TwoPhaseRunner();
    const { conn } = makeServer(runner, GEN, { maxDocBytes: 8 });
    conn.send("textDocument/didOpen", { textDocument: { uri: URI, version: 1, text: "v1\n" } });
    await tick();
    expect(runner.calls).toBe(1);
    conn.send("textDocument/didChange", { textDocument: { uri: URI, version: 2 }, contentChanges: [{ text: "x".repeat(64) }] });
    await conn.waitFor(() => conn.publishedFor(URI).length === 1); // the skip's publishEmpty
    runner.settle(0);
    await tick();
    // the parked v1 answer must not surface after the over-cap skip
    expect(conn.publishedFor(URI)).toHaveLength(1);
    expect(conn.lastPublishedFor(URI)?.version).toBe(2);
    expect(conn.lastPublishedFor(URI)?.diagnostics).toEqual([]);
  });
});

/** A runner that parks every call until the test releases it in order. */
class TwoPhaseRunner implements Runner {
  calls = 0;
  private resolvers: Array<(r: CheckResult) => void> = [];
  info(): RunnerInfo {
    return { kind: "two-phase", detail: "test double" };
  }
  async dispose(): Promise<void> {}
  check(_req: CheckRequest, _deadlineMs: number): Promise<CheckResult> {
    this.calls += 1;
    return new Promise((resolve) => {
      this.resolvers.push(resolve);
    });
  }
  async format(_req: CheckRequest, _deadlineMs: number): Promise<FormatResult> {
    throw new Error("not used in this suite");
  }
  settle(index: number): void {
    const r = this.resolvers[index];
    if (r !== undefined) {
      this.resolvers[index] = () => undefined; // settle idempotently
      r({ ok: true, exitCode: 0, stderr: "", stdout: "(wat)", timedOut: false, durationMs: 1, parsed: { diags: [], raw: [] } });
    }
  }
}

describe("scanner features", () => {
  it("hover answers the declaration line in a rho fence", async () => {
    const { conn } = makeServer(new FakeRunner());
    await conn.call("initialize", {});
    conn.send("textDocument/didOpen", { textDocument: { uri: URI, languageId: "rho", version: 1, text: GOOD } });
    const hover = await conn.callOk("textDocument/hover", { textDocument: { uri: URI }, position: { line: 0, character: 4 } });
    expect(hover).toEqual({ contents: { kind: "markdown", value: "```rho\nfn main() -> i32 { return 0; }\n```" } });
  });

  it("definition answers a same-document location", async () => {
    const text = "fn helper() -> i32 { return 1; }\nfn main() -> i32 { return helper(); }\n";
    const { conn } = makeServer(new FakeRunner());
    await conn.call("initialize", {});
    conn.send("textDocument/didOpen", { textDocument: { uri: URI, languageId: "rho", version: 1, text } });
    const loc = (await conn.callOk("textDocument/definition", { textDocument: { uri: URI }, position: { line: 1, character: 28 } })) as { uri: string; range: { start: { character: number } } } | null;
    expect(loc?.uri).toBe(URI);
    expect(loc?.range.start.character).toBe(3); // the `h` of helper: "fn helper" -> index 3
  });

  it("completion merges declarations, prelude, and keywords in label order", async () => {
    const { conn } = makeServer(new FakeRunner());
    await conn.call("initialize", {});
    conn.send("textDocument/didOpen", { textDocument: { uri: URI, languageId: "rho", version: 1, text: GOOD } });
    const items = (await conn.callOk("textDocument/completion", { textDocument: { uri: URI }, position: { line: 0, character: 0 } })) as { label: string }[];
    const labels = items.map((i) => i.label);
    expect(labels).toContain("main");
    expect(labels).toContain("printf");
    expect(labels).toContain("match");
    expect(labels).toEqual([...labels].sort((a, b) => (a < b ? -1 : a > b ? 1 : 0)));
  });
});

describe("formatting", () => {
  it("one whole-document TextEdit from the fmt face", async () => {
    const runner = new FakeRunner();
    runner.script({ kind: "okFmt", text: "fn main() -> i32 {\n  return 0;\n}\n" });
    const { conn } = makeServer(runner);
    await conn.call("initialize", {});
    conn.send("textDocument/didOpen", { textDocument: { uri: URI, languageId: "rho", version: 1, text: GOOD } });
    await conn.waitFor(() => conn.publishedFor(URI).length > 0);
    const edits = (await conn.callOk("textDocument/formatting", { textDocument: { uri: URI }, options: { tabSize: 4 } })) as Array<{ range: { start: unknown; end: unknown }; newText: string }> | null;
    expect(edits).toHaveLength(1);
    expect(edits?.[0]?.newText).toBe("fn main() -> i32 {\n  return 0;\n}\n");
    expect(edits?.[0]?.range.start).toEqual({ line: 0, character: 0 });
    expect(edits?.[0]?.range.end).toEqual({ line: 1, character: 0 }); // after the trailing \n
  });

  it("a refused fmt answers NO edits, never a guess", async () => {
    const runner = new FakeRunner();
    runner.script({ kind: "refused", stderr: "parse: 1 unresolved form(s)\n" });
    const { conn } = makeServer(runner);
    await conn.call("initialize", {});
    conn.send("textDocument/didOpen", { textDocument: { uri: URI, languageId: "rho", version: 1, text: "fn {" } });
    await conn.waitFor(() => conn.publishedFor(URI).length > 0);
    const refusedEdits = await conn.callOk("textDocument/formatting", { textDocument: { uri: URI }, options: {} });
    expect(refusedEdits).toBeNull();
  });
});

describe("lifecycle and determinism", () => {
  it("exit after shutdown is 0; exit without shutdown is 1 (LSP law)", async () => {
    const a = makeServer(new FakeRunner());
    await a.conn.call("shutdown", {});
    a.conn.send("exit", {});
    expect(a.conn.exits).toEqual([0]);
    const b = makeServer(new FakeRunner());
    b.conn.send("exit", {});
    expect(b.conn.exits).toEqual([1]);
  });

  it("the same scenario twice publishes identical diagnostics arrays", async () => {
    const run = (): Promise<string> => {
      const runner = new FakeRunner();
      runner.script({ kind: "refused", stderr: "check: unknown name 'y'\ncheck: 1 error(s)\n" });
      const { conn } = makeServer(runner);
      conn.send("textDocument/didOpen", {
        textDocument: { uri: URI, languageId: "rho", version: 1, text: 'fn main() {\n  let mut x = 1;\n  printf("{}", y);\n}\n' },
      });
      return conn.waitFor(() => conn.publishedFor(URI).length > 0).then(() => JSON.stringify(conn.lastPublishedFor(URI)));
    };
    expect(await run()).toBe(await run());
  });
});
