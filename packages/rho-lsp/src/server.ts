// server.ts — the language-service shell. It is a byte pump and a
// babysitter (the thin-shell law): transport-shaped answers, editor
// policy, supervision — and exactly two pieces of local knowledge, both
// because the compiler exports nothing better today: the minimal
// declaration scanner (symbols.ts) and the §18 mut scan (mut-hints.ts).
// All checking truth comes from the Runner.
//
// The reliability contract (ecosystem.md §4, acceptance law):
//   - every request is time-capped, twice: the runner aborts its
//     children at the cap; the server races the same call at cap +
//     grace and abandons whatever never settles;
//   - a failed/timed-out check publishes NO diagnostics for the
//     document (empty array) — never stale bytes, never invented ones;
//   - CONSECUTIVE failures past the threshold latch the server into
//     degraded mode: it keeps speaking LSP, publishes nothing, answers
//     null, and says so once on the log — no-LSP, never wrong-LSP. A
//     restart (or a fresh session) is the recovery path; a flapping
//     compiler must not flap the editor;
//   - an invalid generation (version self-validation) or a missing
//     runner starts the server INERT: full protocol, zero answers.
//
// Pins: test/server-transcript.test.ts (the full transcript suite),
// test/server-degrade.test.ts (caps, DNF, latch, inert, stats).

import { basename, dirname, relative } from "node:path";
import type { ParsedDiags } from "./diag-parser.js";
import { DocumentStore } from "./documents.js";
import { mutHints } from "./mut-hints.js";
import { bytePosToLsp, endPosition, jsOffsetToLsp, lineStarts } from "./positions.js";
import {
  completionsAt,
  definitionAt,
  hoverAt,
} from "./symbols.js";
import type {
  CompletionItem,
  Connection,
  Diagnostic,
  DidChangeTextDocumentParams,
  DidCloseTextDocumentParams,
  DidOpenTextDocumentParams,
  Hover,
  Location,
  PublishDiagnosticsParams,
  TextEdit,
} from "./protocol.js";
import { MAX_ARG_BYTES, RUNNER_TIMEOUT, assembleMods, raceDeadline, transportOverflow } from "./runner.js";
import type { CheckRequest, Runner } from "./runner.js";
import type { GenerationCheck } from "./generation.js";

export interface ServerOptions {
  /** Outer per-request cap in ms (the runner gets the same number). */
  requestTimeoutMs?: number;
  /** didChange quiet period before a check fires; 0 in tests. */
  debounceMs?: number;
  /** Consecutive runner failures before the degraded latch trips. */
  maxConsecutiveFailures?: number;
  /** §18 style hints (default on; the ask's capability switch). */
  mutHints?: boolean;
  /** Document size cap; over it a document is skipped, never half-served. */
  maxDocBytes?: number;
}

const DEFAULTS = {
  requestTimeoutMs: 5000,
  debounceMs: 150,
  maxConsecutiveFailures: 3,
  mutHints: true,
  maxDocBytes: 512 * 1024,
};

/** Grace the server adds on top of the runner's own cap before it
 * abandons a call that never settles (the DNF path). */
const RUNNER_GRACE_MS = 250;

export const SERVER_NAME = "rho-lsp";
export const SERVER_VERSION = "0.1.0";

interface Stats {
  checks: number;
  formats: number;
  failures: number;
  timeouts: number;
  durations: number[];
}

export class RhoLspServer {
  private readonly store: DocumentStore;
  private readonly opts: typeof DEFAULTS;
  private readonly tokens = new Map<string, number>();
  private readonly timers = new Map<string, ReturnType<typeof setTimeout>>();
  private consecutiveFailures = 0;
  private degraded = false;
  private loggedDegraded = false;
  private shutdownReceived = false;
  private readonly stats: Stats = { checks: 0, formats: 0, failures: 0, timeouts: 0, durations: [] };

  constructor(
    private readonly connection: Connection,
    private readonly runner: Runner | null,
    private readonly generation: GenerationCheck | null,
    options: ServerOptions = {},
  ) {
    this.opts = {
      requestTimeoutMs: options.requestTimeoutMs ?? DEFAULTS.requestTimeoutMs,
      debounceMs: options.debounceMs ?? DEFAULTS.debounceMs,
      maxConsecutiveFailures: options.maxConsecutiveFailures ?? DEFAULTS.maxConsecutiveFailures,
      mutHints: options.mutHints ?? DEFAULTS.mutHints,
      maxDocBytes: options.maxDocBytes ?? DEFAULTS.maxDocBytes,
    };
    this.store = new DocumentStore(this.opts.maxDocBytes);
  }

  /** The server answers nothing when it has no runner or no valid pin. */
  get inert(): boolean {
    return this.runner === null || this.generation === null || !this.generation.ok;
  }

  get isDegraded(): boolean {
    return this.degraded;
  }

  // ---------------------------------------------------------------- lifecycle

  handleInitialize(params: { rootUri?: string | null; initializationOptions?: Record<string, unknown> }): unknown {
    // initializationOptions may override the toggles per session
    if (params.initializationOptions) {
      const o = params.initializationOptions;
      if (typeof o["mutHints"] === "boolean") this.opts.mutHints = o["mutHints"];
      if (typeof o["requestTimeoutMs"] === "number" && o["requestTimeoutMs"] > 0) this.opts.requestTimeoutMs = o["requestTimeoutMs"];
    }
    return {
      capabilities: {
        textDocumentSync: { openClose: true, change: 1 }, // full sync only
        hoverProvider: true,
        definitionProvider: true,
        completionProvider: { triggerCharacters: ["."], resolveProvider: false },
        documentFormattingProvider: true,
      },
      serverInfo: { name: SERVER_NAME, version: SERVER_VERSION },
      rho: {
        inert: this.inert,
        degraded: this.degraded,
        runner: this.runner?.info() ?? null,
        generation: this.generation && this.generation.ok
          ? {
              rhoCommit: this.generation.manifest.rhoCommit,
              tools: this.generation.manifest.tools,
              rebuildIdentical: this.generation.manifest.rebuildIdentical,
            }
          : { error: this.generation && !this.generation.ok ? this.generation.reason : "no generation" },
      },
    };
  }

  handleShutdown(): null {
    this.shutdownReceived = true;
    return null;
  }

  handleExit(): void {
    // LSP exit law: exit 0 after shutdown, 1 without
    this.connection.exit(this.shutdownReceived ? 0 : 1);
  }

  // ---------------------------------------------------------------- document sync

  handleDidOpen(p: DidOpenTextDocumentParams): void {
    const doc = this.store.open(p.textDocument.uri, p.textDocument.version, p.textDocument.text, pathOfUri(p.textDocument.uri));
    if (this.inert || this.degraded) {
      this.suppressChecks(doc.uri);
      this.publishEmpty(doc.uri, doc.version);
      return;
    }
    if (this.store.overCap(doc)) {
      this.suppressChecks(doc.uri);
      this.log(`document over ${this.opts.maxDocBytes} bytes; validation skipped: ${doc.uri}`);
      this.publishEmpty(doc.uri, doc.version);
      return;
    }
    this.scheduleCheck(doc.uri);
  }

  handleDidChange(p: DidChangeTextDocumentParams): void {
    const change = p.contentChanges[p.contentChanges.length - 1];
    if (!change) return;
    const doc = this.store.change(p.textDocument.uri, p.textDocument.version, change.text);
    if (!doc) return; // protocol misuse: change without open — ignored
    if (this.inert || this.degraded) {
      this.suppressChecks(doc.uri);
      this.publishEmpty(doc.uri, doc.version);
      return;
    }
    if (this.store.overCap(doc)) {
      this.suppressChecks(doc.uri);
      this.log(`document over ${this.opts.maxDocBytes} bytes; validation skipped: ${doc.uri}`);
      this.publishEmpty(doc.uri, doc.version);
      return;
    }
    this.scheduleCheck(doc.uri);
  }

  handleDidClose(p: DidCloseTextDocumentParams): void {
    this.cancelPending(p.textDocument.uri);
    this.tokens.delete(p.textDocument.uri);
    this.store.close(p.textDocument.uri);
    // clear the editor's panel: publishing empty is the only honest close
    this.publishEmpty(p.textDocument.uri, null);
  }

  // ---------------------------------------------------------------- features

  async handleHover(p: { textDocument: { uri: string }; position: { line: number; character: number } }): Promise<Hover | null> {
    if (this.inert || this.degraded) return null;
    const doc = this.store.get(p.textDocument.uri);
    if (!doc || this.store.overCap(doc)) return null;
    const value = hoverAt(doc.text, p.position.line, p.position.character);
    return value === undefined ? null : { contents: { kind: "markdown", value } };
  }

  async handleDefinition(p: { textDocument: { uri: string }; position: { line: number; character: number } }): Promise<Location | null> {
    if (this.inert || this.degraded) return null;
    const doc = this.store.get(p.textDocument.uri);
    if (!doc || this.store.overCap(doc)) return null;
    const def = definitionAt(doc.text, p.position.line, p.position.character);
    if (def === undefined) return null;
    const starts = lineStarts(doc.text);
    return {
      uri: p.textDocument.uri,
      range: {
        start: jsOffsetToLsp(doc.text, starts, def.start),
        end: jsOffsetToLsp(doc.text, starts, def.end),
      },
    };
  }

  async handleCompletion(p: { textDocument: { uri: string }; position: { line: number; character: number } }): Promise<CompletionItem[] | null> {
    if (this.inert || this.degraded) return null;
    const doc = this.store.get(p.textDocument.uri);
    if (!doc || this.store.overCap(doc)) return null;
    return completionsAt(doc.text, p.position.line);
  }

  async handleFormatting(p: { textDocument: { uri: string }; options?: unknown }): Promise<TextEdit[] | null> {
    if (this.inert || this.degraded) return null;
    const runner = this.runner;
    if (runner === null) return null;
    const doc = this.store.get(p.textDocument.uri);
    if (!doc || this.store.overCap(doc)) return null;
    const req = this.requestFor(doc.uri);
    // same transport law as the check face: over the argv budget the
    // request is skipped (null edits), never spawned, never a failure
    if (transportOverflow(req.rootText, assembleMods(req.modules)) !== undefined) return null;
    this.stats.formats += 1;
    try {
      const result = await raceDeadline(runner.format(req, this.opts.requestTimeoutMs), this.opts.requestTimeoutMs + RUNNER_GRACE_MS);
      if (result === RUNNER_TIMEOUT) {
        this.stats.timeouts += 1;
        this.countFailure("format request exceeded its time cap");
        return null;
      }
      this.stats.durations.push(result.durationMs);
      if (!result.ok || result.text === undefined) {
        // fmt is exempt from recovery, permanently: broken source gets
        // NO edits, never a guess
        return null;
      }
      this.resetFailures();
      const starts = lineStarts(doc.text);
      const edit: TextEdit = {
        range: { start: { line: 0, character: 0 }, end: endPosition(doc.text, starts) },
        newText: result.text,
      };
      return [edit];
    } catch (err) {
      this.countFailure(`format runner error: ${errorMessage(err)}`);
      return null;
    }
  }

  /** Custom request: the latency ledger (deterministic given the runs). */
  handleStats(): unknown {
    const ds = [...this.stats.durations].sort((a, b) => a - b);
    const n = ds.length;
    const median = n === 0 ? 0 : n % 2 === 1 ? (ds[(n - 1) / 2] as number) : ((ds[n / 2 - 1] as number) + (ds[n / 2] as number)) / 2;
    const p95 = n === 0 ? 0 : (ds[Math.ceil(0.95 * n) - 1] as number);
    return {
      checks: this.stats.checks,
      formats: this.stats.formats,
      failures: this.stats.failures,
      timeouts: this.stats.timeouts,
      settledRuns: n,
      medianMs: median,
      p95Ms: p95,
      maxMs: n === 0 ? 0 : (ds[n - 1] as number),
      degraded: this.degraded,
      inert: this.inert,
    };
  }

  // ---------------------------------------------------------------- checking

  private scheduleCheck(uri: string): void {
    this.cancelPending(uri);
    const token = (this.tokens.get(uri) ?? 0) + 1;
    this.tokens.set(uri, token);
    if (this.opts.debounceMs <= 0) {
      void this.checkNow(uri, token);
      return;
    }
    const timer = setTimeout(() => {
      this.timers.delete(uri);
      void this.checkNow(uri, token);
    }, this.opts.debounceMs);
    timer.unref?.();
    this.timers.set(uri, timer);
  }

  private cancelPending(uri: string): void {
    const timer = this.timers.get(uri);
    if (timer !== undefined) {
      clearTimeout(timer);
      this.timers.delete(uri);
    }
  }

  /**
   * Every path that will NOT check the document still invalidates any
   * check already in flight: the pending timer dies and the latest-wins
   * token moves, so a parked runner answer can never publish past a
   * degraded flip or a size skip. Without the bump, an in-flight check
   * would sail through the token gate and publish (re-reading the newer
   * text) while the server is degraded — the "degraded publishes
   * nothing" law would hold everywhere but the window.
   */
  private suppressChecks(uri: string): void {
    this.cancelPending(uri);
    this.tokens.set(uri, (this.tokens.get(uri) ?? 0) + 1);
  }

  private async checkNow(uri: string, token: number): Promise<void> {
    const runner = this.runner;
    if (runner === null) return;
    const doc = this.store.get(uri);
    if (!doc) return;
    if (this.store.overCap(doc)) return;
    const req = this.requestFor(uri);
    // Transport law: each --set value rides ONE argv element, and the
    // POSIX per-element ceiling (Linux MAX_ARG_STRLEN, 128 KiB) is the
    // checkable budget. Over it the exec itself dies E2BIG on Linux —
    // and three such deaths would wrongly latch the whole server
    // degraded for what is a document property, not toolchain trouble.
    // Skip and clear, never spawn, never count a failure.
    const overflow = transportOverflow(req.rootText, assembleMods(req.modules));
    if (overflow !== undefined) {
      this.log(`request's ${overflow} value exceeds the ${MAX_ARG_BYTES}-byte argv budget; validation skipped: ${uri}`);
      this.publishEmpty(uri, doc.version);
      return;
    }
    this.stats.checks += 1;
    let result;
    try {
      result = await raceDeadline(runner.check(req, this.opts.requestTimeoutMs), this.opts.requestTimeoutMs + RUNNER_GRACE_MS);
    } catch (err) {
      this.onCheckFailed(uri, token, `check runner error: ${errorMessage(err)}`);
      return;
    }
    if (result === RUNNER_TIMEOUT) {
      // the ledger counts the abandon on both faces (the format face
      // counts its own); the failure counter rides onCheckFailed
      this.stats.timeouts += 1;
      this.onCheckFailed(uri, token, "check request exceeded its time cap");
      return;
    }
    // latest-wins: an answer for a superseded document state is dropped
    if (this.tokens.get(uri) !== token) return;
    if (result.timedOut) {
      this.onCheckFailed(uri, token, "check aborted at the runner's cap");
      return;
    }
    this.stats.durations.push(result.durationMs);
    this.resetFailures();
    this.publish(uri, doc.version, result.parsed);
  }

  private onCheckFailed(uri: string, token: number, why: string): void {
    // a failed check still supersedes: clear the panel, never stale bytes
    if (this.tokens.get(uri) !== token) return;
    const doc = this.store.get(uri);
    this.publishEmpty(uri, doc ? doc.version : null);
    this.countFailure(why);
  }

  private countFailure(why: string): void {
    this.stats.failures += 1;
    this.consecutiveFailures += 1;
    this.log(`runner failure (${this.consecutiveFailures}/${this.opts.maxConsecutiveFailures}): ${why}`);
    if (this.consecutiveFailures >= this.opts.maxConsecutiveFailures && !this.degraded) {
      this.degraded = true;
      if (!this.loggedDegraded) {
        this.loggedDegraded = true;
        this.log("rho-lsp: degraded to no-LSP (compiler unavailable); diagnostics are cleared and hover/format answer nothing. Restart the server to retry.");
      }
    }
  }

  private resetFailures(): void {
    this.consecutiveFailures = 0;
  }

  // ---------------------------------------------------------------- plumbing

  private requestFor(uri: string): CheckRequest {
    const doc = this.store.get(uri);
    if (!doc) return { rootText: "", modules: [] };
    const siblings = this.store.siblingsOf(doc);
    const dir = doc.path !== undefined ? dirname(doc.path) : undefined;
    return {
      rootText: doc.text,
      modules: siblings.map((s) => ({
        path: dir !== undefined && s.path !== undefined ? relative(dir, s.path) : basenameOf(s.path),
        text: s.text,
      })),
      ...(doc.path !== undefined ? { docPath: doc.path } : {}),
    };
  }

  private publish(uri: string, version: number, parsed: ParsedDiags): void {
    const diagnostics: Diagnostic[] = [];
    const doc = this.store.get(uri);
    const text = doc?.text ?? "";
    const starts = lineStarts(text);
    for (const d of parsed.diags) {
      if (
        d.kind === "positioned" &&
        d.line1 !== undefined &&
        d.byteCol1 !== undefined &&
        positionedForThisDoc(d.file, doc)
      ) {
        const pos = bytePosToLsp(text, starts, d.line1, d.byteCol1);
        diagnostics.push({
          range: { start: pos, end: pos },
          severity: d.severity === "note" ? 3 : 1,
          code: "rho-check",
          source: "rho",
          message: d.message,
        });
      } else {
        // positionless faces (check lines, parse count, mods) and any
        // positioned line naming a DIFFERENT file: attached as
        // file-level diagnostics with the compiler's verbatim line —
        // the editor must see what `rho check` sees, and a foreign
        // file's coordinates must never be mapped onto this text
        diagnostics.push({
          range: { start: { line: 0, character: 0 }, end: { line: 0, character: 0 } },
          severity: d.severity === "note" ? 3 : 1,
          code: d.kind === "parse" ? "rho-parse" : d.kind === "mods" ? "rho-mods" : "rho-check",
          source: "rho",
          message: d.message,
        });
      }
    }
    if (this.opts.mutHints) {
      diagnostics.push(...mutHints(text));
    }
    if (parsed.raw.length > 0) {
      this.log(`unrecognized compiler output on ${uri} (counted, not published): ${parsed.raw.join(" | ")}`);
    }
    const params: PublishDiagnosticsParams = { uri, version, diagnostics };
    this.connection.notify("textDocument/publishDiagnostics", params);
  }

  private publishEmpty(uri: string, version: number | null): void {
    const params: PublishDiagnosticsParams = { uri, version, diagnostics: [] };
    this.connection.notify("textDocument/publishDiagnostics", params);
  }

  private log(message: string): void {
    this.connection.notify("window/logMessage", { type: 2, message } satisfies { type: 2; message: string });
    logToStderr(message);
  }

  /** Wire the protocol handlers onto the connection. */
  listen(): void {
    this.connection.onRequest("initialize", (p: Parameters<RhoLspServer["handleInitialize"]>[0]) => this.handleInitialize(p));
    this.connection.onRequest("shutdown", () => this.handleShutdown());
    this.connection.onRequest<{ textDocument: { uri: string }; position: { line: number; character: number } }>(
      "textDocument/hover",
      (p) => this.handleHover(p),
    );
    this.connection.onRequest<{ textDocument: { uri: string }; position: { line: number; character: number } }>(
      "textDocument/definition",
      (p) => this.handleDefinition(p),
    );
    this.connection.onRequest<{ textDocument: { uri: string }; position: { line: number; character: number } }>(
      "textDocument/completion",
      (p) => this.handleCompletion(p),
    );
    this.connection.onRequest<{ textDocument: { uri: string } }>("textDocument/formatting", (p) => this.handleFormatting(p));
    this.connection.onRequest("rho/lspStats", () => this.handleStats());
    this.connection.onNotification<DidOpenTextDocumentParams>("textDocument/didOpen", (p) => this.handleDidOpen(p));
    this.connection.onNotification<DidChangeTextDocumentParams>("textDocument/didChange", (p) => this.handleDidChange(p));
    this.connection.onNotification<DidCloseTextDocumentParams>("textDocument/didClose", (p) => this.handleDidClose(p));
    this.connection.onNotification<Record<string, never>>("exit", () => this.handleExit());
    this.connection.onNotification<Record<string, never>>("initialized", () => {});
  }
}

function basenameOf(p: string | undefined): string {
  return p === undefined ? "module.rho" : basename(p);
}

/**
 * Whether a positioned diagnostic's `file` names the document under
 * the cursor — the only case allowed to drive in-text coordinates.
 * The wasm face is positionless today, so this guard is installed
 * ahead of the face landing; without it a sibling module's line (or
 * any foreign file) would be mapped onto the root document's text and
 * highlight the wrong character. Match rules, deliberately narrow:
 * the exact editor path, or a bare file name (no directory part) whose
 * basename is the document's — an embedding may hand the compiler the
 * document under its bare name, but a path WITH directories must match
 * exactly. Anything unverifiable degrades to file-level (degrade,
 * never wrong).
 */
function positionedForThisDoc(file: string | undefined, doc: { path?: string } | undefined): boolean {
  if (file === undefined) return true; // no file claim: the root document's own (the parser always sets one today)
  const path = doc?.path;
  if (path === undefined) return false; // untitled document: coordinates cannot be verified
  if (file === path) return true;
  // a bare file name (no directory part) may name the document by its
  // basename; a path carrying directories must have matched exactly
  return !file.includes("/") && basename(file) === basename(path);
}

export function pathOfUri(uri: string): string | undefined {
  if (!uri.startsWith("file:")) return undefined;
  try {
    return decodeURIComponent(new URL(uri).pathname);
  } catch {
    return undefined;
  }
}

function errorMessage(err: unknown): string {
  return err instanceof Error ? err.message : String(err);
}

let stderrLog: ((line: string) => void) | null = null;

/** The stdio entry installs this; logs NEVER touch stdout (it is the protocol channel). */
export function setStderrLogger(fn: (line: string) => void): void {
  stderrLog = fn;
}

export function logToStderr(message: string): void {
  if (stderrLog !== null) stderrLog(`[rho-lsp] ${message}\n`);
}
