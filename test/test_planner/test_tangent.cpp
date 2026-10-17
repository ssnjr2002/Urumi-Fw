/**
 * A riding along XY: linear on a line, the heading on a tangential Bézier;
 * A's caps on both, the join where the turn rate steps, and the executor's
 * tick-to-tick tracking.
 */

#include "doctest.h"

#include <planner/executor.h>

#include <math.h>

using namespace planner;

namespace {

AxisLimits lim() {
    AxisLimits l;
    l.max_feed[0] = l.max_feed[1] = 500;
    l.max_accel[0] = l.max_accel[1] = 2000;
    l.max_feed[3] = 360;      // deg/s
    l.max_accel[3] = 2000;    // deg/s²
    return l;
}

const float kDev = 0.02f;
const float kDt = 0.001f;
const float kHandle = 0.5522847f;
const float kPi = 3.14159265f;

// Quarter circle about `c`, radius r, from angle a0, counter-clockwise or not.
Bezier quarter(Vec2 c, float r, float a0, bool ccw = true) {
    const float sweep = ccw ? kPi / 2 : -kPi / 2;
    const float h = r * kHandle * (ccw ? 1 : -1);
    const float c0 = cosf(a0), s0 = sinf(a0), c1 = cosf(a0 + sweep), s1 = sinf(a0 + sweep);
    const Vec2 p0 = {c.x + r * c0, c.y + r * s0};
    const Vec2 p3 = {c.x + r * c1, c.y + r * s1};
    const Vec2 p1 = {p0.x - h * s0, p0.y + h * c0};
    const Vec2 p2 = {p3.x + h * s1, p3.y - h * c1};
    Bezier b;
    REQUIRE(analyzeBezier(p0, p1, p2, p3, b) == BezierError::None);
    return b;
}

Pos atA(float x, float y, float a) {
    Pos p;
    p.x = x; p.y = y;
    p.addA(a);
    return p;
}

double aAbs(const Pos& p) { return p.turns * 360.0 + p.a; }

void plan(Planner& p, Executor& e) {
    for (int i = 0; i < 100; i++) {
        p.replan(e.clock());
        if (p.commit(e.clock())) return;
        e.adopt(p);
        e.tick(p, kDt);
    }
    FAIL("commit never accepted");
}

}  // namespace

TEST_CASE("tangent: a curve's turn is its unwrapped heading change") {
    const BezierTurn ccw = bezierTurn(quarter({0, 0}, 20, -kPi / 2));
    CHECK(ccw.total == doctest::Approx(90).epsilon(1e-3));
    CHECK(ccw.lo == doctest::Approx(0).scale(1));
    CHECK(ccw.hi == doctest::Approx(90).epsilon(1e-3));

    const BezierTurn cw = bezierTurn(quarter({0, 0}, 20, kPi / 2, false));
    CHECK(cw.total == doctest::Approx(-90).epsilon(1e-3));
    CHECK(cw.lo == doctest::Approx(-90).epsilon(1e-3));

    // Across ±180: heading 135° turning to 225°.
    const BezierTurn back = bezierTurn(quarter({0, 0}, 20, kPi / 4));
    CHECK(back.total == doctest::Approx(90).epsilon(1e-3));

    // From halfway: the rest of the turn.
    const Bezier q = quarter({0, 0}, 20, 0);
    CHECK(bezierTurn(q, q.length / 2).total == doctest::Approx(45).epsilon(1e-2));

    // An S leaving at 45°: right to −26.6°, then back to 45°.
    Bezier s;
    REQUIRE(analyzeBezier({0, 0}, {10, 10}, {20, -10}, {30, 0}, s) == BezierError::None);
    const BezierTurn st = bezierTurn(s);
    CHECK(st.total == doctest::Approx(0).scale(1));
    CHECK(st.hi == doctest::Approx(0).scale(1));
    CHECK(st.lo == doctest::Approx(-71.565).epsilon(1e-3));   // B'(0.5) = (30, -15)
}

TEST_CASE("tangent: A's limits cap the curve") {
    const AxisLimits l = lim();
    const Bezier b = quarter({0, 0}, 5, 0);   // κ = 0.2 /mm
    const Path plain = pathOf(b, 500, l);
    const Path tan = pathOf(b, 500, l, true);
    const float w = 360 * kRadPerDeg, alpha = 2000 * kRadPerDeg;
    CHECK(sqrtf(tan.v_max_sqr) <= w / b.kappa_max * 1.0001f);
    CHECK(tan.v_max_sqr <= kAAccelShare * alpha / b.dkappa_max * 1.0001f + 1e-3f);
    CHECK(tan.accel == doctest::Approx((1 - kAAccelShare) * alpha / b.kappa_max));
    CHECK(tan.v_max_sqr < plain.v_max_sqr);
    CHECK(tan.a_rate_start == doctest::Approx(b.kappa_start));
    CHECK(plain.a_rate_start == 0);
}

TEST_CASE("tangent: A riding along a line caps it by A's share") {
    Planner p;
    p.reset(Pos());
    REQUIRE(p.pushMove(atA(10, 0, 90), 500, lim(), kDev));   // 9 deg per mm
    const Path& path = p.oldest()->path;
    CHECK(sqrtf(path.v_max_sqr) == doctest::Approx(360.0f / 9));
    CHECK(path.accel == doctest::Approx(2000.0f / 9));
    CHECK(path.a_rate_start == doctest::Approx(9 * kRadPerDeg));

    AxisLimits none = lim();
    none.max_feed[3] = none.max_accel[3] = 0;
    CHECK_FALSE(p.pushMove(atA(20, 0, 180), 500, none, kDev));
}

TEST_CASE("tangent: a join where the turn rate steps is capped by e_max") {
    const AxisLimits l = lim();
    const Bezier arc = quarter({50, 20}, 20, -kPi / 2);   // κ = 0.05, tangent to the X axis
    const Path line = pathOf(makeLine({0, 0}, {50, 0}, 500, l));
    const float v = aJump(l) / arc.kappa_start;
    CHECK(junctionMaxSqr(line, pathOf(arc, 500, l, true), kDev) ==
          doctest::Approx(v * v).epsilon(1e-3));
    CHECK(junctionMaxSqr(line, pathOf(arc, 500, l), kDev) > v * v * 10);
}

TEST_CASE("tangent: the executor follows the heading and lands on the planned A") {
    Planner p;
    Executor e;
    p.reset(Pos());
    e.reset(Pos());
    const Bezier arc = quarter({50, 20}, 20, -kPi / 2);
    const BezierTurn turn = bezierTurn(arc);
    REQUIRE(p.pushLine({50, 0}, 500, lim(), kDev));
    REQUIRE(p.pushBezierFrom(arc, 0, 500, lim(), kDev, PathCap(), &turn));
    REQUIRE(p.pushLine({70, 80}, 500, lim(), kDev));
    CHECK(aAbs(p.end()) == doctest::Approx(90).epsilon(1e-3));
    plan(p, e);

    double prev = 0;
    float worst = 0;
    for (int i = 0; i < 20000 && (p.count() || p.claimed()); i++) {
        e.adopt(p);
        e.tick(p, kDt);
        const Pos q = e.position();
        const double a = aAbs(q);
        CHECK(a >= prev - 1e-3);   // never turns back
        prev = a;
        if (q.x > 50.01f && q.y < 19.99f) {
            // On the arc, A is the heading: 90° past the radius.
            const float h = atan2f(q.y - 20, q.x - 50) / kRadPerDeg + 90;
            worst = fmaxf(worst, fabsf((float)a - h));
        }
    }
    CHECK(worst < 0.5f);
    CHECK(aAbs(e.position()) == doctest::Approx(90).epsilon(1e-4));
    CHECK(e.aMisses() == 0);
}

TEST_CASE("tangent: a hold on the curve resumes from where A stopped") {
    Planner p;
    Executor e;
    Pos start = atA(0, -20, 0);
    p.reset(start);
    e.reset(start);
    // Two turns of a circle, so A runs past 360.
    for (int k = 0; k < 8; k++) {
        const Bezier q = quarter({0, 0}, 20, -kPi / 2 + k * kPi / 2);
        const BezierTurn t = bezierTurn(q);
        REQUIRE(p.pushBezierFrom(q, 0, 200, lim(), kDev, PathCap(), &t));
    }
    CHECK(aAbs(p.end()) == doctest::Approx(720).epsilon(1e-3));
    plan(p, e);
    for (int i = 0; i < 400; i++) { e.adopt(p); e.tick(p, kDt); }
    e.hold();
    for (int i = 0; i < 2000 && e.state() != Executor::State::Held; i++) e.tick(p, kDt);
    REQUIRE(e.state() == Executor::State::Held);
    e.resume(p);
    for (int i = 0; i < 60000 && (p.count() || p.claimed()); i++) { e.adopt(p); e.tick(p, kDt); }
    CHECK(aAbs(e.position()) == doctest::Approx(720).epsilon(1e-4));
    CHECK(e.aMisses() == 0);
}

TEST_CASE("tangent: A turns linearly along a travel line") {
    Planner p;
    Executor e;
    p.reset(Pos());
    e.reset(Pos());
    REQUIRE(p.pushMove(atA(10, 0, -90), 500, lim(), kDev));
    plan(p, e);
    for (int i = 0; i < 20000 && (p.count() || p.claimed()); i++) {
        e.adopt(p);
        e.tick(p, kDt);
        const Pos q = e.position();
        if (p.claimed()) CHECK(aAbs(q) == doctest::Approx(-9.0 * q.x).epsilon(1e-3).scale(1));
    }
    CHECK(aAbs(e.position()) == doctest::Approx(-90));
}
