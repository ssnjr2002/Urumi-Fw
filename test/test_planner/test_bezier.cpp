/**
 * Bézier contract: analysis matches known geometry, the evaluator stays on the
 * curve at an even pace, and planned motion keeps the vector acceleration
 * within the axes' inscribed circle, holds and resumes included.
 */

#include "doctest.h"

#include <planner/executor.h>

#include <initializer_list>
#include <math.h>

using namespace planner;

static AxisLimits lim() {
    AxisLimits l;
    l.max_feed[0] = l.max_feed[1] = 500;
    l.max_accel[0] = l.max_accel[1] = 2000;
    return l;
}

static const float kDev = 0.02f;
static const float kDt = 0.001f;
static const float kKappa = 0.5522847f;   // circle handle factor
static const float kPi = 3.14159265f;

static float dist(Vec2 a, Vec2 b) { return hypotf(a.x - b.x, a.y - b.y); }

// Quarter circle about `c`, radius r, counter-clockwise from angle a0.
static Bezier quarter(Vec2 c, float r, float a0) {
    const float h = r * kKappa;
    const float c0 = cosf(a0), s0 = sinf(a0), c1 = cosf(a0 + kPi / 2), s1 = sinf(a0 + kPi / 2);
    const Vec2 p0 = {c.x + r * c0, c.y + r * s0};
    const Vec2 p3 = {c.x + r * c1, c.y + r * s1};
    const Vec2 p1 = {p0.x - h * s0, p0.y + h * c0};
    const Vec2 p2 = {p3.x + h * s1, p3.y - h * c1};
    Bezier b;
    REQUIRE(analyzeBezier(p0, p1, p2, p3, b) == BezierError::None);
    return b;
}

TEST_CASE("bezier: a quarter circle has the circle's length and curvature") {
    for (float r : {5.0f, 50.0f, 400.0f}) {
        CAPTURE(r);
        Bezier b;
        float err = 0;
        const float h = r * kKappa;
        REQUIRE(analyzeBezier({r, 0}, {r, h}, {h, r}, {0, r}, b, &err) == BezierError::None);
        CHECK(b.length == doctest::Approx(kPi * r / 2).epsilon(1e-3));
        CHECK(b.kappa_max == doctest::Approx(1 / r).epsilon(1e-2));
        CHECK(b.kappa_start > 0);   // counter-clockwise turns left
        CHECK(b.kappa_end > 0);
        CHECK(err < 1e-3f);
    }
}

TEST_CASE("bezier: t(s) runs from 0 to exactly 1 and never backwards") {
    const Bezier cases[] = {
        quarter({0, 0}, 20, 0),
        [] { Bezier b; REQUIRE(analyzeBezier({0, 0}, {20, 40}, {40, 40}, {60, 0}, b) == BezierError::None); return b; }(),
        [] { Bezier b; REQUIRE(analyzeBezier({0, 0}, {1, 0}, {99, 0}, {100, 0}, b) == BezierError::None); return b; }(),
    };
    for (const Bezier& b : cases) {
        CHECK(bezierT(b, 0) == 0);
        CHECK(bezierT(b, b.length) == doctest::Approx(1).epsilon(1e-5));
        float prev = 0;
        for (int i = 1; i <= 1000; i++) {
            const float t = bezierT(b, b.length * i / 1000);
            CHECK(t >= prev);
            prev = t;
        }
    }
}

TEST_CASE("bezier: the evaluator hits both ends and paces evenly along a circle") {
    const Bezier b = quarter({10, -3}, 30, 0.4f);
    const Vec2 a = bezierPoint(b, bezierT(b, 0));
    CHECK(a.x == b.p[0].x);
    CHECK(a.y == b.p[0].y);
    CHECK(dist(bezierPoint(b, bezierT(b, b.length)), b.p[3]) < 1e-4f);

    const int n = 200;
    const float step = b.length / n;
    Vec2 prev = b.p[0];
    for (int i = 1; i <= n; i++) {
        const Vec2 q = bezierPoint(b, bezierT(b, step * i));
        CHECK(dist(q, prev) == doctest::Approx(step).epsilon(1e-2));   // speed ripple < 1 %
        CHECK(dist(q, {10, -3}) == doctest::Approx(30).epsilon(1e-3));
        prev = q;
    }
}

TEST_CASE("bezier: degenerate input is refused") {
    Bezier b;
    CHECK(analyzeBezier({0, 0}, {0, 0}, {5, 5}, {10, 0}, b) == BezierError::DegenerateHandle);
    CHECK(analyzeBezier({0, 0}, {5, 5}, {10, 0}, {10, 0}, b) == BezierError::DegenerateHandle);
    // B'(0.5) = 0: the curve stops and turns back on itself.
    CHECK(analyzeBezier({0, 0}, {10, 10}, {0, 10}, {10, 0}, b) == BezierError::Cusp);
}

TEST_CASE("bezier: limits share the inscribed circle between turning and speeding up") {
    AxisLimits l = lim();
    l.max_accel[1] = 1000;
    l.max_feed[1] = 300;
    const float a = 1000 / sqrtf(2.0f);

    const Bezier straight = [] { Bezier b; REQUIRE(analyzeBezier({0, 0}, {10, 0}, {20, 0}, {30, 0}, b) == BezierError::None); return b; }();
    const Path ps = pathOf(straight, 1000, l);
    CHECK(ps.accel == doctest::Approx(a));
    CHECK(ps.v_max_sqr == doctest::Approx(300 * 300));
    CHECK(ps.dir_start.x == doctest::Approx(1));
    CHECK(ps.v_max_sqr <= pathOf(makeLine({0, 0}, {30, 0}, 1000, l)).v_max_sqr);

    const Bezier tight = quarter({0, 0}, 5, 0);
    const Path pt = pathOf(tight, 1000, l);
    CHECK(pt.v_max_sqr == doctest::Approx(a / tight.kappa_max));
    CHECK(pt.dir_start.x == doctest::Approx(0).epsilon(1e-6));
    CHECK(pt.dir_start.y == doctest::Approx(1));
    CHECK(pt.dir_end.x == doctest::Approx(-1));
}

namespace {

// Line → arc → line, tangent at both joins, with the arc's centre known.
struct Track {
    Planner p;
    Executor e;
    Vec2 centre = {50, 20};
    float r = 20;

    Track() {
        p.reset({0, 0});
        e.reset({0, 0});
        REQUIRE(p.pushLine({50, 0}, 500, lim(), kDev));
        REQUIRE(p.pushBezier(quarter(centre, r, -kPi / 2), 500, lim(), kDev));
        REQUIRE(p.pushLine({70, 80}, 500, lim(), kDev));
        p.replan();
        REQUIRE(p.commit());
    }
    bool onArc(Vec2 q) const { return q.x > centre.x + 1e-3f && q.y < centre.y - 1e-3f; }
    bool onPath(Vec2 q) const {
        if (q.x <= centre.x) return fabsf(q.y) < 1e-3f;
        if (q.y >= centre.y) return fabsf(q.x - 70) < 1e-3f;
        return fabsf(dist(q, centre) - r) < 1e-2f;   // the cubic is 0.03 % off a circle
    }
};

}  // namespace

TEST_CASE("bezier: tangent joins run through without stopping, within the acceleration circle") {
    Track tr;
    Vec2 q0 = tr.e.position(), q1 = q0;
    float min_speed = INFINITY;
    float max_acc = 0;
    int ticks = 0;
    while (tr.p.count() > 0 || tr.p.claimed()) {
        const Vec2 q2 = tr.e.tick(tr.p, kDt);
        CHECK(tr.onPath(q2));
        if (ticks >= 2) {
            // Second difference: the vector acceleration the motors see.
            const float ax = (q2.x - 2 * q1.x + q0.x) / (kDt * kDt);
            const float ay = (q2.y - 2 * q1.y + q0.y) / (kDt * kDt);
            max_acc = fmaxf(max_acc, hypotf(ax, ay));
        }
        if (tr.e.position().x > 10 && tr.e.position().y < 70) min_speed = fminf(min_speed, tr.e.speed());
        q0 = q1;
        q1 = q2;
        ticks++;
        REQUIRE(ticks < 100000);
    }
    CHECK(dist(tr.e.position(), {70, 80}) < 1e-4f);
    CHECK(min_speed > 50);
    // Lines may use the full 2000 along an axis; the arc stays inside it too.
    CHECK(max_acc <= 2000 * 1.05f);
}

TEST_CASE("bezier: a hold inside the arc stops on the curve within v²/2a, and resume finishes") {
    Track tr;
    int guard = 0;
    while (!tr.onArc(tr.e.position()) || dist(tr.e.position(), {50, 0}) < 5) {
        tr.e.tick(tr.p, kDt);
        REQUIRE(++guard < 100000);
    }
    const float v = tr.e.speed();
    REQUIRE(v > 0);
    const float a = 2000 / sqrtf(2.0f);
    const Vec2 at = tr.e.position();
    tr.e.hold();

    float travelled = 0;
    Vec2 prev = at;
    while (tr.e.state() != Executor::State::Held) {
        const Vec2 q = tr.e.tick(tr.p, kDt);
        CHECK(tr.onPath(q));
        travelled += dist(prev, q);
        prev = q;
        REQUIRE(++guard < 100000);
    }
    CHECK(travelled <= v * v / (2 * a) * 1.01f + 1e-3f);
    CHECK(tr.onArc(tr.e.position()));

    const Vec2 held = tr.e.position();
    tr.e.resume(tr.p);
    const Vec2 first = tr.e.tick(tr.p, kDt);
    CHECK(dist(first, held) < 1e-2f);   // restarts from rest where it stopped
    while (tr.p.count() > 0 || tr.p.claimed()) {
        CHECK(tr.onPath(tr.e.tick(tr.p, kDt)));
        REQUIRE(++guard < 200000);
    }
    CHECK(dist(tr.e.position(), {70, 80}) < 1e-4f);
}

// What the host sends: the analysis without c1 and the end curvatures.
static Bezier hostRecord(const Bezier& a) {
    Bezier r;
    for (int i = 0; i < 4; i++) r.p[i] = a.p[i];
    r.length = a.length;
    r.kappa_max = a.kappa_max;
    r.dkappa_max = a.dkappa_max;
    r.ts[1] = a.ts[1];
    r.ts[2] = a.ts[2];
    return r;
}

TEST_CASE("bezier: a host-analysed curve is accepted, and c1 and the end curvatures derived") {
    Bezier s;
    REQUIRE(analyzeBezier({0, 0}, {20, 40}, {40, 40}, {60, 0}, s) == BezierError::None);
    for (const Bezier& a : {quarter({10, -3}, 30, 0.4f), s}) {
        Bezier r = hostRecord(a);
        REQUIRE(checkBezier(r) == BezierError::None);
        CHECK(r.ts[0] == doctest::Approx(a.ts[0]).epsilon(1e-5));
        CHECK(r.kappa_start == doctest::Approx(a.kappa_start).epsilon(1e-5));
        CHECK(r.kappa_end == doctest::Approx(a.kappa_end).epsilon(1e-5));
    }
}

TEST_CASE("bezier: host numbers that disagree with the control points are refused") {
    const Bezier a = quarter({0, 0}, 30, 0);
    struct Case { const char* name; void (*bad)(Bezier&); BezierError err; };
    const Case cases[] = {
        {"handle on its end", [](Bezier& r) { r.p[1] = r.p[0]; }, BezierError::DegenerateHandle},
        {"shorter than the chord", [](Bezier& r) { r.length = 30; }, BezierError::Inconsistent},
        {"longer than the polygon", [](Bezier& r) { r.length = 100; }, BezierError::Inconsistent},
        {"not a number", [](Bezier& r) { r.length = NAN; }, BezierError::Inconsistent},
        {"negative dkappa", [](Bezier& r) { r.dkappa_max = -1; }, BezierError::Inconsistent},
        {"kappa_max below an end", [](Bezier& r) { r.kappa_max = 0.5f / 30; }, BezierError::Inconsistent},
        {"fit runs backwards", [](Bezier& r) { r.ts[1] = 3 / (r.length * r.length); }, BezierError::NonMonotonic},
    };
    for (const Case& c : cases) {
        CAPTURE(c.name);
        Bezier r = hostRecord(a);
        c.bad(r);
        CHECK(checkBezier(r) == c.err);
    }
}
