import js from "@eslint/js";
import globals from "globals";
import tseslint from "typescript-eslint";

export default tseslint.config(
  {
    ignores: ["node_modules", "dist"],
  },
  js.configs.recommended,
  tseslint.configs.recommended,
  {
    rules: {
      "@typescript-eslint/no-unused-vars": [
        "error",
        { argsIgnorePattern: "^_", varsIgnorePattern: "^_" },
      ],
    },
  },
  // The demo is plain JS in the browser; its vite config runs in Node.
  {
    files: ["demo/**/*.js"],
    languageOptions: { globals: globals.browser },
  },
  {
    files: ["demo/vite.config.js"],
    languageOptions: { globals: globals.node },
  },
);
