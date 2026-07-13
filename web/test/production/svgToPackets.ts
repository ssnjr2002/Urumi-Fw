/**
 * svgToPackets.ts — FROZEN parity harness (test scaffolding, not production).
 *
 * This is the original single-tool bake, kept verbatim as the byte-for-byte
 * reference the parity tests pin against Python's
 * `python -m host.production.svg_to_packets --out`. It carries its own copy of
 * the stage 3-8 chain ON PURPOSE: it must NOT change, so that the living
 * production compile (production/compileBlock.ts) can be refactored freely
 * while a test asserts compileBlock still reproduces this harness's output
 * (and therefore Python's).
 *
 * Do not import this from production code. Do not "clean it up" — its value is
 * that it is frozen.
 */

import type { CubicBezier } from "../../src/toolpath/geometry.js";
import type { MachineConfig, ToolProfile, QualityConfig } from "../../src/config/config.js";
import { resolvedAxes } from "../../src/config/config.js";
import { loadSvgMmSubpaths } from "../../src/svg/ingest.js";
import { enforceC1 } from "../../src/toolpath/repair.js";
import { flatten } from "../../src/toolpath/flatten.js";
import { constrain } from "../../src/toolpath/constrain.js";
import { plan } from "../../src/toolpath/plan.js";
import { discretize } from "../../src/toolpath/discretize.js";
import type { MicroSegment } from "../../src/wire/microsegment.js";
import { serialiseMicrosegments, writeStream } from "../../src/wire/packet.js";

// ── types ─────────────────────────────────────────────────────────────────────

export interface SvgBakeOptions {
    /** Per-call travel jog feed (mm/s); falls back to profile, then machine. */
    readonly jogFeed?: number;
    /** Per-call Z lift height (mm); falls back to profile. */
    readonly liftHeight?: number;
    /** Per-call Z feed (mm/s); falls back to profile, then machine. */
    readonly zFeed?: number;
}

// ── the tool-aware pipeline core ──────────────────────────────────────────────

/**
 * mm subpaths + a ToolProfile → MicroSegment[] (the stage 3-8 chain).
 *
 * Mirrors Python host.production.svg_to_packets.subpaths_to_packets.
 * feed_max defaults to profile.feedMax, aMax defaults to machine.x.accel,
 * quality is required (no hidden default — the caller sources it from
 * qualityConfig() or overrides).
 *
 * The A-axis config bridging matches the Python source exactly:
 *   - constrain gets aRate/aAccel ONLY when profile.tangential (else 0)
 *   - plan reads machine.a.accel directly (always, regardless of tangential)
 * This subtle asymmetry is intentional in the Python and replicated here
 * for byte-identical parity.
 */
export function subpathsToPackets(
    subpathsMm: readonly (readonly CubicBezier[])[],
    machine: MachineConfig,
    profile: ToolProfile,
    quality: QualityConfig,
    overrides?: SvgBakeOptions,
): MicroSegment[] {
    const tangential = profile.tangential;
    const axes = resolvedAxes(machine);
    const feedMax = profile.feedMax;
    const aMax = machine.x.accel;

    // Stage 3: repair — one enforceC1 per subpath
    const repaired = subpathsMm.map((sp) =>
        enforceC1(sp, { angleTolDeg: quality.angleTol, gapTolMm: quality.gapTol }).repaired,
    );

    // A-axis config: constrain gets the conditional (tangential-only) form
    const cornerStop = tangential ? profile.cornerAngleDeg : undefined;
    const aRate = tangential ? axes.a.maxRate : 0;
    const aAccelForConstrain = tangential ? axes.a.accel : 0;

    // Stage 4: flatten
    const samples = flatten(repaired, {
        chordTol: quality.chordTol,
        dsMax: quality.dsMax,
        dthetaMax: quality.dthetaMax,
        dtMax: quality.dtMax,
        dtMin: quality.dtMin,
    });

    // Stage 5: constrain (pure — returns fresh ConstrainedSample[])
    const constrained = constrain(samples, {
        feedMax,
        aMax,
        junctionDeviation: quality.junctionDeviation,
        aRateDegS: aRate,
        aAccelDegS2: aAccelForConstrain,
        cornerStopAngleDeg: cornerStop,
    });

    // Stage 6: plan — reads machine.a.accel directly (NOT the conditional form)
    const planned = plan(constrained, {
        xAccel: machine.x.accel,
        yAccel: machine.y.accel,
        aAccelDegS2: axes.a.accel,
        aMax,
    });

    // Stage 8: discretize (calls choreograph at transitions)
    const segments = discretize(planned, machine, profile, quality, overrides);

    return segments;
}

// ── full SVG-text-to-bin entry point ──────────────────────────────────────────

/**
 * SVG text → length-prefixed .bin bytes (the full pipeline).
 *
 * Parses + normalises the SVG (stages 1-2 via loadSvgMmSubpaths), runs the
 * stage 3-8 chain via subpathsToPackets, then frames the result as
 * [u16 LE 26][26-byte packet] per packet — byte-for-byte comparable with
 * `python -m host.production.svg_to_packets svg --out file.bin`.
 */
export function bakeBin(
    svgText: string,
    machine: MachineConfig,
    profile: ToolProfile,
    quality: QualityConfig,
    overrides?: SvgBakeOptions,
): Uint8Array {
    const { subpaths } = loadSvgMmSubpaths(svgText);
    const segments = subpathsToPackets(subpaths, machine, profile, quality, overrides);
    const packets = [...serialiseMicrosegments(segments)];
    return writeStream(packets);
}
