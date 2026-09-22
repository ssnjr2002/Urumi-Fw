/**
 * flatten.cpp — see motion/flatten.h.
 *
 * Transcribed from web/src/toolpath/flatten.ts. Expression association and
 * comparison direction are load-bearing; see lib/motion/geometry.cpp.
 */

#include "motion/flatten.h"
#include "motion/jsmath.h"

#include <cmath>
#include <limits>

namespace motion {

namespace {

double tangentDeg(const CubicBezier& c, double t, double fallback = 0) {
    const Pt d1 = bezierDeriv1(c, t);
    if (d1.x * d1.x + d1.y * d1.y < 1e-20) return fallback;
    return (jsAtan2(d1.y, d1.x) * 180) / PI;
}

/**
 * Geometry-only adaptive step: min of three caps (chord, spacing, tangent).
 *
 * PREDICTION ONLY. Every cap here is evaluated at the step's START and then
 * applied across the whole step, so where the curve speeds up or bends harder
 * over that interval the realised value overshoots — systematically, not as
 * float noise (`snake.svg`: 142 of 356 steps over `dsMax`). `tsForCurve` is
 * what turns these predictions into bounds.
 *
 * The epsilon guards are the other half of the problem (audit F1). Both are
 * gated on "is this quantity measurable", when at a cusp the correct behaviour
 * is the opposite: near-zero speed is exactly where the tangent is least stable
 * and the cap matters most. Left as they are here — deliberately, because the
 * fix belongs in the enforcement loop rather than in a second epsilon — so a
 * cusp yields dtMax from this function and gets cut down by measurement instead
 * of by prediction.
 */
double dtAt(const CubicBezier& c, double t, double chordTol, double dsMax,
            double dthetaMax, double dtMax) {
    double dt = dtMax;
    const Pt d1 = bezierDeriv1(c, t);
    const double speed = jsHypot(d1.x, d1.y);
    const Pt d2 = bezierDeriv2(c, t);
    const double mag2 = d2.x * d2.x + d2.y * d2.y;
    if (mag2 > 1e-20) {
        dt = jsMin(dt, std::sqrt((8 * chordTol) / std::sqrt(mag2)));
    }
    if (speed > 1e-12) {
        dt = jsMin(dt, dsMax / speed);
        const double k = curvature(c, t);
        if (k > 1e-9) {
            dt = jsMin(dt, ((dthetaMax * PI) / 180) / (k * speed));
        }
    }
    return dt;
}

/**
 * Parameter values [0..1] at which to sample one curve (both ends inclusive).
 *
 * Each candidate step is proposed by `dtAt` and then MEASURED: the realised
 * chord and the realised tangent turn are computed from the two endpoints, and
 * a step that overshot either cap is halved and re-measured (audit F7 option 1,
 * chosen deliberately over restating the caps as targets). Enforcement also
 * closes F1 without a second epsilon rule — a cusp, where `dtAt`'s guards skip
 * both caps and return `dtMax`, is now cut down by the turn it actually makes
 * instead of being stepped straight over.
 *
 * `maxRefine` bounds the halving, so a pathological curve costs bounded extra
 * samples rather than unbounded ones. On exhaustion the step is taken anyway:
 * the caps are enforced as far as the sample budget allows, and no further.
 */
std::vector<double> tsForCurve(const CubicBezier& c, double chordTol, double dsMax,
                               double dthetaMax, double dtMax, double dtMin,
                               int maxRefine) {
    std::vector<double> ts{0.0};
    double t = 0;
    while (t < 1) {
        double dt = jsMax(dtMin, dtAt(c, t, chordTol, dsMax, dthetaMax, dtMax));
        if (maxRefine > 0) {
            const Pt p0 = bezierPoint(c, t);
            const double th0 = tangentDeg(c, t);
            double prevTurn = std::numeric_limits<double>::infinity();
            for (int r = 0; r < maxRefine; r++) {
                const double tEnd = jsMin(t + dt, 1);
                const Pt p1 = bezierPoint(c, tEnd);
                const double chord = jsHypot(p1.x - p0.x, p1.y - p0.y);
                const double turn = std::fabs(angleDelta(th0, tangentDeg(c, tEnd, th0)));
                if (chord <= dsMax + 1e-12 && turn <= dthetaMax + 1e-12) break;
                if (dt <= dtMin) break;
                // A TRUE cusp cannot be resolved by sampling harder: the tangent
                // reverses at a single parameter value, so the realised turn
                // tends to 180 deg no matter how small the step gets. Halving
                // further would buy samples and change nothing. Stop, and let
                // the 180 deg jump be read as the CORNER it is — constrain stops
                // there and discretize pivots (F2/C1). This is the correction to
                // F1's original premise that "a cusp must force fine sampling".
                if (chord <= dsMax + 1e-12 && turn > dthetaMax && turn >= prevTurn * 0.99) break;
                prevTurn = turn;
                dt = jsMax(dtMin, dt / 2);
            }
        }
        t = jsMin(t + dt, 1);
        ts.push_back(t);
    }
    return ts;
}

} // namespace

std::vector<Sample> flatten(
    const std::vector<std::vector<CubicBezier>>& subpaths,
    const FlattenOptions& options) {
    const double chordTol = options.chordTol;
    const double dsMax = options.dsMax;
    const double dthetaMax = options.dthetaMax;
    const double dtMax = options.dtMax;
    const double dtMin = options.dtMin;
    const int maxRefine = options.maxRefine;

    std::vector<Sample> out;

    for (const std::vector<CubicBezier>& subpath : subpaths) {
        if (subpath.empty()) continue;
        const size_t subStart = out.size();
        double prevTheta = tangentDeg(subpath[0], 0);

        for (size_t ci = 0; ci < subpath.size(); ci++) {
            const CubicBezier& c = subpath[ci];
            const std::vector<double> ts =
                tsForCurve(c, chordTol, dsMax, dthetaMax, dtMax, dtMin, maxRefine);
            for (size_t k = 0; k < ts.size(); k++) {
                const double t = ts[k];
                const Pt p = bezierPoint(c, t);
                const double theta = tangentDeg(c, t, prevTheta);
                const double kappa = curvature(c, t);
                uint32_t flags = 0;
                if (ci > 0 && k == 0) flags |= CURVE_BOUNDARY;
                out.push_back(Sample{p.x, p.y, theta, kappa, 0.0, flags});
                prevTheta = theta;
            }
        }

        // mark subpath ends
        out[subStart].flags |= PATH_START;
        out[out.size() - 1].flags |= PATH_END;

        // fill ds = chord to next sample, within this subpath only
        for (size_t i = subStart; i + 1 < out.size(); i++) {
            const Sample& next = out[i + 1];
            out[i].ds = jsHypot(next.x - out[i].x, next.y - out[i].y);
        }
        // last sample of subpath: ds stays 0 (set at creation)
    }

    return out;
}

} // namespace motion
