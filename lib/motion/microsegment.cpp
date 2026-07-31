#include "motion/microsegment.h"

#include "motion/jsmath.h"

#include <cmath>

namespace motion {

namespace {

/**
 * The plain major-axis rate: cycles per step so the axis turns at vv * xSpu
 * steps/s.
 *
 * The `stepRate < 1e-6` guard returns a full second rather than dividing. It
 * cannot be reached through interval()'s public entry points with a positive
 * vMin — vv is floored at vMin first — but vMin is a caller-supplied number
 * and the guard is what stops a zero one from producing an infinity here.
 */
double majorRate(double vv, const ResolvedAxes& axes) {
    const double stepRate = vv * axes.x.stepsPerUnit;
    if (stepRate < 1e-6) return axes.fCpu;
    return jsMax(1, jsMin(std::trunc(axes.fCpu / stepRate), axes.fCpu));
}

} // namespace

double interval(double v, const ResolvedAxes& axes, double vMin) {
    return majorRate(jsMax(v, vMin), axes);
}

double interval(double v, const ResolvedAxes& axes, double vMin,
                double dx, double dy, double dz, double da) {
    const double vv = jsMax(v, vMin);
    const double xSpu = axes.x.stepsPerUnit;
    const double ySpu = axes.y.stepsPerUnit;
    const double fCpu = axes.fCpu;

    const double major =
        jsMax(jsMax(std::fabs(dx), std::fabs(dy)), jsMax(std::fabs(dz), std::fabs(da)));
    if (major == 0) return fCpu;

    // Per-axis rate floor: no axis may exceed maxFeed * stepsPerUnit. A 0
    // ceiling is "uncapped" (axes.h), so R <= 0 skips the axis rather than
    // flooring the segment at infinity.
    double tRate = 0;
    const double deltas[4] = {dx, dy, dz, da};
    const AxisConfig* cfgs[4] = {&axes.x, &axes.y, &axes.z, &axes.a};
    for (int i = 0; i < 4; i++) {
        const double R = cfgs[i]->maxFeed * cfgs[i]->stepsPerUnit;
        if (R > 0 && deltas[i] != 0) {
            tRate = jsMax(tRate, std::fabs(deltas[i]) / R);
        }
    }

    const double distMm = jsHypot(dx / xSpu, dy / ySpu); // true XY tool distance
    if (distMm < 1e-9) {
        // Pure rotation or Z move — no XY feed to govern. Use the rate floor if
        // one applies, otherwise fall back to the major-axis rate.
        if (tRate > 0) {
            const double cycles = (tRate / major) * fCpu;
            return jsMax(1, jsMin(std::trunc(cycles), fCpu));
        }
        return majorRate(vv, axes);
    }

    const double segTime = jsMax(distMm / vv, tRate); // feed time, floored by axis rates
    const double cycles = (segTime / major) * fCpu;   // per major-axis step
    return jsMax(1, jsMin(std::trunc(cycles), fCpu));
}

} // namespace motion
