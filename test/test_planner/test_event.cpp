/**
 * Event blocks: no motion, at rest on both sides, and the executor waits at
 * each one until the consumer has run it.
 */

#include "doctest.h"

#include <planner/executor.h>

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

static void plan(Planner& p, Executor& e) {
    p.replan(e.clock());
    REQUIRE(p.commit(e.clock()));
}

// Tick until the executor stops at an event or the ring drains.
static int runToEvent(Planner& p, Executor& e, float* vmax_near = nullptr) {
    for (int i = 0; i < 100000; i++) {
        if (e.event()) return i;
        if (p.count() == 0 && !e.current()) return i;
        e.tick(p, kDt);
        if (vmax_near) *vmax_near = fmaxf(*vmax_near, e.speed());
    }
    FAIL("never stopped");
    return -1;
}

TEST_CASE("event: the line before it ends at rest, the line after starts from rest") {
    Planner p;
    Executor e;
    p.reset({0, 0});
    e.reset({0, 0});
    REQUIRE(p.pushLine({10, 0}, 100, lim(), kDev));
    REQUIRE(p.pushEvent(7));
    REQUIRE(p.pushLine({20, 0}, 100, lim(), kDev));
    plan(p, e);

    runToEvent(p, e);
    REQUIRE(e.event() != nullptr);
    CHECK(e.event()->event == 7);
    CHECK(e.speed() == 0);
    CHECK(e.position().x == doctest::Approx(10));

    // Waiting costs time but no motion.
    for (int i = 0; i < 50; i++) e.tick(p, kDt);
    CHECK(e.event() != nullptr);
    CHECK(e.position().x == doctest::Approx(10));
    CHECK_FALSE(e.needsRing(kDt));

    e.eventDone();
    CHECK(e.needsRing(kDt));
    e.tick(p, kDt);
    CHECK(e.event() == nullptr);
    // The next line starts from rest: one tick at full accel at most.
    CHECK(e.speed() <= 2000 * kDt * 1.01f);
    runToEvent(p, e);
    CHECK(e.position().x == doctest::Approx(20));
}

TEST_CASE("event: no blend across an event between collinear lines") {
    Planner p;
    Executor e;
    p.reset({0, 0});
    e.reset({0, 0});
    REQUIRE(p.pushLine({10, 0}, 100, lim(), kDev));
    REQUIRE(p.pushEvent(1));
    REQUIRE(p.pushLine({20, 0}, 100, lim(), kDev));
    plan(p, e);
    const Block* first = p.oldest();
    REQUIRE(first);
    CHECK(first->exit_sqr == 0);
}

TEST_CASE("event: a hold at an event completes at once and resume keeps it") {
    Planner p;
    Executor e;
    p.reset({0, 0});
    e.reset({0, 0});
    REQUIRE(p.pushLine({5, 0}, 100, lim(), kDev));
    REQUIRE(p.pushEvent(3));
    REQUIRE(p.pushLine({8, 0}, 100, lim(), kDev));
    plan(p, e);
    runToEvent(p, e);
    REQUIRE(e.event());

    e.hold();
    e.tick(p, kDt);
    CHECK(e.state() == Executor::State::Held);

    e.resume(p);
    CHECK(e.state() == Executor::State::Running);
    e.tick(p, kDt);
    REQUIRE(e.event() != nullptr);
    CHECK(e.event()->event == 3);
}

TEST_CASE("event: one already run is released by a hold, not replayed") {
    Planner p;
    Executor e;
    p.reset({0, 0});
    e.reset({0, 0});
    REQUIRE(p.pushEvent(4));
    REQUIRE(p.pushLine({5, 0}, 100, lim(), kDev));
    plan(p, e);
    e.tick(p, kDt);
    REQUIRE(e.event());
    e.eventDone();
    e.hold();
    e.tick(p, kDt);
    CHECK(e.event() == nullptr);
    CHECK(p.count() == 1);
}

TEST_CASE("event: an abort at an event empties the ring") {
    Planner p;
    Executor e;
    p.reset({0, 0});
    e.reset({0, 0});
    REQUIRE(p.pushEvent(2));
    REQUIRE(p.pushLine({5, 0}, 100, lim(), kDev));
    plan(p, e);
    e.tick(p, kDt);
    REQUIRE(e.event());
    e.abort();
    CHECK(e.needsRing(kDt));
    e.tick(p, kDt);
    CHECK(p.count() == 0);
    CHECK(e.event() == nullptr);
}

TEST_CASE("event: a full ring refuses one") {
    Planner p;
    p.reset({0, 0});
    for (int i = 0; i < Planner::kSize; i++) REQUIRE(p.pushEvent(0));
    CHECK_FALSE(p.pushEvent(0));
}
