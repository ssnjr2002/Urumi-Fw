/**
 * Line contract: axis limits hold on every axis for any direction, and corner
 * speed falls monotonically from straight-on to reversal.
 */

#include "doctest.h"

#include <planner/line.h>

#include <math.h>

using namespace planner;

static AxisLimits limits(float fx, float fy, float ax, float ay) {
    AxisLimits l;
    l.max_feed[0] = fx;
    l.max_feed[1] = fy;
    l.max_accel[0] = ax;
    l.max_accel[1] = ay;
    return l;
}

TEST_CASE("axis-aligned line takes that axis's limits") {
    const Line ln = makeLine({0, 0}, {10, 0}, 1000, limits(200, 100, 3000, 1000));
    CHECK(ln.length == doctest::Approx(10));
    CHECK(sqrtf(ln.v_max_sqr) == doctest::Approx(200));
    CHECK(ln.accel == doctest::Approx(3000));
}

TEST_CASE("requested feed below the axis limits wins") {
    const Line ln = makeLine({0, 0}, {0, -5}, 50, limits(200, 100, 3000, 1000));
    CHECK(sqrtf(ln.v_max_sqr) == doctest::Approx(50));
    CHECK(ln.accel == doctest::Approx(1000));
}

TEST_CASE("per-axis components stay within limits in every direction") {
    const AxisLimits lim = limits(200, 100, 3000, 1000);
    for (int deg = 0; deg < 360; deg += 7) {
        const float th = deg * 3.14159265f / 180;
        const Line ln = makeLine({1, 2}, {1 + 10 * cosf(th), 2 + 10 * sinf(th)}, 1e6f, lim);
        const float v = sqrtf(ln.v_max_sqr);
        CHECK(v * fabsf(ln.dir.x) <= 200 * 1.0001f);
        CHECK(v * fabsf(ln.dir.y) <= 100 * 1.0001f);
        CHECK(ln.accel * fabsf(ln.dir.x) <= 3000 * 1.0001f);
        CHECK(ln.accel * fabsf(ln.dir.y) <= 1000 * 1.0001f);
        // And one axis is at its limit: the cap is not needlessly low.
        const bool at_limit = fabsf(v * fabsf(ln.dir.x) - 200) < 0.1f ||
                              fabsf(v * fabsf(ln.dir.y) - 100) < 0.1f;
        CHECK(at_limit);
    }
}

TEST_CASE("zero-length line reports length 0") {
    const Line ln = makeLine({3, 4}, {3, 4}, 100, limits(200, 200, 1000, 1000));
    CHECK(ln.length == 0);
}

TEST_CASE("junction speed: straight, reversal, and monotonic in between") {
    const AxisLimits lim = limits(500, 500, 2000, 2000);
    const Line in = makeLine({0, 0}, {10, 0}, 300, lim);

    const Line straight = makeLine({10, 0}, {20, 0}, 300, lim);
    CHECK(junctionMaxSqr(in, straight, 0.02f) == doctest::Approx(300 * 300));

    const Line back = makeLine({10, 0}, {0, 0}, 300, lim);
    CHECK(junctionMaxSqr(in, back, 0.02f) == 0);

    float prev = INFINITY;
    for (int deg = 1; deg < 180; deg += 5) {   // turn angle
        const float th = deg * 3.14159265f / 180;
        const Line out = makeLine({10, 0}, {10 + 10 * cosf(th), 10 * sinf(th)}, 300, lim);
        const float v = junctionMaxSqr(in, out, 0.02f);
        CHECK(v <= prev);
        CHECK(v <= 300 * 300);
        prev = v;
    }
}

TEST_CASE("right-angle junction matches the deviation formula") {
    const AxisLimits lim = limits(500, 500, 2000, 2000);
    const Line in = makeLine({0, 0}, {10, 0}, 300, lim);
    const Line out = makeLine({10, 0}, {10, 10}, 300, lim);
    const float s = sqrtf(0.5f);
    const float expected = 2000 * 0.05f * s / (1 - s);
    CHECK(junctionMaxSqr(in, out, 0.05f) == doctest::Approx(expected));
    // Larger deviation, faster corner.
    CHECK(junctionMaxSqr(in, out, 0.1f) > junctionMaxSqr(in, out, 0.05f));
}
