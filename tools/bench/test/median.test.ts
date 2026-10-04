// Pins: docs/ecosystem.md section 1 — "fixed iteration counts, medians
// printed". The median is the report's only reduction, so its exact
// behavior is pinned here: numeric (never lexical) ordering, the
// standard odd/even definitions, input-order independence, input
// immutability, and the honest throw on an empty sample.

import test from "node:test";
import assert from "node:assert/strict";

import { median } from "../src/median.ts";

test("median: single sample is itself", () => {
  assert.equal(median([5]), 5);
  assert.equal(median([0]), 0);
  assert.equal(median([12.5]), 12.5);
});

test("median: odd count picks the middle element", () => {
  assert.equal(median([3, 1, 2]), 2);
  assert.equal(median([30, 10, 20, 50, 40]), 30);
});

test("median: even count is the mean of the two middle elements", () => {
  assert.equal(median([4, 1, 3, 2]), 2.5);
  assert.equal(median([10, 20]), 15);
});

test("median: sorts numerically, never lexically", () => {
  // Lexical order would put "10" before "2" and return 6 instead of 9.
  assert.equal(median([10, 2, 8, 1]), 5);
  assert.equal(median([2, 10]), 6);
});

test("median: independent of input order", () => {
  assert.equal(median([10, 2, 8]), median([2, 8, 10]));
  assert.equal(median([10, 2, 8]), 8);
});

test("median: does not mutate its input", () => {
  const input = [3, 1, 2];
  const before = [...input];
  median(input);
  assert.deepEqual(input, before);
});

test("median: empty sample throws — a leg never samples zero times", () => {
  assert.throws(() => median([]), /empty sample/);
});
