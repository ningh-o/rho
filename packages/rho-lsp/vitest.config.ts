// vitest.config.ts — pin the suite's discovery and budgets. Tests are
// static deliverables in CI terms: the owner runs them with `npm test`.

import { defineConfig } from "vitest/config";

export default defineConfig({
  test: {
    include: ["test/**/*.test.ts"],
    testTimeout: 20_000,
    // the transcript/degrade suites poll real timers; keep files
    // isolated so per-file env (RHO_FAKE_DIR) never crosses
    pool: "forks",
    maxConcurrency: 1,
  },
});
