// protocol.ts — the minimal LSP surface this server speaks, declared by
// hand so the package carries zero runtime dependencies. Only the types
// the server actually emits or consumes are declared; anything else is
// out of scope by design (the thin-shell law: the server is a byte pump
// and a babysitter, not a protocol museum).
//
// Acceptance law pinned across the suites: test/server-transcript.test.ts
// (the wire-visible shapes — capabilities, diagnostics, edits) and
// test/jsonrpc.test.ts (the framing). What the editor sees is byte for
// byte.

/** LSP position: 0-based line, 0-based UTF-16 code-unit offset. */
export interface Position {
  line: number;
  character: number;
}

/** LSP range: start inclusive, end exclusive. */
export interface Range {
  start: Position;
  end: Position;
}

export interface Location {
  uri: string;
  range: Range;
}

/** DiagnosticSeverity: 1 Error, 2 Warning, 3 Information, 4 Hint. */
export type DiagnosticSeverity = 1 | 2 | 3 | 4;

export interface Diagnostic {
  range: Range;
  severity: DiagnosticSeverity;
  /** Stable machine-readable code, e.g. "rho-check", "mut-never-written". */
  code?: string;
  source: string;
  message: string;
}

export interface TextEdit {
  range: Range;
  newText: string;
}

/** CompletionItemKind values the server emits (subset of the LSP enum). */
export type CompletionKind =
  | 2 // Method
  | 3 // Function
  | 6 // Variable
  | 8 // Interface (traits)
  | 9 // Module (use)
  | 13 // Enum
  | 14 // Keyword
  | 20 // EnumMember
  | 21 // Constant
  | 22; // Struct

export interface CompletionItem {
  label: string;
  kind: CompletionKind;
  detail?: string;
}

export interface Hover {
  contents: { kind: "markdown"; value: string };
  range?: Range;
}

// ---------------------------------------------------------------- documents

export interface DidOpenTextDocumentParams {
  textDocument: { uri: string; languageId: string; version: number; text: string };
}

export interface DidChangeTextDocumentParams {
  textDocument: { uri: string; version: number };
  /** Full sync only: exactly one change carrying the whole text. */
  contentChanges: { text: string }[];
}

export interface DidCloseTextDocumentParams {
  textDocument: { uri: string };
}

// ---------------------------------------------------------------- publish

export interface PublishDiagnosticsParams {
  uri: string;
  version: number | null;
  diagnostics: Diagnostic[];
}

export interface LogMessageParams {
  type: 1 | 2 | 3 | 4 | 5; // MessageType: Error..Debug
  message: string;
}

// ---------------------------------------------------------------- server -> client transport

/**
 * The transport seam. The stdio entry (main.ts) adapts JSON-RPC framing
 * to this; tests use an in-memory recorder. The server never sees bytes.
 */
export interface Connection {
  /** Register a request handler; the handler's result is the wire reply. */
  onRequest<P>(method: string, handler: (params: P) => Promise<unknown> | unknown): void;
  /** Register a notification handler. */
  onNotification<P>(method: string, handler: (params: P) => void): void;
  /** Server -> client notification. */
  notify(method: string, params: unknown): void;
  /** Reply to a request. The connection owns id bookkeeping. */
  reply(id: RequestId, result: unknown): void;
  /** Reply to a request with a JSON-RPC error object. */
  fail(id: RequestId, code: number, message: string): void;
  /** Ask the transport to end the process (exit code semantics: LSP). */
  exit(code: number): void;
}

export type RequestId = number | string | null;
