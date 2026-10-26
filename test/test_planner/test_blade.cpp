/**
 * The knife axis d mm ahead of the tip: XY's limits at the axis on paths whose
 * A turns (bladeCaps), and the ring applying them from setBlade.
 */

#include "doctest.h"

#include <planner/planner.h>

#include <math.h>

using namespace planner;

namespace {

AxisLimits lim() {
    AxisLimits l;
    l.max_feed[0] = l.max_feed[1] = 500;
    l.max_accel[0] = l.max_accel[1] = 2000;
    l.max_feed[3] = 36000;   // deg/s, high enough that XY binds
    l.max_accel[3] = 1e6f;
    return l;
}

Path aPath(float deg) { return axisPath(AXES_A, deg, 1e9f, lim()); }

}  // namespace

TEST_CASE("blade: no offset leaves a path as it is") {
    Path p = aPath(90), q = p;
    bladeCaps(p, 0, 0, lim());
    CHECK(p.v_max_sqr == q.v_max_sqr);
    CHECK(p.accel == q.accel);
}

TEST_CASE("blade: an A-only swivel keeps the axis's XY speed and accel in limits") {
    const float d = 10;
    Path p = aPath(90);
    bladeCaps(p, d, 0, lim());
    const float w = sqrtf(p.v_max_sqr) * kRadPerDeg;   // rad/s
    CHECK(d * w <= doctest::Approx(500));
    CHECK(d * w * w <= doctest::Approx(2000));
    CHECK(d * p.accel * kRadPerDeg <= doctest::Approx(2000));
}

TEST_CASE("blade: a turning XY path slows by 1 + d·rate") {
    Path p;
    p.axes = AXES_XY;
    p.v_max_sqr = 100 * 100;
    p.accel = 1000;
    const float d = 0.5f, rate = 0.2f;   // rad/mm
    bladeCaps(p, d, rate, lim());
    CHECK(sqrtf(p.v_max_sqr) == doctest::Approx(100 / 1.1f));
    CHECK(p.accel == doctest::Approx(1000 / 1.1f));

    Path q;
    q.v_max_sqr = 100 * 100;
    bladeCaps(q, d, 0, lim());   // A does not turn: the axis moves as the tip
    CHECK(q.v_max_sqr == 100 * 100);
}

TEST_CASE("blade: the ring caps an A-only block from setBlade") {
    Planner p;
    p.reset(Pos());
    p.setBlade(10);
    Pos to;
    to.addA(90);
    REQUIRE(p.pushMove(to, 1e9f, lim(), 0.02f));
    const float w = sqrtf(p.oldest()->path.v_max_sqr) * kRadPerDeg;
    CHECK(10 * w * w <= doctest::Approx(2000));
}
