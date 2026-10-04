// report.ts — the deterministic report renderer.
//
// Law: docs/ecosystem.md section 1 — "Same tree, same machine,
// byte-identical numbers (determinism is the language's law; the
// harness obeys it too)"; the ask operationalizes it as four
// properties: results sorted, fixed width, no timestamps, no
// machine-local absolute paths (design section 5 applies to the
// harness itself).
//
// How each property is met:
//   sorted     — rows are pre-sorted upstream in plain code-unit
//                order (never localeCompare); the renderer preserves
//                the given order and the tests pin the sort.
//   fixed      — the name column is the longest name in the round; a
//                time cell is always the same width within a leg, DNF
//                and ERR included.
//   timestamps — no wall-clock call exists on the report path; the
//                banner names versions, iteration counts, and caps.
//   abs paths  — the report carries program names only; tool output
//                never enters stdout (runner.ts), and the tests pin
//                the banned absolute-path prefixes.
//
// The measured time cells are the one thing that moves between runs —
// they are measurements, not furniture. The acceptance command in
// tools/bench/README.md diffs two runs with the time cells scrubbed.
//
// The report is pure ASCII (byte-diffable across environments).

export type RowStatus = "ok" | "dnf" | "err";

export interface Row {
  readonly name: string;
  readonly status: RowStatus;
  // Median milliseconds; meaningful when status === "ok".
  readonly ms: number;
  // The governing cap, printed on a cap-hit DNF row.
  readonly capMs: number;
  // The exit code, printed on an ERR row.
  readonly exitCode: number;
  // Why a row did not deliver its samples; "" on ok and err rows and
  // on cap-hit DNFs (whose cell already says "> cap ms"). Non-empty
  // only on a DNF that was not a cap hit — a stage starved by a dead
  // upstream stage — and the renderer prints it verbatim as the cell:
  // "> cap ms" on a row that never ran would misattribute a hang it
  // never had.
  readonly reason: string;
}

export interface LegReport {
  readonly title: string;
  readonly rows: readonly Row[];
  readonly skipped: boolean;
  // Present only when skipped.
  readonly skipReason: string;
}

export interface BenchReport {
  readonly banner: string;
  readonly legs: readonly LegReport[];
}

export function renderReport(report: BenchReport): string {
  const lines: string[] = [];
  lines.push(report.banner);
  for (const leg of report.legs) {
    if (leg.skipped) {
      lines.push(`== leg: ${leg.title} -- skipped: ${leg.skipReason}`);
      continue;
    }
    lines.push(`== leg: ${leg.title}`);
    const nameWidth = Math.max(0, ...leg.rows.map((r) => r.name.length));
    const cells = leg.rows.map((r) => timeCell(r));
    const cellWidth = Math.max(0, ...cells.map((c) => c.length));
    for (let i = 0; i < leg.rows.length; i++) {
      const row = leg.rows[i];
      const cell = cells[i];
      if (row === undefined || cell === undefined) continue;
      const status = (row.status.toUpperCase() + "   ").slice(0, 3);
      lines.push(
        `  ${row.name.padEnd(nameWidth)}  ${status}  ${cell.padStart(cellWidth)}`,
      );
    }
  }
  return lines.join("\n");
}

// The status column classifies; the cell carries the detail:
//   ok  -> the median milliseconds
//   dnf -> the governing cap as "> cap ms" when the cap was hit; a
//          row's reason text when the DNF was not a cap hit (a stage
//          starved by a dead upstream stage — it never ran, so no cap
//          was exceeded and "> cap ms" would misattribute)
//   err -> the exit code the tool refused with
function timeCell(row: Row): string {
  switch (row.status) {
    case "ok":
      return `${row.ms.toFixed(1)} ms`;
    case "dnf":
      return row.reason !== "" ? row.reason : `> ${row.capMs.toFixed(1)} ms`;
    case "err":
      return `exit ${row.exitCode}`;
  }
}
