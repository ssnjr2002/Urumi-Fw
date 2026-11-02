/**
 * Writes the Pico's runtime field header from RUNTIME_FIELDS.
 *
 *   pnpm gen:runtime
 */

import { writeFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { renderRuntimeHeader } from "../src/machine/runtimeHeader.js";

const out = fileURLToPath(
    new URL("../../src/rp2350/core0/config/runtime_fields.h", import.meta.url),
);
writeFileSync(out, renderRuntimeHeader());
console.log(`wrote ${out}`);
