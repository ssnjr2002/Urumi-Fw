/**
 * Planner contract: every committed plan is continuous and feasible, respects
 * junction limits, ends at rest, never touches the claimed block, and only
 * rises as more blocks arrive.
 */

#include "doctest.h"

#include <planner/planner.h>

#include <math.h>

using namespace planner;

static AxisLimits lim() {
    AxisLimits l;
    l.max_feed[0] = l.max_feed[1] = 500;
    l.max_accel[0] = l.max_accel[1] = 2000;
    return l;
}

static const float kDev = 0.02f;

static void replanCommit(Planner& p) {
    p.replan();
    REQUIRE(p.commit());
}

// Walk the committed ring through the claim interface without disturbing it.
// A negative `entry_sqr` accepts whatever the first block was pinned to.
static void checkPlan(Planner p, float entry_sqr) {
    const float rel = 1e-4f;
    float prev_exit = entry_sqr;
    if (entry_sqr < 0) {
        Planner q = p;
        const Block* b = q.claim();
        prev_exit = b ? b->entry_sqr : 0;
    }
    const Block* b = nullptr;
    int n = 0;
    while ((b = p.claim()) != nullptr) {
        CHECK(b->entry_sqr == doctest::Approx(prev_exit).epsilon(rel));
        CHECK(b->entry_sqr <= b->line.v_max_sqr * (1 + rel));
        CHECK(b->exit_sqr <= b->line.v_max_sqr * (1 + rel));
        if (n > 0) CHECK(b->entry_sqr <= b->max_entry_sqr * (1 + rel) + 1e-3f);
        const float span = 2 * b->line.accel * b->line.length;
        CHECK(fabsf(b->entry_sqr - b->exit_sqr) <= span * (1 + rel) + 1e-3f);
        CHECK(b->profile.position(b->profile.duration()) ==
              doctest::Approx(b->line.length).epsilon(1e-4));
        prev_exit = b->exit_sqr;
        p.release();
        n++;
    }
    CHECK(prev_exit == 0);
}

TEST_CASE("single line plans to rest at both ends") {
    Planner p;
    p.reset({0, 0});
    REQUIRE(p.push({100, 0}, 300, lim(), kDev));
    replanCommit(p);
    checkPlan(p, 0);
    const Block* b = p.claim();
    CHECK(b->profile.v_cruise == doctest::Approx(300));
}

TEST_CASE("collinear chain of short lines cruises through its joins") {
    Planner p;
    p.reset({0, 0});
    for (int i = 1; i <= 50; i++) REQUIRE(p.push({float(i), 0}, 300, lim(), kDev));
    replanCommit(p);
    checkPlan(p, 0);
    // Mid-chain speed reaches the feed, which one 1 mm block alone never could.
    Planner q = p;
    for (int i = 0; i < 25; i++) { q.claim(); q.release(); }
    CHECK(sqrtf(q.claim()->entry_sqr) == doctest::Approx(300));
}

TEST_CASE("square corners are held to the junction limit") {
    Planner p;
    p.reset({0, 0});
    const Vec2 pts[] = {{50, 0}, {50, 50}, {0, 50}, {0, 0}};
    for (const Vec2& v : pts) REQUIRE(p.push(v, 300, lim(), kDev));
    replanCommit(p);
    checkPlan(p, 0);
    Planner q = p;
    q.claim();
    q.release();
    const Block* b = q.claim();
    CHECK(b->entry_sqr < 300 * 300);
    CHECK(b->entry_sqr == doctest::Approx(b->max_entry_sqr));
}

TEST_CASE("reversal stops at the join") {
    Planner p;
    p.reset({0, 0});
    REQUIRE(p.push({20, 0}, 300, lim(), kDev));
    REQUIRE(p.push({0, 0}, 300, lim(), kDev));
    replanCommit(p);
    checkPlan(p, 0);
    CHECK(p.claim()->exit_sqr == 0);
}

TEST_CASE("claimed block is untouched and pins the next entry") {
    Planner p;
    p.reset({0, 0});
    for (int i = 1; i <= 3; i++) REQUIRE(p.push({10.0f * i, 0}, 300, lim(), kDev));
    replanCommit(p);
    const Block* running = p.claim();
    const Block before = *running;

    for (int i = 4; i <= 20; i++) REQUIRE(p.push({10.0f * i, 0}, 300, lim(), kDev));
    replanCommit(p);

    CHECK(running->entry_sqr == before.entry_sqr);
    CHECK(running->exit_sqr == before.exit_sqr);
    CHECK(running->profile.duration() == before.profile.duration());

    p.release();
    CHECK(p.claim()->entry_sqr == before.exit_sqr);
}

TEST_CASE("commit refuses a plan made stale by claim, release or push") {
    Planner p;
    p.reset({0, 0});
    for (int i = 1; i <= 5; i++) REQUIRE(p.push({10.0f * i, 0}, 300, lim(), kDev));

    p.replan();
    p.claim();
    CHECK_FALSE(p.commit());

    p.replan();
    p.release();
    CHECK_FALSE(p.commit());

    p.replan();
    REQUIRE(p.push({60, 0}, 300, lim(), kDev));
    CHECK_FALSE(p.commit());

    p.replan();
    CHECK(p.commit());
    CHECK_FALSE(p.commit());   // a plan commits once
}

TEST_CASE("more look-ahead only raises speeds") {
    Planner p;
    p.reset({0, 0});
    const Vec2 pts[] = {{10, 0}, {20, 3}, {30, 0}, {40, 5}, {50, 0}, {60, 2}, {70, 0}};
    float prev[8] = {};
    for (int k = 0; k < 7; k++) {
        REQUIRE(p.push(pts[k], 300, lim(), kDev));
        replanCommit(p);
        Planner q = p;
        for (int i = 0; i < k; i++) {
            const Block* b = q.claim();
            CHECK(b->entry_sqr >= prev[i] * (1 - 1e-5f));
            prev[i] = b->entry_sqr;
            q.release();
        }
        prev[k] = q.claim()->entry_sqr;
    }
}

TEST_CASE("random polylines always plan feasibly") {
    uint32_t seed = 12345;
    auto rnd = [&seed]() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0f; };
    for (int trial = 0; trial < 50; trial++) {
        Planner p;
        p.reset({0, 0});
        Vec2 at{0, 0};
        const int n = 5 + int(rnd() * 58);
        for (int i = 0; i < n; i++) {
            at = {at.x + (rnd() - 0.5f) * 40 * rnd(), at.y + (rnd() - 0.5f) * 40 * rnd()};
            REQUIRE(p.push(at, 50 + rnd() * 450, lim(), kDev));
        }
        replanCommit(p);
        checkPlan(p, 0);
    }
}

TEST_CASE("ring capacity and zero-length moves") {
    Planner p;
    p.reset({0, 0});
    CHECK(p.push({0, 0}, 300, lim(), kDev));
    CHECK(p.count() == 0);
    for (int i = 1; i <= Planner::kSize; i++) REQUIRE(p.push({float(i), 0}, 300, lim(), kDev));
    CHECK(p.full());
    CHECK_FALSE(p.push({1000, 0}, 300, lim(), kDev));
    CHECK(p.end().x == Planner::kSize);
}

TEST_CASE("ring wraps around") {
    Planner p;
    p.reset({0, 0});
    float x = 0;
    for (int round = 0; round < 5; round++) {
        while (!p.full()) { x += 1; REQUIRE(p.push({x, 0}, 300, lim(), kDev)); }
        replanCommit(p);
        for (int i = 0; i < 40; i++) { REQUIRE(p.claim()); p.release(); }
        replanCommit(p);
        checkPlan(p, -1);
    }
}
