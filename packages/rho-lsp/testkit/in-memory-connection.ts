// testkit/in-memory-connection.ts — a Connection that records instead
// of transmitting. The transcript tests drive the server through it
// and assert on the recorded frames; no sockets, no processes, fully
// deterministic given the scripted runner.

import type { Connection, LogMessageParams, PublishDiagnosticsParams, RequestId } from "../src/protocol.js";

export interface RecordedReply {
  id: RequestId;
  result?: unknown;
  error?: { code: number; message: string };
}

export class InMemoryConnection implements Connection {
  readonly published: PublishDiagnosticsParams[] = [];
  readonly logs: LogMessageParams[] = [];
  readonly replies: RecordedReply[] = [];
  readonly exits: number[] = [];
  readonly unknownNotifications: string[] = [];
  private readonly requestHandlers = new Map<string, (params: unknown) => Promise<unknown> | unknown>();
  private readonly notificationHandlers = new Map<string, (params: unknown) => void>();

  onRequest<P>(method: string, handler: (params: P) => Promise<unknown> | unknown): void {
    this.requestHandlers.set(method, handler as (params: unknown) => Promise<unknown> | unknown);
  }

  onNotification<P>(method: string, handler: (params: P) => void): void {
    this.notificationHandlers.set(method, handler as (params: unknown) => void);
  }

  notify(method: string, params: unknown): void {
    if (method === "textDocument/publishDiagnostics") {
      this.published.push(params as PublishDiagnosticsParams);
      return;
    }
    if (method === "window/logMessage") {
      this.logs.push(params as LogMessageParams);
      return;
    }
    this.unknownNotifications.push(method);
  }

  reply(id: RequestId, result: unknown): void {
    this.replies.push({ id, result });
  }

  fail(id: RequestId, code: number, message: string): void {
    this.replies.push({ id, error: { code, message } });
  }

  exit(code: number): void {
    this.exits.push(code);
  }

  /** Drive a request through the registered handler (awaited). */
  async call<P>(method: string, params: P): Promise<{ ok: true; result: unknown } | { ok: false; error: { code: number; message: string } }> {
    const handler = this.requestHandlers.get(method);
    if (handler === undefined) throw new Error(`no handler registered for ${method}`);
    try {
      return { ok: true, result: await handler(params) };
    } catch (err) {
      return { ok: false, error: { code: -32603, message: String(err) } };
    }
  }

  /** call(), with a failed reply surfaced as a thrown error — the
   * result narrows to `unknown` ready for the test's cast. */
  async callOk<P>(method: string, params: P): Promise<unknown> {
    const r = await this.call(method, params);
    if (!r.ok) throw new Error(`${method} failed: ${JSON.stringify(r.error)}`);
    return r.result;
  }

  /** Drive a notification through the registered handler. */
  send<P>(method: string, params: P): void {
    const handler = this.notificationHandlers.get(method);
    if (handler === undefined) throw new Error(`no handler registered for ${method}`);
    handler(params);
  }

  /** Wait until a predicate holds (polling; deterministic waits for async pipelines). */
  async waitFor(pred: () => boolean, timeoutMs = 2000): Promise<void> {
    const started = Date.now();
    while (!pred()) {
      if (Date.now() - started > timeoutMs) throw new Error("waitFor: condition not met in time");
      await new Promise((r) => setTimeout(r, 5));
    }
  }

  /** Diagnostics published for a uri (all of them, in order). */
  publishedFor(uri: string): PublishDiagnosticsParams[] {
    return this.published.filter((p) => p.uri === uri);
  }

  lastPublishedFor(uri: string): PublishDiagnosticsParams | undefined {
    for (let i = this.published.length - 1; i >= 0; i--) {
      const p = this.published[i];
      if (p !== undefined && p.uri === uri) return p;
    }
    return undefined;
  }
}
