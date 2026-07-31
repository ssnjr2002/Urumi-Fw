#include "motion/discretize.h"

#include "motion/choreograph.h"
#include "motion/geometry.h"
#include "motion/jsmath.h"

#include <cmath>
#include <stdexcept>
#include <string>

namespace motion {

namespace {

/** Append one emission onto the running output. Z and A are both ramps now. */
void appendAll(std::vector<MicroSegment>& out, const std::vector<MicroSegment>& v) {
    out.insert(out.end(), v.begin(), v.end());
}

} // namespace

namespace {

/**
 * Speed a fraction `f` of the way along a sub-segment, under the constant
 * acceleration `plan` actually produces: v^2 is linear in distance, v is not.
 * Reduces to plain interpolation when v0 == v1 (cruise), and is exact at f = 0
 * and f = 1.
 */
double subV(double v0, double v1, double f) {
    const double sq = v0 * v0 + f * (v1 * v1 - v0 * v0);
    return sq > 0 ? std::sqrt(sq) : 0;
}

/** Ceiling on how finely one sample pair may be subdivided. */
constexpr double MAX_SUBDIVISION = 256;

} // namespace

std::vector<MicroSegment> discretize(const std::vector<PlannedSample>& samples,
                                     const DiscretizeOptions& options) {
    if (options.offsetMm > OFFSET_TOLERANCE_MM) {
        throw std::runtime_error(
            "tool profile has offsetMm=" + std::to_string(options.offsetMm) +
            " (> OFFSET_TOLERANCE_MM); blade-offset compensation is not "
            "implemented. Use a centre-pivot tool until it lands.");
    }

    const ResolvedAxes& axes = options.axes;
    const bool tangential = options.tangential;
    const double cornerAngle = options.cornerAngleDeg;

    const double xSpu = axes.x.stepsPerUnit;
    const double ySpu = axes.y.stepsPerUnit;
    const double aSpd = axes.a.stepsPerUnit;

    const double zSteps = zStepCount(options.liftHeight, axes);
    const bool lift = zSteps > 0;

    std::vector<MicroSegment> out;
    double posX = 0;  // float step accumulators (rounded at emit)
    double posY = 0;
    double theta = 0;   // logical current tangent (deg)
    double aAccum = 0;  // float A steps (tracking; telescopes to the exact net)
    double aPhys = 0;   // physical A steps (TRUE rotation; for unwind)
    bool started = false;

    for (const Range& range : subpathRanges(samples)) {
        const size_t lo = range.first;
        const size_t hi = range.second;
        const PlannedSample& first = samples[lo];
        const double targetX = first.s.x * xSpu;
        const double targetY = first.s.y * ySpu;

        // travel jog from the previous subpath's end
        if (started) {
            const std::vector<MicroSegment> jog =
                travelJog(posX, posY, targetX, targetY, axes, options.vMin, options.jogFeed);
            out.insert(out.end(), jog.begin(), jog.end());
        }

        // A pre-orientation to the entry tangent (pen-up), including unwind
        const double entryTheta = first.s.theta;
        const AMoveResult orient =
            preOrient(entryTheta, theta, aPhys, axes, tangential, options.unwind, options.slew);
        out.insert(out.end(), orient.segments.begin(), orient.segments.end());
        aPhys = orient.newAPhys;

        posX = targetX;
        posY = targetY;
        theta = entryTheta;
        aAccum = aPhys;
        started = true;

        if (lift) appendAll(out, zMove(-zSteps, axes, options.zFeed, options.zAccel)); // lower to cut

        // Index of the last segment that may carry this subpath's PATH_END. It
        // tracks the last CUTTING segment; if the subpath's final sub-step turns
        // out to be zero-motion (and so is skipped, D1) the marker lands here
        // instead — same position in the stream, minus the empty second.
        //
        // Signed, because an empty `out` must be representable as "nowhere to
        // put the marker" and is the condition the throw below tests.
        long long endIdx = static_cast<long long>(out.size()) - 1;
        bool endEmitted = false;

        // ── walk the cutting samples ─────────────────────────────────────────
        for (size_t i = lo; i < hi; i++) {
            const PlannedSample& a = samples[i];
            const PlannedSample& b = samples[i + 1];
            const double dtheta = angleDelta(theta, b.s.theta);
            const bool isCorner = tangential && std::fabs(dtheta) >= cornerAngle;
            const bool final = (i + 1 == hi);

            // Velocity-aware subdivision (premortem P3): split the pair into k
            // sub-segments so the speed never changes by more than dvMax within
            // one MicroSegment. Cruise (dv~0) stays k=1; only ramps subdivide.
            // Corners (v~0 both ends, dtheta huge) also stay k=1.
            double k;
            if (isCorner) {
                k = 1;
            } else {
                k = jsMax(1, std::ceil(std::fabs(b.v - a.v) / options.dvMax));
                k = jsMin(k, MAX_SUBDIVISION);
            }

            const double baseX = posX;
            const double baseY = posY;
            double thPrev = 0;
            for (double j = 1; j <= k; j += 1) {
                const double f = j / k;
                const double tgtX = baseX + (b.s.x - a.s.x) * xSpu * f;
                const double tgtY = baseY + (b.s.y - a.s.y) * ySpu * f;
                const double dx = jsRound(tgtX) - jsRound(posX);
                const double dy = jsRound(tgtY) - jsRound(posY);

                const double thF = theta + dtheta * f;
                double da = 0;
                if (tangential && !isCorner) {
                    const double refTheta = (j == 1) ? theta : thPrev;
                    const double aNew = aAccum + (thF - refTheta) * aSpd;
                    da = jsRound(aNew) - jsRound(aAccum);
                    aAccum = aNew;
                }
                thPrev = thF;

                const bool lastSub = (j == k);
                const bool segFinal = final && lastSub;

                // Skip every zero-motion sub-step, including the final one and a
                // corner's last one (D1). An empty segment is not free: interval()
                // returns fCpu when no axis moves, so emitting one would park the
                // machine for a full second. PATH_END is not lost — it is applied
                // to the last segment that actually moved, after the walk.
                if (dx == 0 && dy == 0 && da == 0) {
                    posX = tgtX;
                    posY = tgtY;
                    continue;
                }

                // Speed across a sub-segment follows constant acceleration, so it
                // is linear in v^2, not in distance (D2):
                // v(f) = sqrt(v0^2 + f*(v1^2-v0^2)). Interpolating linearly in f
                // makes each sub-segment's mean wrong and the error grows the
                // harder the pair subdivides — up to 1.51x at dvMax=0.75. Under
                // this form each sub-time is exact and they sum back to the
                // undivided pair time.
                const double v0 = subV(a.v, b.v, (j - 1) / k);
                const double v1 = subV(a.v, b.v, f);
                const double vbar = 0.5 * (v0 + v1);
                const double iv = interval(vbar, axes, options.vMin, dx, dy, 0, da);
                const uint32_t flags = segFinal ? MICRO_PATH_END : 0;
                out.push_back(microSegment(axes.x.invert ? -dx : dx,
                                           axes.y.invert ? -dy : dy,
                                           0,
                                           axes.a.invert ? -da : da,
                                           iv, flags));
                endIdx = static_cast<long long>(out.size()) - 1;
                if (segFinal) endEmitted = true;
                aPhys += da;
                posX = tgtX;
                posY = tgtY;
            }

            theta = b.s.theta;

            // lift-pivot-lower at the corner we just arrived at (v is ~0 here)
            if (isCorner) {
                const double daTrue = jsRound(dtheta * aSpd);
                if (daTrue != 0) {
                    const std::vector<MicroSegment> p =
                        pivot(daTrue, lift, zSteps, axes, options.zFeed, options.zAccel,
                              options.slew);
                    out.insert(out.end(), p.begin(), p.end());
                    aPhys += daTrue;
                }
                aAccum = aPhys;
            }
        }

        // Every subpath ends with exactly one PATH_END. If the final sub-step
        // moved nothing it was skipped, so re-home the marker onto the last
        // segment this subpath did emit — cutting if there was one, otherwise
        // the Z-lower/pre-orient that opened it. A subpath that emitted nothing
        // at all is not representable and would silently vanish from the stream.
        if (!endEmitted) {
            if (endIdx < 0) {
                throw std::runtime_error(
                    "discretize: subpath produced no motion at all — cannot place "
                    "its PATH_END. Upstream emitted a degenerate subpath.");
            }
            out[static_cast<size_t>(endIdx)].flags |= MICRO_PATH_END;
        }

        if (lift) appendAll(out, zMove(+zSteps, axes, options.zFeed, options.zAccel)); // raise after the stroke
    }

    return out;
}

} // namespace motion
