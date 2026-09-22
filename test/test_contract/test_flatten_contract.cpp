/**
 * test_flatten_contract.cpp — CONTRACT tests for stage 4, ported from
 * web/test/toolpath/flatten.test.ts.
 *
 * Two kinds of test live here, and the distinction is the reason the file
 * exists:
 *
 *   1. INVARIANTS — true of any correct flattener. Endpoints preserved, flags
 *      placed, values finite, output deterministic, termination bounded.
 *   2. CONTRACT PROPERTIES — the three caps that DEFINE the stage:
 *          chord deviation <= chordTol
 *          sample spacing  <= dsMax
 *          tangent turn    <= dthetaMax
 *      These are its whole reason for existing. Two of the three had no test at
 *      all before the TypeScript file was rewritten, which is how audit F1
 *      survived as long as it did.
 *
 * The caps are measured IMPLEMENTATION-INDEPENDENTLY. Deviation is the distance
 * from a densely probed true curve to the emitted polyline — NOT a re-run of
 * flatten's own `dtAt` predictor. A test that re-derives the code under test
 * proves only that the code is self-consistent, and self-consistency is exactly
 * what F7 had: `dt <= dsMax/|B'(t)|` reads speed at the step START, so the
 * predictor agreed with itself while 142 of snake.svg's 356 steps landed long.
 *
 * Companion to test_parity/test_flatten.cpp. That file asks whether the bits
 * match V8; this one asks whether the sampler samples finely enough, and it is
 * the one that still means something after the port is optimised.
 */

#include <doctest.h>

#include "motion/flatten.h"
#include "motion/geometry.h"
#include "motion/sample.h"

#include "support/curves.h"
#include "support/quality.h"
#include "support/svgfix.h"

#include <cmath>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <string>
#include <vector>

using namespace motion;
using curves::casesWithCusp;

namespace {

std::vector<Sample> flat(const std::vector<CubicBezier>& c,
                         const FlattenOptions& o = quality::flattenOpts()) {
    return flatten({c}, o);
}

/**
 * Arc length by dense chord summation.
 *
 * Slower than quadrature and far more trustworthy on curves whose |B'| varies
 * sharply. geometry.ts once exported a 5-point Gauss-Legendre arcLength() that
 * under-reported a near-cusp badly, and a test was pinned to that error — it
 * asserted chordSum >= GL5, which is only true because GL5 was wrong, since a
 * chord sum can never exceed true arc length. That function had no production
 * caller and was not ported (audit F6).
 */
double denseArcLength(const CubicBezier& c, int n = 20000) {
    double total = 0;
    Pt prev = bezierPoint(c, 0);
    for (int i = 1; i <= n; i++) {
        const Pt p = bezierPoint(c, static_cast<double>(i) / n);
        total += std::hypot(p.x - prev.x, p.y - prev.y);
        prev = p;
    }
    return total;
}

double denseArcLengthAll(const std::vector<CubicBezier>& cs) {
    double sum = 0;
    for (const CubicBezier& c : cs) sum += denseArcLength(c);
    return sum;
}

/** Perpendicular distance from p to segment ab, or to the nearer endpoint. */
double distToSegment(Pt p, Pt a, Pt b) {
    const double vx = b.x - a.x;
    const double vy = b.y - a.y;
    const double len2 = vx * vx + vy * vy;
    if (len2 < 1e-24) return std::hypot(p.x - a.x, p.y - a.y);
    double t = ((p.x - a.x) * vx + (p.y - a.y) * vy) / len2;
    t = std::fmax(0.0, std::fmin(1.0, t));
    return std::hypot(p.x - (a.x + t * vx), p.y - (a.y + t * vy));
}

/**
 * One-sided Hausdorff distance from the true curves to the emitted polyline:
 * for each densely probed point on the real geometry, how far is the nearest
 * point on the polyline the machine will actually travel?
 *
 * This is what chordTol MEANS. It needs none of flatten's `t` values, so it
 * cannot be fooled by a predictor that is self-consistent but wrong.
 */
double polylineDeviation(const std::vector<CubicBezier>& cs,
                         const std::vector<Sample>& s,
                         int probesPerCurve = 400) {
    double worst = 0;
    for (const CubicBezier& c : cs) {
        for (int i = 0; i <= probesPerCurve; i++) {
            const Pt p = bezierPoint(c, static_cast<double>(i) / probesPerCurve);
            double best = 1e308;
            for (size_t j = 0; j + 1 < s.size(); j++) {
                const double d = distToSegment(p, Pt{s[j].x, s[j].y},
                                               Pt{s[j + 1].x, s[j + 1].y});
                if (d < best) best = d;
                if (best == 0) break;
            }
            if (best > worst) worst = best;
        }
    }
    return worst;
}

/** Shortest absolute angular difference in degrees, range [0, 180]. */
double absAngleDelta(double a, double b) {
    double d = std::fmod(std::fabs(b - a), 360.0);
    if (d > 180) d = 360 - d;
    return d;
}

std::string fmt(double v, int prec = 6) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*g", prec, v);
    return std::string(buf);
}

/**
 * Run `probe` over every fixture and fail ONCE with all violations.
 *
 * Not cosmetic. A bare CHECK inside a fixture loop reports each violation where
 * it happens but buries the worst one in a wall of output, and a REQUIRE aborts
 * before reaching it — the first version of the TypeScript file failed on
 * `near_cusp` (2.8deg, a mild predictor overshoot) and never reached `cusp`
 * (178deg, the actual defect). A property test that hides its worst case behind
 * its first is worse than no test, because it looks like it ran.
 */
void forEachFixture(
    const std::function<std::vector<std::string>(const std::string&,
                                                 const std::vector<CubicBezier>&)>& probe) {
    std::vector<std::string> problems;
    for (const curves::Case& c : casesWithCusp()) {
        for (std::string& s : probe(c.first, *c.second)) problems.push_back(s);
    }
    if (problems.empty()) {
        CHECK(true);
        return;
    }
    std::string msg = std::to_string(problems.size()) + " violation(s):";
    for (size_t i = 0; i < problems.size() && i < 12; i++) msg += "\n  " + problems[i];
    if (problems.size() > 12) {
        msg += "\n  ...and " + std::to_string(problems.size() - 12) + " more";
    }
    FAIL(msg);
}

} // namespace

// ═══ 1. INVARIANTS ═══════════════════════════════════════════════════════════

TEST_CASE("flatten: arc length") {
    SUBCASE("chord sum never exceeds true arc length") {
        // Structural: a chord is the shortest path between its endpoints, so the
        // polyline can only under-measure. If this ever fails, the samples are
        // not ordered along the curve.
        for (const curves::Case& kase : casesWithCusp()) {
            CAPTURE(kase.first);
            double chordTotal = 0;
            for (const Sample& s : flat(*kase.second)) chordTotal += s.ds;
            const double trueLen = denseArcLengthAll(*kase.second);
            CHECK_MESSAGE(chordTotal <= trueLen + 1e-9,
                          kase.first << ": chord " << chordTotal << " > true " << trueLen);
        }
    }

    SUBCASE("chord sum is within 0.5% of true arc length") {
        // Density, not correctness: enough samples that the polyline does not
        // visibly short-cut the curve. Cusps included — no exemptions.
        for (const curves::Case& kase : casesWithCusp()) {
            CAPTURE(kase.first);
            double chordTotal = 0;
            for (const Sample& s : flat(*kase.second)) chordTotal += s.ds;
            const double trueLen = denseArcLengthAll(*kase.second);
            const double err = std::fabs(chordTotal - trueLen) / trueLen;
            CHECK_MESSAGE(err < 0.005,
                          kase.first << ": chord " << chordTotal << " vs true " << trueLen);
        }
    }

    SUBCASE("straight line measures 100mm") {
        double total = 0;
        for (const Sample& s : flat(curves::straightLine())) total += s.ds;
        CHECK(std::fabs(total - 100.0) < 0.01);
    }
}

TEST_CASE("flatten: endpoints") {
    SUBCASE("first sample is p0 of the first curve, last is p3 of the last") {
        for (const curves::Case& kase : casesWithCusp()) {
            CAPTURE(kase.first);
            const std::vector<Sample> s = flat(*kase.second);
            REQUIRE(!s.empty());
            const Pt p0 = kase.second->front().p0;
            const Pt p3 = kase.second->back().p3;
            CHECK_MESSAGE(std::hypot(s.front().x - p0.x, s.front().y - p0.y) < 1e-9,
                          kase.first << ": start");
            CHECK_MESSAGE(std::hypot(s.back().x - p3.x, s.back().y - p3.y) < 1e-6,
                          kase.first << ": end");
        }
    }
}

TEST_CASE("flatten: curvature") {
    SUBCASE("quarter circle r50 — kappa ~ 0.02 at every sample") {
        for (const Sample& s : flat(curves::quarterCircleR50())) {
            CHECK(std::fabs(s.kappa - 0.02) < 0.02 * 0.05);
        }
    }

    SUBCASE("straight line — kappa ~ 0 everywhere") {
        double mx = 0;
        for (const Sample& s : flat(curves::straightLine())) mx = std::fmax(mx, s.kappa);
        CHECK(mx < 1e-6);
    }

    SUBCASE("kappa is finite and non-negative on every fixture") {
        // Cheap, but the cusp fixture drives |B'| to exactly zero and kappa is
        // |B'xB''|/|B'|^3 — one missing guard away from inf or NaN, which would
        // propagate silently through constrain into a bad velocity.
        for (const curves::Case& kase : casesWithCusp()) {
            CAPTURE(kase.first);
            for (const Sample& s : flat(*kase.second)) {
                CHECK(std::isfinite(s.kappa));
                CHECK(s.kappa >= 0.0);
            }
        }
    }

    SUBCASE("every emitted field is finite") {
        for (const curves::Case& kase : casesWithCusp()) {
            CAPTURE(kase.first);
            for (const Sample& s : flat(*kase.second)) {
                CHECK(std::isfinite(s.x));
                CHECK(std::isfinite(s.y));
                CHECK(std::isfinite(s.theta));
                CHECK(std::isfinite(s.ds));
            }
        }
    }
}

TEST_CASE("flatten: flags") {
    SUBCASE("s_curve — exactly one PATH_START and one PATH_END") {
        const std::vector<Sample> s = flat(curves::sCurve());
        CHECK((s.front().flags & PATH_START) != 0u);
        CHECK((s.back().flags & PATH_END) != 0u);
        size_t starts = 0, ends = 0;
        for (const Sample& x : s) {
            if (x.flags & PATH_START) starts++;
            if (x.flags & PATH_END) ends++;
        }
        CHECK(starts == 1u);
        CHECK(ends == 1u);
    }

    SUBCASE("CURVE_BOUNDARY count is (curves - 1) on every fixture") {
        // Was asserted for two hand-picked fixtures; it is a general rule.
        for (const curves::Case& kase : casesWithCusp()) {
            CAPTURE(kase.first);
            size_t n = 0;
            for (const Sample& s : flat(*kase.second)) {
                if (s.flags & CURVE_BOUNDARY) n++;
            }
            CHECK(n == kase.second->size() - 1);
        }
    }
}

TEST_CASE("flatten: multi-subpath") {
    SUBCASE("two subpaths — flags doubled and ds does not bridge the gap") {
        const std::vector<Sample> s =
            flatten({curves::straightLine(), curves::quarterCircleR50()},
                    quality::flattenOpts());
        size_t starts = 0, ends = 0;
        std::vector<size_t> startAt;
        for (size_t i = 0; i < s.size(); i++) {
            if (s[i].flags & PATH_START) { starts++; startAt.push_back(i); }
            if (s[i].flags & PATH_END) ends++;
        }
        CHECK(starts == 2u);
        CHECK(ends == 2u);
        REQUIRE(startAt.size() == 2u);
        // The sample before the second subpath's first is the first subpath's
        // last; its ds must be 0, not the jump across the pen-up move.
        CHECK(s[startAt[1] - 1].ds == 0.0);
    }
}

TEST_CASE("flatten: determinism") {
    SUBCASE("same input produces identical output") {
        // The C++ port must reproduce this stream. A stage that is not
        // deterministic cannot be checked for parity at all.
        for (const curves::Case& kase : casesWithCusp()) {
            CAPTURE(kase.first);
            const std::vector<Sample> a = flat(*kase.second);
            const std::vector<Sample> b = flat(*kase.second);
            REQUIRE(a.size() == b.size());
            for (size_t i = 0; i < a.size(); i++) {
                // Bit-level, not approximate: two runs of the same pure function
                // on the same input have no licence to differ at all.
                CHECK(testbits::sameBits(a[i].x, b[i].x));
                CHECK(testbits::sameBits(a[i].y, b[i].y));
                CHECK(testbits::sameBits(a[i].theta, b[i].theta));
                CHECK(testbits::sameBits(a[i].kappa, b[i].kappa));
                CHECK(testbits::sameBits(a[i].ds, b[i].ds));
                CHECK(a[i].flags == b[i].flags);
            }
        }
    }

    SUBCASE("terminates with a bounded sample count on every fixture") {
        // The marcher is `while (t < 1)` with a dtMin floor. A cusp that drove
        // the step to the floor would emit ~1/dtMin = 1e6 samples per curve,
        // which no bounded Pico-side window could hold.
        for (const curves::Case& kase : casesWithCusp()) {
            CAPTURE(kase.first);
            const size_t n = flat(*kase.second).size();
            CHECK_MESSAGE(n < 20000u, kase.first << ": " << n << " samples");
        }
    }
}

// ═══ 2. CONTRACT PROPERTIES — the three caps ═════════════════════════════════

TEST_CASE("flatten: cap 1 — chord deviation <= chordTol") {
    forEachFixture([](const std::string& name, const std::vector<CubicBezier>& cs) {
        const double dev = polylineDeviation(cs, flat(cs));
        if (dev <= quality::CHORD_TOL + 1e-9) return std::vector<std::string>{};
        return std::vector<std::string>{
            name + ": deviation " + fmt(dev) + "mm > chordTol " +
            fmt(quality::CHORD_TOL)};
    });
}

TEST_CASE("flatten: cap 2 — spacing <= dsMax") {
    // Previously asserted on the straight line only — the one case where the cap
    // is trivially satisfied.
    //
    // WAS FAILING (audit F7): `dt <= dsMax / |B'(t)|` reads speed at the step
    // START, so wherever the curve accelerated across a step the chord landed
    // longer than dsMax — a predictor, not a guarantee. The step is now MEASURED
    // after being proposed and halved on overshoot, which makes the cap a bound.
    forEachFixture([](const std::string& name, const std::vector<CubicBezier>& cs) {
        const std::vector<Sample> s = flat(cs);
        double worst = 0;
        size_t count = 0;
        for (size_t i = 0; i + 1 < s.size(); i++) {
            if (s[i].ds > quality::DS_MAX + 1e-9) {
                count++;
                worst = std::fmax(worst, s[i].ds);
            }
        }
        if (count == 0) return std::vector<std::string>{};
        return std::vector<std::string>{
            name + ": " + std::to_string(count) + " step(s) over dsMax, worst " +
            fmt(worst, 10) + "mm (excess " + fmt(worst - quality::DS_MAX, 3) + "mm)"};
    });
}

TEST_CASE("flatten: cap 3 — tangent turn <= dthetaMax") {
    SUBCASE("holds between consecutive samples within a curve") {
        // Curve joins are excluded: a tangent jump THERE is the corner signal
        // constrain reads, and is deliberate. Anywhere else it is an unplanned
        // discontinuity the planner never decelerates for.
        //
        // The cusp is EXEMPT, and that is a correction to audit F1 rather than a
        // concession. A true cusp reverses the tangent at a single parameter
        // value, so the realised turn tends to 180deg however small the step
        // gets — "force fine sampling at a cusp" is not achievable, because there
        // is no sampling density at which a reversal is a small turn. The
        // pipeline reads it as the CORNER it is instead: constrain stops there
        // and discretize lift-pivots. The next SUBCASE bounds the exemption.
        forEachFixture([](const std::string& name, const std::vector<CubicBezier>& cs) {
            if (name == "cusp") return std::vector<std::string>{};
            const std::vector<Sample> s = flat(cs);
            double worst = 0;
            size_t at = 0;
            for (size_t i = 1; i < s.size(); i++) {
                if (s[i].flags & (CURVE_BOUNDARY | PATH_START)) continue;
                const double turn = absAngleDelta(s[i - 1].theta, s[i].theta);
                if (turn > worst) { worst = turn; at = i; }
            }
            if (worst <= quality::DTHETA_MAX + 1e-9) return std::vector<std::string>{};
            return std::vector<std::string>{
                name + ": sample " + std::to_string(at) + " turned " + fmt(worst, 5) +
                "deg (" + fmt(worst / quality::DTHETA_MAX, 3) + "x dthetaMax)"};
        });
    }

    SUBCASE("a true cusp overshoots at exactly one sample, and it is a reversal") {
        // The exemption above, bounded. If refinement ever started giving up
        // early on ORDINARY geometry this count would climb; if the cusp stopped
        // being read as a near-reversal, the pipeline's corner handling would
        // silently stop applying to it.
        const std::vector<Sample> s = flat(curves::cusp());
        std::vector<size_t> over;
        for (size_t i = 1; i < s.size(); i++) {
            if (s[i].flags & (CURVE_BOUNDARY | PATH_START)) continue;
            if (absAngleDelta(s[i - 1].theta, s[i].theta) > quality::DTHETA_MAX + 1e-9) {
                over.push_back(i);
            }
        }
        REQUIRE(over.size() == 1u);
        CHECK(absAngleDelta(s[over[0] - 1].theta, s[over[0]].theta) > 170.0);
    }

    SUBCASE("refinement is bounded: maxRefine caps what a cusp costs in samples") {
        // The firmware ships one fixed maxRefine and a sample-count-bounded
        // window; the host may raise it. Depth 4 already removes every overshoot
        // on every fixture, so the shipped 8 is margin, not need.
        const auto at = [](int maxRefine) {
            return flatten({curves::cusp()}, quality::flattenOpts(maxRefine)).size();
        };
        CHECK(at(0) == 78u);
        CHECK(at(4) == at(8));
        CHECK(at(8) == at(16));
        CHECK(at(8) < at(0) * 2);
    }
}

TEST_CASE("flatten: sample hygiene") {
    SUBCASE("emits no degenerate slivers inside a curve") {
        // `t = min(t + dt, 1)` truncates the final step of every curve, which can
        // emit an arbitrarily short segment (audit F3). Near-zero ds AT a curve
        // join is legitimate — the two samples share a position by design — so
        // those are excluded.
        forEachFixture([](const std::string& name, const std::vector<CubicBezier>& cs) {
            const std::vector<Sample> s = flat(cs);
            std::vector<size_t> bad;
            for (size_t i = 0; i + 1 < s.size(); i++) {
                if (s[i + 1].flags & (CURVE_BOUNDARY | PATH_END)) continue;
                if (s[i].ds <= 1e-6) bad.push_back(i);
            }
            if (bad.empty()) return std::vector<std::string>{};
            std::string at;
            for (size_t i = 0; i < bad.size() && i < 5; i++) {
                at += (i ? ", " : "") + std::to_string(bad[i]);
            }
            return std::vector<std::string>{
                name + ": " + std::to_string(bad.size()) + " sliver(s) at sample(s) " + at};
        });
    }
}

// ═══ 3. REAL SVG ═════════════════════════════════════════════════════════════
// The caps applied to real artwork, which is where they actually have to hold.
// Every cap failure the audit found (F1, F7) was on geometry that accelerates
// across a step harder than any hand-built fixture does — porting these tests
// without the artwork would port the easy half.

TEST_CASE("flatten: real SVG") {
    const svgfix::Subpaths& repaired = svgfix::load("snake");
    REQUIRE_MESSAGE(!repaired.empty(),
                    "test/data/svg_fixtures.txt missing — regenerate with "
                    "`cd web && GEN_CPP_REF=1 npx vitest run test/port/cppRefFixtures`");

    SUBCASE("snake.svg — spacing cap holds") {
        const std::vector<Sample> s = flatten(repaired, quality::flattenOpts());
        double worst = 0;
        size_t count = 0;
        for (size_t i = 0; i + 1 < s.size(); i++) {
            if (s[i].flags & PATH_END) continue;
            if (s[i].ds > quality::DS_MAX + 1e-9) {
                count++;
                worst = std::fmax(worst, s[i].ds);
            }
        }
        CHECK_MESSAGE(count == 0u,
                      count << " steps over dsMax, worst " << fmt(worst));
    }

    SUBCASE("snake.svg — tangent cap holds within curves") {
        const std::vector<Sample> s = flatten(repaired, quality::flattenOpts());
        double worst = 0;
        size_t at = 0;
        for (size_t i = 1; i < s.size(); i++) {
            if (s[i].flags & (CURVE_BOUNDARY | PATH_START)) continue;
            const double turn = absAngleDelta(s[i - 1].theta, s[i].theta);
            if (turn > worst) { worst = turn; at = i; }
        }
        CHECK_MESSAGE(worst <= quality::DTHETA_MAX + 1e-9,
                      "sample " << at << " turned " << fmt(worst, 5) << "deg");
    }

    SUBCASE("snake.svg — chord deviation holds per subpath") {
        for (size_t k = 0; k < repaired.size(); k++) {
            CAPTURE(k);
            const double dev = polylineDeviation(
                repaired[k], flatten({repaired[k]}, quality::flattenOpts()), 120);
            CHECK_MESSAGE(dev <= quality::CHORD_TOL + 1e-9,
                          "subpath " << k << ": " << fmt(dev) << "mm");
        }
    }
}

// ═══ 4. CORNER SIGNAL ════════════════════════════════════════════════════════

TEST_CASE("flatten: corner signal") {
    SUBCASE("a 90-degree join appears as a tangent jump across a ~zero-length step") {
        // This is the contract constrain depends on: same position, different
        // theta, CURVE_BOUNDARY set. If flatten ever stops emitting the duplicate
        // sample, corner detection silently stops working.
        const std::vector<CubicBezier> elbow = {
            lineToCubic(Pt{0, 0}, Pt{10, 0}),
            lineToCubic(Pt{10, 0}, Pt{10, 10}),
        };
        const std::vector<Sample> s = flat(elbow);
        size_t bi = 0;
        for (size_t i = 0; i < s.size(); i++) {
            if (s[i].flags & CURVE_BOUNDARY) { bi = i; break; }
        }
        REQUIRE(bi > 0u);
        CHECK(std::fabs(absAngleDelta(s[bi - 1].theta, s[bi].theta) - 90.0) < 1.0);
        CHECK(s[bi - 1].ds < 1e-6);
    }

    SUBCASE("the cusp reversal is NOT reported as a curve boundary") {
        // Documents the asymmetry behind audit F2: a cusp is a tangent
        // discontinuity with no CURVE_BOUNDARY flag, so constrain's corner branch
        // never inspected it while discretize's ungated dtheta check did — the
        // two stages disagreed about what a corner is. This passes today and
        // should be REVISITED, not deleted, when F2 is resolved.
        const std::vector<Sample> s = flat(curves::cusp());
        size_t flagged = 0;
        double worst = 0;
        for (size_t i = 0; i < s.size(); i++) {
            if (s[i].flags & CURVE_BOUNDARY) flagged++;
            if (i > 0) worst = std::fmax(worst, absAngleDelta(s[i - 1].theta, s[i].theta));
        }
        CHECK(flagged == 0u);
        // The reversal is real and large; nothing in the sample stream marks it.
        CHECK(worst > 45.0);
    }
}

// ═══ 5. TANGENT CONTINUITY vs THE REAL CURVE ═════════════════════════════════

TEST_CASE("flatten: theta tracks the real tangent") {
    SUBCASE("each sample's theta matches B'(t) at that point") {
        // theta drives the A axis on a tangential tool. It is computed from B'
        // with a fallback to the PREVIOUS theta when |B'| ~ 0 — so at a cusp the
        // stream reports a tangent the curve does not have. Verify against the
        // geometry directly.
        const std::vector<CubicBezier>& cs = curves::quarterCircleR50();
        const std::vector<Sample> s = flat(cs);
        REQUIRE(s.size() > 1u);
        for (size_t i = 0; i < s.size(); i++) {
            CAPTURE(i);
            const double t = static_cast<double>(i) / static_cast<double>(s.size() - 1);
            const Pt d = bezierDeriv1(cs[0], t);
            const double trueTheta = std::atan2(d.y, d.x) * 180.0 / PI;
            // Sampling is not uniform in t, so allow a generous band; this is a
            // sanity check on sign and branch, not on placement.
            CHECK(absAngleDelta(s[i].theta, trueTheta) < 15.0);
        }
    }
}
