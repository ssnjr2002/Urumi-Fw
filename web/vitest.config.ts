import { defineConfig } from "vitest/config";

export default defineConfig({
  test: {
    environment: "node",
    include: ["config/**/*.test.ts", "toolpath/**/*.test.ts"],
    cacheDir: "node_modules/.cache/vitest",
  },
});
