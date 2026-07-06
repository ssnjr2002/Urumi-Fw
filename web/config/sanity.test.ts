import { describe, it, expect } from "vitest";

/**
 * Scaffold sanity check — confirms the TS + Vitest toolchain compiles,
 * resolves modules, and runs in this folder. Delete once real modules land.
 */
describe("config scaffold", () => {
  it("runs vitest", () => {
    expect(1 + 1).toBe(2);
  });
});
