/**
 * constrain.cpp — stage 5, transcribed from web/src/toolpath/constrain.ts.
 *
 * BIT-EXACT transcription, not an improvement. See docs/planner_audit.md,
 * "Numeric porting rule". In particular the min() chain's ASSOCIATION is
 * load-bearing: `min` on doubles is not associative in the presence of signed
 * zero, and reordering the caps changes which of two equal-magnitude ceilings
 * survives. Keep the order the TypeScript has.
 */

#include "motion/constrain.h"

#include "motion/geometry.h"
#include "motion/jsmath.h"

#include <cmath>

namespace motion {

namespace {

// k-discontinuity flags: a finite difference of curvature must not straddle a
// curve/subpath boundary (kappa is discontinuous there).
constexpr uint32_t KAPPA_BREAK = PATH_START | PATH_END | CURVE_BOUNDARY;

/**
 * |dkappa/ds| at sample i by central difference, 0 where it would straddle a
 * kappa-discontinuity or a ~zero-length span. Units: rad/mm^2.
 */
double kappaPrime(const std::vector<Sample>& samples, size_t i) {
    const size_t n = samples.size();
    if (i == 0 || i == n - 1) return 0;
    // The flag test looks at i and i+1 but NOT i-1, while the span below runs
    // i-1..i+1. That asymmetry looks like an off-by-one and is not: a flag
    // marks a sample as the FIRST after a discontinuity, so the break sits
    // between i-1 and i (flag at i) or between i and i+1 (flag at i+1) —
    // exactly the two ways the span can straddle one. A flag at i-1 puts the
    // break between i-2 and i-1, outside the span, and disqualifying it would
    // discard a difference that is perfectly well defined.
    if ((samples[i].flags & KAPPA_BREAK) || (samples[i + 1].flags & KAPPA_BREAK)) return 0;
    const double span = samples[i - 1].ds + samples[i].ds; // arc length i-1 -> i+1
    if (span < 1e-6) return 0;
    return std::fabs(samples[i + 1].kappa - samples[i - 1].kappa) / span;
}

} // namespace

double junctionCap(double turnDeg, double aLat, double deviation, double feedMax) {
    // jsCos, not std::cos: mingw's cos disagrees with V8 on 2.8% of inputs by
    // up to 26 ULP. See jsmath.cpp.
    const double halfCos = jsCos((std::fabs(turnDeg) * PI / 180) / 2);
    if (halfCos >= 1 - 1e-9) return feedMax;
    if (halfCos <= 1e-9) return 0;
    const double radius = (deviation * halfCos) / (1 - halfCos);
    return jsMin(feedMax, std::sqrt(aLat * radius));
}

std::vector<ConstrainedSample> constrain(
    const std::vector<Sample>& samples,
    const ConstrainOptions& options) {

    const double aRateRad =
        options.aRateDegS > 0 ? (options.aRateDegS * PI) / 180 : 0;
    const double aAccRad =
        options.aAccelDegS2 > 0 ? (options.aAccelDegS2 * PI) / 180 : 0;

    std::vector<ConstrainedSample> out;
    out.reserve(samples.size());

    for (size_t i = 0; i < samples.size(); i++) {
        const Sample& s = samples[i];

        // Checked first and pushed immediately: a forced stop is an OVERRIDE,
        // not another candidate ceiling to min() against. Nothing below can
        // raise a zero, but going through the motions would invite a later edit
        // to reorder the min() chain and quietly resurrect the sample.
        if (options.forcedStops.count(i) != 0) {
            out.push_back({s, 0.0});
            continue;
        }

        double cap = options.feedMax;
        if (s.kappa > 1e-9) {
            cap = jsMin(cap, std::sqrt(options.aMax / s.kappa)); // centripetal (XY)
            if (aRateRad > 0) {
                cap = jsMin(cap, aRateRad / s.kappa); // A slew (velocity)
            }
        }

        // A angular-accel, curvature-gradient term: v <= sqrt(alpha_max / |k'|)
        if (aAccRad > 0) {
            const double kp = kappaPrime(samples, i);
            if (kp > 1e-9) {
                cap = jsMin(cap, std::sqrt(aAccRad / kp));
            }
        }

        // Tangent jump — the corner signal.
        //
        // The STOP test is ungated (audit F2/D4): discretize computes dtheta
        // between consecutive samples with no flag test at all, so an
        // intra-curve cusp is a corner there. The junction-deviation cap stays
        // gated on CURVE_BOUNDARY: it models a VERTEX between two curves, and
        // applying it to ordinary in-curve samples would double-count the
        // centripetal cap, which already owns continuous turning.
        if (i > 0) {
            const double turn = angleDelta(samples[i - 1].theta, s.theta);
            if (options.hasCornerStopAngle &&
                std::fabs(turn) >= options.cornerStopAngleDeg) {
                cap = 0;
            } else if ((s.flags & CURVE_BOUNDARY) && std::fabs(turn) > 1e-6) {
                cap = jsMin(cap, junctionCap(turn, options.aMax,
                                             options.junctionDeviation,
                                             options.feedMax));
            }
        }

        // A ceiling under the floor the machine will actually execute is a stop
        // (audit C1). Forcing it to 0 makes plan decelerate into it and
        // accelerate out, so the executed profile is the planned one.
        if (options.vMin > 0 && cap < options.vMin) cap = 0;

        out.push_back({s, cap});
    }

    return out;
}

} // namespace motion
