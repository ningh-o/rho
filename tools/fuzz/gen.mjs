#!/usr/bin/env node
// tools/fuzz/gen.mjs — seeded random rho programs, differential boot vs the
// self-hosted chain (T3.3 + T3.17's discovery leg).
//
// Law of the campaign: the SAME generated source program goes through boot
// (`rho run` — compile + execute under wasmtime) and through the self-hosted
// chain (a fresh per-program bake of libs/compiler with the program as its
// frozen SRC input — tests/run-corpus-diff.sh's construction; a compiled
// compiler's input is frozen at bake, so one bake per program is the only
// honest mirror face). What must agree is stdout + exit code — never a byte
// compare of the two compilers' wasm artifacts.
//
// Reproducibility: every random choice comes from a seeded LCG. Math.random
// is banned in this file — a seed fully determines the program, so any diff
// the campaign reports can be regenerated with `node tools/fuzz/gen.mjs --emit <seed>`.
//
// Grammar discipline: the generator only emits language forms proven by
// corpus/ programs (which the gate's corpus-diff leg already grades green on
// both compilers): fully parenthesized expressions, `as` casts, all int
// widths + f32/f64 + bool, let/let-mut, assignment to mut locals and statics,
// if/else, bounded while with break, literal `match` in a helper, printf
// with literal format strings whose `{}` count matches the values. Every
// intermediate value is printf'd, so a semantic divergence shows up in the
// captured stdout, not just in a truncated tail after a panic.
//
// The one `as` law (campaign v1, 2026-09-23, found it the hard way — 69 of
// 150 seeds both-invalid): a BARE parenthesized literal directly under `as`
// (`100.0 as i16`, `5 as f32`, `(-89) as f32`) folds into an ill-typed
// constant; BOTH compilers accept the program at check time and emit an
// invalid module (wasmtime: "type mismatch: expected …, found …"). This is
// the defect corpus/053's header pins, and at random-program scale it fires
// on ~46% of seeds — the generator now anchors every cast operand: typed
// local, binary-op fold, int-literal→int-target (probed safe), or the
// corpus's own bind-to-a-typed-local workaround in castStmt. Mixed f32/f64
// arithmetic needs no guard: the checker rejects it (probed), so it can
// never reach the both-invalid bucket.
//
// Type discipline: nested expressions never change type — a cast may only
// appear wrapped so the whole subexpression has the type its context expects
// (`(int-expr as f32)` in float space, `(expr as T)` as a whole let
// initializer). Arbitrary-width casts are their own statement.
//
// Usage:
//   node tools/fuzz/gen.mjs --emit 42        print the program for seed 42
//   node tools/fuzz/gen.mjs                  run the campaign, seeds 1..150
//                                          (validated 1..400 as of 2026-09-24:
//                                          389/390 identical — seed 46's
//                                          shift-width fold family is the
//                                          one recorded divergence, see
//                                          docs/todo.md + tools/repro/)
//   node tools/fuzz/gen.mjs --from 10 --to 20 --budget 60
//
// Campaign bounds (the ask): ≤40 lines per program, ≤10s per program (each
// rho command additionally runs under its own perl-alarm wall-clock cap,
// same wrapper as tools/gate.sh — macOS has no GNU timeout), total budget
// 15 minutes; on mismatch the program is saved to
// build/gate/fuzz_fail_<seed>.rho (never into corpus/).

import { spawn } from 'node:child_process';
import { copyFileSync, existsSync, mkdirSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..', '..');
const GATE = join(ROOT, 'build', 'gate');
const FUZZ_DIR = join(GATE, 'fuzz');

// ---------------------------------------------------------------------------
// The seeded LCG — the only source of randomness in this file.
// ---------------------------------------------------------------------------
function makeRng(seed) {
  let s = seed >>> 0 || 0x9e3779b9;
  const next = () => {
    s = (Math.imul(s, 1103515245) + 12345) >>> 0;
    return s / 4294967296;
  };
  return {
    next,
    // inclusive on both ends
    int: (lo, hi) => lo + Math.floor(next() * (hi - lo + 1)),
    pick: (arr) => arr[Math.floor(next() * arr.length)],
    chance: (p) => next() < p,
  };
}

// ---------------------------------------------------------------------------
// Types and literal pools. Every literal form below appears (up to value) in
// some corpus/ program both compilers already agree on — nothing invented.
// ---------------------------------------------------------------------------
const INT_TYPES = ['i8', 'i16', 'i32', 'i64', 'u8', 'u16', 'u32', 'u64', 'usize'];
const FLOAT_TYPES = ['f32', 'f64'];
const SIGNED = new Set(['i8', 'i16', 'i32', 'i64']);
const INT_KIND = new Set(INT_TYPES);
const kindOf = (t) => (INT_KIND.has(t) ? 'int' : FLOAT_TYPES.includes(t) ? 'float' : 'bool');

// Big literals drawn only from proven typed initializers (corpus 041-052).
// They appear ONLY as the entire initializer of a typed let — never nested
// inside an expression (boot types untyped literal operators as i32 by
// default, and a u64-max literal has no i32 home; probe /tmp/probe3+4, boot
// check rc=0 for the whole grammar this file emits).
const BIG_LITS = {
  i8: ['(-128)', '127'],
  i16: ['32767', '(-32768)', '(-83)'],
  i32: ['(-2147483648)', '1024'],
  i64: ['9223372036854775807', '(-9223372036854775808)', '4294967301', '9007199254740993'],
  u8: ['255', '250', '200'],
  u16: ['65535', '65000', '32768'],
  u32: ['4294967295', '4000000000', '2147483648'],
  u64: ['18446744073709551615', '70000', '511'],
  isize: ['(-83)', '(-12)'],
  usize: ['18446744073709551615'],
};

// Proven float literal shapes (corpus 013/025/068).
const FLOAT_LITS = ['0.5', '1.5', '2.25', '2.5', '3.5', '0.1', '100.0', '123.456', '0.000025', '1e300', '5e-324'];

// Small literals only: in range for every integer type whatever it unifies
// to (i32 default or the anchored type).
function literalOf(t, rng) {
  if (SIGNED.has(t)) {
    const n = rng.int(-100, 100);
    return n < 0 ? `(${n})` : String(n);
  }
  return String(rng.int(0, 200));
}

// A leaf of type t. A NEGATIVE literal pins an all-untyped tree to the
// i32 default in boot (probed: `let x: i16 = ((-8) - 40)` rejects while
// `(5 - 3)` folds at the anchor) — so negative leaves always carry an
// explicit `as t` and positive ones stay untyped (they fold at whatever
// anchor or typed sibling surrounds them).
function literalLeaf(t, rng) {
  const lit = literalOf(t, rng);
  if (lit[0] === '(') return T(`(${lit.slice(1, -1)} as ${t})`);
  return U(lit);
}

// ---------------------------------------------------------------------------
// Program generation. ctx carries the rng, the live variable scope, the
// output lines, and a line budget the ask caps at 40.
// ---------------------------------------------------------------------------
const MAX_LINES = 40;

function newCtx(seed) {
  return {
    seed,
    rng: makeRng(seed),
    lines: [],
    budget: MAX_LINES,
    vars: [], // { name, type, kind: 'int'|'float'|'bool', mut }
    statics: [], // { name } — all i32
    helpers: [], // { name, params: [{name,type,kind}], ret: 'i32' }
    nameCnt: 0,
    printCnt: 0,
  };
}

function emit(ctx, line) {
  ctx.lines.push(line);
  ctx.budget -= 1;
  if (ctx.lines.length > MAX_LINES) throw new Error(`seed ${ctx.seed}: line budget overrun`);
}

const freshName = (ctx) => `t${ctx.nameCnt++}`;
const printTag = (ctx) => `p${ctx.printCnt++}`;

// A variable of exactly type t from the live scope, or a (small) literal.
// Float literals here are BARE only: `(0.0 - x)` paren-neg forms are typed
// f64 by the inner `-` (probe4/seed 9) and may not land in f32 space.
// Call-arg literals stay non-negative: a negative literal's fold at a
// callee param is an unproven shape (the let-anchor law does not reach
// here).
function varOrLit(ctx, t, kind, rng) {
  const vs = ctx.vars.filter((v) => v.type === t);
  if (vs.length && rng.chance(0.6)) return rng.pick(vs).name;
  return kind === 'float' ? rng.pick(FLOAT_LITS) : String(rng.int(0, 100));
}

// Expression nodes carry `typed`: whether the rho unification has an anchor
// (a variable, a cast) inside. Untyped-literal op untyped-literal collapses
// to the i32/f64 default — an error under a narrower expected type (seed 10:
// `((-83) * (-82))` under i16) — so whenever both operands would be untyped,
// one gets an explicit `as` anchor. `leaf` marks a bare literal: boot's
// anchor reaches exactly one node deep (the init's top node and its literal
// children — probed: `(5 - 3)` folds at i8 while `((5 - 3) + 1)` is "value
// is i32"), so an untyped NON-leaf child must carry `as t`.
const U = (code) => ({ code, typed: false, leaf: true });
const T = (code) => ({ code, typed: true, leaf: false });
const castAs = (n, t) => T(`(${n.code} as ${t})`);
// an untyped subtree that is not a leaf collapses to the i32 default —
// anchor it before it poisons a binop
const anchorDeep = (n, t) => (!n.typed && !n.leaf ? castAs(n, t) : n);

function genIntExpr(ctx, t, depth) {
  const rng = ctx.rng;
  if (depth <= 0 || rng.chance(0.35)) {
    const vs = ctx.vars.filter((v) => v.type === t);
    if (vs.length && rng.chance(0.6)) return T(rng.pick(vs).name);
    return literalLeaf(t, rng);
  }
  const a = anchorDeep(genIntExpr(ctx, t, depth - 1), t);
  const roll = rng.int(1, 100);
  if (roll <= 44) {
    const op = rng.pick(['+', '-', '*', '&', '|', '^', '+', '-', '*']);
    const b = anchorDeep(genIntExpr(ctx, t, depth - 1), t);
    // boot's fold law (probed): an all-untyped tree of positive
    // literals folds AT the let anchor — but exactly one node deep, so
    // an untyped non-leaf child is anchored (anchorDeep) before it
    // poisons the binop into the i32 default. A typed sibling simply
    // wins the width for its untyped leaf neighbors.
    return { code: `(${a.code} ${op} ${b.code})`, typed: a.typed || b.typed };
  }
  if (roll <= 54) {
    // div/rem may hit a zero divisor: a defined panic (exit 101, corpus 043)
    // or a wasm trap — both are behavior both compilers must reproduce.
    const op = rng.pick(['/', '%']);
    const b = anchorDeep(genIntExpr(ctx, t, depth - 1), t);
    return { code: `(${a.code} ${op} ${b.code})`, typed: a.typed || b.typed };
  }
  if (roll <= 70) {
    // shift amounts are literals (proven shapes, corpus 045); the masking
    // law makes amounts past the operand width differential-relevant.
    // The count masks by the OPERAND's width on both compilers — but an
    // untyped lhs folds at the let anchor in boot while the mirror's
    // lane folds at the i32 default, so counts stay under 32 there
    // (under 8 for a narrow anchor) where both masks agree.
    const op = rng.pick(['<<', '>>']);
    const narrow = t === 'i8' || t === 'u8' || t === 'i16' || t === 'u16';
    const amt = !a.typed ? (narrow ? rng.int(0, 7) : rng.int(0, 31)) : rng.int(0, 70);
    return { code: `(${a.code} ${op} ${amt})`, typed: a.typed };
  }
  if (roll <= 78) {
    // `(0 - lit)` defaults to i32 and will not unify with an unsigned
    // expected type (probe6) — anchor the child when it is untyped
    const inner = a.typed ? a : castAs(a, t);
    return { code: `(0 - ${inner.code})`, typed: true };
  }
  if (roll <= 84) {
    const inner = a.typed ? a : castAs(a, t);
    return { code: `(~${inner.code})`, typed: true };
  }
  // cast INTO t from another int type — the subexpression keeps type t. The
  // operand must be anchored: a bare literal under `as` folds ill-typed
  // (corpus/053 pins the defect), so an untyped child degrades to a plain
  // in-range literal of t instead of a cast.
  const s = rng.pick(INT_TYPES.filter((x) => x !== t));
  const child = genIntExpr(ctx, s, depth - 1);
  if (!child.typed) return literalLeaf(t, rng);
  return T(`(${child.code} as ${t})`);
}

function genFloatExpr(ctx, t, depth) {
  const rng = ctx.rng;
  if (depth <= 0 || rng.chance(0.4)) {
    const vs = ctx.vars.filter((v) => v.type === t);
    if (vs.length && rng.chance(0.6)) return T(rng.pick(vs).name);
    // the paren-neg shape is typed f64 — emit it only in f64 position
    if (t === 'f64' && rng.chance(0.25)) return T(`(0.0 - ${rng.pick(FLOAT_LITS)})`);
    return U(rng.pick(FLOAT_LITS));
  }
  const a = genFloatExpr(ctx, t, depth - 1);
  const roll = rng.int(1, 100);
  if (roll <= 70) {
    // no castAs anchor here: `(0.5 as f32)` is exactly the bare-literal cast
    // that folds ill-typed (corpus/053). Two untyped literals need no anchor —
    // the let's expected type unifies them (probed: `let x: f32 =
    // 123.456 * 0.5;` and the f32-var variant both check and run green).
    // BUT only single-level: an untyped-binop child collapses to the f64
    // default and cannot meet an f32 anchor (seeds 18/133: `((1e300 + 2.25)
    // * 123.456)` under f32) — in f32 space such children degrade to leaves.
    const op = rng.pick(['+', '-', '*', '/', '+', '-', '*']);
    const f32Leaf = () => {
      const vs = ctx.vars.filter((v) => v.type === 'f32');
      if (vs.length && rng.chance(0.6)) return T(rng.pick(vs).name);
      return U(rng.pick(FLOAT_LITS));
    };
    const fix = (n) => (t === 'f32' && n.bin && !n.typed ? f32Leaf() : n);
    const b = genFloatExpr(ctx, t, depth - 1);
    const a2 = fix(a);
    const b2 = fix(b);
    return { code: `(${a2.code} ${op} ${b2.code})`, typed: a2.typed || b2.typed, bin: true };
  }
  // cast an integer expression into float space (corpus 054) — the operand
  // must be anchored (a bare literal under `as` folds ill-typed), so an
  // untyped child degrades to a plain float literal of the target space
  const s = rng.pick(INT_TYPES);
  const child = genIntExpr(ctx, s, depth - 1);
  if (!child.typed) return U(rng.pick(FLOAT_LITS));
  return T(`(${child.code} as ${t})`);
}

function genBoolExpr(ctx, depth) {
  const rng = ctx.rng;
  if (depth <= 0 || rng.chance(0.3)) {
    const bs = ctx.vars.filter((v) => v.kind === 'bool');
    if (bs.length && rng.chance(0.4)) return rng.pick(bs).name;
    return rng.pick(['true', 'false']);
  }
  const roll = rng.int(1, 100);
  if (roll <= 55) {
    // comparison of two same-type numeric expressions; all literals here are
    // small, so an all-untyped pair collapsing to the i32/f64 default is
    // still in range and still a legal comparison (and a typed sibling
    // simply pins the width for its untyped neighbors — probed law)
    const cmp = rng.pick(['==', '!=', '<', '<=', '>', '>=']);
    if (rng.chance(0.75)) {
      const t = rng.pick(INT_TYPES);
      const a = genIntExpr(ctx, t, depth - 1);
      return `(${a.code} ${cmp} ${genIntExpr(ctx, t, depth - 1).code})`;
    }
    const t = rng.pick(FLOAT_TYPES);
    const a = genFloatExpr(ctx, t, depth - 1);
    return `(${a.code} ${cmp} ${genFloatExpr(ctx, t, depth - 1).code})`;
  }
  if (roll <= 80) {
    const op = rng.pick(['&&', '||']);
    return `(${genBoolExpr(ctx, depth - 1)} ${op} ${genBoolExpr(ctx, depth - 1)})`;
  }
  return `(!${genBoolExpr(ctx, depth - 1)})`;
}

// The initializer expression for a let of type t — always type-correct.
// Narrow int types occasionally take a BIG literal as the WHOLE initializer
// (the corpus-proven anchoring context — never inside a larger expression).
function genInit(ctx, t, depth) {
  const kind = kindOf(t);
  if (kind === 'int' && ctx.rng.chance(0.15)) return literalBig(t, ctx.rng);
  if (kind === 'int') return genIntExpr(ctx, t, depth).code;
  if (kind === 'float') return genFloatExpr(ctx, t, depth).code;
  return genBoolExpr(ctx, depth);
}
const literalBig = (t, rng) => rng.pick(BIG_LITS[t]);

// Declare a fresh var and print it (the ask: print the intermediate values).
// Scoped to the current block; block emitters pop scoped vars so no use ever
// escapes its braces. Bools are immutable (that is the corpus-proven form);
// numerics may be mut.
function letStmt(ctx, forcedType) {
  const rng = ctx.rng;
  const t =
    forcedType ??
    (rng.chance(0.6) ? rng.pick(INT_TYPES) : rng.chance(0.5) ? rng.pick(FLOAT_TYPES) : 'bool');
  const kind = kindOf(t);
  const name = freshName(ctx);
  const mut = kind !== 'bool' && rng.chance(0.6);
  const expr = genInit(ctx, t, rng.int(0, 2));
  emit(ctx, `  let ${mut ? 'mut ' : ''}${name}: ${t} = ${expr};`);
  emit(ctx, `  printf("${name}={}\\n", ${name});`);
  ctx.vars.push({ name, type: t, kind, mut });
}

// Arbitrary-width cast as its own statement (corpus 048 shape:
// `let up: i64 = rt as i64;`), float->int included (corpus 053). The source
// expression comes straight from the typed generators — no BIG literals
// here, only init positions take those.
//
// Operand law (probed against boot, wasmtime as validator, 2026-09-23):
// int-valued operands of every shape are safe under any target; float-fold
// constants and runtime float expressions are accepted but yield an INVALID
// module under a float target (`(0.0 - 0.5) as f64`, `(a * 2.25) as f32` —
// the corpus/053 bare-literal fold defect reaching past the literal), and
// only a bare typed float VARIABLE is proven there (p-81/82). So float->float
// always casts a fresh anchor local; untyped sources of any kind anchor too
// (the corpus's own documented workaround); int sources and float->int cast
// their expression directly.
function castStmt(ctx) {
  const rng = ctx.rng;
  const fromFloat = rng.chance(0.25);
  const from = rng.pick(fromFloat ? FLOAT_TYPES : INT_TYPES);
  const pool = rng.chance(0.7) ? INT_TYPES : FLOAT_TYPES;
  const to = rng.pick(pool.filter((x) => x !== from));
  const name = freshName(ctx);
  const anchor = () => {
    const anc = freshName(ctx);
    const node =
      kindOf(from) === 'int' ? genIntExpr(ctx, from, rng.int(0, 1)) : genFloatExpr(ctx, from, rng.int(0, 1));
    emit(ctx, `  let ${anc}: ${from} = ${node.code};`);
    return anc;
  };
  if (kindOf(from) === 'float' && kindOf(to) === 'float') {
    const anc = anchor();
    emit(ctx, `  let mut ${name}: ${to} = ${anc} as ${to};`);
  } else {
    const node =
      kindOf(from) === 'int' ? genIntExpr(ctx, from, rng.int(0, 1)) : genFloatExpr(ctx, from, rng.int(0, 1));
    if (node.typed) emit(ctx, `  let mut ${name}: ${to} = ${node.code} as ${to};`);
    else {
      const anc = freshName(ctx);
      emit(ctx, `  let ${anc}: ${from} = ${node.code};`);
      emit(ctx, `  let mut ${name}: ${to} = ${anc} as ${to};`);
    }
  }
  emit(ctx, `  printf("${name}={}\\n", ${name});`);
  ctx.vars.push({ name, type: to, kind: kindOf(to), mut: true });
}

function mutateStmt(ctx, exclude) {
  const rng = ctx.rng;
  const vs = ctx.vars.filter((v) => v.mut && v.name !== exclude);
  if (!vs.length) return letStmt(ctx);
  const v = rng.pick(vs);
  const expr = genInit(ctx, v.type, rng.int(0, 2));
  const op = rng.pick(['+=', '-=', '=', '+=', '-=']);
  emit(ctx, `  ${v.name} ${op} ${expr};`);
  emit(ctx, `  printf("${v.name}={}\\n", ${v.name});`);
}

function printMoreStmt(ctx) {
  const rng = ctx.rng;
  const start = rng.int(0, Math.max(0, ctx.vars.length - 1));
  const vs = ctx.vars.slice(start).slice(0, 3);
  if (!vs.length) return letStmt(ctx);
  emit(ctx, `  printf("${printTag(ctx)} ${vs.map(() => '{}').join(' ')}\\n", ${vs.map((v) => v.name).join(', ')});`);
}

function staticStmt(ctx) {
  if (!ctx.statics.length) return letStmt(ctx);
  const rng = ctx.rng;
  const s = rng.pick(ctx.statics);
  emit(ctx, `  ${s.name} ${rng.pick(['+=', '-=', '='])} ${genIntExpr(ctx, 'i32', rng.int(0, 1)).code};`);
  emit(ctx, `  printf("${s.name}={}\\n", ${s.name});`);
}

function ifStmt(ctx, exclude) {
  const rng = ctx.rng;
  emit(ctx, `  if (${genBoolExpr(ctx, rng.int(1, 2))}) {`);
  simpleBlock(ctx, exclude, rng.int(1, 2), '    ');
  emit(ctx, `  } else {`);
  simpleBlock(ctx, exclude, rng.int(1, 2), '    ');
  emit(ctx, `  }`);
}

function whileStmt(ctx, exclude) {
  const rng = ctx.rng;
  const c = freshName(ctx);
  const k = rng.int(2, 12);
  emit(ctx, `  let mut ${c}: i32 = ${rng.int(0, 3)};`);
  emit(ctx, `  while (${c} < ${k}) {`);
  const vmark = ctx.vars.length;
  ctx.vars.push({ name: c, type: 'i32', kind: 'int', mut: true });
  // the counter advances first: progress is guaranteed no matter what the
  // rest of the body does (break/continue included)
  emit(ctx, `    ${c} += 1;`);
  simpleBlock(ctx, c, rng.int(1, 2), '    ');
  if (rng.chance(0.4)) {
    emit(ctx, `    if (${genBoolExpr(ctx, 1)}) {`);
    simpleBlock(ctx, c, 1, '      ');
    emit(ctx, `    }`);
  }
  ctx.vars.length = vmark;
  emit(ctx, `  }`);
  emit(ctx, `  printf("${c}={}\\n", ${c});`);
}

// One level of simple statements inside a block (no nested control flow:
// keeps the line accounting and the scoping stack trivial). `indent` is the
// block's own indent depth; flat statement lines are emitted at two spaces
// and re-indented.
function simpleBlock(ctx, exclude, n, indent) {
  const rng = ctx.rng;
  const vmark = ctx.vars.length;
  const lmark = ctx.lines.length;
  for (let i = 0; i < n && ctx.budget > 2; i++) {
    const roll = rng.int(1, 100);
    if (roll <= 40) letStmt(ctx);
    else if (roll <= 65) mutateStmt(ctx, exclude);
    else if (roll <= 80) printMoreStmt(ctx);
    else if (roll <= 90 && ctx.statics.length) staticStmt(ctx);
    else letStmt(ctx, rng.pick(FLOAT_TYPES));
  }
  for (let i = lmark; i < ctx.lines.length; i++) {
    ctx.lines[i] = indent + ctx.lines[i].slice(2);
  }
  ctx.vars.length = vmark;
}

// call a generated helper and print the result (main only)
function callStmt(ctx) {
  const rng = ctx.rng;
  if (!ctx.helpers.length) return letStmt(ctx);
  const h = rng.pick(ctx.helpers);
  const args = h.params.map((p) => varOrLit(ctx, p.type, p.kind, rng));
  const name = freshName(ctx);
  emit(ctx, `  let ${name}: i32 = ${h.name}(${args.join(', ')});`);
  emit(ctx, `  printf("${name}={}\\n", ${name});`);
  ctx.vars.push({ name, type: 'i32', kind: 'int', mut: false });
}

// A helper: typed params, a couple of statements, an i32 return. Params are
// never assigned (param mutation is not a corpus-proven form) — each gets
// printf'd, which is both a use and an observable.
function genHelper(ctx, idx) {
  const rng = ctx.rng;
  const nparams = rng.int(1, 2);
  const params = [];
  for (let i = 0; i < nparams; i++) {
    const t = rng.pick(rng.chance(0.7) ? INT_TYPES : FLOAT_TYPES);
    params.push({ name: `a${i}`, type: t, kind: kindOf(t) });
  }
  const name = `h${idx}`;
  const sig = params.map((p) => `${p.name}: ${p.type}`).join(', ');
  emit(ctx, `fn ${name}(${sig}) -> i32 {`);
  const vmark = ctx.vars.length;
  for (const p of params) {
    ctx.vars.push({ name: p.name, type: p.type, kind: p.kind, mut: false });
    emit(ctx, `  printf("${name}.${p.name}={}\\n", ${p.name});`);
  }
  const stmts = rng.int(1, 3);
  for (let i = 0; i < stmts && ctx.budget > 6; i++) {
    if (rng.chance(0.5)) letStmt(ctx);
    else printMoreStmt(ctx);
  }
  emit(ctx, `  return ${genIntExpr(ctx, 'i32', rng.int(0, 1)).code};`);
  emit(ctx, `}`);
  ctx.helpers.push({ name, params, ret: 'i32' });
  ctx.vars.length = vmark;
}

// literal match in a helper (corpus 070 shape: `return match n { … };`)
function genMatchHelper(ctx, idx) {
  const rng = ctx.rng;
  const name = `h${idx}`;
  emit(ctx, `fn ${name}(n: i32) -> i32 {`);
  emit(ctx, `  return match n {`);
  const arms = [0, 1, 2, 7].filter(() => rng.chance(0.8));
  if (!arms.length) arms.push(0);
  for (const a of arms) emit(ctx, `    ${a} => ${rng.int(0, 99)},`);
  emit(ctx, `    _ => ${rng.int(0, 99)},`);
  emit(ctx, `  };`);
  emit(ctx, `}`);
  ctx.helpers.push({ name, params: [{ name: 'n', type: 'i32', kind: 'int' }], ret: 'i32' });
}

// worst-case line cost per main-level statement kind, for the budget guard
// (if: brace lines 3 + two bodies of up to 2 stmts × 2 lines = 11;
// while: let+while+inc+close+printf 5 + body 2×2 + optional break-if 4 = 13)
const NEED = { let: 2, cast: 3, mutate: 2, more: 1, call: 2, static: 2, if: 11, while: 13 };

function genProgram(seed) {
  const ctx = newCtx(seed);
  const rng = ctx.rng;
  emit(ctx, `// fuzz seed ${seed} — generated by tools/fuzz/gen.mjs (do not edit)`);
  // optional static (i32, corpus 059 shape)
  if (rng.chance(0.4)) {
    emit(ctx, `static mut S0: i32 = ${rng.int(0, 50)};`);
    ctx.statics.push({ name: 'S0' });
  }
  // helpers first (main may call them); their budget share is bounded so
  // main always has room — the differential meat lives in main's printf trail
  if (rng.chance(0.5)) genHelper(ctx, ctx.helpers.length);
  if (rng.chance(0.25)) genMatchHelper(ctx, ctx.helpers.length);
  emit(ctx, `fn main() -> i32 {`);
  const want = rng.int(5, 12);
  for (let i = 0; i < want; i++) {
    const roll = rng.int(1, 100);
    const pick =
      roll <= 26 ? 'let'
      : roll <= 36 ? 'cast'
      : roll <= 50 ? 'mutate'
      : roll <= 58 ? 'more'
      : roll <= 70 ? 'if'
      : roll <= 84 ? 'while'
      : roll <= 94 ? 'call'
      : 'static';
    // +2 reserves the closing `return 0;` and `}` — never squeezed out
    if (ctx.budget < NEED[pick] + 2) break;
    if (pick === 'let') letStmt(ctx);
    else if (pick === 'cast') castStmt(ctx);
    else if (pick === 'mutate') mutateStmt(ctx);
    else if (pick === 'more') printMoreStmt(ctx);
    else if (pick === 'if') ifStmt(ctx);
    else if (pick === 'while') whileStmt(ctx);
    else if (pick === 'call') callStmt(ctx);
    else staticStmt(ctx);
  }
  emit(ctx, `  return 0;`);
  emit(ctx, `}`);
  const text = ctx.lines.join('\n') + '\n';
  if (ctx.lines.length > MAX_LINES) throw new Error(`seed ${seed}: ${ctx.lines.length} lines`);
  return text;
}

// ---------------------------------------------------------------------------
// The run harness. Every rho/wasmtime invocation goes through the same
// perl-alarm wrapper tools/gate.sh uses (macOS has no GNU timeout; the alarm
// survives exec, and an unresolvable binary must die nonzero, never fall
// through silently).
// ---------------------------------------------------------------------------
const WASMTIME_ARGS = ['run', '--dir', '.'];

function runT(sec, cmd, args) {
  return new Promise((done) => {
    const t0 = Date.now();
    const child = spawn(
      'perl',
      ['-e', 'alarm shift; exec @ARGV or die "gen.mjs: cannot exec: $!\\n"', String(sec), cmd, ...args],
      { cwd: ROOT },
    );
    let out = '';
    let err = '';
    child.stdout.on('data', (d) => (out += d));
    child.stderr.on('data', (d) => (err += d));
    const watchdog = setTimeout(() => {
      try { child.kill('SIGKILL'); } catch { /* already gone */ }
    }, sec * 1000 + 3000);
    child.on('close', (code, signal) => {
      clearTimeout(watchdog);
      done({ code, signal, out, err, timedOut: signal === 'SIGALRM', ms: Date.now() - t0 });
    });
  });
}

// tools/gate.sh's strip_dbg, in JS: drop the scratch HEAP@/WE lines before a
// text compare. Nothing else is filtered.
const stripDbgLines = (text) => {
  const ls = text.split('\n').filter((l) => !l.startsWith('HEAP@') && !l.startsWith('WE '));
  while (ls.length && ls[ls.length - 1] === '') ls.pop();
  return ls;
};
const stripDbgStr = (text) => stripDbgLines(text).join('\n');
const tail = (s, n = 240) => {
  if (!s) return '';
  const one = s.replace(/\n+/g, ' | ').trim();
  return one.length > n ? one.slice(-n) : one;
};

// One differential round for a seed. Every step draws from the program's own
// 10s budget AND the campaign's remaining budget, whichever is smaller; if
// the program's budget runs dry mid-way, the seed is recorded as a
// timeout diff and its program survives on disk.
//
// rc conventions (the corpus differential's own): rc 1 = a clean refusal
// (diagnostics on stderr — a grammar overreach, not a behavior); rc 101 =
// the program's defined panic; anything else = the program ran and exited.
// The generated mains always `return 0`, so a 1-vs-accepted split is a real
// refusal divergence and a >1-vs->1 split must agree on the code.
async function diffSeed(seed, deadlineMs, stepSec) {
  const srcRel = `build/gate/fuzz/${seed}.rho`;
  const srcAbs = join(FUZZ_DIR, `${seed}.rho`);
  const mirrorWasm = `build/gate/fuzz/${seed}.c.wasm`;
  const progWat = `build/gate/fuzz/${seed}.prog.wat`;
  const selfWasm = `build/gate/fuzz/${seed}.self.wasm`;
  const program = genProgram(seed);
  writeFileSync(srcAbs, program);

  const left = () => deadlineMs - Date.now();
  const cap = () => Math.max(1, Math.min(stepSec, Math.ceil(left() / 1000)));

  // 1 — boot: compile + run in one verb (the corpus differential's face)
  let r = await runT(cap(), process.env.RHO || './build/rho', ['run', srcRel]);
  if (r.timedOut) return { status: 'diff', kind: 'timeout:boot-run', seed };
  if (r.code < 0) return { status: 'diff', kind: `boot signal ${r.code}`, seed };
  const brc = r.code;
  const bout = stripDbgLines(r.out);
  if (left() < 1500) return { status: 'diff', kind: 'timeout:budget@post-boot-run', seed };

  // 2 — the self-hosted chain: bake a fresh compiler with this program as
  // its frozen input (MODS empty — the generated programs are self-contained)
  r = await runT(cap(), process.env.RHO || './build/rho', [
    'build', 'libs/compiler/main.rho', '-o', mirrorWasm,
    '--set', `SRC=${program}`, '--set', 'MODS=',
  ]);
  if (r.timedOut) return { status: 'diff', kind: 'timeout:mirror-bake', seed };
  if (r.code !== 0 || !existsSync(join(ROOT, mirrorWasm))) {
    return { status: 'diff', kind: 'bake:mirror', seed, note: tail(stripDbgStr(r.err)) };
  }
  if (left() < 1500) return { status: 'diff', kind: 'timeout:budget@post-mirror-bake', seed };

  // 3 — the baked mirror compiles the program (WAT on stdout)
  r = await runT(cap(), 'wasmtime', [...WASMTIME_ARGS, mirrorWasm]);
  if (r.timedOut) return { status: 'diff', kind: 'timeout:mirror-run', seed };
  const srcRc = r.code;
  if (srcRc === 1) {
    // a clean refusal from the self-hosted compiler
    if (brc === 1) return { status: 'both-reject', seed, note: tail(stripDbgStr(r.err)) };
    return { status: 'diff', kind: 'refuse:mirror-only', seed, note: tail(stripDbgStr(r.err)) };
  }
  if (srcRc > 1) {
    // the self-hosted compiler itself died — worse than any behavioral diff
    return { status: 'panic', seed, side: 'mirror', note: tail(stripDbgStr(r.err)) };
  }
  if (brc === 1) {
    return { status: 'diff', kind: 'refuse:boot-only', seed, note: tail(stripDbgStr(r.err)) };
  }
  if (!r.out.trim()) return { status: 'diff', kind: 'emit:mirror-empty', seed };
  writeFileSync(join(ROOT, progWat), r.out);

  // 4 — assemble and run the self-hosted program artifact
  r = await runT(cap(), 'wat2wasm', [progWat, '-o', selfWasm]);
  if (r.timedOut || r.code !== 0) {
    return { status: 'panic', seed, side: 'emitter', note: tail(stripDbgStr(r.err)) };
  }
  const m = await runT(cap(), 'wasmtime', [...WASMTIME_ARGS, selfWasm]);
  if (m.timedOut) return { status: 'diff', kind: 'timeout:self-run', seed };

  const mout = stripDbgLines(m.out);
  if (brc !== m.code) {
    return { status: 'diff', kind: `rc ${brc} vs ${m.code}`, seed, boot: bout, mirror: mout };
  }
  if (bout.join('\n') !== mout.join('\n')) {
    const at = bout.findIndex((l, i) => l !== mout[i]);
    return {
      status: 'diff',
      kind: `stdout@line ${at + 1}`,
      seed,
      boot: bout.slice(Math.max(0, at - 1), at + 2),
      mirror: mout.slice(Math.max(0, at - 1), at + 2),
    };
  }
  return { status: 'pass', seed };
}

// ---------------------------------------------------------------------------
// CLI + campaign loop
// ---------------------------------------------------------------------------
function parseArgs(argv) {
  const o = { emit: null, from: 1, to: 150, budgetSec: 15 * 60, stepSec: 10 };
  for (let i = 0; i < argv.length; i++) {
    if (argv[i] === '--emit') o.emit = Number(argv[++i]);
    else if (argv[i] === '--from') o.from = Number(argv[++i]);
    else if (argv[i] === '--to') o.to = Number(argv[++i]);
    else if (argv[i] === '--budget') o.budgetSec = Number(argv[++i]);
    else if (argv[i] === '--step') o.stepSec = Number(argv[++i]);
  }
  return o;
}

const args = parseArgs(process.argv.slice(2));

if (args.emit != null) {
  process.stdout.write(genProgram(args.emit));
} else {
  mkdirSync(FUZZ_DIR, { recursive: true });
  const t0 = Date.now();
  const deadline = t0 + args.budgetSec * 1000;
  let pass = 0;
  const diffs = [];
  const panics = [];
  let bothReject = 0;
  let done = 0;
  for (let seed = args.from; seed <= args.to; seed++) {
    if (Date.now() >= deadline) {
      console.log(`budget ${args.budgetSec}s reached — stopping after ${done} programs`);
      break;
    }
    const r = await diffSeed(seed, deadline, args.stepSec);
    done += 1;
    if (r.status === 'pass') {
      pass += 1;
      console.log(`seed ${seed} PASS`);
    } else if (r.status === 'panic') {
      // a compiler-side death (the baked mirror traps or its WAT fails to
      // assemble): the robustness bar is clean refusal or clean compile
      panics.push(r);
      copyFileSync(join(FUZZ_DIR, `${seed}.rho`), join(GATE, `fuzz_panic_${seed}.rho`));
      console.log(`seed ${seed} PANIC (${r.side}) ${r.note ?? ''}`);
    } else if (r.status === 'both-reject') {
      bothReject += 1;
      console.log(`seed ${seed} BOTH-REJECT ${r.note}`);
    } else {
      diffs.push(r);
      copyFileSync(join(FUZZ_DIR, `${seed}.rho`), join(GATE, `fuzz_fail_${seed}.rho`));
      console.log(`seed ${seed} DIFF ${r.kind}${r.note ? ` ${r.note}` : ''}`);
      if (r.boot) console.log(`  boot  : ${r.boot.join(' | ').slice(0, 200)}`);
      if (r.mirror) console.log(`  mirror: ${r.mirror.join(' | ').slice(0, 200)}`);
    }
  }
  const secs = ((Date.now() - t0) / 1000).toFixed(1);
  console.log('== fuzz campaign ==');
  console.log(`programs compared : ${done} (seeds ${args.from}..${args.to})`);
  console.log(`identical behavior: ${pass}`);
  console.log(`both reject       : ${bothReject}${bothReject ? '  (generator overreached the proven grammar — investigate)' : ''}`);
  console.log(`compiler panics   : ${panics.length}${panics.length ? ' -> ' + panics.map((p) => `seed ${p.seed} (${p.side})`).join(', ') : ''}`);
  console.log(`differences       : ${diffs.length}${diffs.length ? ' -> ' + diffs.map((d) => `seed ${d.seed} (${d.kind})`).join(', ') : ''}`);
  if (diffs.length) {
    console.log(`failing programs  : ${diffs.map((d) => `build/gate/fuzz_fail_${d.seed}.rho`).join(', ')}`);
  }
  console.log(`wall clock        : ${secs}s of ${args.budgetSec}s budget`);
  process.exitCode = diffs.length || panics.length ? 1 : 0;
}
