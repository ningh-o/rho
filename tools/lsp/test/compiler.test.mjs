// Unit tests: compiler host (both backends), diagnostic parsing, the line
// index, and the symbol index. The compiler-facing tests run against the
// real rho compiler wasm discovered next to the package.
import test from 'node:test';
import assert from 'node:assert/strict';

import { createCompiler, resolveCompilerWasm, resolveNativeBinary } from '../dist/compiler.js';
import { parseCompilerDiags, toLspDiags, tokenRangeAt, LineIndex } from '../dist/diag.js';
import { indexSymbols, resolveName, identAt } from '../dist/symbols.js';
import { GOOD_RHO, BAD_RHO, MESSY_RHO, CANONICAL_RHO, BROKEN_RHO, SYMBOLS_RHO } from '../testkit/fixtures.mjs';

function rawLines(index) {
  const lines = [];
  for (let i = 0; i < index.lineCount; i++) lines.push(index.lineText(i));
  return lines;
}

// ------------------------------------------------------------- compiler --

test('compiler wasm is discovered next to the package', () => {
  const wasmPath = resolveCompilerWasm();
  assert.ok(wasmPath, 'expected build/rho.wasm or site/assets/rho.wasm');
  assert.match(wasmPath, /rho(-boot)?\.wasm$/);
});

test('wasm-worker backend: check reports rho check semantics', async () => {
  const issues = [];
  const compiler = createCompiler({ onIssue: (m) => issues.push(m), backend: 'wasm' });
  assert.equal(compiler.backend, 'wasm-worker');
  try {
    const good = await compiler.check(GOOD_RHO);
    assert.equal(good.exitCode, 0);
    assert.equal(good.stdout, 'ok /main.rho\n');
    assert.equal(good.stderr, '');

    const bad = await compiler.check(BAD_RHO);
    assert.equal(bad.exitCode, 1);
    assert.equal(bad.stderr, '/main.rho:2:18: error: initializer: expected `i32`, found `string`\n');
  } finally {
    await compiler.dispose();
  }
  assert.equal(issues.length, 0, `unexpected issues: ${issues.join('; ')}`);
});

test('wasm-worker backend: fmt returns canonical text and flags broken input', async () => {
  const compiler = createCompiler({ backend: 'wasm' });
  try {
    const fmt = await compiler.fmt(MESSY_RHO);
    assert.equal(fmt.exitCode, 0);
    assert.equal(fmt.stdout, CANONICAL_RHO);

    const broken = await compiler.fmt(BROKEN_RHO);
    assert.equal(broken.exitCode, 1);
    assert.match(broken.stderr, /:1:\d+: error:/);
    assert.equal(broken.stdout, '');
  } finally {
    await compiler.dispose();
  }
});

test('wasm-worker backend: the richer fixture typechecks (fixture pin)', async () => {
  const compiler = createCompiler({ backend: 'wasm' });
  try {
    const r = await compiler.check(SYMBOLS_RHO);
    assert.equal(r.exitCode, 0, `fixture should typecheck: ${r.stderr}`);
  } finally {
    await compiler.dispose();
  }
});

test('wasm-worker backend: host survives an abrupt worker crash (respawn)', async () => {
  const compiler = createCompiler({ backend: 'wasm' });
  try {
    const before = await compiler.check(GOOD_RHO);
    assert.equal(before.exitCode, 0);
    compiler.crashForTest(); // terminate the worker out from under the host
    const after = await compiler.check(GOOD_RHO);
    assert.equal(after.exitCode, 0, 'a fresh worker must serve the next request');
  } finally {
    await compiler.dispose();
  }
});

test('native-cli backend: same check semantics as the wasm worker', async (t) => {
  if (!resolveNativeBinary()) return t.skip('no native rho binary on this machine');
  const compiler = createCompiler({ backend: 'native' });
  assert.equal(compiler.backend, 'native-cli');
  try {
    const bad = await compiler.check(BAD_RHO);
    assert.equal(bad.exitCode, 1);
    // the native CLI reports against the temp file it was given, not /main.rho
    assert.match(bad.stderr, /main\.rho:2:18: error: initializer: expected `i32`, found `string`/);

    const fmt = await compiler.fmt(MESSY_RHO);
    assert.equal(fmt.exitCode, 0);
    assert.equal(fmt.stdout, CANONICAL_RHO);
  } finally {
    await compiler.dispose();
  }
});

test('inert backend: outcomes are failures without exceptions', async () => {
  const issues = [];
  const compiler = createCompiler({ backend: 'none', onIssue: (m) => issues.push(m) });
  assert.equal(compiler.backend, 'none');
  const r = await compiler.check(GOOD_RHO);
  assert.equal(r.exitCode, -1);
  const f = await compiler.fmt(GOOD_RHO);
  assert.equal(f.exitCode, -1);
  await compiler.dispose();
  assert.ok(issues.length > 0, 'the missing-compiler reason must be reported');
});

// ---------------------------------------------------------------- diag --

test('parseCompilerDiags: parses rho check stderr, skips foreign lines', () => {
  const stderr = [
    'rho: cannot open /io/file.rho', // no line:col — must be ignored
    '/main.rho:2:18: error: initializer: expected `i32`, found `string`',
    '',
    '<prelude>:716:34: error: type mismatch in `+`: `*u8` vs `usize`',
    '/main.rho:3:11: error: unknown name `y`',
  ].join('\n');
  const diags = parseCompilerDiags(stderr);
  assert.deepEqual(
    diags.map((d) => [d.file, d.line, d.col, d.message]),
    [
      ['/main.rho', 2, 18, 'initializer: expected `i32`, found `string`'],
      ['<prelude>', 716, 34, 'type mismatch in `+`: `*u8` vs `usize`'],
      ['/main.rho', 3, 11, 'unknown name `y`'],
    ],
  );
});

test('toLspDiags: filters foreign files, maps to 0-based token ranges', () => {
  const raw = parseCompilerDiags(
    '<prelude>:716:34: error: prelude noise\n/main.rho:2:18: error: initializer: expected `i32`, found `string`\n',
  );
  const lsp = toLspDiags(raw, rawLines(new LineIndex(BAD_RHO)), (f) => f === '/main.rho');
  assert.equal(lsp.length, 1);
  assert.equal(lsp[0].severity, 1); // DiagnosticSeverity.Error
  assert.equal(lsp[0].source, 'rho');
  assert.deepEqual(lsp[0].range.start, { line: 1, character: 17 });
  assert.deepEqual(lsp[0].range.end, { line: 1, character: 18 }); // the opening quote
  assert.match(lsp[0].message, /expected `i32`, found `string`/);
});

test('tokenRangeAt: identifiers span, punctuation is one char, end of line clamps', () => {
  const lines = ['  return alpha;'];
  const alpha = tokenRangeAt(lines, 0, 11);
  assert.deepEqual([alpha.start.character, alpha.end.character], [9, 14]);
  const semi = tokenRangeAt(lines, 0, 14);
  assert.deepEqual([semi.start.character, semi.end.character], [14, 15]);
  const eol = tokenRangeAt(lines, 0, 15);
  assert.deepEqual([eol.start.character, eol.end.character], [14, 15]); // clamped
  const outOfRange = tokenRangeAt(lines, 7, 0);
  assert.deepEqual([outOfRange.start.line, outOfRange.start.character], [7, 0]);
});

// ----------------------------------------------------------- LineIndex --

test('LineIndex: offset/position roundtrip on \\n and \\r\\n', () => {
  for (const text of ['aa\nbbbb\n\ncc', 'aa\r\nbbbb\r\n\r\ncc']) {
    const index = new LineIndex(text);
    assert.equal(index.lineCount, 4);
    assert.equal(index.lineText(1), 'bbbb');
    assert.equal(index.lineText(2), '');
    const p = index.positionAt(text.indexOf('bb'));
    assert.deepEqual(p, { line: 1, character: 0 });
    assert.equal(index.offsetAt(p.line, p.character), text.indexOf('bb'));
    assert.equal(index.lineText(3), 'cc');
  }
});

test('LineIndex: offsetAt clamps past end of line and doc', () => {
  const index = new LineIndex('ab\ncd');
  assert.equal(index.offsetAt(0, 99), 2);
  assert.equal(index.offsetAt(99, 0), index.text.length);
  assert.equal(index.offsetAt(-1, -1), 0);
});

// -------------------------------------------------------------- symbols --

const symIndex = indexSymbols(SYMBOLS_RHO);
const symText = SYMBOLS_RHO;

function names(syms) {
  return syms.map((s) => s.name);
}

test('indexSymbols: file-level declarations and enum variants', () => {
  assert.deepEqual(names(symIndex.topLevel), [
    'Point', 'Shape', 'Circle', 'Unit', 'area', 'Point.dist', 'norm', 'main',
  ]);
});

test('indexSymbols: fn details carry the full signature', () => {
  const area = symIndex.topLevel.find((s) => s.name === 'area');
  assert.equal(area.detail, 'fn area(s: Shape) -> f64');
  const dist = symIndex.topLevel.find((s) => s.name === 'Point.dist');
  assert.equal(dist.detail, 'fn Point.dist(self: *Point) -> f64');
  const circle = symIndex.topLevel.find((s) => s.name === 'Circle');
  assert.equal(circle.kind, 'variant');
  assert.equal(circle.detail, 'Shape.Circle(f64)');
});

test('indexSymbols: params and locals are scoped to their function', () => {
  const main = symIndex.topLevel.find((s) => s.name === 'main');
  assert.deepEqual(names(symIndex.localsByFn.get(main) ?? []), ['p', 'n']);
  const area = symIndex.topLevel.find((s) => s.name === 'area');
  assert.deepEqual(names(symIndex.localsByFn.get(area) ?? []), ['s']);
  const normFn = symIndex.topLevel.find((s) => s.name === 'norm');
  assert.deepEqual(names(symIndex.localsByFn.get(normFn) ?? []), ['p']);
});

test('resolveName: use of a file-level function at a call site', () => {
  const callSite = symText.indexOf('norm(p) + p.x');
  const { definition } = resolveName(symIndex, symText, callSite);
  assert.equal(definition.kind, 'fn');
  assert.equal(definition.detail, 'fn norm(p: *Point) -> i32');
});

test('resolveName: locals resolve inside their function', () => {
  const nUse = symText.indexOf('+ n;') + 2; // the `n` token
  const { definition } = resolveName(symIndex, symText, nUse);
  assert.equal(definition.kind, 'let');
  assert.equal(definition.detail, 'let n: i32');
});

test('resolveName: parameters resolve with their type', () => {
  const body = symText.indexOf('return p.x + p.y;') + 'return '.length; // the `p`
  const { definition, owner } = resolveName(symIndex, symText, body);
  assert.equal(owner.name, 'norm');
  assert.equal(definition.kind, 'param');
  assert.equal(definition.detail, 'p: *Point');
});

test('resolveName: enum variant at a match pattern', () => {
  const pat = symText.indexOf('Circle(r) =>');
  const { definition } = resolveName(symIndex, symText, pat);
  assert.equal(definition.kind, 'variant');
  assert.equal(definition.name, 'Circle');
});

test('resolveName: hovering a declaration name resolves to itself', () => {
  const area = symIndex.topLevel.find((s) => s.name === 'area');
  const { definition } = resolveName(symIndex, symText, area.nameRange.start + 1);
  assert.equal(definition, area);
});

test('resolveName: struct fields are not indexed (honest null)', () => {
  const fieldUse = symText.indexOf('.x + n') + 1; // the `x` of p.x
  const { definition } = resolveName(symIndex, symText, fieldUse);
  assert.equal(definition, null);
});

test('identAt: token edges and non-identifiers', () => {
  const text = 'fn main() -> i32';
  assert.deepEqual(identAt(text, 4)?.text, 'main');
  assert.deepEqual(identAt(text, 7)?.text, 'main'); // right edge
  assert.equal(identAt(text, 8), null); // on '('
  assert.equal(identAt('', 0), null);
});

// ------------------------------------------------- malformed robustness --

test('symbol index never throws on garbage input', () => {
  for (const garbage of ['', '   \n\n', 'fn fn fn', 'struct {', '"unterminated', '}}}', 'let = ;', 'enum X { A(', 'fn ( [ {']) {
    assert.doesNotThrow(() => indexSymbols(garbage));
  }
});

test('parseCompilerDiags never throws on garbage input', () => {
  assert.deepEqual(parseCompilerDiags(''), []);
  assert.deepEqual(parseCompilerDiags('total nonsense\n::::\n1:2\n'), []);
});
