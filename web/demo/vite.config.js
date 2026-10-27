import { fileURLToPath } from 'node:url';
import { defineConfig } from 'vite';

const src = (p) => fileURLToPath(new URL(`../src/${p}`, import.meta.url));

// Default mode serves the library from src/ so wire edits show on reload.
// `--mode dist` resolves urumi-host through package.json exports, as a third
// party would (needs `pnpm build`).
export default defineConfig(({ mode }) => ({
    resolve: {
        alias: mode === 'dist' ? [] : [
            { find: /^urumi-host$/, replacement: src('index.ts') },
            { find: /^urumi-host\/(.*)$/, replacement: src('$1.ts') },
        ],
    },
}));
