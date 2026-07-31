/**
 * machine.h — the bench machine and the two tool profiles, as the contract
 * tests see them, plus the flatten->constrain->plan pipeline the stage-7 tests
 * run their fixtures through.
 *
 * Duplicated from web/src/config/fixtures.ts (defaultMachine), defaults.ts and
 * tools.ts by hand, for the same reason quality.h is: a generated copy tracks
 * the code under test silently, and a contract test whose machine moves with
 * the implementation is not a contract.
 *
 * The numbers are the physical bench machine, DM542 at 1/32 microstepping:
 *   X/Y  GT2 20T pulley, 40 mm/rev  ->  160 steps/mm
 *   Z    lead screw               ->  1200 steps/mm
 *   A    tangential rotary        ->  51.667 steps/deg
 *
 * They are NOT round, and that is the point — the TypeScript's discretize
 * tests run on this machine, so the port's fixtures have to as well or the two
 * suites are not asking the same question. 51.667 steps/deg in particular is
 * what makes the A rounding in the tracking accumulator observable at all.
 */

#ifndef TEST_SUPPORT_MACHINE_H
#define TEST_SUPPORT_MACHINE_H

#include "motion/constrain.h"
#include "motion/discretize.h"
#include "motion/flatten.h"
#include "motion/geometry.h"
#include "motion/plan.h"
#include "support/quality.h"

#include <vector>

namespace machine {

/** Programmed cut feed (mm/s) — machine.path.feed, and rapid.feed too. */
constexpr double FEED = 80.0;
/** Lateral accel budget (mm/s^2) — min(x,y maxAccel) on this machine. */
constexpr double A_MAX = 1000.0;
/** Z engage feed (mm/s) — DEFAULTS.machine.z.feed. */
constexpr double Z_FEED = 20.0;
/** Z engage accel (mm/s^2) — the axis ceiling; zMove clamps the feed to 10. */
constexpr double Z_ACCEL = 300.0;

inline const motion::ResolvedAxes& axes() {
    static const motion::ResolvedAxes v = [] {
        motion::ResolvedAxes r;
        r.x = motion::AxisConfig{160.0, 80.0, 1000.0, true};
        r.y = motion::AxisConfig{160.0, 80.0, 1000.0, false};
        // Z declares no accel — the shipped config's placeholder, and the
        // reason zMove is unramped (H3). A must declare both or aMove refuses.
        // Z declares 300 mm/s^2 — PROVISIONAL, see planner_audit H3. It used
        // to be a 0 placeholder, which is why zMove was unramped.
        r.z = motion::AxisConfig{1200.0, 10.0, 300.0, true};
        r.a = motion::AxisConfig{51.667, 100.0, 2000.0, true};
        r.fCpu = 150000000.0;
        return r;
    }();
    return v;
}

/**
 * The default machine declares no standalone-A slew target, so aMove falls
 * through to the A axis ceiling. Both flags absent, never 0 — see axes.h.
 */
inline motion::OpTarget noSlew() { return motion::OpTarget{}; }

/** KNIFE: tangential, unwinding, corners at 20 degrees. */
inline motion::DiscretizeOptions knife() {
    motion::DiscretizeOptions o;
    o.axes = axes();
    o.tangential = true;
    o.unwind = true;
    o.cornerAngleDeg = 20.0;
    o.offsetMm = 0.0;
    o.dvMax = quality::DV_MAX;
    o.vMin = quality::V_MIN;
    o.jogFeed = FEED;
    o.liftHeight = 0.0;
    o.zFeed = Z_FEED;
    o.zAccel = Z_ACCEL;
    o.slew = noSlew();
    return o;
}

/** PEN: no tangent tracking, no lift, no pivot. */
inline motion::DiscretizeOptions pen() {
    motion::DiscretizeOptions o = knife();
    o.tangential = false;
    o.unwind = false;
    return o;
}

/**
 * Flatten -> constrain -> plan, with exactly the options the production bridge
 * (compileBlock) passes. Mirrors `planFor` in the TypeScript test, including
 * the vMin that audit C1 established the tests must pass or they measure a
 * pipeline nobody ships.
 */
inline std::vector<motion::PlannedSample> planFor(
    const std::vector<std::vector<motion::CubicBezier>>& subpaths,
    const motion::DiscretizeOptions& tool) {
    const std::vector<motion::Sample> s = motion::flatten(subpaths, quality::flattenOpts());

    motion::ConstrainOptions c;
    c.feedMax = FEED;
    c.aMax = A_MAX;
    c.junctionDeviation = quality::JUNCTION_DEVIATION;
    if (tool.tangential) {
        c.aRateDegS = axes().a.maxFeed;
        c.aAccelDegS2 = axes().a.maxAccel;
        c.cornerStopAngleDeg = tool.cornerAngleDeg;
        c.hasCornerStopAngle = true;
    }
    c.vMin = quality::V_MIN;

    motion::PlanOptions p;
    p.xAccel = axes().x.maxAccel;
    p.yAccel = axes().y.maxAccel;
    p.aAccelDegS2 = axes().a.maxAccel;
    p.aMax = A_MAX;

    return motion::plan(motion::constrain(s, c), p);
}

/** One fixture, one tool, all the way to MicroSegments. */
inline std::vector<motion::MicroSegment> prep(
    const std::vector<std::vector<motion::CubicBezier>>& subpaths,
    const motion::DiscretizeOptions& tool) {
    return motion::discretize(planFor(subpaths, tool), tool);
}

} // namespace machine

#endif // TEST_SUPPORT_MACHINE_H
