// main.ts — the stdio entry (bin "rho-lsp"). Wires the JSON-RPC frame
// reader/writer to the server through a registering Connection: the
// same dispatch path the tests exercise over an in-memory connection.
// Logs go to stderr only — stdout is the protocol channel, exclusively.
//
// Exit law (LSP): shutdown then exit -> 0; exit without shutdown -> 1.
// A fatal configuration problem (no generation pin, no boot binary)
// does NOT crash the process: the server runs INERT — it still speaks
// LSP, answers capabilities, and serves nothing — because a dead
// server forces the editor to guess, and guessing is how wrong
// diagnostics happen. no-LSP over wrong-LSP, always.

import process from "node:process";
import { existsSync } from "node:fs";
import { join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { FrameReader, parseMessage, writeFrame } from "./jsonrpc.js";
import type { JsonRpcMessage } from "./jsonrpc.js";
import { verifyGeneration } from "./generation.js";
import type { GenerationCheck } from "./generation.js";
import { RhoLspServer, setStderrLogger } from "./server.js";
import { SubprocessRunner } from "./subprocess-runner.js";
import type { Connection, RequestId } from "./protocol.js";

function env(name: string): string | undefined {
  const v = process.env[name];
  return v === undefined || v === "" ? undefined : v;
}

/** Resolve the rho repo root: RHO_LSP_REPO, or the package's rho-tree
 * home (../../.. from dist/, when the package lives at <rho>/plugins/rho-lsp). */
function resolveRepoRoot(): string {
  const fromEnv = env("RHO_LSP_REPO");
  if (fromEnv !== undefined) return resolve(fromEnv);
  const here = fileURLToPath(new URL(".", import.meta.url)); // dist/
  return resolve(here, "..", "..", "..");
}

async function main(): Promise<void> {
  setStderrLogger((line) => process.stderr.write(line));

  const repoRoot = resolveRepoRoot();
  const rhoBinary = env("RHO_LSP_RHO") ?? join(repoRoot, "build", "rho");
  const artifactsDir = env("RHO_LSP_ARTIFACTS") ?? join(repoRoot, "plugins", "rho-lsp", "artifacts");

  let generation: GenerationCheck | null = null;
  let runner: SubprocessRunner | null = null;

  if (!existsSync(rhoBinary)) {
    process.stderr.write(`[rho-lsp] boot binary not found at ${rhoBinary} (set RHO_LSP_RHO) — running inert\n`);
  } else {
    generation = await verifyGeneration({
      manifestPath: join(artifactsDir, "generation.json"),
      artifactsDir,
      compilerDir: join(repoRoot, "libs", "compiler"),
    });
    if (!generation.ok) {
      process.stderr.write(`[rho-lsp] generation invalid: ${generation.reason} — running inert\n`);
    } else {
      const wasmtime = env("RHO_LSP_WASMTIME");
      runner = new SubprocessRunner({ rhoBinary, repoRoot, ...(wasmtime !== undefined ? { wasmtime } : {}) });
    }
  }

  // ------------------------------------------------------------ connection
  const requestHandlers = new Map<string, (params: unknown) => Promise<unknown> | unknown>();
  const notificationHandlers = new Map<string, (params: unknown) => void>();

  const connection: Connection = {
    onRequest<P>(_method: string, handler: (params: P) => Promise<P> | P): void {
      requestHandlers.set(_method, handler as (params: unknown) => Promise<unknown> | unknown);
    },
    onNotification<P>(_method: string, handler: (params: P) => void): void {
      notificationHandlers.set(_method, handler as (params: unknown) => void);
    },
    notify(method: string, params: unknown): void {
      process.stdout.write(writeFrame({ jsonrpc: "2.0", method, params }));
    },
    reply(id: RequestId, result: unknown): void {
      process.stdout.write(writeFrame({ jsonrpc: "2.0", id, result }));
    },
    fail(id: RequestId, code: number, message: string): void {
      process.stdout.write(writeFrame({ jsonrpc: "2.0", id, error: { code, message } }));
    },
    exit(code: number): void {
      process.exit(code);
    },
  };

  const server = new RhoLspServer(connection, runner, generation);
  server.listen();

  const reader = new FrameReader((payload) => {
    const msg: JsonRpcMessage | null = parseMessage(payload);
    if (msg === null) {
      process.stderr.write("[rho-lsp] dropping an unparseable frame\n");
      return;
    }
    if (msg.method !== undefined) {
      if (msg.id === undefined || msg.id === null) {
        const fn = notificationHandlers.get(msg.method);
        if (fn) fn(msg.params);
        else process.stderr.write(`[rho-lsp] ignoring unknown notification ${msg.method}\n`);
        return;
      }
      const fn = requestHandlers.get(msg.method);
      const id: RequestId = msg.id; // narrowed above; captured for the async reply
      if (!fn) {
        connection.fail(id, -32601, `method not supported: ${msg.method}`); // -32601 MethodNotFound
        return;
      }
      void Promise.resolve()
        .then(() => fn(msg.params))
        .then(
          (result) => connection.reply(id, result),
          (err: unknown) => connection.fail(id, -32603, `internal error: ${err instanceof Error ? err.message : String(err)}`),
        );
      return;
    }
    // a response to a server->client request: none are issued today
    process.stderr.write("[rho-lsp] ignoring an unexpected response frame\n");
  });

  process.stdin.on("data", (chunk: Buffer) => reader.feed(chunk));
  process.stdin.on("error", () => {
    /* the editor went away; exit cleanly */
    process.exit(0);
  });
  process.stdin.resume();
}

main().catch((err) => {
  process.stderr.write(`[rho-lsp] fatal: ${err instanceof Error ? (err.stack ?? err.message) : String(err)}\n`);
  process.exit(1);
});
