/**
 * Trapezoid contract: the profile covers exactly its length, respects its
 * speed and acceleration bounds, and is continuous in position and speed.
 */

#include "doctest.h"

#include <planner/trapezoid.h>

#include <math.h>

using planner::makeTrapezoid;
using planner::Trapezoid;

static void checkProfile(const Trapezoid& tr, float v_max) {
    const float tol = 1e-3f;
    CHECK(tr.position(tr.duration()) == doctest::Approx(tr.length).epsilon(1e-5));
    CHECK(tr.v_cruise <= v_max + tol);

    // Sample finely: bounded speed and accel, monotonic position.
    const int n = 2000;
    const float dt = tr.duration() / n;
    float prev_s = 0, prev_v = tr.v_entry;
    for (int i = 1; i <= n; i++) {
        const float t = dt * i;
        const float s = tr.position(t);
        const float v = tr.velocity(t);
        CHECK(s >= prev_s - 1e-5f);
        CHECK(v <= tr.v_cruise + tol);
        CHECK(fabsf(v - prev_v) <= tr.accel * dt * 1.01f + tol);
        // Position is the integral of speed.
        CHECK(s - prev_s == doctest::Approx(0.5f * (v + prev_v) * dt).epsilon(1e-2).scale(tr.length));
        prev_s = s;
        prev_v = v;
    }
}

TEST_CASE("full trapezoid reaches cruise and ends at rest") {
    const Trapezoid tr = makeTrapezoid(100, 1000, 0, 100 * 100, 0);
    CHECK(tr.v_cruise == doctest::Approx(100));
    CHECK(tr.t_cruise > 0);
    CHECK(tr.v_exit == 0);
    checkProfile(tr, 100);
}

TEST_CASE("short block becomes a triangle below v_max") {
    const Trapezoid tr = makeTrapezoid(1, 1000, 0, 500 * 500, 0);
    CHECK(tr.t_cruise == 0);
    CHECK(tr.v_cruise == doctest::Approx(sqrtf(1000)));
    checkProfile(tr, 500);
}

TEST_CASE("entry and exit speeds are honoured") {
    const Trapezoid tr = makeTrapezoid(20, 2000, 50 * 50, 150 * 150, 80 * 80);
    CHECK(tr.v_entry == doctest::Approx(50));
    CHECK(tr.v_exit == doctest::Approx(80));
    CHECK(tr.velocity(0) == doctest::Approx(50));
    CHECK(tr.velocity(tr.duration()) == doctest::Approx(80));
    checkProfile(tr, 150);
}

TEST_CASE("pure deceleration block") {
    // Entry exactly the speed that decelerates to rest over the length.
    const float a = 1000, L = 5;
    const Trapezoid tr = makeTrapezoid(L, a, 2 * a * L, 1e6f, 0);
    CHECK(tr.t_acc == doctest::Approx(0).epsilon(1e-4));
    CHECK(tr.t_cruise == doctest::Approx(0).epsilon(1e-4));
    checkProfile(tr, 1000);
}

TEST_CASE("cruise-only block") {
    const Trapezoid tr = makeTrapezoid(10, 1000, 100 * 100, 100 * 100, 100 * 100);
    CHECK(tr.duration() == doctest::Approx(0.1));
    CHECK(tr.position(0.05f) == doctest::Approx(5));
}

TEST_CASE("position clamps outside the block's time") {
    const Trapezoid tr = makeTrapezoid(10, 1000, 0, 100 * 100, 0);
    CHECK(tr.position(-1) == 0);
    CHECK(tr.position(tr.duration() + 1) == 10);
}
