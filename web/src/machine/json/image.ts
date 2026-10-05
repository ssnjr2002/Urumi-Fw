/**
 * image.ts — a config file → the Pico's /config.bin, for flashing with
 * `pio run -e pico -t uploadfs` (scripts/pio/config_image.py).
 *
 * The file is the 16-byte header of config_store.h (`ConfigBlobHeader`) and
 * the same blob a CFG_SET push stores, so a flashed config and a pushed one
 * are byte-identical past the header.
 */

import { crc32 } from "../../wire/format/crc.js";
import { CFG_MAX_BYTES } from "../../wire/format/cfg.js";
import { encodeConfigBlob } from "./blob.js";
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

/** /config.bin for `payload`: header (little-endian) then the payload. */
export function configFileImage(payload: Uint8Array, seq = 0): Uint8Array {
    if (payload.length === 0 || payload.length > CFG_MAX_BYTES) {
        throw new Error(`config blob is ${payload.length} bytes (1..${CFG_MAX_BYTES})`);
    }
    const out = new Uint8Array(CFG_HEADER_BYTES + payload.length);
    const dv = new DataView(out.buffer);
    dv.setUint16(0, CFG_FILE_VERSION, true);
    dv.setUint32(4, seq >>> 0, true);
    dv.setUint32(8, payload.length, true);
    dv.setUint32(12, crc32(payload) >>> 0, true);
    out.set(payload, CFG_HEADER_BYTES);
    return out;
}

/** A JSON or JSONC config's text → /config.bin. Throws with every load error. */
export function configImageFromText(text: string): Uint8Array {
    const r = parseConfig(stripJsonComments(text));
    if (!r.ok) throw new Error(`config invalid:\n  ${r.errors.join("\n  ")}`);
    return configFileImage(encodeConfigBlob(r.config));
}
