/**
 * image.ts — JSON → the controller's /config.bin, for flashing with
 * `pio run -e pico -t uploadfs` (scripts/pio/config_image.py).
 *
 * config.bin is the 16-byte header of config_store.h (`ConfigBlobHeader`) and
 * the encoded config a CFG_SET push stores, so a flashed config and a pushed
 * one are byte-identical past the header.
 */

import { crc32 } from "../packets/crc.js";
import { CFG_MAX_BYTES } from "../packets/cfg.js";
import { encodeConfig } from "./blob.js";
import { parseConfig } from "./load.js";

/** config_store.h CFG_VERSION: the file header's format, not the payload's. */
export const CFG_FILE_VERSION = 1;
export const CFG_HEADER_BYTES = 16;

/**
 * JSONC → JSON: drops `//` and `/* *\/` comments outside strings. Comment
 * text becomes spaces (newlines kept), so parse errors keep their positions.
 */
export function stripJsonComments(text: string): string {
    let out = "";
    let i = 0;
    while (i < text.length) {
        const c = text[i]!;
        if (c === '"') {
            const start = i++;
            while (i < text.length && text[i] !== '"') i += text[i] === "\\" ? 2 : 1;
            out += text.slice(start, ++i);
        } else if (c === "/" && text[i + 1] === "/") {
            while (i < text.length && text[i] !== "\n") { out += " "; i++; }
        } else if (c === "/" && text[i + 1] === "*") {
            const end = text.indexOf("*/", i + 2);
            const stop = end < 0 ? text.length : end + 2;
            out += text.slice(i, stop).replace(/[^\n]/g, " ");
            i = stop;
        } else {
            out += c;
            i++;
        }
    }
    return out;
}

/** /config.bin for an encoded config: header (little-endian), then the encoded config. */
export function wrapConfigBin(encoded: Uint8Array, seq = 0): Uint8Array {
    if (encoded.length === 0 || encoded.length > CFG_MAX_BYTES) {
        throw new Error(`encoded config is ${encoded.length} bytes (1..${CFG_MAX_BYTES})`);
    }
    const out = new Uint8Array(CFG_HEADER_BYTES + encoded.length);
    const dv = new DataView(out.buffer);
    dv.setUint16(0, CFG_FILE_VERSION, true);
    dv.setUint32(4, seq >>> 0, true);
    dv.setUint32(8, encoded.length, true);
    dv.setUint32(12, crc32(encoded) >>> 0, true);
    out.set(encoded, CFG_HEADER_BYTES);
    return out;
}

/** JSON or JSONC → /config.bin. Throws with every load error. */
export function configBin(json: string): Uint8Array {
    const r = parseConfig(stripJsonComments(json));
    if (!r.ok) throw new Error(`config invalid:\n  ${r.errors.join("\n  ")}`);
    return wrapConfigBin(encodeConfig(r.config));
}
