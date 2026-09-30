/**
 * Planner contract: every committed plan is continuous and feasible, respects
 * junction limits, ends at rest, raises the claimed block's exit only through
 * a staged piece, and only rises as more blocks arrive.
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
        CHECK(b->entry_sqr <= b->path.v_max_sqr * (1 + rel));
        CHECK(b->exit_sqr <= b->path.v_max_sqr * (1 + rel));
        if (n > 0) CHECK(b->entry_sqr <= b->max_entry_sqr * (1 + rel) + 1e-3f);
        const float span = 2 * b->path.accel * b->path.length;
        CHECK(fabsf(b->entry_sqr - b->exit_sqr) <= span * (1 + rel) + 1e-3f);
        CHECK(b->profile.position(b->profile.duration()) ==
              doctest::Approx(b->path.length).epsilon(1e-4));
        prev_exit = b->exit_sqr;
        p.release();
        n++;
    }
    CHECK(prev_exit == 0);
}

TEST_CASE("single line plans to rest at both ends") {
    Planner p;
    p.reset({0, 0});
    REQUIRE(p.pushLine({100, 0}, 300, lim(), kDev));
    replanCommit(p);
    checkPlan(p, 0);
    const Block* b = p.claim();
    CHECK(b->profile.v_cruise == doctest::Approx(300));
}

TEST_CASE("collinear chain of short lines cruises through its joins") {
    Planner p;
    p.reset({0, 0});
    for (int i = 1; i <= 50; i++) REQUIRE(p.pushLine({float(i), 0}, 300, lim(), kDev));
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
    for (const Vec2& v : pts) REQUIRE(p.pushLine(v, 300, lim(), kDev));
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
    REQUIRE(p.pushLine({20, 0}, 300, lim(), kDev));
    REQUIRE(p.pushLine({0, 0}, 300, lim(), kDev));
    replanCommit(p);
    checkPlan(p, 0);
    CHECK(p.claim()->exit_sqr == 0);
}

TEST_CASE("claimed block's exit rises; the next entry follows") {
    Planner p;
    p.reset({0, 0});
    REQUIRE(p.pushLine({10, 0}, 300, lim(), kDev));
    replanCommit(p);
    const Block* running = p.claim();
    const Trapezoid before = running->profile;
    REQUIRE(running->exit_sqr == 0);

    for (int i = 2; i <= 20; i++) REQUIRE(p.pushLine({10.0f * i, 0}, 300, lim(), kDev));
    const float t = 0.01f;
    p.replan(t);
    REQUIRE(p.commit(t));

    // The block's own profile is kept; the rise travels as a staged piece.
    CHECK(running->profile.duration() == before.duration());
    CHECK(running->exit_sqr > 0);
    Piece piece;
    REQUIRE(p.takeStaged(piece));
    CHECK_FALSE(p.takeStaged(piece));
    CHECK(piece.t0 > t);
    CHECK(piece.s0 == doctest::Approx(before.position(piece.t0)));
    CHECK(piece.profile.v_entry == doctest::Approx(before.velocity(piece.t0)).epsilon(1e-4));
    CHECK(piece.profile.v_exit * piece.profile.v_exit == doctest::Approx(running->exit_sqr).epsilon(1e-4));
    CHECK(piece.s0 + piece.profile.length == doctest::Approx(10));

    const float exit_sqr = running->exit_sqr;
    p.release();
    CHECK(p.claim()->entry_sqr == doctest::Approx(exit_sqr));
}

TEST_CASE("the horizon: replace, wait, then a fresh horizon") {
    Planner p;
    p.reset({0, 0});
    p.setTiming(0.005f, 0.0012f);
    REQUIRE(p.pushLine({50, 0}, 300, lim(), kDev));
    replanCommit(p);
    REQUIRE(p.claim());

    // First offer at t + 5 ms.
    REQUIRE(p.pushLine({60, 0}, 300, lim(), kDev));
    p.replan(0.100f);
    REQUIRE(p.commit(0.100f));
    Piece first;
    REQUIRE(p.takeStaged(first));
    CHECK(first.t0 == doctest::Approx(0.105f));

    // Case 1: well before the switch, replaced from the same horizon.
    REQUIRE(p.pushLine({70, 0}, 300, lim(), kDev));
    p.replan(0.101f);
    REQUIRE(p.commit(0.101f));
    Piece second;
    REQUIRE(p.takeStaged(second));
    CHECK(second.t0 == first.t0);
    CHECK(second.s0 == first.s0);
    CHECK(second.profile.v_exit > first.profile.v_exit);

    // Case 2: within the guard of the switch, refused.
    REQUIRE(p.pushLine({80, 0}, 300, lim(), kDev));
    p.replan(0.104f);
    CHECK_FALSE(p.commit(0.104f));

    // Case 3: past the switch, a fresh horizon on the piece now running.
    p.replan(0.106f);
    REQUIRE(p.commit(0.106f));
    Piece third;
    REQUIRE(p.takeStaged(third));
    CHECK(third.t0 == doctest::Approx(0.111f));
    const float u = third.t0 - second.t0;
    CHECK(third.s0 == doctest::Approx(second.s0 + second.profile.position(u)));
}

TEST_CASE("an offer is refused while the last is untaken or too close") {
    Planner p;
    p.reset({0, 0});
    REQUIRE(p.pushLine({50, 0}, 300, lim(), kDev));
    replanCommit(p);
    REQUIRE(p.claim());
    REQUIRE(p.pushLine({60, 0}, 300, lim(), kDev));
    p.replan(0.1f);
    REQUIRE(p.commit(0.1f));

    // Not taken yet: a replacement must wait.
    REQUIRE(p.pushLine({70, 0}, 300, lim(), kDev));
    p.replan(0.1f);
    CHECK_FALSE(p.commit(0.1f));
    Piece piece;
    REQUIRE(p.takeStaged(piece));
    p.replan(0.1f);
    CHECK(p.commit(0.1f));

    // The consumer's clock passed the horizon during replan.
    REQUIRE(p.pushLine({80, 0}, 300, lim(), kDev));
    REQUIRE(p.takeStaged(piece));
    p.replan(0.2f);
    CHECK_FALSE(p.commit(0.2f + 0.005f));
}

TEST_CASE("no offer once the rest of the block cannot use more speed") {
    Planner p;
    p.reset({0, 0});
    REQUIRE(p.pushLine({10, 0}, 300, lim(), kDev));
    replanCommit(p);
    const Block* running = p.claim();
    const float end = running->profile.duration();
    REQUIRE(p.pushLine({20, 0}, 300, lim(), kDev));
    p.replan(end - 0.001f);   // horizon past the block's end
    REQUIRE(p.commit(end - 0.001f));
    Piece piece;
    CHECK_FALSE(p.takeStaged(piece));
    CHECK(running->exit_sqr == 0);
    p.release();
    CHECK(p.claim()->entry_sqr == 0);
}

TEST_CASE("commit refuses a plan made stale by claim, release or push") {
    Planner p;
    p.reset({0, 0});
    for (int i = 1; i <= 5; i++) REQUIRE(p.pushLine({10.0f * i, 0}, 300, lim(), kDev));

    p.replan();
    p.claim();
    CHECK_FALSE(p.commit());

    p.replan();
    p.release();
    CHECK_FALSE(p.commit());

    p.replan();
    REQUIRE(p.pushLine({60, 0}, 300, lim(), kDev));
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
        REQUIRE(p.pushLine(pts[k], 300, lim(), kDev));
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
            REQUIRE(p.pushLine(at, 50 + rnd() * 450, lim(), kDev));
        }
        replanCommit(p);
        checkPlan(p, 0);
    }
}

TEST_CASE("ring capacity and zero-length moves") {
    Planner p;
    p.reset({0, 0});
    CHECK(p.pushLine({0, 0}, 300, lim(), kDev));
    CHECK(p.count() == 0);
    for (int i = 1; i <= Planner::kSize; i++) REQUIRE(p.pushLine({float(i), 0}, 300, lim(), kDev));
    CHECK(p.full());
    CHECK_FALSE(p.pushLine({1000, 0}, 300, lim(), kDev));
    CHECK(p.end().x == Planner::kSize);
}

TEST_CASE("ring wraps around") {
    Planner p;
    p.reset({0, 0});
    float x = 0;
    for (int round = 0; round < 5; round++) {
        while (!p.full()) { x += 1; REQUIRE(p.pushLine({x, 0}, 300, lim(), kDev)); }
        replanCommit(p);
        for (int i = 0; i < 40; i++) { REQUIRE(p.claim()); p.release(); }
        replanCommit(p);
        checkPlan(p, -1);
    }
}
