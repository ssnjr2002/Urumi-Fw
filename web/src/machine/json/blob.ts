/**
 * blob.ts — ControllerConfig ↔ the msgpack blob stored on the Pico.
 *
 * The blob is the RESOLVED config (every default already filled in by
 * loadConfig) plus a payload schema version `v`. The Pico has no defaults
 * table: a field it consumes and does not find is a rejection
 * (src/rp2350/config/config_decode.cpp). Bytes and framing are the wire
 * layer's (wire/format/cfg.ts); this module only says what the bytes mean.
 */

import { decode, encode } from "@msgpack/msgpack";
import type { ControllerConfig } from "../schema.js";

/** Payload schema version. Bump when a field the Pico decodes changes shape. */
export const CONFIG_BLOB_VERSION = 15;

export function encodeConfigBlob(config: ControllerConfig): Uint8Array {
    return encode({ v: CONFIG_BLOB_VERSION, ...config }, { ignoreUndefined: true });
}

/**
 * Decode a blob pulled from the Pico. Shape is trusted, not re-validated: the
 * blob was a ControllerConfig when it was pushed, and the Pico stores it
 * verbatim. Throws on a blob that is not a map or carries an unknown `v`.
 */
export function decodeConfigBlob(bytes: Uint8Array): ControllerConfig {
    const obj = decode(bytes);
    if (typeof obj !== "object" || obj === null || Array.isArray(obj)) {
        throw new Error("config blob: not a map");
    }
    const { v, ...config } = obj as { v?: unknown };
    if (v !== CONFIG_BLOB_VERSION) {
        throw new Error(`config blob: unsupported version ${String(v)}`);
    }
    return config as ControllerConfig;
}
