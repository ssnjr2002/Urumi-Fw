/**
 * test_plan_contract.cpp — the CONTRACT tests for stage 6, ported from
 * web/test/toolpath/plan.test.ts.
 *
 * This is a different kind of test from test_plan.cpp's differential, and the
 * difference is the point:
 *
 *   the differential  asks "does the C++ agree with the TypeScript, bit for
 *                     bit?" — it cannot survive an optimisation, because an
 *                     optimisation is precisely a change that moves the bits.
 *   these             ask "does the planner do its job?" — they are stated in
 *                     terms of physics and invariants, so they survive any
 *                     rewrite that keeps the stage correct, and they are what
 *                     will catch a regression once bit-parity is retired.
 *
 * Method notes carried over from the TypeScript, each earned:
 *
 *   - Measure independently of the implementation where possible.
 *     `feasibilityViolations` recomputes segAccel the way plan does, so it
 *     validates the SWEEPS and is blind to segAccel itself being wrong;
 *     `axisAccelViolations` re-derives acceleration from planned speeds and
 *     geometry alone, so it catches what the self-consistent check cannot.
 *   - Aggregate across fixtures, then fail ONCE. An assertion inside a fixture
 *     loop reports the first violation and never reaches the worst one — a
 *     property test that hides its worst case behind its first is worse than
 *     no test, because it looks like it ran.
 *   - Pin exponents with scaling laws, not directions. "Goes up" passes for a
 *     dimensionally wrong formula; "4x the budget buys exactly 4x" does not.
 */

#include <doctest.h>

#include "motion/constrain.h"
#include "motion/flatten.h"
#include "motion/plan.h"

#include "support/curves.h"

#include <cmath>
#include <functional>
#include <string>
#include <vector>

using namespace motion;

namespace {

// ── the shipped fixture config, mirroring plan.test.ts ───────────────────────
// Sourced from web/src/config/defaults.ts and fixtures.ts. Hard-coded rather
// than parsed: a contract test that silently follows a config edit stops being
// a contract test.
constexpr double X_ACCEL = 1000.0;
constexpr double Y_ACCEL = 1000.0;
constexpr double A_ACCEL_DEG = 2000.0;
constexpr double A_MAX = 1000.0; // min(x, y)
constexpr double FEED = 80.0;
constexpr double V_MIN = 0.5;
constexpr double JUNCTION_DEV = 0.05;

FlattenOptions flattenOpts() {
    FlattenOptions o{};
    o.chordTol = 0.01;
    o.dsMax = 0.5;
    o.dthetaMax = 2.0;
    o.dtMax = 0.05;
    o.dtMin = 1e-6;
    o.maxRefine = 8;
    return o;
}

PlanOptions planOpts() {
    PlanOptions o{};
    o.xAccel = X_ACCEL;
    o.yAccel = Y_ACCEL;
    o.aAccelDegS2 = A_ACCEL_DEG;
    o.aMax = A_MAX;
    return o;
}

std::vector<ConstrainedSample> constrained(
    const std::vector<std::vector<CubicBezier>>& subpaths,
    double aRate = 0,
    bool hasCornerStop = false,
    double cornerStop = 0) {

    ConstrainOptions c{};
    c.feedMax = FEED;
    c.aMax = A_MAX;
    c.junctionDeviation = JUNCTION_DEV;
    c.aRateDegS = aRate;
    c.aAccelDegS2 = A_ACCEL_DEG;
    c.hasCornerStopAngle = hasCornerStop;
    c.cornerStopAngleDeg = cornerStop;
    // The production bridge passes this; the tests must too, or they measure a
    // pipeline nobody ships (audit C1).
    c.vMin = V_MIN;
    return constrain(flatten(subpaths, flattenOpts()), c);
}

std::vector<PlannedSample> prep(
    const std::vector<std::vector<CubicBezier>>& subpaths,
    double aRate = 0,
    bool hasCornerStop = false,
    double cornerStop = 0) {
    return plan(constrained(subpaths, aRate, hasCornerStop, cornerStop), planOpts());
}

std::vector<std::vector<CubicBezier>> one(const std::vector<CubicBezier>& c) {
    return {c};
}

CubicBezier line(Pt p0, Pt p1) { return lineToCubic(p0, p1); }

/**
 * Run `probe` over every fixture, collecting violation strings, and fail once
 * with all of them.
 */
void forEachFixture(
    const std::function<void(const std::string&,
                             const std::vector<CubicBezier>&,
                             std::vector<std::string>&)>& probe) {
    std::vector<std::string> violations;
    for (const curves::Case& c : curves::casesWithCusp()) {
        probe(c.first, *c.second, violations);
    }
    if (!violations.empty()) {
        std::string msg = std::to_string(violations.size()) + " violation(s):";
        for (const std::string& v : violations) msg += "\n  " + v;
        FAIL(msg);
    }
}

/**
 * The stage's own feasibility contract: between adjacent samples the speed
 * change must fit the segment accel budget in BOTH directions.
 *
 * Self-consistent by construction — it recomputes segAccel the same way plan
 * does, so it validates the sweeps, not the accel model.
 */
void feasibilityViolations(const std::vector<PlannedSample>& s,
                           const std::string& label,
                           std::vector<std::string>& out) {
    for (const Range& r : subpathRanges(s)) {
        for (size_t i = r.first; i < r.second; i++) {
            const double budget =
                2 * segAccel(s[i].s, s[i + 1].s, planOpts()) * s[i].s.ds + 1e-6;
            if (s[i + 1].v * s[i + 1].v > s[i].v * s[i].v + budget) {
                out.push_back(label + ": accel jump at " + std::to_string(i));
            }
            if (s[i].v * s[i].v > s[i + 1].v * s[i + 1].v + budget) {
                out.push_back(label + ": decel jump at " + std::to_string(i));
            }
        }
    }
}

/**
 * Total acceleration demanded of each axis, derived from speeds and geometry
 * ONLY — no plan internals. The tool's acceleration has two orthogonal
 * components (tangential dv/dt and centripetal v^2*kappa); each axis must
 * supply the projection of their VECTOR SUM, and bounding the two separately
 * is not the same as bounding the sum (audit P1).
 *
 * std::cos/std::sin are deliberate here: this is a tolerance-based measurement,
 * not a bit-parity path, so the owned transcendentals buy nothing.
 */
void axisAccelViolations(const std::vector<PlannedSample>& s,
                         const std::string& label,
                         std::vector<std::string>& out,
                         double tol = 1.001) {
    double worstX = 0;
    double worstY = 0;
    for (size_t i = 0; i + 1 < s.size(); i++) {
        const PlannedSample& a = s[i];
        const PlannedSample& b = s[i + 1];
        if (a.s.ds < 1e-9) continue;
        const double aTan = (b.v * b.v - a.v * a.v) / (2 * a.s.ds);
        const double aCen = a.v * a.v * a.s.kappa;
        const double th = (a.s.theta * PI) / 180;
        worstX = std::fmax(worstX, std::fabs(aTan * std::cos(th) - aCen * std::sin(th)));
        worstY = std::fmax(worstY, std::fabs(aTan * std::sin(th) + aCen * std::cos(th)));
    }
    if (worstX > X_ACCEL * tol) {
        out.push_back(label + ": |ax| " + std::to_string(worstX) + " = " +
                      std::to_string(worstX / X_ACCEL) + "x x.maxAccel");
    }
    if (worstY > Y_ACCEL * tol) {
        out.push_back(label + ": |ay| " + std::to_string(worstY) + " = " +
                      std::to_string(worstY / Y_ACCEL) + "x y.maxAccel");
    }
}

/** Peak planned speed over a stream. */
double peak(const std::vector<PlannedSample>& s) {
    double m = 0;
    for (const PlannedSample& p : s) m = std::fmax(m, p.v);
    return m;
}

} // namespace

// ═════════════════════════════════════════════════════════════════════════════
// INVARIANTS — must hold for every input, forever.
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("stage 6 INVARIANT: purity and determinism") {
    const std::vector<ConstrainedSample> in = constrained(one(curves::sCurve()));

    SUBCASE("does not mutate its input") {
        const std::vector<ConstrainedSample> before = in;
        plan(in, planOpts());
        REQUIRE(in.size() == before.size());
        for (size_t i = 0; i < in.size(); i++) {
            CHECK(in[i].vCeiling == before[i].vCeiling);
            CHECK(in[i].s.x == before[i].s.x);
            CHECK(in[i].s.kappa == before[i].s.kappa);
            CHECK(in[i].s.flags == before[i].s.flags);
        }
    }

    SUBCASE("is deterministic") {
        const std::vector<PlannedSample> a = plan(in, planOpts());
        const std::vector<PlannedSample> b = plan(in, planOpts());
        REQUIRE(a.size() == b.size());
        for (size_t i = 0; i < a.size(); i++) CHECK(a[i].v == b[i].v);
    }

    SUBCASE("preserves every sample and its geometry") {
        const std::vector<PlannedSample> out = plan(in, planOpts());
        REQUIRE(out.size() == in.size());
        for (size_t i = 0; i < out.size(); i++) {
            CHECK(out[i].s.x == in[i].s.x);
            CHECK(out[i].s.y == in[i].s.y);
            CHECK(out[i].s.theta == in[i].s.theta);
            CHECK(out[i].s.kappa == in[i].s.kappa);
            CHECK(out[i].s.ds == in[i].s.ds);
            CHECK(out[i].s.flags == in[i].s.flags);
            CHECK(out[i].vCeiling == in[i].vCeiling);
        }
    }
}

TEST_CASE("stage 6 INVARIANT: v is a sane, finite, bounded speed") {
    SUBCASE("0 <= v <= vCeiling, finite, on every sample of every fixture") {
        forEachFixture([](const std::string& name,
                          const std::vector<CubicBezier>& c,
                          std::vector<std::string>& out) {
            const std::vector<PlannedSample> s = prep(one(c));
            for (size_t i = 0; i < s.size(); i++) {
                if (!std::isfinite(s[i].v)) {
                    out.push_back(name + ": v[" + std::to_string(i) + "] not finite");
                } else if (s[i].v < 0) {
                    out.push_back(name + ": v[" + std::to_string(i) + "] negative");
                } else if (s[i].v > s[i].vCeiling + 1e-9) {
                    out.push_back(name + ": v[" + std::to_string(i) + "] exceeds its ceiling");
                }
            }
        });
    }

    SUBCASE("survives the cusp without NaN (zero speed, zero ds, huge kappa)") {
        const std::vector<PlannedSample> s = prep(one(curves::cusp()));
        REQUIRE(s.size() > 0);
        for (const PlannedSample& p : s) {
            REQUIRE(std::isfinite(p.v));
            REQUIRE(p.v >= 0);
        }
    }
}

TEST_CASE("stage 6 INVARIANT: subpath endpoints are at rest") {
    SUBCASE("every PATH_START and PATH_END sample plans to exactly 0") {
        forEachFixture([](const std::string& name,
                          const std::vector<CubicBezier>& c,
                          std::vector<std::string>& out) {
            const std::vector<PlannedSample> s = prep(one(c));
            for (size_t i = 0; i < s.size(); i++) {
                if ((s[i].s.flags & (PATH_START | PATH_END)) && s[i].v != 0.0) {
                    out.push_back(name + ": endpoint " + std::to_string(i) +
                                  " plans to " + std::to_string(s[i].v) + ", not 0");
                }
            }
        });
    }

    SUBCASE("plans each subpath independently — a slow one does not tax its neighbour") {
        const std::vector<PlannedSample> alone = prep(one(curves::straightLine()));
        const std::vector<PlannedSample> together =
            prep({curves::quarterCircleR5(), curves::straightLine()});
        // The straight line's peak must not change for having a tight arc in
        // front of it: subpaths are separated by a pen-up jog, not by motion.
        double peakTogether = 0;
        const std::vector<Range> ranges = subpathRanges(together);
        REQUIRE(ranges.size() == 2);
        for (size_t i = ranges[1].first; i <= ranges[1].second; i++) {
            peakTogether = std::fmax(peakTogether, together[i].v);
        }
        CHECK(peakTogether == doctest::Approx(peak(alone)).epsilon(1e-9));
    }
}

TEST_CASE("stage 6 INVARIANT: the feasibility contract of the two sweeps") {
    SUBCASE("every adjacent pair is reachable and stoppable, on every fixture") {
        forEachFixture([](const std::string& name,
                          const std::vector<CubicBezier>& c,
                          std::vector<std::string>& out) {
            feasibilityViolations(prep(one(c)), name, out);
        });
    }

    SUBCASE("holds across multiple subpaths in one stream") {
        std::vector<std::string> out;
        feasibilityViolations(
            prep({curves::sCurve(), curves::quarterCircleR5(), curves::straightLine()}),
            "multi", out);
        CHECK(out.empty());
    }

    SUBCASE("holds when a corner forces a mid-subpath stop") {
        std::vector<std::string> out;
        const std::vector<CubicBezier> elbow = {
            line({0, 0}, {20, 0}),
            line({20, 0}, {20, 20}),
        };
        feasibilityViolations(prep(one(elbow), 0, true, 30.0), "corner", out);
        CHECK(out.empty());
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// PROFILE SHAPE
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("stage 6: profile shape") {
    SUBCASE("a long line ramps up, cruises at feed, ramps down") {
        const std::vector<PlannedSample> s = prep(one({line({0, 0}, {200, 0})}));
        REQUIRE(s.size() > 10);
        CHECK(s.front().v == 0.0);
        CHECK(s.back().v == 0.0);
        // 200mm at 1000 mm/s^2 reaches 80 mm/s in 3.2mm — cruise dominates.
        CHECK(peak(s) == doctest::Approx(FEED).epsilon(1e-6));
    }

    SUBCASE("a short line is triangular — peak is sqrt(a*L), NOT feed") {
        // Symmetric ramp over length L from rest to rest: the two halves meet at
        // v = sqrt(2*a*(L/2)) = sqrt(a*L). Pinning the closed form (not merely
        // "< feed") is what makes a wrong accel model fail here.
        const double L = 4.0;
        const std::vector<PlannedSample> s = prep(one({line({0, 0}, {L, 0})}));
        CHECK(peak(s) < FEED);
        CHECK(peak(s) == doctest::Approx(std::sqrt(A_MAX * L)).epsilon(0.01));
    }

    SUBCASE("pathAccel reshapes the whole profile, not just segAccel") {
        // segAccel's pathAccel subcase is a unit check; this pins that the value
        // actually reaches the sweeps. A commanded 100 mm/s^2 makes a 20mm line
        // triangular where the machine's 1000 would have cruised at feed.
        const double L = 20.0;
        const auto peakOf = [&](double pathAccel) {
            PlanOptions o = planOpts();
            o.pathAccel = pathAccel;
            return peak(plan(constrained(one({line({0, 0}, {L, 0})})), o));
        };
        CHECK(peakOf(0) == doctest::Approx(FEED).epsilon(1e-6));
        CHECK(peakOf(100) == doctest::Approx(std::sqrt(100 * L)).epsilon(0.01));
    }

    SUBCASE("4x the length of a triangular move buys exactly 2x the peak") {
        // The scaling law, not the direction: v_peak ~ sqrt(L).
        const std::vector<PlannedSample> a = prep(one({line({0, 0}, {1.0, 0})}));
        const std::vector<PlannedSample> b = prep(one({line({0, 0}, {4.0, 0})}));
        REQUIRE(peak(a) < FEED);
        REQUIRE(peak(b) < FEED);
        CHECK(peak(b) / peak(a) == doctest::Approx(2.0).epsilon(0.02));
    }

    SUBCASE("a corner brings the path to rest on both sides") {
        const std::vector<CubicBezier> elbow = {
            line({0, 0}, {20, 0}),
            line({20, 0}, {20, 20}),
        };
        const std::vector<PlannedSample> s = prep(one(elbow), 0, true, 30.0);
        // Somewhere in the middle there must be a sample at rest that is not an
        // endpoint — the lift-pivot precondition.
        bool foundInteriorStop = false;
        for (size_t i = 1; i + 1 < s.size(); i++) {
            if (s[i].v == 0.0) foundInteriorStop = true;
        }
        CHECK(foundInteriorStop);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// MONOTONICITY — more budget never plans slower.
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("stage 6: monotonicity — more budget never plans slower") {
    // This is the property the P1 headroom term was deliberately shaped to
    // preserve: computing the centripetal load from the CEILING rather than
    // from a first pass's v costs a little tightness and buys this.
    const auto monotone = [](PlanOptions lo, PlanOptions hi, const char* label) {
        for (const curves::Case& c : curves::casesWithCusp()) {
            const std::vector<ConstrainedSample> in = constrained(one(*c.second));
            const std::vector<PlannedSample> a = plan(in, lo);
            const std::vector<PlannedSample> b = plan(in, hi);
            REQUIRE(a.size() == b.size());
            for (size_t i = 0; i < a.size(); i++) {
                if (b[i].v < a[i].v - 1e-9) {
                    FAIL(label << " lowered v at " << c.first << "[" << i
                               << "]: " << a[i].v << " -> " << b[i].v);
                }
            }
        }
    };

    SUBCASE("raising xAccel never lowers any planned speed") {
        PlanOptions lo = planOpts();
        PlanOptions hi = planOpts();
        hi.xAccel = X_ACCEL * 4;
        monotone(lo, hi, "xAccel");
    }

    SUBCASE("raising yAccel never lowers any planned speed") {
        PlanOptions lo = planOpts();
        PlanOptions hi = planOpts();
        hi.yAccel = Y_ACCEL * 4;
        monotone(lo, hi, "yAccel");
    }

    SUBCASE("raising aAccelDegS2 never lowers any planned speed") {
        PlanOptions lo = planOpts();
        PlanOptions hi = planOpts();
        hi.aAccelDegS2 = A_ACCEL_DEG * 4;
        monotone(lo, hi, "aAccelDegS2");
    }

    SUBCASE("raising pathAccel never lowers any planned speed") {
        PlanOptions lo = planOpts();
        lo.pathAccel = 200;
        PlanOptions hi = planOpts();
        hi.pathAccel = 800;
        monotone(lo, hi, "pathAccel");
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// segAccel
// ═════════════════════════════════════════════════════════════════════════════

namespace {
Sample sAt(double x, double y, double kappa = 0) {
    Sample s{};
    s.x = x;
    s.y = y;
    s.kappa = kappa;
    return s;
}
} // namespace

TEST_CASE("stage 6: segAccel") {
    const PlanOptions o = planOpts();

    SUBCASE("pure-X move is bounded by xAccel alone") {
        CHECK(segAccel(sAt(0, 0), sAt(10, 0), o) == doctest::Approx(X_ACCEL));
    }

    SUBCASE("pure-Y move is bounded by yAccel alone") {
        CHECK(segAccel(sAt(0, 0), sAt(0, 10), o) == doctest::Approx(Y_ACCEL));
    }

    SUBCASE("a non-square machine uses the right axis, not the aMax fallback") {
        PlanOptions ns = planOpts();
        ns.xAccel = 500;
        ns.yAccel = 2000;
        ns.aMax = 123456; // must not be reachable
        CHECK(segAccel(sAt(0, 0), sAt(10, 0), ns) == doctest::Approx(500));
        CHECK(segAccel(sAt(0, 0), sAt(0, 10), ns) == doctest::Approx(2000));
    }

    SUBCASE("a 45-degree diagonal allows exactly sqrt(2) times the axis limit") {
        // Each axis sees a/sqrt(2), so the tool may accelerate sqrt(2) faster
        // than a single scalar limit would allow.
        CHECK(segAccel(sAt(0, 0), sAt(10, 10), o) ==
              doctest::Approx(X_ACCEL * std::sqrt(2.0)).epsilon(1e-9));
    }

    SUBCASE("a degenerate (zero-length) segment falls back to the scalar aMax") {
        CHECK(segAccel(sAt(3, 3), sAt(3, 3), o) == doctest::Approx(A_MAX));
    }

    SUBCASE("an unlimited (0) axis does not constrain") {
        PlanOptions u = planOpts();
        u.xAccel = 0;
        u.aAccelDegS2 = 0;
        // Pure-X move with X unlimited and no other term: falls back to aMax.
        CHECK(segAccel(sAt(0, 0), sAt(10, 0), u) == doctest::Approx(A_MAX));
    }

    SUBCASE("the A term is rad(aAccel)/kappa — 4x aAccel buys exactly 4x") {
        PlanOptions a1 = planOpts();
        a1.xAccel = 0;
        a1.yAccel = 0;
        a1.aAccelDegS2 = 100;
        PlanOptions a4 = a1;
        a4.aAccelDegS2 = 400;
        const double r1 = segAccel(sAt(0, 0, 2.0), sAt(1, 0, 2.0), a1);
        const double r4 = segAccel(sAt(0, 0, 2.0), sAt(1, 0, 2.0), a4);
        CHECK(r4 / r1 == doctest::Approx(4.0).epsilon(1e-12));
        CHECK(r1 == doctest::Approx((100 * PI / 180) / 2.0).epsilon(1e-12));
    }

    SUBCASE("the A term is inverse-LINEAR in kappa — 10x kappa costs exactly 10x") {
        PlanOptions a = planOpts();
        a.xAccel = 0;
        a.yAccel = 0;
        const double lo = segAccel(sAt(0, 0, 0.1), sAt(1, 0, 0.1), a);
        const double hi = segAccel(sAt(0, 0, 1.0), sAt(1, 0, 1.0), a);
        CHECK(lo / hi == doctest::Approx(10.0).epsilon(1e-12));
    }

    SUBCASE("the A term uses the LARGER kappa of the pair (the conservative one)") {
        PlanOptions a = planOpts();
        a.xAccel = 0;
        a.yAccel = 0;
        const double mixed = segAccel(sAt(0, 0, 0.1), sAt(1, 0, 1.0), a);
        const double both = segAccel(sAt(0, 0, 1.0), sAt(1, 0, 1.0), a);
        CHECK(mixed == doctest::Approx(both).epsilon(1e-12));
    }

    SUBCASE("a zero kappa skips the A term entirely") {
        PlanOptions a = planOpts();
        a.xAccel = 0;
        a.yAccel = 0;
        CHECK(segAccel(sAt(0, 0, 0), sAt(1, 0, 0), a) == doctest::Approx(A_MAX));
    }

    SUBCASE("pathAccel caps the per-axis-derived limit") {
        PlanOptions p = planOpts();
        p.pathAccel = 250;
        CHECK(segAccel(sAt(0, 0), sAt(10, 0), p) == doctest::Approx(250));
    }

    SUBCASE("pathAccel above the derived limit changes nothing") {
        PlanOptions p = planOpts();
        p.pathAccel = 99999;
        CHECK(segAccel(sAt(0, 0), sAt(10, 0), p) ==
              doctest::Approx(segAccel(sAt(0, 0), sAt(10, 0), planOpts())));
    }

    SUBCASE("is symmetric in its two samples") {
        for (const curves::Case& c : curves::casesWithCusp()) {
            const std::vector<PlannedSample> s = prep(one(*c.second));
            for (size_t i = 0; i + 1 < s.size(); i++) {
                const double ab = segAccel(s[i].s, s[i + 1].s, o);
                const double ba = segAccel(s[i + 1].s, s[i].s, o);
                if (ab != ba) {
                    FAIL("asymmetric at " << c.first << "[" << i << "]: " << ab
                                          << " vs " << ba);
                }
            }
        }
    }

    SUBCASE("is always positive and finite, on every segment of every fixture") {
        forEachFixture([&o](const std::string& name,
                            const std::vector<CubicBezier>& c,
                            std::vector<std::string>& out) {
            const std::vector<PlannedSample> s = prep(one(c));
            for (size_t i = 0; i + 1 < s.size(); i++) {
                const double a = segAccel(s[i].s, s[i + 1].s, o);
                if (!std::isfinite(a) || a <= 0) {
                    out.push_back(name + ": segAccel[" + std::to_string(i) +
                                  "] = " + std::to_string(a));
                }
            }
        });
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// subpathRanges
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("stage 6: subpathRanges") {
    SUBCASE("yields one inclusive range per PATH_START..PATH_END") {
        const std::vector<PlannedSample> s =
            prep({curves::straightLine(), curves::sCurve(), curves::quarterCircleR5()});
        const std::vector<Range> r = subpathRanges(s);
        REQUIRE(r.size() == 3);
        for (const Range& x : r) {
            CHECK((s[x.first].s.flags & PATH_START) != 0u);
            CHECK((s[x.second].s.flags & PATH_END) != 0u);
        }
    }

    SUBCASE("handles a single-sample subpath (START and END on one sample)") {
        std::vector<Sample> s(1);
        s[0].flags = PATH_START | PATH_END;
        const std::vector<Range> r = subpathRanges(s);
        REQUIRE(r.size() == 1);
        CHECK(r[0].first == 0);
        CHECK(r[0].second == 0);
    }

    SUBCASE("covers every sample flatten produces, with no gaps or overlaps") {
        const std::vector<PlannedSample> s =
            prep({curves::straightLine(), curves::sCurve(), curves::fullCircleR30()});
        size_t next = 0;
        for (const Range& r : subpathRanges(s)) {
            CHECK(r.first == next);
            next = r.second + 1;
        }
        CHECK(next == s.size());
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// P1 (FIXED): the axis accel budget is one budget
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("stage 6 P1 (FIXED): the axis accel budget is one budget") {
    SUBCASE("no axis is asked for more acceleration than it has") {
        forEachFixture([](const std::string& name,
                          const std::vector<CubicBezier>& c,
                          std::vector<std::string>& out) {
            axisAccelViolations(prep(one(c)), name, out);
        });
    }

    SUBCASE("the cusp no longer reaches ~sqrt(2) x aMax") {
        std::vector<std::string> out;
        // The pre-fix behaviour measured 1412 against a 1000 limit. A tolerance
        // of 1.001 would have caught that by a factor of 400.
        axisAccelViolations(prep(one(curves::cusp())), "cusp", out);
        CHECK(out.empty());
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// P2: an unbracketed stream is refused, not silently unplanned
// ═════════════════════════════════════════════════════════════════════════════

namespace {
ConstrainedSample cs(uint32_t flags, double vCeiling = 10.0) {
    ConstrainedSample c{};
    c.s.flags = flags;
    c.s.ds = 1.0;
    c.vCeiling = vCeiling;
    return c;
}
} // namespace

TEST_CASE("stage 6 P2: an unbracketed stream is refused, not silently unplanned") {
    SUBCASE("throws on a stream with no PATH_START/PATH_END at all") {
        const std::vector<ConstrainedSample> s = {cs(0), cs(0), cs(0)};
        CHECK_THROWS_AS(plan(s, planOpts()), std::runtime_error);
    }

    SUBCASE("throws on a subpath whose PATH_END is missing") {
        const std::vector<ConstrainedSample> s = {cs(PATH_START), cs(0), cs(0)};
        CHECK_THROWS_AS(plan(s, planOpts()), std::runtime_error);
    }

    SUBCASE("throws on a gap BETWEEN two otherwise well-formed subpaths") {
        const std::vector<ConstrainedSample> s = {
            cs(PATH_START), cs(PATH_END),
            cs(0), // the gap
            cs(PATH_START), cs(PATH_END),
        };
        CHECK_THROWS_AS(plan(s, planOpts()), std::runtime_error);
    }

    SUBCASE("still accepts what flatten actually emits, single and multi subpath") {
        CHECK_NOTHROW(prep(one(curves::sCurve())));
        CHECK_NOTHROW(prep({curves::sCurve(), curves::straightLine()}));
    }

    SUBCASE("accepts an empty stream") {
        const std::vector<ConstrainedSample> empty;
        CHECK_NOTHROW(plan(empty, planOpts()));
    }
}
