// median.ts — the median of a fixed sample.
//
// Law: docs/ecosystem.md section 1 — "fixed iteration counts, medians
// printed". The reduction is deterministic by construction: the input
// is copied (never mutated), sorted numerically (never lexically —
// "10" < "2" lexically), and reduced by the standard definition: the
// middle element for an odd count, the mean of the two middle
// elements for an even count.

export function median(samples: readonly number[]): number {
  if (samples.length === 0) {
    throw new Error("median: empty sample (every leg samples at least once)");
  }
  const sorted = [...samples].sort((a, b) => a - b);
  const mid = Math.floor(sorted.length / 2);
  if (sorted.length % 2 === 1) {
    const v = sorted[mid];
    if (v === undefined) throw new Error("median: index out of range");
    return v;
  }
  const lo = sorted[mid - 1];
  const hi = sorted[mid];
  if (lo === undefined || hi === undefined) {
    throw new Error("median: index out of range");
  }
  return (lo + hi) / 2;
}
