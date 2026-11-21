/**
 * blob.ts — a config ↔ the encoded config the controller stores (msgpack).
 *
 * The encoded config is the resolved config plus a schema version `v`. The controller
 * has no defaults: a field it reads and does not find is a rejection
 * (src/rp2350/core0/config/config_decode.cpp). Framing is packets/cfg.ts.
 */

import { decode, encode } from "@msgpack/msgpack";
import type { ControllerConfig } from "./schema.js";

/** The encoded config's schema version. Bump when a field the controller decodes changes shape; equals CFG_SCHEMA_VERSION. */
export const CONFIG_BLOB_VERSION = 17;

export function encodeConfig(config: ControllerConfig): Uint8Array {
    return encode({ v: CONFIG_BLOB_VERSION, ...config }, { ignoreUndefined: true });
}

/**
 * Decode an encoded config pulled from the controller. Shape is trusted, since the
 * controller stores what was pushed. Throws on bytes that are not a map or
 * carries an unknown `v`.
 */
export function decodeConfig(bytes: Uint8Array): ControllerConfig {
    const obj = decode(bytes);
    if (typeof obj !== "object" || obj === null || Array.isArray(obj)) {
        throw new Error("decodeConfig: not an encoded config (not a map)");
    }
    const { v, ...config } = obj as { v?: unknown };
    if (v !== CONFIG_BLOB_VERSION) {
        throw new Error(`decodeConfig: unsupported version ${String(v)}`);
    }
    return config as ControllerConfig;
}
