#!/usr/bin/env node
// rho-lsp: language server for the rho language over stdio.
//
// Powered by the real rho compiler (the boot toolchain as a wasm32-wasi
// program, run inside a worker thread). Capabilities:
//
//   - diagnostics on open/change with `rho check` semantics
//   - full-document formatting with `rho fmt` semantics
//   - hover signatures and single-file go-to-definition (syntactic index)
//
// Reliability contract: no handler may throw at the connection, every
// compiler invocation is wrapped, and a compiler that crashes or hangs is
// terminated and respawned — the server degrades to "no diagnostics" and
// logs, the editor session survives. The worker-thread isolation means a
// pathological document can stall the compiler without stalling the
// message loop that talks to the editor.

import {
  createConnection,
  ProposedFeatures,
  TextDocuments,
  TextDocumentSyncKind,
  Diagnostic,
  MarkupKind,
  Location,
  TextEdit,
  type InitializeResult,
  type TextDocumentPositionParams,
  type DocumentFormattingParams,
  type Hover,
} from 'vscode-languageserver/node';

import { TextDocument } from 'vscode-languageserver-textdocument';

import { createCompiler, type Compiler } from './compiler.js';
import {
  parseCompilerDiags,
  toLspDiags,
  LineIndex,
} from './diag.js';
import { indexSymbols, resolveName } from './symbols.js';

const DEBOUNCE_MS = intEnv('RHO_LSP_DEBOUNCE_MS', 150);
const MAX_DOC_CHARS = intEnv('RHO_LSP_MAX_DOC', 512 * 1024);

function intEnv(name: string, fallback: number): number {
  const raw = process.env[name];
  if (!raw) return fallback;
  const n = parseInt(raw, 10);
  return Number.isFinite(n) && n >= 0 ? n : fallback;
}

// Streams are passed explicitly: the server speaks LSP over stdio whether
// it was launched bare (bin), with --stdio, or by an editor client.
const connection = createConnection(ProposedFeatures.all, process.stdin, process.stdout);
const documents: TextDocuments<TextDocument> = new TextDocuments(TextDocument);

let compiler: Compiler | null = null;
let loggedNoCompiler = false;

/** Host-level issues (worker crash, timeout, missing compiler) go to the
 *  client log exactly once per distinct message. */
const seenIssues = new Set<string>();
function onIssue(message: string) {
  if (seenIssues.has(message)) return;
  seenIssues.add(message);
  connection.console.log(message);
}

// ---------------------------------------------------------- validation --

/** Per-document validation state: latest-wins by sequence number. */
const seqByUri = new Map<string, number>();
const timers = new Map<string, NodeJS.Timeout>();

function scheduleValidation(uri: string, version: number) {
  const prev = timers.get(uri);
  if (prev) clearTimeout(prev);
  const seq = (seqByUri.get(uri) ?? 0) + 1;
  seqByUri.set(uri, seq);
  timers.set(
    uri,
    setTimeout(() => {
      timers.delete(uri);
      void validate(uri, version, seq);
    }, DEBOUNCE_MS),
  );
}

/** Run `rho check` over the document and publish its diagnostics. Never
 *  throws: a compiler failure keeps the previous diagnostics and logs. */
async function validate(uri: string, version: number, seq: number): Promise<void> {
  const doc = documents.get(uri);
  if (!doc) return;
  if (seq !== (seqByUri.get(uri) ?? 0)) return; // superseded

  let diagnostics: Diagnostic[] = [];
  try {
    if (doc.getText().length > MAX_DOC_CHARS) {
      // too big to check promptly: clear rather than show stale results
      logOnce(uri, `document over ${MAX_DOC_CHARS} chars — diagnostics disabled for it`);
    } else if (compiler && compiler.backend !== 'none') {
      const result = await compiler.check(doc.getText());
      if (seq !== (seqByUri.get(uri) ?? 0)) return; // superseded while waiting
      if (result.exitCode < 0) {
        // host-level failure: keep previous diagnostics, reason already logged
      } else {
        const index = new LineIndex(doc.getText());
        diagnostics = toLspDiags(parseCompilerDiags(result.stderr), rawLines(index), (f) =>
          f === '/main.rho' || f.endsWith('/main.rho'),
        );
      }
    } else {
      if (!loggedNoCompiler) {
        loggedNoCompiler = true;
        connection.console.log('rho-lsp: no compiler backend — diagnostics unavailable');
      }
    }
  } catch (e) {
    onIssue(`rho-lsp: validation failed: ${e instanceof Error ? e.message : String(e)}`);
    return; // keep whatever was last published
  }

  connection.sendDiagnostics({ uri, version, diagnostics });
}

/** The document text as an array of raw lines (for the 1-based compiler
 *  coordinates). */
function rawLines(index: LineIndex): string[] {
  const lines: string[] = [];
  for (let i = 0; i < index.lineCount; i++) lines.push(index.lineText(i));
  return lines;
}

function logOnce(uri: string, message: string) {
  const key = `${uri}: ${message}`;
  if (seenIssues.has(key)) return;
  seenIssues.add(key);
  connection.console.log(message);
}

// ----------------------------------------------------------- lifecycle --

connection.onInitialize((_params): InitializeResult => {
  try {
    compiler = createCompiler({ onIssue });
    connection.console.log(
      `rho-lsp: compiler backend ${compiler.backend}` +
        (compiler.wasmPath ? ` (${compiler.wasmPath})` : '') +
        (compiler.binaryPath ? ` (${compiler.binaryPath})` : ''),
    );
  } catch (e) {
    onIssue(`rho-lsp: compiler init failed: ${e instanceof Error ? e.message : String(e)}`);
    compiler = createCompiler({ onIssue: () => {} }); // inert backend
  }
  return {
    capabilities: {
      textDocumentSync: TextDocumentSyncKind.Full,
      hoverProvider: true,
      documentFormattingProvider: true,
      definitionProvider: true,
    },
    serverInfo: { name: 'rho-lsp', version: '0.1.0' },
  };
});

connection.onShutdown(async () => {
  try {
    await compiler?.dispose();
  } catch {
    // shutdown must never fail
  }
});

documents.onDidOpen((event) => {
  try {
    scheduleValidation(event.document.uri, event.document.version);
  } catch (e) {
    onIssue(`rho-lsp: didOpen handling failed: ${e instanceof Error ? e.message : String(e)}`);
  }
});

documents.onDidChangeContent((event) => {
  try {
    scheduleValidation(event.document.uri, event.document.version);
  } catch (e) {
    onIssue(`rho-lsp: didChange handling failed: ${e instanceof Error ? e.message : String(e)}`);
  }
});

documents.onDidClose((event) => {
  try {
    const prev = timers.get(event.document.uri);
    if (prev) clearTimeout(prev);
    timers.delete(event.document.uri);
    seqByUri.delete(event.document.uri);
    connection.sendDiagnostics({ uri: event.document.uri, diagnostics: [] });
  } catch (e) {
    onIssue(`rho-lsp: didClose handling failed: ${e instanceof Error ? e.message : String(e)}`);
  }
});

// ----------------------------------------------------------------- hover --

connection.onHover((params: TextDocumentPositionParams): Hover | null => {
  try {
    // hover is purely syntactic: it works even with the inert backend
    const doc = documents.get(params.textDocument.uri);
    if (!doc) return null;
    const text = doc.getText();
    const index = new LineIndex(text);
    const offset = index.offsetAt(params.position.line, params.position.character);
    const symIndex = indexSymbols(text);
    const { definition } = resolveName(symIndex, text, offset);
    if (!definition) return null;
    const kindLabel =
      definition.kind === 'fn'
        ? 'function'
        : definition.kind === 'use'
          ? 'import'
          : definition.kind === 'param'
            ? 'parameter'
            : definition.kind;
    return {
      contents: {
        kind: MarkupKind.Markdown,
        value: `rho ${kindLabel}\n\n\`\`\`rho\n${definition.detail}\n\`\`\``,
      },
    };
  } catch (e) {
    onIssue(`rho-lsp: hover failed: ${e instanceof Error ? e.message : String(e)}`);
    return null;
  }
});

// ----------------------------------------------------------- definition --

connection.onDefinition((params: TextDocumentPositionParams): Location | null => {
  try {
    // single-file syntactic navigation: independent of the compiler backend
    const doc = documents.get(params.textDocument.uri);
    if (!doc) return null;
    const text = doc.getText();
    const index = new LineIndex(text);
    const offset = index.offsetAt(params.position.line, params.position.character);
    const symIndex = indexSymbols(text);
    const { definition } = resolveName(symIndex, text, offset);
    if (!definition) return null;
    return {
      uri: params.textDocument.uri,
      range: {
        start: index.positionAt(definition.nameRange.start),
        end: index.positionAt(definition.nameRange.end),
      },
    };
  } catch (e) {
    onIssue(`rho-lsp: definition failed: ${e instanceof Error ? e.message : String(e)}`);
    return null;
  }
});

// ------------------------------------------------------------ formatting --

connection.onDocumentFormatting(async (params: DocumentFormattingParams): Promise<TextEdit[]> => {
  try {
    if (!compiler || compiler.backend === 'none') return [];
    const doc = documents.get(params.textDocument.uri);
    if (!doc) return [];
    const result = await compiler.fmt(doc.getText());
    if (result.exitCode !== 0) {
      // parse errors: the formatter produced nothing; surface the reason in
      // the log, never as a thrown error ([] is the LSP-idiomatic "no")
      const first = parseCompilerDiags(result.stderr)[0];
      connection.console.log(
        `rho fmt: cannot format (${first ? `${first.line}:${first.col} ${first.message}` : 'unknown parse error'})`,
      );
      return [];
    }
    const formatted = result.stdout;
    const text = doc.getText();
    if (formatted === text) return []; // already canonical
    const index = new LineIndex(text);
    return [
      TextEdit.replace(
        {
          start: { line: 0, character: 0 },
          end: index.positionAt(text.length),
        },
        formatted,
      ),
    ];
  } catch (e) {
    onIssue(`rho-lsp: formatting failed: ${e instanceof Error ? e.message : String(e)}`);
    return [];
  }
});

// -------------------------------------------------------------- startup --

documents.listen(connection);
connection.listen();

// last-ditch guard: the process must exit cleanly, never wedged or noisy
process.on('unhandledRejection', (reason) => {
  onIssue(`rho-lsp: unhandled rejection: ${reason instanceof Error ? reason.message : String(reason)}`);
});
process.on('uncaughtException', (err) => {
  onIssue(`rho-lsp: uncaught exception: ${err.message}`);
});
