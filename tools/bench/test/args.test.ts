// Pins: the command-line surface of docs/ecosystem.md section 1's
// harness — "fixed iteration counts" (the defaults), the default-off
// chain flag, and honest refusals (exit 2 semantics; every invalid
// input is an error, never a silent fallback). --help must name every
// flag the harness speaks.

import test from "node:test";
import assert from "node:assert/strict";

import { DEFAULTS, HELP, parseArgs } from "../src/args.ts";

test("args: the defaults are the fixed iteration counts and caps", () => {
  assert.deepEqual(DEFAULTS, {
    help: false,
    verbose: false,
    compile: true,
    exec: true,
    chain: false, // the chain leg stays OFF until T3.1 leg 4 is green
    iters: 5,
    execIters: 5,
    chainIters: 1,
    capMs: 30000,
    chainCapMs: 600000,
    filter: "",
  });
});

test("args: no flags parses to the defaults", () => {
  const r = parseArgs([]);
  assert.equal(r.error, null);
  assert.deepEqual(r.args, DEFAULTS);
});

test("args: --chain turns the chain leg on without touching anything else", () => {
  const r = parseArgs(["--chain"]);
  assert.equal(r.error, null);
  assert.equal(r.args.chain, true);
  assert.deepEqual({ ...r.args, chain: false }, DEFAULTS);
});

test("args: --no-compile / --no-exec turn their legs off", () => {
  const a = parseArgs(["--no-compile"]);
  assert.equal(a.args.compile, false);
  assert.equal(a.args.exec, true);
  const b = parseArgs(["--no-exec"]);
  assert.equal(b.args.exec, false);
  assert.equal(b.args.compile, true);
});

test("args: the sampling and cap flags parse", () => {
  const r = parseArgs([
    "--iters",
    "3",
    "--exec-iters",
    "7",
    "--chain-iters",
    "2",
    "--cap-ms",
    "5000",
    "--chain-cap-ms",
    "120000",
    "--filter",
    "0",
  ]);
  assert.equal(r.error, null);
  assert.equal(r.args.iters, 3);
  assert.equal(r.args.execIters, 7);
  assert.equal(r.args.chainIters, 2);
  assert.equal(r.args.capMs, 5000);
  assert.equal(r.args.chainCapMs, 120000);
  assert.equal(r.args.filter, "0");
});

test("args: --help sets the help bit", () => {
  const r = parseArgs(["--help"]);
  assert.equal(r.error, null);
  assert.equal(r.args.help, true);
});

test("args: zero iterations is refused — fixed counts are at least one", () => {
  assert.match(parseArgs(["--iters", "0"]).error ?? "", />= 1/);
  assert.match(parseArgs(["--exec-iters", "0"]).error ?? "", />= 1/);
  assert.match(parseArgs(["--chain-iters", "0"]).error ?? "", />= 1/);
});

test("args: zero caps are refused — the time-cap law wants real caps", () => {
  assert.match(parseArgs(["--cap-ms", "0"]).error ?? "", />= 1/);
  assert.match(parseArgs(["--chain-cap-ms", "0"]).error ?? "", />= 1/);
});

test("args: non-numeric iteration values are refused", () => {
  assert.match(parseArgs(["--iters", "five"]).error ?? "", /integer/);
  assert.match(parseArgs(["--iters", "-1"]).error ?? "", /integer/);
  assert.match(parseArgs(["--iters", "2.5"]).error ?? "", /integer/);
});

test("args: a missing value is refused, not silently defaulted", () => {
  assert.match(parseArgs(["--iters"]).error ?? "", /missing value/);
  assert.match(parseArgs(["--filter"]).error ?? "", /missing value/);
});

test("args: an unknown flag is refused", () => {
  assert.match(parseArgs(["--iter"]).error ?? "", /unknown flag/);
  assert.match(parseArgs(["--junk"]).error ?? "", /unknown flag/);
  assert.match(parseArgs(["junk"]).error ?? "", /unknown flag/);
});

test("args: --help names every flag the harness speaks", () => {
  for (const flag of [
    "--compile",
    "--no-compile",
    "--exec",
    "--no-exec",
    "--chain",
    "--iters",
    "--exec-iters",
    "--chain-iters",
    "--cap-ms",
    "--chain-cap-ms",
    "--filter",
    "--verbose",
    "--help",
    "RHO=",
  ]) {
    assert.ok(HELP.includes(flag), `--help must mention ${flag}`);
  }
});

test("args: --help documents the chain leg's default-off and why", () => {
  assert.match(HELP, /Default: OFF/);
  assert.match(HELP, /T3\.1 leg 4/);
});

test("args: --help documents the determinism contract", () => {
  assert.match(HELP, /no timestamps/);
  assert.match(HELP, /no absolute paths/);
  assert.match(HELP, /40\.0\.0/);
  assert.match(HELP, /1\.0\.39/);
});
