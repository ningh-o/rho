// subset.js — the corpus programs this generation's fmt chain
// canonicalizes byte-identically to `build/rho fmt` (measured
// 2026-09-27; the other 74 corpus files ride the mirror's
// parse-retention gaps — owned by T2.2+, ledgered in
// WORKTREE-NOTES.md). Each path is relative to the worktree root.
export const worktreeRoot = new URL("../../..", import.meta.url).pathname;

export const corpusSubset = [
  "001_hello",
  "002_arith",
  "003_control",
  "004_branches",
  "007_slices",
  "008_strings",
  "010_statics",
  "011_defer",
  "012_recursion",
  "015_panic_oob",
  "016_panic_div",
  "026_short_circuit_index",
  "031_multiline",
  "043_panic_div_zero",
  "044_panic_rem_zero",
  "048_cast_widths",
  "059_short_circuit",
  "066_escapes_text",
  "074_defer_order",
  "075_defer_loops",
  "079_loops_break_continue",
  "094_panic_oob",
  "096_statics_int",
  "100_empty_slice",
  "103_static_str",
  "n03_escapes_basic",
  "n05_cat_nest",
  "n06_cat_empty",
  "n07_cat_helpers",
  "n08_cat_chain",
  "n09_printf_edges",
  "n10_format_edges",
  "n11_str_slice_edges",
  "t10_verbatim",
  "t11_compound_bitwise",
  "t12_precedence",
].map((n) => `corpus/${n}.rho`);

// the fmt parity fixtures the repo already pins (tests/run-fmt-self.sh)
export const fmtSelfFixtures = ["data", "flow", "strings", "strings2"].map(
  (n) => `tests/fmt-self/${n}.rho`,
);
