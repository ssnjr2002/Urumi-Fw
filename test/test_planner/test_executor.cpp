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

    explicit Rig(Pos start = {0, 0}) { p.reset(start); e.reset(start); }

    void pushLine(Vec2 to, float feed) {
        planned_length += dist(p.end().xy(), to);
        REQUIRE(p.pushLine(to, feed, lim(), kDev));
    }
    // Replan and commit as the producer does, retrying a refused commit.
    void plan() {
        for (int i = 0; i < 100; i++) {
            p.replan(e.clock());
            if (p.commit(e.clock())) return;
            if (p.claimed()) tick();   // refused near a switch: let time pass
        }
        FAIL("commit never accepted");
    }

    void tick(float dt = kDt) {
        const Vec2 before = e.position().xy();
        const float v0 = e.speed();
        e.adopt(p);
        e.tick(p, dt);
        travelled += dist(before, e.position().xy());
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
    Vec2 at = r.p.end().xy();
    for (int i = 0; i < n; i++) {
        at = {at.x + (rnd() - 0.5f) * 40 * rnd(), at.y + (rnd() - 0.5f) * 40 * rnd()};
        r.pushLine(at, 50 + rnd() * 450);
    }
    return at;
}

TEST_CASE("single line runs to its end in the profile's time") {
    Rig r;
    r.pushLine({100, 0}, 300);
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
    CHECK(dist(r.e.position().xy(), end) < 1e-3f);
    CHECK(r.travelled == doctest::Approx(r.planned_length).epsilon(1e-3));
    // Axis accel is 2000; along a diagonal the path accel can reach 2000·√2.
    CHECK(r.max_dv <= 2000 * 1.4143f * kDt * 1.01f);
}

TEST_CASE("large dt spills across several blocks") {
    Rig r;
    for (int i = 1; i <= 20; i++) r.pushLine({float(i), 0}, 300);
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
        const Vec2 held = r.e.position().xy();
        for (int i = 0; i < 50; i++) r.tick();
        CHECK(dist(held, r.e.position().xy()) == 0);

        r.e.resume(r.p);
        r.run();
        CHECK(dist(r.e.position().xy(), end) < 1e-3f);
        CHECK(r.travelled == doctest::Approx(r.planned_length).epsilon(1e-3));
        CHECK(r.max_dv <= 2000 * 1.4143f * kDt * 1.01f);
    }
    CHECK(held_mid_path >= 30);
}

TEST_CASE("hold across a block join brakes through it") {
    Rig r;
    for (int i = 1; i <= 50; i++) r.pushLine({float(i), 0}, 300);
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
    const Vec2 stopped = r.e.position().xy();
    CHECK(dist(r.p.end().xy(), stopped) == 0);

    // A new move starts from there.
    REQUIRE(r.p.pushLine({stopped.x + 10, stopped.y}, 100, lim(), kDev));
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

TEST_CASE("needsRing is true whenever a tick touches the ring") {
    int ring_ticks = 0, ticks = 0;
    for (uint32_t trial = 0; trial < 20; trial++) {
        Rig r;
        uint32_t seed = 500 + trial;
        randomPolyline(r, seed, 30);
        r.plan();
        const int hold_at = 50 + int(trial * 53 % 300);
        for (int i = 0; i < 20000; i++) {
            if (i == hold_at) r.e.hold();
            if (i == hold_at + 400) r.e.resume(r.p);
            if (trial % 4 == 3 && i == hold_at + 600) r.e.abort();
            r.e.adopt(r.p);
            const bool needs = r.e.needsRing(kDt);
            const int count = r.p.count();
            const bool claimed = r.p.claimed();
            r.tick();
            if (!needs) {
                CHECK(r.p.count() == count);
                CHECK(r.p.claimed() == claimed);
            }
            ring_ticks += needs;
            ticks++;
            if (r.p.count() == 0 && !r.p.claimed() && i > hold_at + 600) break;
        }
    }
    // Only block changes need the lock, not every tick.
    CHECK(ring_ticks * 10 < ticks);
}

// The running block's exit rises while more blocks arrive.

TEST_CASE("the first move from rest does not stop when more arrive while it runs") {
    Rig r;
    r.pushLine({20, 0}, 300);
    r.plan();
    for (int i = 0; i < 5; i++) r.tick();   // claimed and accelerating
    REQUIRE(r.p.claimed());
    r.pushLine({40, 0}, 300);
    r.plan();
    r.pushLine({60, 0}, 300);
    r.plan();

    float min_v = 1e9f;
    while (r.p.count() > 0 || r.p.claimed()) {
        r.tick();
        const float x = r.e.position().x;
        // Cruise, 300 mm/s, spans 22.5 mm to 37.5 mm, across the first join.
        if (x > 23 && x < 37) min_v = fminf(min_v, r.e.speed());
        REQUIRE(r.time < 5);
    }
    CHECK(r.e.position().x == doctest::Approx(60));
    CHECK(min_v == doctest::Approx(300).epsilon(1e-3));
    CHECK(r.max_dv <= 2000 * kDt * 1.01f);
    CHECK(r.travelled == doctest::Approx(60).epsilon(1e-3));
    CHECK(r.e.lateAdoptions() == 0);
}

TEST_CASE("a raise late in braking is capped by the distance left") {
    Rig r;
    r.pushLine({20, 0}, 300);
    r.plan();
    // A 20 mm block alone peaks below 300 mm/s and brakes over its second half.
    while (r.e.position().x < 17) r.tick();
    const float v = r.e.speed();
    r.pushLine({40, 0}, 300);
    r.plan();
    float v_join = 0;
    while (r.p.count() > 0 || r.p.claimed()) {
        const float x0 = r.e.position().x;
        r.tick();
        if (x0 < 20 && r.e.position().x >= 20) v_join = r.e.speed();
    }
    CHECK(v_join > 0);
    // No faster than accelerating from 17 mm to 20 mm allows.
    CHECK(v_join * v_join <= v * v + 2 * 2000 * 3 * 1.01f);
    CHECK(r.max_dv <= 2000 * kDt * 1.01f);
    CHECK(r.e.position().x == doctest::Approx(40));
    CHECK(r.e.lateAdoptions() == 0);
}

TEST_CASE("needsRing sees a block end brought forward by an adopted piece") {
    int ends = 0;
    for (int k = 0; k < 20; k++) {
        Rig r;
        r.pushLine({5, 0}, 300);
        r.plan();
        const int gap = 3 + k * 7;
        for (int i = 0; i < 4000 && (i <= 2 * gap || r.p.count() > 0 || r.p.claimed()); i++) {
            if (i == gap || i == 2 * gap) { r.pushLine({r.p.end().x + 5, 0}, 300); r.plan(); }
            r.e.adopt(r.p);
            const bool needs = r.e.needsRing(kDt);
            const int count = r.p.count();
            const bool claimed = r.p.claimed();
            r.e.tick(r.p, kDt);
            if (!needs) {
                CHECK(r.p.count() == count);
                CHECK(r.p.claimed() == claimed);
            } else if (r.p.count() != count) {
                ends++;
            }
        }
        CHECK(r.e.position().x == doctest::Approx(15));
        CHECK(r.e.lateAdoptions() == 0);
    }
    CHECK(ends > 0);
}

TEST_CASE("a hold ignores a pending piece, and resume finishes the path") {
    Rig r;
    r.pushLine({20, 0}, 300);
    r.plan();
    for (int i = 0; i < 20; i++) r.tick();
    r.pushLine({40, 0}, 300);
    r.plan();              // staged, not yet adopted
    r.e.hold();
    r.run();
    REQUIRE(r.e.state() == Executor::State::Held);
    CHECK(r.e.speed() == 0);
    r.e.resume(r.p);
    r.run();
    CHECK(r.e.position().x == doctest::Approx(40));
    CHECK(r.e.lateAdoptions() == 0);
}

TEST_CASE("random polylines typed while running keep speed continuous") {
    for (uint32_t trial = 0; trial < 20; trial++) {
        Rig r;
        uint32_t seed = 900 + trial;
        auto rnd = [&seed]() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0f; };
        Vec2 at{0, 0};
        int pushed = 0, i = 0;
        while (pushed < 40 || r.p.count() > 0 || r.p.claimed()) {
            if (pushed < 40 && (i % (1 + int(rnd() * 30))) == 0) {
                at = {at.x + (rnd() - 0.5f) * 30 * rnd(), at.y + (rnd() - 0.5f) * 30 * rnd()};
                r.pushLine(at, 50 + rnd() * 450);
                r.plan();
                pushed++;
            }
            r.tick();
            REQUIRE(++i < 200000);
        }
        CHECK(dist(r.e.position().xy(), at) < 1e-3f);
        CHECK(r.travelled == doctest::Approx(r.planned_length).epsilon(1e-3));
        CHECK(r.max_dv <= 2000 * 1.4143f * kDt * 1.01f);
        CHECK(r.e.lateAdoptions() == 0);
    }
}

TEST_CASE("a piece found after its switch time is refused and holds") {
    Rig r;
    r.pushLine({20, 0}, 300);
    r.plan();
    for (int i = 0; i < 20; i++) r.tick();
    r.pushLine({40, 0}, 300);
    r.plan();
    // Tick past the piece's t0 without taking it.
    for (int i = 0; i < 10; i++) r.e.tick(r.p, kDt);
    r.e.adopt(r.p);
    CHECK(r.e.lateAdoptions() == 1);
    CHECK(r.e.state() == Executor::State::Holding);
    r.run();
    REQUIRE(r.e.state() == Executor::State::Held);
    r.e.resume(r.p);
    r.run();
    CHECK(r.e.position().x == doctest::Approx(40));
}
