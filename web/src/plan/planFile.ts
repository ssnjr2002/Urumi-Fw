/**
 * planFile.ts — the .plan binary codec (slot-aware, deviates from Python).
 *
 * A self-describing container for a whole multi-tool job. One .plan file runs
 * on any machine (single or dual head); the runtime orchestrator resolves head
 * offsets and revolver slot angles from config at execution time.
 *
 * Format (all little-endian):
 *   HEADER
 *     magic:   4B   AB CD 50 03   (0x03 = startSteps added; 0x02 was slot-aware)
 *     version: 1B   0x03
 *     n_tools: 1B   unique ToolType values in the manifest (feasibility gate)
 *     n_ops:   2B   block count
 *
 *   TOOL MANIFEST   n_tools × 1B ToolType   (in first-appearance order)
 *
 *   OPERATIONS      n_ops ×:
 *     tool_type: 1B   ToolType enum value
 *     slot:      1B   revolver slot index, or 0xFF when the block has no slot
 *     start_x:   4B   int32 LE   block start X in TRUE machine steps (pre-invert)
 *     start_y:   4B   int32 LE   block start Y in TRUE machine steps (pre-invert)
 *     pkt_count: 4B   number of MSEG packets that follow
 *     packets:   pkt_count × 26B   raw MicroSegment packets (magic 0xAB each)
 *
 * The PAUSE flag is NOT baked here — the sender injects it at the last packet
 * of each non-final block, as with the wire stream.
 */

import type { ToolProfile } from "../config/config.js";
import { TOOL_PROFILES_BY_TYPE } from "../config/config.js";
import type { MicroSegment } from "../wire/microsegment.js";
import { PACKET_SIZE, packMicrosegment, decodePacket } from "../wire/packet.js";
import type { Block, Plan } from "./plan.js";
import { planToolTypes } from "./plan.js";

export const PLAN_MAGIC = Uint8Array.of(0xab, 0xcd, 0x50, 0x03);
export const PLAN_VERSION = 0x03;
export const SLOT_NONE = 0xff;

const HDR_SIZE = 8; // magic(4) + version(1) + n_tools(1) + n_ops(2)
const OP_HDR_SIZE = 14; // tool_type(1) + slot(1) + start_x(4) + start_y(4) + pkt_count(4)

/** Serialise a Plan to .plan bytes. */
export function savePlan(plan: Plan): Uint8Array {
    const toolTypes = planToolTypes(plan);
    if (plan.blocks.length > 0xffff) {
        throw new Error(`savePlan: ${plan.blocks.length} blocks exceeds u16 op count`);
    }

    // pre-pack every block's segments so we know the exact size up front
    const packed: Uint8Array[][] = plan.blocks.map((b) =>
        b.segments.map((ms) => packMicrosegment(ms)),
    );

    const total =
        HDR_SIZE +
        toolTypes.length +
        packed.reduce(
            (acc, pkts) => acc + OP_HDR_SIZE + pkts.length * PACKET_SIZE,
            0,
        );

    const out = new Uint8Array(total);
    const dv = new DataView(out.buffer);
    let off = 0;

    // header
    out.set(PLAN_MAGIC, off);
    off += PLAN_MAGIC.length;
    dv.setUint8(off, PLAN_VERSION); off += 1;
    dv.setUint8(off, toolTypes.length); off += 1;
    dv.setUint16(off, plan.blocks.length, true); off += 2;

    // manifest
    for (const tt of toolTypes) {
        dv.setUint8(off, tt); off += 1;
    }

    // operations
    plan.blocks.forEach((block, i) => {
        const pkts = packed[i]!;
        dv.setUint8(off, block.profile.toolType); off += 1;
        dv.setUint8(off, block.slot ?? SLOT_NONE); off += 1;
        dv.setInt32(off, block.startSteps.x, true); off += 4;
        dv.setInt32(off, block.startSteps.y, true); off += 4;
        dv.setUint32(off, pkts.length, true); off += 4;
        for (const pkt of pkts) {
            out.set(pkt, off); off += PACKET_SIZE;
        }
    });

    return out;
}

/**
 * Parse .plan bytes back into a Plan. Resolves ToolType → ToolProfile via
 * `profilesByType` (defaults to the built-in registry) and reconstructs each
 * MicroSegment via decodePacket. Throws with a precise reason on any
 * truncation, bad magic/version, or unknown tool type.
 */
export function loadPlan(
    bytes: Uint8Array,
    profilesByType: Readonly<Record<number, ToolProfile>> = TOOL_PROFILES_BY_TYPE,
): Plan {
    const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    let off = 0;

    if (bytes.length < HDR_SIZE) throw new Error("truncated .plan file (header)");
    for (let i = 0; i < PLAN_MAGIC.length; i++) {
        if (bytes[off + i] !== PLAN_MAGIC[i]) {
            throw new Error("not a .plan file (bad magic)");
        }
    }
    off += PLAN_MAGIC.length;
    const version = dv.getUint8(off); off += 1;
    if (version !== PLAN_VERSION) {
        const hint = version === 0x02 ? " (re-bake to get startSteps / travel jogs)" : "";
        throw new Error(`unsupported .plan version 0x${version.toString(16)}${hint}`);
    }
    const nTools = dv.getUint8(off); off += 1;
    const nOps = dv.getUint16(off, true); off += 2;

    // manifest — validate every tool type is known before touching packets
    if (off + nTools > bytes.length) throw new Error("truncated .plan manifest");
    for (let i = 0; i < nTools; i++) {
        const tt = dv.getUint8(off + i);
        if (!(tt in profilesByType)) {
            throw new Error(`unknown ToolType 0x${tt.toString(16)} in .plan manifest`);
        }
    }
    off += nTools;

    const blocks: Block[] = [];
    for (let i = 0; i < nOps; i++) {
        if (off + OP_HDR_SIZE > bytes.length) {
            throw new Error(`truncated op header at block ${i}`);
        }
        const toolType = dv.getUint8(off); off += 1;
        const slotByte = dv.getUint8(off); off += 1;
        const startX = dv.getInt32(off, true); off += 4;
        const startY = dv.getInt32(off, true); off += 4;
        const pktCount = dv.getUint32(off, true); off += 4;

        const profile = profilesByType[toolType];
        if (!profile) {
            throw new Error(`unknown ToolType 0x${toolType.toString(16)} at block ${i}`);
        }
        if (off + pktCount * PACKET_SIZE > bytes.length) {
            throw new Error(`truncated packets at block ${i}`);
        }

        const segments: MicroSegment[] = [];
        for (let j = 0; j < pktCount; j++) {
            const d = decodePacket(bytes, off);
            if (!d.crcOk) throw new Error(`bad CRC at packet ${j} of block ${i}`);
            segments.push({ dx: d.dx, dy: d.dy, dz: d.dz, da: d.da, interval: d.interval, flags: d.flags });
            off += PACKET_SIZE;
        }

        const startSteps = { x: startX, y: startY };
        const block: Block =
            slotByte === SLOT_NONE
                ? { profile, segments, startSteps }
                : { profile, slot: slotByte, segments, startSteps };
        blocks.push(block);
    }

    return { blocks };
}
