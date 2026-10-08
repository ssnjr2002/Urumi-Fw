/**
 * Axis sets: a line moves XY, Z alone or A alone; joins between sets stop;
 * A keeps its heading in [0, 360) and counts whole turns.
 */

#include "doctest.h"

#include <planner/executor.h>

#include <math.h>

using namespace planner;

static AxisLimits lim() {
    AxisLimits l;
    for (int i = 0; i < 4; i++) {
        l.max_feed[i] = 500;
        l.max_accel[i] = 2000;
    }
    return l;
}

static Pos at(float x, float y, float z, float a, int32_t turns = 0) {
    Pos p;
    p.x = x; p.y = y; p.z = z; p.a = a; p.turns = turns;
    return p;
}

TEST_CASE("addA keeps the heading in [0, 360) and counts turns") {
    struct Row { float a; int32_t turns; float deg; float a_out; int32_t turns_out; };
    const Row rows[] = {
        {10, 0, 20, 30, 0},          // within the turn
        {350, 0, 20, 10, 1},         // forward across 0
        {10, 0, -20, 350, -1},       // backward across 0
        {0, 0, 720, 0, 2},           // whole turns
        {90, 5, -1080, 90, 2},       // several turns back
        {0, 0, -360, 0, -1},         // exactly one turn back
        {359.5f, 1000, 0.5f, 0, 1001},
    };
    for (const Row& r : rows) {
        CAPTURE(r.a); CAPTURE(r.turns); CAPTURE(r.deg);
        Pos p = at(0, 0, 0, r.a, r.turns);
        p.addA(r.deg);
        CHECK(p.a == doctest::Approx(r.a_out).epsilon(1e-5));
        CHECK(p.turns == r.turns_out);
        CHECK(p.a >= 0);
        CHECK(p.a < 360);
    }
}

TEST_CASE("aSince measures across turns") {
    struct Row { Pos from, to; float deg; };
    const Row rows[] = {
        {at(0, 0, 0, 10), at(0, 0, 0, 30), 20},
        {at(0, 0, 0, 350), at(0, 0, 0, 10, 1), 20},
        {at(0, 0, 0, 10, 1), at(0, 0, 0, 350), -20},
        {at(0, 0, 0, 0, 1000), at(0, 0, 0, 0, 1002), 720},
    };
    for (const Row& r : rows) CHECK(r.to.aSince(r.from) == doctest::Approx(r.deg));
}

TEST_CASE("a move's axis set") {
    struct Row { Pos to; Axes axes; };
    const Pos from = at(1, 2, 3, 40);
    const Row rows[] = {
        {at(1, 2, 3, 40), AXES_NONE},
        {at(5, 2, 3, 40), AXES_XY},
        {at(5, 6, 3, 40), AXES_XY},
        {at(1, 2, 9, 40), AXES_Z},
        {at(1, 2, 3, 40, 1), AXES_A},     // a whole turn is a move
        {at(5, 2, 9, 40), AXES_MIXED},
        {at(1, 2, 9, 50), AXES_MIXED},
        {at(5, 2, 3, 50), AXES_MIXED},
    };
    for (const Row& r : rows) CHECK(axesOf(from, r.to) == r.axes);
}

TEST_CASE("junctions by axis set") {
    const AxisLimits l = lim();
    const Path xy = pathOf(makeLine({0, 0}, {10, 0}, 300, l));
    struct Row { const char* name; Path prev, next; bool full; };
    const Row rows[] = {
        {"Z straight on", axisPath(AXES_Z, 5, 300, l), axisPath(AXES_Z, 2, 300, l), true},
        {"Z reversal", axisPath(AXES_Z, 5, 300, l), axisPath(AXES_Z, -2, 300, l), false},
        {"A straight on", axisPath(AXES_A, -90, 300, l), axisPath(AXES_A, -10, 300, l), true},
        {"A reversal", axisPath(AXES_A, 90, 300, l), axisPath(AXES_A, -10, 300, l), false},
        {"XY to Z", xy, axisPath(AXES_Z, 5, 300, l), false},
        {"Z to A", axisPath(AXES_Z, 5, 300, l), axisPath(AXES_A, 5, 300, l), false},
    };
    for (const Row& r : rows) {
        CAPTURE(r.name);
        const float v = junctionMaxSqr(r.prev, r.next, 0.02f);
        if (r.full) CHECK(v == doctest::Approx(300 * 300));
        else CHECK(v == 0);
    }
}

TEST_CASE("a Z or A line takes that axis's limits") {
    AxisLimits l = lim();
    l.max_feed[2] = 20; l.max_accel[2] = 100;
    l.max_feed[3] = 720; l.max_accel[3] = 5000;
    const Path z = axisPath(AXES_Z, -4, 1000, l);
    CHECK(z.length == doctest::Approx(4));
    CHECK(sqrtf(z.v_max_sqr) == doctest::Approx(20));
    CHECK(z.accel == doctest::Approx(100));
    CHECK(z.dir_start.x == -1);
    const Path a = axisPath(AXES_A, 90, 100, l);
    CHECK(sqrtf(a.v_max_sqr) == doctest::Approx(100));   // feed below the cap
    CHECK(a.accel == doctest::Approx(5000));
}

TEST_CASE("a line mixing axis sets is refused") {
    Planner p;
    p.reset(at(0, 0, 0, 0));
    CHECK_FALSE(p.pushMove(at(10, 0, 5, 0), 100, lim(), 0.02f));
    CHECK(p.count() == 0);
}

// Runs every queued block to rest; the last position ticked.
static Pos runOut(Planner& p, Executor& e) {
    p.replan();
    REQUIRE(p.commit());
    Pos last = e.position();
    for (int i = 0; i < 200000 && (p.count() > 0 || e.speed() > 0); i++) last = e.tick(p, 0.001f);
    return last;
}

TEST_CASE("Z and A lines run to their targets and leave the other axes") {
    struct Row { const char* name; Pos to; };
    const Pos start = at(10, 20, 5, 350, 3);
    const Row rows[] = {
        {"Z down", at(10, 20, -2, 350, 3)},
        {"A forward across 0", at(10, 20, 5, 30, 4)},
        {"A back two turns", at(10, 20, 5, 340, 1)},
    };
    for (const Row& r : rows) {
        CAPTURE(r.name);
        Planner p;
        Executor e;
        p.reset(start);
        e.reset(start);
        REQUIRE(p.pushMove(r.to, 200, lim(), 0.02f));
        const Pos end = runOut(p, e);
        CHECK(end.x == r.to.x);
        CHECK(end.y == r.to.y);
        CHECK(end.z == doctest::Approx(r.to.z));
        CHECK(end.a == doctest::Approx(r.to.a));
        CHECK(end.turns == r.to.turns);
    }
}

TEST_CASE("an A line passes through 0 without a jump") {
    Planner p;
    Executor e;
    const Pos start = at(0, 0, 0, 300);
    p.reset(start);
    e.reset(start);
    REQUIRE(p.pushMove(at(0, 0, 0, 60, 1), 200, lim(), 0.02f));
    p.replan();
    REQUIRE(p.commit());
    Pos prev = start;
    for (int i = 0; i < 100000 && (p.count() > 0 || e.speed() > 0); i++) {
        const Pos q = e.tick(p, 0.001f);
        const float step = q.aSince(prev);
        CHECK(step >= 0);
        CHECK(step < 1);
        prev = q;
    }
    CHECK(prev.aSince(start) == doctest::Approx(120));
}
