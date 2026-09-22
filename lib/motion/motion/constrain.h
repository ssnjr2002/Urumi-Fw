/**
 * constrain.h — stage 5: assign each Sample a LOCAL velocity ceiling.
 *
 * Transcribed from web/src/toolpath/constrain.ts.
 *
 * Pure, per-sample, no propagation — "how fast could the tool ever go right
 * here?" The forward/backward feasibility sweeps that turn these ceilings into
 * a reachable profile are stage 6 (plan).
 *
 * Ceiling at sample i = min of:
 *   feedMax                         programmed cruise limit
 *   sqrt(aMax / kappa_i)            centripetal (XY radial accel) — LOCAL kappa
 *   rad(aRateDegS) / kappa_i        A-axis slew: a tangential tool rotates at
 *                                   dtheta/dt = kappa*v
 *   sqrt(rad(aAccelDegS2) / |k'|)   A angular-accel, curvature-gradient term
 *   junction-deviation cap          at a curve-boundary tangent jump below the
 *                                   corner threshold (GRBL-style cornering)
 *   0                               at a tangent jump >= corner threshold, at a
 *                                   forced stop, or below vMin
 *
 * Using LOCAL kappa per sample is the whole point: a degenerate curvature spike
 * caps ONE sample, not a whole curve.
 */

#ifndef MOTION_CONSTRAIN_H
#define MOTION_CONSTRAIN_H

#include "motion/sample.h"

#include <cstddef>
#include <set>
#include <vector>

namespace motion {

/**
 * Stage 5 output: a Sample with a local velocity ceiling filled in.
 *
 * The TypeScript expresses the stage progression as an interface extension
 * (`ConstrainedSample extends Sample`); here it is composition, because the
 * differential test compares the Sample fields for equality and a flat struct
 * with a base subobject makes that comparison read worse, not better.
 */
struct ConstrainedSample {
    Sample s;
    double vCeiling;
};

/**
 * Constrain stage options.
 *
 * The TypeScript distinguishes "absent" from "zero" with optional properties;
 * the port uses zero as the disabled sentinel throughout, which is safe only
 * because the destructuring defaults on the TS side are themselves 0 for
 * `aRateDegS`, `aAccelDegS2` and `vMin`, and every one of those is then gated
 * on `> 0`.
 *
 * `cornerStopAngleDeg` is the exception and needs a real presence flag:
 * undefined means "no forced corner stops", but 0 would mean "stop at EVERY
 * sample", since the test is `>=`. Collapsing the two would be a silent
 * catastrophe rather than a rounding difference, hence the explicit bool.
 */
struct ConstrainOptions {
    /** Programmed cruise ceiling (mm/s). Source: resolved pathFeed. */
    double feedMax = 0;
    /** Lateral accel for the centripetal cap (mm/s^2). Source: min(x,y maxAccel). */
    double aMax = 0;
    /** Corner-rounding budget (mm). Source: QualityConfig.junctionDeviation. */
    double junctionDeviation = 0;

    /** Tangential tool A-slew ceiling (deg/s); 0 disables. */
    double aRateDegS = 0;
    /** Tangential tool A angular-accel ceiling (deg/s^2); 0 disables. */
    double aAccelDegS2 = 0;

    /** Boundary tangent jump (deg) at/above which vCeiling is forced to 0. */
    double cornerStopAngleDeg = 0;
    bool hasCornerStopAngle = false;

    /**
     * Sample indices forced to vCeiling 0 regardless of geometry — a stop the
     * CALLER needs for a reason the geometry knows nothing about (a duty-limited
     * tool that must release its enable line inside a budget). Out-of-range
     * entries are ignored.
     */
    std::set<size_t> forcedStops;

    /**
     * Execution speed floor (mm/s); 0 disables. A ceiling below this is not a
     * ceiling — it is a stop that has not admitted it (audit C1). Source:
     * QualityConfig.vMin, the SAME value discretize clamps the step interval to.
     */
    double vMin = 0;
};

/**
 * GRBL junction-deviation cornering speed for a tangent turn of turnDeg across
 * a near-zero-length boundary. Models the corner as a circular arc deviating
 * from the exact vertex by at most `deviation` mm. Straight -> feedMax;
 * reversal -> 0.
 */
double junctionCap(double turnDeg, double aLat, double deviation, double feedMax);

/** Assign each sample a local velocity ceiling. Pure; returns a fresh vector. */
std::vector<ConstrainedSample> constrain(
    const std::vector<Sample>& samples,
    const ConstrainOptions& options);

} // namespace motion

#endif // MOTION_CONSTRAIN_H
