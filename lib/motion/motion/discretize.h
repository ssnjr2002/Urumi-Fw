/**
 * discretize.h — stage 7: the planned Sample stream becomes MicroSegments.
 *
 * Transcribed from web/src/toolpath/discretize.ts.
 *
 * The bottom of the pipeline. Each consecutive planned sample pair becomes one
 * or more MicroSegments: per-axis integer step deltas (float accumulators,
 * rounded at emit), tangent-tracking da, and an interval derived from the
 * planned speed. Velocity-aware subdivision splits a pair into k sub-segments
 * so the speed never changes by more than dvMax within one segment — cruise
 * stays k=1, ramps subdivide, corners stay k=1 and are handled as a single
 * near-zero step plus a pivot.
 *
 * At each transition (PATH_START, corner, PATH_END) it calls choreograph for
 * the non-cutting motion: travel jog between subpaths, A pre-orientation and
 * Z-lower at PATH_START, lift-pivot-lower at corners, Z-raise at PATH_END. The
 * walk state (posX, posY, theta, aAccum, aPhys) is local; the function is pure
 * and never touches its input.
 *
 * Why this is simpler than it sounds: velocity planning already brought the
 * tool to v=0 at every corner (constrain set vCeiling=0, plan propagated it),
 * so a corner is just "two adjacent samples whose tangent jumps by at least the
 * tool's corner angle" — between-curve corners and in-curve cusps collapse into
 * ONE rule.
 */

#ifndef MOTION_DISCRETIZE_H
#define MOTION_DISCRETIZE_H

#include "motion/axes.h"
#include "motion/microsegment.h"
#include "motion/plan.h"

#include <vector>

namespace motion {

/**
 * Everything the walk reads, already resolved.
 *
 * The TypeScript signature is `discretize(samples, machine, profile, quality,
 * overrides)` and resolves inside itself — resolvedAxes(), resolveTargets(),
 * and the `overrides ?? profile ?? machine` fallback chain. This struct is
 * where that resolution has already happened, which is what keeps the config
 * layer out of the port. The chain is `??` operators over config rather than
 * motion math, and it is exercised by the TypeScript's own config tests.
 *
 * Consequence worth stating plainly: the port cannot reproduce a defect that
 * lives IN the chain, only one that lives in what the chain produces.
 */
struct DiscretizeOptions {
    ResolvedAxes axes;

    // ── tool ──
    bool tangential = false;
    bool unwind = false;
    double cornerAngleDeg = 0;
    /**
     * Blade caster offset, mm. Only ever compared against OFFSET_TOLERANCE_MM
     * — the stage refuses to run a tool that needs compensation it does not
     * implement. Kept in the port because it guards the stage's own
     * precondition; dropping it would let the C++ silently cut geometry the
     * TypeScript refuses.
     */
    double offsetMm = 0;

    // ── quality ──
    double dvMax = 0;
    double vMin = 0;

    // ── travel (already resolved through overrides -> profile -> machine) ──
    double jogFeed = 0;
    double liftHeight = 0;
    double zFeed = 0;
    double zAccel = 0;
    OpTarget slew;
};

/** Blade offset above which compensation is required. web/src/config/tools.ts. */
constexpr double OFFSET_TOLERANCE_MM = 0.05;

/**
 * Walk the planned stream and emit a flat MicroSegment list. Pure.
 *
 * Throws std::runtime_error when the tool needs unimplemented blade-offset
 * compensation, and when a subpath produces no motion at all — the latter
 * because such a subpath has nowhere to put its PATH_END and would otherwise
 * vanish from the stream silently.
 */
std::vector<MicroSegment> discretize(const std::vector<PlannedSample>& samples,
                                     const DiscretizeOptions& options);

} // namespace motion

#endif // MOTION_DISCRETIZE_H
