// Protocol-level integration tests: spawn the real server process, speak
// LSP over stdio (Content-Length framing), and assert on the wire traffic.
import test from 'node:test';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

import { GOOD_RHO, BAD_RHO, MESSY_RHO, CANONICAL_RHO, SYMBOLS_RHO, waitFor } from '../testkit/fixtures.mjs';

const pkgRoot = join(dirname(fileURLToPath(import.meta.url)), '..');
const SERVER = join(pkgRoot, 'dist', 'server.js');

/** A minimal LSP client over a spawned server's stdio. */
class LspClient {
  constructor(env = {}) {
    this.proc = spawn(process.execPath, [SERVER], {
      env: { ...process.env, ...env },
      stdio: ['pipe', 'pipe', 'pipe'],
    });
    this.messages = [];
    this.nextId = 1;
    this.buffer = Buffer.alloc(0);
    this.stderr = '';
    this.proc.stdout.on('data', (chunk) => this.#onData(chunk));
    this.proc.stderr.on('data', (chunk) => (this.stderr += chunk.toString('utf8')));
    this.exited = new Promise((resolve) => this.proc.on('exit', (code) => resolve(code)));
  }

  #onData(chunk) {
    this.buffer = Buffer.concat([this.buffer, chunk]);
    for (;;) {
      const headerEnd = this.buffer.indexOf('\r\n\r\n');
      if (headerEnd < 0) return;
      const header = this.buffer.slice(0, headerEnd).toString('utf8');
      const match = /Content-Length: (\d+)/.exec(header);
      if (!match) return;
      const length = parseInt(match[1], 10);
      if (this.buffer.length < headerEnd + 4 + length) return;
      const body = this.buffer.slice(headerEnd + 4, headerEnd + 4 + length).toString('utf8');
      this.buffer = this.buffer.slice(headerEnd + 4 + length);
      this.messages.push(JSON.parse(body));
    }
  }

  send(message) {
    const body = Buffer.from(JSON.stringify(message), 'utf8');
    this.proc.stdin.write(`Content-Length: ${body.length}\r\n\r\n`);
    this.proc.stdin.write(body);
  }

  notify(method, params) {
    this.send({ jsonrpc: '2.0', method, params });
  }

  /** Sends a request and resolves with its `result` (throws on JSON-RPC error). */
  async request(method, params) {
    const id = this.nextId++;
    this.send({ jsonrpc: '2.0', id, method, params });
    const msg = await waitFor(() => {
      const m = this.messages.find((x) => x.id === id);
      return m && (m.result !== undefined || m.error) ? m : null;
    });
    if (msg.error) throw new Error(`${method} failed: ${JSON.stringify(msg.error)}`);
    return msg.result;
  }

  /** Resolves with the first server notification matching the predicate. */
  notification(method, pred = () => true) {
    return waitFor(() => this.messages.find((m) => m.method === method && pred(m)) ?? null);
  }

  async initialize() {
    const result = await this.request('initialize', {
      processId: process.pid,
      rootUri: null,
      capabilities: {},
    });
    this.notify('initialized', {});
    return result;
  }

  open(text, languageId = 'rho') {
    const uri = 'file:///workspace/main.rho';
    this.notify('textDocument/didOpen', {
      textDocument: { uri, languageId, version: 1, text },
    });
    return uri;
  }

  async close() {
    await this.request('shutdown', null); // shutdown is a request in LSP
    this.notify('exit', null);
    const code = await Promise.race([
      this.exited,
      new Promise((_, reject) => setTimeout(() => reject(new Error('server did not exit')), 10000)),
    ]);
    return code;
  }

  kill() {
    this.proc.kill('SIGKILL');
  }
}

test('initialize reports the expected capabilities and server info', async () => {
  const client = new LspClient();
  try {
    const result = await client.initialize();
    assert.equal(result.serverInfo.name, 'rho-lsp');
    assert.equal(result.capabilities.textDocumentSync, 1); // Full
    assert.equal(result.capabilities.hoverProvider, true);
    assert.equal(result.capabilities.documentFormattingProvider, true);
    assert.equal(result.capabilities.definitionProvider, true);
  } finally {
    client.kill();
  }
});

test('publishDiagnostics arrives for a document with a type error', async () => {
  const client = new LspClient();
  try {
    await client.initialize();
    const uri = client.open(BAD_RHO);
    const pub = await client.notification('textDocument/publishDiagnostics', (m) => m.params.uri === uri);
    assert.equal(pub.params.diagnostics.length, 1);
    const d = pub.params.diagnostics[0];
    assert.equal(d.severity, 1);
    assert.equal(d.source, 'rho');
    assert.match(d.message, /initializer: expected `i32`, found `string`/);
    assert.deepEqual(d.range.start, { line: 1, character: 17 });
    assert.deepEqual(d.range.end, { line: 1, character: 18 });
  } finally {
    client.kill();
  }
});

test('a clean document publishes empty diagnostics and edits clear them', async () => {
  const client = new LspClient();
  try {
    await client.initialize();
    const uri = client.open(GOOD_RHO);
    const clean = await client.notification('textDocument/publishDiagnostics', (m) => m.params.uri === uri);
    assert.deepEqual(clean.params.diagnostics, []);

    // introduce the type error via didChange (Full sync: send whole text)
    client.send({
      jsonrpc: '2.0',
      method: 'textDocument/didChange',
      params: {
        textDocument: { uri, version: 2 },
        contentChanges: [{ text: BAD_RHO }],
      },
    });
    const bad = await client.notification(
      'textDocument/publishDiagnostics',
      (m) => m.params.uri === uri && m.params.version === 2 && m.params.diagnostics.length > 0,
    );
    assert.match(bad.params.diagnostics[0].message, /expected `i32`, found `string`/);

    // fix it again: diagnostics clear
    client.send({
      jsonrpc: '2.0',
      method: 'textDocument/didChange',
      params: {
        textDocument: { uri, version: 3 },
        contentChanges: [{ text: GOOD_RHO }],
      },
    });
    const fixed = await client.notification(
      'textDocument/publishDiagnostics',
      (m) => m.params.uri === uri && m.params.version === 3 && m.params.diagnostics.length === 0,
    );
    assert.ok(fixed);
  } finally {
    client.kill();
  }
});

test('formatting returns the canonical text as one whole-document edit', async () => {
  const client = new LspClient();
  try {
    await client.initialize();
    const uri = client.open(MESSY_RHO);
    // let validation settle first (diagnostics in flight do not block fmt)
    await client.notification('textDocument/publishDiagnostics', (m) => m.params.uri === uri);
    const result = await client.request('textDocument/formatting', {
      textDocument: { uri },
      options: { tabSize: 4, insertSpaces: true },
    });
    assert.equal(result.length, 1);
    const edit = result[0];
    assert.equal(edit.range.start.line, 0);
    assert.equal(edit.range.start.character, 0);
    assert.equal(edit.newText, CANONICAL_RHO);
  } finally {
    client.kill();
  }
});

test('formatting a broken document returns no edits (graceful)', async () => {
  const client = new LspClient();
  try {
    await client.initialize();
    const uri = client.open('fn main( -> i32 { return 0; }');
    const result = await client.request('textDocument/formatting', {
      textDocument: { uri },
      options: { tabSize: 4, insertSpaces: true },
    });
    assert.deepEqual(result, []);
  } finally {
    client.kill();
  }
});

test('hover shows the signature of the symbol under the cursor', async () => {
  const client = new LspClient();
  try {
    await client.initialize();
    const uri = client.open(SYMBOLS_RHO);
    await client.notification('textDocument/publishDiagnostics', (m) => m.params.uri === uri);
    const lines = SYMBOLS_RHO.split('\n');
    const lineOf = (needle) => lines.findIndex((l) => l.includes(needle));

    // over `fn area` name itself
    const areaLine = lineOf('fn area(');
    const hoverFn = await client.request('textDocument/hover', {
      textDocument: { uri },
      position: { line: areaLine, character: 7 },
    });
    assert.match(hoverFn.contents.value, /fn area\(s: Shape\) -> f64/);

    // over the local `n` declaration
    const nLine = lineOf('let n: i32');
    const hoverLet = await client.request('textDocument/hover', {
      textDocument: { uri },
      position: { line: nLine, character: 6 },
    });
    assert.match(hoverLet.contents.value, /let n: i32/);

    // over a method name
    const distLine = lineOf('fn Point.dist(');
    const hoverMethod = await client.request('textDocument/hover', {
      textDocument: { uri },
      position: { line: distLine, character: 10 },
    });
    assert.match(hoverMethod.contents.value, /fn Point\.dist\(self: \*Point\) -> f64/);
  } finally {
    client.kill();
  }
});

test('go-to-definition jumps from a use to the declaration', async () => {
  const client = new LspClient();
  try {
    await client.initialize();
    const uri = client.open(SYMBOLS_RHO);
    await client.notification('textDocument/publishDiagnostics', (m) => m.params.uri === uri);
    const lines = SYMBOLS_RHO.split('\n');
    const callLine = lines.findIndex((l) => l.includes('return norm(p) + p.x + n;'));
    assert.ok(callLine >= 0);

    // `norm` at the call site
    const normCol = lines[callLine].indexOf('norm');
    const def = await client.request('textDocument/definition', {
      textDocument: { uri },
      position: { line: callLine, character: normCol },
    });
    assert.equal(def.uri, uri);
    const declLine = lines.findIndex((l) => l.startsWith('fn norm('));
    assert.equal(def.range.start.line, declLine);

    // local `n` at the end of the return
    const nCol = lines[callLine].lastIndexOf('n;');
    const defLocal = await client.request('textDocument/definition', {
      textDocument: { uri },
      position: { line: callLine, character: nCol },
    });
    assert.equal(defLocal.range.start.line, lines.findIndex((l) => l.includes('let n: i32')));

    // unknown name (struct field `x` is not indexed) -> null, not an error
    const xCol = lines[callLine].indexOf('.x') + 1;
    const defNone = await client.request('textDocument/definition', {
      textDocument: { uri },
      position: { line: callLine, character: xCol },
    });
    assert.equal(defNone, null);
  } finally {
    client.kill();
  }
});

test('concurrent traffic (validations + hovers + definitions) keeps the server responsive', async () => {
  // Worker-thread isolation is unit-tested (crashForTest -> respawn); here we
  // prove the wire protocol stays responsive under concurrent load.
  const client = new LspClient();
  try {
    await client.initialize();
    const uri = client.open(SYMBOLS_RHO);
    await client.notification('textDocument/publishDiagnostics', (m) => m.params.uri === uri);
    const lines = SYMBOLS_RHO.split('\n');
    const callLine = lines.findIndex((l) => l.includes('return norm(p) + p.x + n;'));
    // a burst of interleaved requests while more validations are scheduled
    const hovers = [];
    const defs = [];
    const normCol = lines[callLine].indexOf('norm');
    for (let i = 0; i < 10; i++) {
      hovers.push(client.request('textDocument/hover', {
        textDocument: { uri },
        position: { line: callLine, character: normCol },
      }));
      defs.push(client.request('textDocument/definition', {
        textDocument: { uri },
        position: { line: callLine, character: normCol },
      }));
      client.send({
        jsonrpc: '2.0',
        method: 'textDocument/didChange',
        params: { textDocument: { uri, version: i + 2 }, contentChanges: [{ text: SYMBOLS_RHO }] },
      });
    }
    const hoverResults = await Promise.all(hovers);
    const defResults = await Promise.all(defs);
    for (const r of hoverResults) {
      assert.match(r?.contents?.value ?? '', /fn norm\(p: \*Point\) -> i32/);
    }
    for (const d of defResults) {
      assert.equal(d.uri, uri);
      assert.equal(d.range.start.line, lines.findIndex((l) => l.startsWith('fn norm(')));
    }
  } finally {
    client.kill();
  }
});

test('shutdown/exit terminates the server cleanly', async () => {
  const client = new LspClient();
  await client.initialize();
  client.open(GOOD_RHO);
  await client.notification('textDocument/publishDiagnostics', () => true);
  const code = await client.close();
  assert.equal(code, 0);
});

test('rapid open/change/close cycles never wedge the server', async () => {
  const client = new LspClient();
  try {
    await client.initialize();
    const uri = 'file:///workspace/main.rho';
    for (let v = 1; v <= 25; v++) {
      client.notify('textDocument/didOpen', {
        textDocument: { uri, languageId: 'rho', version: v, text: v % 2 ? BAD_RHO : GOOD_RHO },
      });
    }
    client.notify('textDocument/didClose', { textDocument: { uri } });
    // the server must still answer requests after the churn
    const result = await client.request('textDocument/hover', {
      textDocument: { uri },
      position: { line: 0, character: 0 },
    });
    assert.equal(result, null); // document closed; hover is a clean null
  } finally {
    const code = await client.close();
    assert.equal(code, 0);
  }
});
