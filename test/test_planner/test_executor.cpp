/**
 * Executor contract: the ticked path follows the plan to its end with no speed
 * jumps, a hold stops no later than the plan would, and resume or abort leave
 * the machine consistent with the ring.
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

static float dist(Vec2 a, Vec2 b) { return hypotf(a.x - b.x, a.y - b.y); }

struct Rig {
    Planner p;
    Executor e;
    float planned_length = 0;   // mm queued, along the path
    float travelled = 0;        // mm ticked, summed per tick
    float max_dv = 0;           // largest speed change per tick
    float time = 0;

    explicit Rig(Vec2 start = {0, 0}) { p.reset(start); e.reset(start); }

    void push(Vec2 to, float feed) {
        planned_length += dist(p.end(), to);
        REQUIRE(p.push(to, feed, lim(), kDev));
    }
    void plan() { p.replan(); REQUIRE(p.commit()); }

    void tick(float dt = kDt) {
        const Vec2 before = e.position();
        const float v0 = e.speed();
        e.tick(p, dt);
        travelled += dist(before, e.position());
        max_dv = fmaxf(max_dv, fabsf(e.speed() - v0));
        time += dt;
    }
    // Tick until the ring drains or a hold completes.
    void run(float limit_s = 60) {
        while (time < limit_s) {
            tick();
            if (e.state() == Executor::State::Held) return;
            if (p.count() == 0 && !p.claimed()) return;
        }
    }
};

static Vec2 randomPolyline(Rig& r, uint32_t& seed, int n) {
    auto rnd = [&seed]() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0f; };
    Vec2 at = r.p.end();
    for (int i = 0; i < n; i++) {
        at = {at.x + (rnd() - 0.5f) * 40 * rnd(), at.y + (rnd() - 0.5f) * 40 * rnd()};
        r.push(at, 50 + rnd() * 450);
    }
    return at;
}

TEST_CASE("single line runs to its end in the profile's time") {
    Rig r;
    r.push({100, 0}, 300);
    r.plan();
    const float expected = Planner(r.p).claim()->profile.duration();
    r.run();
    CHECK(r.e.position().x == doctest::Approx(100));
    CHECK(r.e.position().y == doctest::Approx(0));
    CHECK(r.e.speed() == 0);
    CHECK(r.time == doctest::Approx(expected).epsilon(kDt / expected + 1e-3));
}

TEST_CASE("speed never jumps, including across block joins") {
    Rig r;
    uint32_t seed = 7;
    const Vec2 end = randomPolyline(r, seed, 60);
    r.plan();
    r.run();
    CHECK(dist(r.e.position(), end) < 1e-3f);
    CHECK(r.travelled == doctest::Approx(r.planned_length).epsilon(1e-3));
    // Axis accel is 2000; along a diagonal the path accel can reach 2000·√2.
    CHECK(r.max_dv <= 2000 * 1.4143f * kDt * 1.01f);
}

TEST_CASE("large dt spills across several blocks") {
    Rig r;
    for (int i = 1; i <= 20; i++) r.push({float(i), 0}, 300);
    r.plan();
    for (int i = 0; i < 10 && r.p.count() > 0; i++) r.tick(0.05f);
    CHECK(r.e.position().x == doctest::Approx(20));
    CHECK(r.p.count() == 0);
}

TEST_CASE("a hold stops no later than the plan, and resume finishes the path") {
    int held_mid_path = 0;
    for (uint32_t trial = 0; trial < 40; trial++) {
        Rig r;
        uint32_t seed = 100 + trial;
        const Vec2 end = randomPolyline(r, seed, 30);
        r.plan();

        // Hold at a varied point of the run.
        const int ticks = 20 + int(trial * 37 % 400);
        for (int i = 0; i < ticks && r.p.count() > 0; i++) r.tick();
        const float remaining = r.planned_length - r.travelled;
        const float before = r.travelled;
        r.e.hold();
        r.run();
        CHECK(r.e.speed() == 0);
        CHECK(r.travelled - before <= remaining + 1e-3f);
        if (r.p.count() == 0) continue;   // the plan ended during the hold
        REQUIRE(r.e.state() == Executor::State::Held);
        held_mid_path++;

        // Held means held.
        const Vec2 held = r.e.position();
        for (int i = 0; i < 50; i++) r.tick();
        CHECK(dist(held, r.e.position()) == 0);

        r.e.resume(r.p);
        r.run();
        CHECK(dist(r.e.position(), end) < 1e-3f);
        CHECK(r.travelled == doctest::Approx(r.planned_length).epsilon(1e-3));
        CHECK(r.max_dv <= 2000 * 1.4143f * kDt * 1.01f);
    }
    CHECK(held_mid_path >= 30);
}

TEST_CASE("hold across a block join brakes through it") {
    Rig r;
    for (int i = 1; i <= 50; i++) r.push({float(i), 0}, 300);
    r.plan();
    while (r.e.speed() < 299) r.tick();
    r.e.hold();
    r.run();
    // 300 mm/s at 2000 mm/s² needs 22.5 mm: many 1 mm blocks.
    CHECK(r.e.state() == Executor::State::Held);
    CHECK(r.p.count() > 0);
    r.e.resume(r.p);
    r.run();
    CHECK(r.e.position().x == doctest::Approx(50));
}

TEST_CASE("abort stops, empties the ring, and restarts from where it stopped") {
    Rig r;
    uint32_t seed = 3;
    randomPolyline(r, seed, 20);
    r.plan();
    for (int i = 0; i < 200; i++) r.tick();
    r.e.abort();
    for (int i = 0; i < 2000; i++) r.tick();
    CHECK(r.e.state() == Executor::State::Running);
    CHECK(r.e.speed() == 0);
    CHECK(r.p.count() == 0);
    CHECK_FALSE(r.p.claimed());
    const Vec2 stopped = r.e.position();
    CHECK(dist(r.p.end(), stopped) == 0);

    // A new move starts from there.
    REQUIRE(r.p.push({stopped.x + 10, stopped.y}, 100, lim(), kDev));
    r.plan();
    r.run();
    CHECK(r.e.position().x == doctest::Approx(stopped.x + 10));
}

TEST_CASE("hold and abort while idle") {
    Rig r({5, 5});
    r.e.hold();
    r.tick();
    CHECK(r.e.state() == Executor::State::Held);
    r.e.resume(r.p);
    CHECK(r.e.state() == Executor::State::Running);
    r.e.abort();
    r.tick();
    CHECK(r.e.state() == Executor::State::Running);
    CHECK(r.e.position().x == 5);
}
