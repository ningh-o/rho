#!/usr/bin/env node
// sweep the run_one case sources of a test harness through boot's check,
// listing every case the current language law refuses
import { readFileSync, writeFileSync } from "node:fs";
import { execSync } from "node:child_process";

const file = process.argv[2];
const src = readFileSync(file, "utf8");
const out = [];
for (const line of src.split("\n")) {
  const m = /^run_one (\S+) '(.*)' /.exec(line);
  if (!m) continue;
  // only the shell-level \' needs unescaping — \n stays two characters
  // here exactly as the shell delivers it to the lexer
  const code = m[2].replace(/\\'/g, "'");
  writeFileSync("tmp/case.rho", code);
  let r = "";
  try {
    r = execSync("./build/rho check tmp/case.rho 2>&1 || true", { encoding: "utf8" });
  } catch (e) {
    r = String(e);
  }
  if (r.includes("immutable") || r.includes("cannot assign")) {
    out.push(m[1] + " :: " + r.split("\n")[0]);
  }
}
console.log(out.join("\n") || "all clean");
