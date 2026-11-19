/**
 * blob.ts — ControllerConfig ↔ the msgpack blob stored on the controller.
 *
 * The blob is the resolved config plus a schema version `v`. The controller
 * has no defaults: a field it reads and does not find is a rejection
 * (src/rp2350/core0/config/config_decode.cpp). Framing is wire/format/cfg.ts.
 */

import { decode, encode } from "@msgpack/msgpack";
import type { ControllerConfig } from "../schema.js";

/** Payload schema version. Bump when a field the controller decodes changes shape; equals CFG_SCHEMA_VERSION. */
export const CONFIG_BLOB_VERSION = 17;

export function encodeConfigBlob(config: ControllerConfig): Uint8Array {
    return encode({ v: CONFIG_BLOB_VERSION, ...config }, { ignoreUndefined: true });
}

/**
 * Decode a blob pulled from the controller. Shape is trusted, since the
 * controller stores what was pushed. Throws on a blob that is not a map or
 * carries an unknown `v`.
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
