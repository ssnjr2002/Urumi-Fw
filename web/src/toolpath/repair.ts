/**
 * Stage 3: C1 continuity enforcement at curve joins.
 * Ported from host/production/repair.py.
 *
 * For each join between curve[i] and curve[i+1]:
 *   - Check C0: endpoints meet (within gapTolMm)
 *   - Check G1: exit tangent of [i] parallel to entry tangent of [i+1]
 *     (direction only — not speed, per plan section 5.1)
 *   - If angle deviation > angleTolDeg: log as cusp, leave the join as-is
 *     (velocity planning handles cornering; inserting a zero-gap blend
 *     produces a tiny loop a plotter would trace)
 *   - If C0 gap > gapTolMm: insert a bridging cubic that respects both
 *     tangent directions
 * Output: repaired curve list + list of RepairLog entries.
 *
 * Pure stage: takes tolerances as explicit parameters, never imports
 * config. The caller sources values from defaultConfig() / qualityConfig()
 * / overrides / a UI form and passes them in.
 */

import {
    cubic,
    sub,
    add,
    scale,
    length,
    angleBetweenDeg,
    exitTangent,
    entryTangent,
    type CubicBezier,
    type Pt,
} from "./geometry.js";

export interface RepairOptions {
    readonly angleTolDeg: number;
    readonly gapTolMm: number;
}

export interface RepairLog {
    readonly joinIndex: number;
    readonly kind: "blend" | "bridge" | "cusp";
    readonly angleDeg: number;
    readonly gapMm: number;
}

export interface RepairResult {
    readonly repaired: CubicBezier[];
    readonly logs: RepairLog[];
}

// ── blending cubic ────────────────────────────────────────────────────────────

const MIN_HANDLE_MM = 1.0;

/**
 * Insert a short cubic from p0 to p3 that respects the tangent directions
 * on both sides. Handle length = 1/3 of chord, minimum 1mm so tangents
 * are preserved even when p0==p3 (zero-gap corner blend).
 */
function blendCubic(p0: Pt, exitTan: Pt, p3: Pt, entryTan: Pt): CubicBezier {
    const chord = length(sub(p3, p0));
    const h = Math.max(chord / 3, MIN_HANDLE_MM);
    const p1 = add(p0, scale(exitTan, h));
    const p2 = add(p3, scale(entryTan, -h));
    return cubic(p0, p1, p2, p3);
}

// ── main stage ────────────────────────────────────────────────────────────────

/**
 * Returns { repaired, logs }.
 * repaired: original curves with blending cubics inserted at bad joins.
 * logs: list of RepairLog for every join that needed intervention.
 */
export function enforceC1(
    curves: readonly CubicBezier[],
    options: RepairOptions,
): RepairResult {
    const { angleTolDeg, gapTolMm } = options;

    if (curves.length <= 1) {
        return { repaired: [...curves], logs: [] };
    }

    const repaired: CubicBezier[] = [curves[0]!];
    const logs: RepairLog[] = [];

    for (let i = 0; i < curves.length - 1; i++) {
        const a = curves[i]!;
        const b = curves[i + 1]!;

        const gap = length(sub(b.p0, a.p3));
        const exitT = exitTangent(a);
        const entryT = entryTangent(b);

        // degenerate tangents (zero-length handle) — treat as cusp
        if (length(exitT) < 0.5 || length(entryT) < 0.5) {
            logs.push({ joinIndex: i, kind: "cusp", angleDeg: 180, gapMm: gap });
            const blend = blendCubic(a.p3, { x: 1, y: 0 }, b.p0, { x: 1, y: 0 });
            repaired.push(blend);
            repaired.push(b);
            continue;
        }

        const angle = angleBetweenDeg(exitT, entryT);

        if (gap > gapTolMm) {
            // C0 broken: bridge the gap
            logs.push({ joinIndex: i, kind: "bridge", angleDeg: angle, gapMm: gap });
            const bridge = blendCubic(a.p3, exitT, b.p0, entryT);
            repaired.push(bridge);
        } else if (angle > angleTolDeg) {
            // Sharp corner at a shared point — log as cusp, leave as-is.
            // Inserting a blend here produces a tiny loop (p0==p3) which a
            // plotter would trace. Velocity planning handles cornering instead.
            logs.push({ joinIndex: i, kind: "cusp", angleDeg: angle, gapMm: gap });
        }

        repaired.push(b);
    }

    return { repaired, logs };
}
