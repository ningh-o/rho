// plugin.test.js — the plugin surface through the REAL prettier API:
// language registration, filepath-based parser resolution, generation
// pinning, the honest failure modes (unparseable source, raw NUL,
// oversized input), the capacity boundary semantics, blank input, and
// the fixpoint gate's refusals of known-mangled shapes.
import { describe, expect, it } from "vitest";
import { join } from "node:path";
import { readFileSync } from "node:fs";
import prettier from "prettier";
import plugin from "../src/index.js";
import { formatRho, generation } from "../src/fmt.js";
import { worktreeRoot } from "./subset.js";

const format = (text, opts = {}) =>
  prettier.format(text, { parser: "rho", plugins: [plugin], ...opts });

describe("real prettier API", () => {
  it("registers the rho language", async () => {
    const info = await prettier.getSupportInfo({ plugins: [plugin] });
    const rho = info.languages.find((l) => l.name === "rho");
    expect(rho).toBeDefined();
    expect(rho.parsers).toContain("rho");
    expect(rho.extensions).toContain(".rho");
  });

  it("resolves the parser from a .rho filepath", async () => {
    const src = 'fn main() -> i32 { printf("x\n"); return 0; }';
    const out = await prettier.format(src, {
      filepath: "program.rho",
      plugins: [plugin],
    });
    expect(typeof out).toBe("string");
    expect(out).not.toBe(src); // canonicalization happened
    expect(out).toBe(await format(src)); // same parser either way
  });

  it("hands prettier the canonical text verbatim (no reflow)", async () => {
    const src = readFileSync(
      join(worktreeRoot, "corpus", "003_control.rho"),
      "utf8",
    );
    const throughPrettier = await format(src);
    const direct = formatRho(src);
    expect(throughPrettier).toBe(direct);
  });
});

describe("generation pinning", () => {
  it("the artifact carries the generation recorded in package.json", () => {
    const pkg = JSON.parse(
      readFileSync(new URL("../package.json", import.meta.url), "utf8"),
    );
    expect(pkg.rho.generation).toBe(generation.generationShort);
    expect(pkg.description).toContain(generation.generationShort);
  });

  it("the generation record is a full sha256 and matches the runtime's slot law", () => {
    const wasm = readFileSync(new URL("../assets/fmt.wasm", import.meta.url));
    expect(wasm.length).toBeGreaterThan(0);
    const rec = JSON.parse(
      readFileSync(new URL("../assets/generation.json", import.meta.url), "utf8"),
    );
    expect(rec.generation).toMatch(/^[0-9a-f]{64}$/);
    // the generation hash covers the build INPUTS (the `inputs` map in
    // generation.json), not the artifact bytes — artifact-vs-chain is
    // `tools/build-fmt.mjs --check`'s job and cannot be recomputed from
    // the wasm here. The cross-file check a test CAN pin: the slot law
    // src/fmt.js enforces (runFmt refuses input past `capacity`, then
    // writes the run-time NUL at offset + input.length) requires the
    // record's capacity to be exactly rawBytes - 1 — one reserved
    // terminator byte inside the raw literal, never behind it.
    expect(rec.inputSlot.capacity).toBe(rec.inputSlot.rawBytes - 1);
  });
});

describe("honest failures", () => {
  it("refuses unparseable source with the compiler's diagnosis", async () => {
    await expect(format("fn main() -> i32 { let }")).rejects.toThrow(
      /rho fmt failed/,
    );
  });

  it("refuses a raw NUL byte", async () => {
    await expect(format("fn main() {}\u0000")).rejects.toThrow(/NUL/);
  });

  it("refuses input beyond the pinned slot capacity", async () => {
    const big = "fn main() -> i32 {}\n" + "// " + "x".repeat(
      generation.inputSlot.rawBytes,
    );
    await expect(format(big)).rejects.toThrow(/input slot/);
  });
});

describe("capacity boundary (usable capacity = rawBytes - 1: the last slot byte is reserved; the host writes the NUL at run time)", () => {
  // a comment-padded file sized to exactly N bytes
  const tail = "fn main() -> i32 { return 0; }\n";
  const sized = (total) => {
    const pad = "// " + "y".repeat(total - tail.length - 4) + "\n";
    return pad + tail;
  };

  it("formats one byte under the usable capacity", async () => {
    const src = sized(generation.inputSlot.capacity - 1);
    expect(src.length).toBe(generation.inputSlot.capacity - 1);
    await expect(format(src)).resolves.toContain("fn main() -> i32 {");
  });

  it("formats at exactly the usable capacity (terminator still inside the slot)", async () => {
    const src = sized(generation.inputSlot.capacity);
    expect(src.length).toBe(generation.inputSlot.capacity);
    // the refused size is the raw literal's: capacity bytes + NUL must fit
    expect(generation.inputSlot.capacity).toBe(
      generation.inputSlot.rawBytes - 1,
    );
    await expect(format(src)).resolves.toContain("fn main() -> i32 {");
  });

  it("refuses one byte past the usable capacity", async () => {
    const src = sized(generation.inputSlot.capacity + 1);
    expect(src.length).toBe(generation.inputSlot.capacity + 1);
    await expect(format(src)).rejects.toThrow(/input slot/);
  });
});

describe("blank input (prettier-core parity: a formatter must not invent code)", () => {
  it("empty input formats to an empty string", async () => {
    await expect(format("")).resolves.toBe("");
  });

  it("whitespace-only input formats to an empty string", async () => {
    await expect(format(" \t \r\n \n")).resolves.toBe("");
  });
});

describe("the fixpoint gate refuses shapes the pipeline mangles", () => {
  // Wave-1 census (WORKTREE-NOTES.md, Disposition ledger #1): these
  // corpus files are ACCEPTED by this generation's pipeline and mangled
  // silently — struct declarations dropped (005), match arms collapsed
  // to "return (s + );" (006), float literals to "( + )" (013),
  // signature types to bare ": fn" (020). The mangled form fails its
  // own re-parse (parse: N unresolved forms, exit 1), so the two-pass
  // gate must refuse the source instead of returning the corruption.
  for (const [name, why] of [
    ["005_structs", "struct declarations dropped entirely"],
    ["006_enums_match", "match arms dropped to (s + )"],
    ["013_floats", "float literals dropped to ( + )"],
    ["020_closures", "signature types dropped to bare : fn"],
  ]) {
    it(`refuses corpus/${name}.rho (${why})`, async () => {
      const src = readFileSync(
        join(worktreeRoot, "corpus", `${name}.rho`),
        "utf8",
      );
      await expect(format(src)).rejects.toThrow(/rho fmt refused this source/);
    });
  }
});
