import { defineConfig } from "vitest/config";

export default defineConfig({
  test: {
    name: "prettier-plugin-rho",
    include: ["test/*.test.js"],
    environment: "node",
    // every run is time-capped (working protocol): a hung format
    // fails the case, never the harness
    testTimeout: 60_000,
  },
});
