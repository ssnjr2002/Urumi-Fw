import { defineConfig } from "vitest/config";

export default defineConfig({
  test: {
    environment: "node",
    include: ["config/**/*.test.ts", "svg/**/*.test.ts", "wire/**/*.test.ts", "choreograph/**/*.test.ts", "toolpath/**/*.test.ts", "production/**/*.test.ts", "plan/**/*.test.ts"],
    setupFiles: ["./test-setup.ts"],
    cacheDir: "node_modules/.cache/vitest",
  },
});
