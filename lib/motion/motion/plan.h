/**
 * plan.h — stage 6: the look-ahead feedrate planner.
 *
 * Transcribed from web/src/toolpath/plan.ts.
 *
 * Turns each sample's LOCAL vCeiling (from constrain) into a globally
 * reachable speed v via two sweeps over each subpath:
 *
 *   backward (decel feasibility), last -> first:
 *       v[i] = min(vCeiling[i], sqrt(v[i+1]^2 + 2*a*ds[i]))
 *   forward (accel feasibility), first -> last:
 *       v[i] = min(v[i],        sqrt(v[i-1]^2 + 2*a*ds[i-1]))
 *
 * After both, every sample's speed is simultaneously reachable-from-behind and
 * stoppable-ahead, so the profile is acceleration-continuous by construction.
 *
 * This is the first ported stage that is NOT per-sample: a value at i depends
 * on the whole subpath. That is why its contract tests are properties
 * (feasibility, monotonicity, endpoints at rest) rather than value pins.
 */

#ifndef MOTION_PLAN_H
#define MOTION_PLAN_H

#include "motion/constrain.h"
#include "motion/sample.h"

#include <cstddef>
#include <utility>
#include <vector>

namespace motion {

/**
 * Stage 6 output. Completes the progression
 * Sample -> ConstrainedSample -> PlannedSample; each stage's output type is
 * its guarantee.
 */
struct PlannedSample {
    Sample s;
    double vCeiling;
    double v;
};

/**
 * Plan stage options. All from MachineConfig.
 *
 * xAccel / yAccel / aAccelDegS2 == 0 means "unlimited" (that term is skipped).
 * aMax is the scalar fallback used when no per-axis term constrains.
 *
 * pathAccel == 0 means unset, matching the TypeScript's `?? undefined` gate —
 * there the test is `!== undefined && > 0`, so 0 and absent behave identically
 * and a single sentinel is faithful. (Contrast constrain's cornerStopAngleDeg,
 * where 0 and absent mean opposite things and a presence flag was required.)
 */
struct PlanOptions {
    double xAccel = 0;
    double yAccel = 0;
    double aAccelDegS2 = 0;
    double aMax = 0;
    double pathAccel = 0;
};

using Range = std::pair<size_t, size_t>; // inclusive [start, end]

// The three sample types are distinct structs rather than a base and two
// derivations, so these overloads give the generic helpers below one way to
// reach the geometry regardless of which stage's output they are handed.
inline const Sample& sampleOf(const Sample& s) { return s; }
inline const Sample& sampleOf(const ConstrainedSample& c) { return c.s; }
inline const Sample& sampleOf(const PlannedSample& p) { return p.s; }

/** Inclusive index ranges, one per PATH_START..PATH_END. */
template <class T>
std::vector<Range> subpathRanges(const std::vector<T>& samples) {
    std::vector<Range> out;
    bool open = false;
    size_t start = 0;
    for (size_t i = 0; i < samples.size(); i++) {
        const uint32_t flags = sampleOf(samples[i]).flags;
        if (flags & PATH_START) {
            start = i;
            open = true;
        }
        if (flags & PATH_END) {
            if (!open) start = i;
            out.emplace_back(start, i);
            open = false;
        }
    }
    return out;
}

/**
 * Tool-path accel over the segment s0->s1 honouring per-axis accel limits.
 *
 * X and Y: the tool accel projects onto each axis as a*|u_axis|, so
 * a <= min(xAccel/|ux|, yAccel/|uy|).
 *
 * A (tangential tracking): speeding up while curved drives A angular accel
 * alpha = kappa*a_tan, so a_tan <= rad(aAccel)/kappa. Pairs with constrain's
 * curvature-gradient velocity ceiling.
 */
double segAccel(const Sample& s0, const Sample& s1, const PlanOptions& options);

/**
 * Resolve v via the backward+forward look-ahead. Pure; returns a fresh vector.
 *
 * Throws std::runtime_error if any sample lies outside a PATH_START/PATH_END
 * bracket — such samples would be silently skipped by the sweeps and returned
 * at their raw ceiling, i.e. full feed from a standing start (audit P2).
 */
std::vector<PlannedSample> plan(
    const std::vector<ConstrainedSample>& samples,
    const PlanOptions& options);

} // namespace motion

#endif // MOTION_PLAN_H
