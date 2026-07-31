/**
 * test_constrain_contract.cpp — CONTRACT tests for stage 5, ported from
 * web/test/toolpath/constrain.test.ts.
 *
 * Same split as test_flatten_contract.cpp:
 *
 *   1. INVARIANTS — true of any correct constrain. Bounded output, purity,
 *      determinism, geometry preserved.
 *   2. CONTRACT PROPERTIES — the caps that define the stage:
 *        vCeiling <= feedMax
 *        vCeiling <= sqrt(aMax / kappa)              centripetal
 *        vCeiling <= rad(aRateDegS) / kappa          A slew
 *        vCeiling <= sqrt(rad(aAccelDegS2) / |k'|)   A angular accel
 *        vCeiling == 0 at a corner stop or a forcedStop
 *      Asserted over every fixture and every sample, not at one hand-picked
 *      sample on one fixture, which is what the originals did.
 *
 * Where a cap's formula would have to be re-derived here to check it directly
 * (the curvature-gradient term needs `kappaPrime`, which is internal to
 * constrain.cpp), MONOTONICITY is asserted instead: tightening any limit must
 * never raise any ceiling. That is implementation-independent, needs none of
 * constrain's internals, and catches a botched min() chain — the single most
 * likely transcription error in this port, and the one bit-parity would catch
 * only if the TypeScript happened to exercise the same branch.
 *
 * Scaling laws are used wherever a cap's FORM matters. "A tighter budget lowers
 * the ceiling" passes for a dimensionally wrong formula; "4x the budget buys
 * exactly 2x the speed" does not. Three of the caps are square roots and one is
 * not, and confusing them is a plausible slip that changes nothing directionally.
 */

#include <doctest.h>

#include "motion/constrain.h"
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
#include <limits>
#include <set>
#include <string>
#include <vector>

using namespace motion;
using curves::casesWithCusp;

namespace {

constexpr double FEED = 80.0;
constexpr double A_MAX = 1000.0;

/** The BASE options every test starts from — the three always-on terms. */
ConstrainOptions base() {
    ConstrainOptions o;
    o.feedMax = FEED;
    o.aMax = A_MAX;
    o.junctionDeviation = quality::JUNCTION_DEVIATION;
    return o;
}

/** BASE with an explicit corner-stop threshold set (presence flag included). */
ConstrainOptions withCornerStop(ConstrainOptions o, double deg) {
    o.cornerStopAngleDeg = deg;
    o.hasCornerStopAngle = true;
    return o;
}

std::vector<Sample> samplesFor(const std::vector<CubicBezier>& c) {
    return flatten({c}, quality::flattenOpts());
}

double midCeiling(const std::vector<ConstrainedSample>& c) {
    return c[c.size() / 2].vCeiling;
}

std::string fmt(double v, int prec = 6) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*g", prec, v);
    return std::string(buf);
}

/**
 * Run `probe` over every fixture and fail ONCE with all violations.
 * See test_flatten_contract.cpp — an assertion inside a fixture loop reports the
 * first failure and buries the worst one, which is how the worst case hides.
 */
void forEachFixture(
    const std::function<std::vector<std::string>(const std::string&,
                                                 const std::vector<Sample>&)>& probe) {
    std::vector<std::string> problems;
    for (const curves::Case& c : casesWithCusp()) {
        for (std::string& s : probe(c.first, samplesFor(*c.second))) problems.push_back(s);
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

/** Tightening a limit must never raise ANY ceiling, on ANY fixture. */
void assertNeverRaises(const std::string& label,
                       const ConstrainOptions& loose,
                       const ConstrainOptions& tight) {
    forEachFixture([&](const std::string& name, const std::vector<Sample>& s) {
        const std::vector<ConstrainedSample> a = constrain(s, loose);
        const std::vector<ConstrainedSample> b = constrain(s, tight);
        std::vector<std::string> raised;
        for (size_t i = 0; i < b.size(); i++) {
            if (b[i].vCeiling > a[i].vCeiling + 1e-9) {
                raised.push_back(std::to_string(i) + ": " + fmt(a[i].vCeiling, 5) +
                                 " -> " + fmt(b[i].vCeiling, 5));
            }
        }
        if (raised.empty()) return std::vector<std::string>{};
        return std::vector<std::string>{
            label + " on " + name + ": " + std::to_string(raised.size()) +
            " ceiling(s) went UP, e.g. " + raised[0]};
    });
}

/** The right-angle elbow the corner-stop tests use. */
std::vector<Sample> rightAngle() {
    return flatten({{lineToCubic(Pt{0, 0}, Pt{10, 0}),
                     lineToCubic(Pt{10, 0}, Pt{10, 10})}},
                   quality::flattenOpts());
}

std::vector<Sample> straightRun() {
    return flatten({{lineToCubic(Pt{0, 0}, Pt{100, 0})}}, quality::flattenOpts());
}

} // namespace

// ═══ 1. INVARIANTS ═══════════════════════════════════════════════════════════

TEST_CASE("constrain: bounds") {
    SUBCASE("vCeiling never exceeds feedMax") {
        ConstrainOptions o = withCornerStop(base(), 20.0);
        o.aRateDegS = 100;
        forEachFixture([&](const std::string& name, const std::vector<Sample>& s) {
            size_t over = 0;
            for (const ConstrainedSample& x : constrain(s, o)) {
                if (x.vCeiling > FEED + 1e-6) over++;
            }
            if (over == 0) return std::vector<std::string>{};
            return std::vector<std::string>{
                name + ": " + std::to_string(over) + " sample(s) above feedMax"};
        });
    }

    SUBCASE("vCeiling is finite and non-negative") {
        // kappa can be enormous at a cusp and kappaPrime larger still; every cap
        // is a division by one of them. A NaN or a negative here would propagate
        // into plan's sqrt and out the far end as a garbage interval.
        ConstrainOptions o = withCornerStop(base(), 20.0);
        o.aRateDegS = 100;
        o.aAccelDegS2 = 50;
        forEachFixture([&](const std::string& name, const std::vector<Sample>& s) {
            std::vector<std::string> bad;
            const std::vector<ConstrainedSample> c = constrain(s, o);
            for (size_t i = 0; i < c.size() && bad.size() < 4; i++) {
                if (!std::isfinite(c[i].vCeiling) || c[i].vCeiling < 0) {
                    bad.push_back(name + ": " + std::to_string(i) + "=" +
                                  fmt(c[i].vCeiling));
                }
            }
            return bad;
        });
    }

    SUBCASE("does not mutate its input") {
        // constrain documents itself as pure. The port will reuse buffers for
        // memory reasons, which is exactly when accidental mutation appears —
        // and the differential could not see it, because it compares outputs.
        std::vector<Sample> s = samplesFor(curves::sCurve());
        const std::vector<Sample> before = s;
        ConstrainOptions o = withCornerStop(base(), 20.0);
        o.aRateDegS = 100;
        o.aAccelDegS2 = 50;
        constrain(s, o);
        REQUIRE(s.size() == before.size());
        for (size_t i = 0; i < s.size(); i++) {
            CHECK(testbits::sameBits(s[i].x, before[i].x));
            CHECK(testbits::sameBits(s[i].y, before[i].y));
            CHECK(testbits::sameBits(s[i].theta, before[i].theta));
            CHECK(testbits::sameBits(s[i].kappa, before[i].kappa));
            CHECK(testbits::sameBits(s[i].ds, before[i].ds));
            CHECK(s[i].flags == before[i].flags);
        }
    }

    SUBCASE("is deterministic") {
        ConstrainOptions o = base();
        o.aRateDegS = 100;
        forEachFixture([&](const std::string& name, const std::vector<Sample>& s) {
            const std::vector<ConstrainedSample> a = constrain(s, o);
            const std::vector<ConstrainedSample> b = constrain(s, o);
            for (size_t i = 0; i < a.size(); i++) {
                if (!testbits::sameBits(a[i].vCeiling, b[i].vCeiling)) {
                    return std::vector<std::string>{name + ": two runs differ at " +
                                                    std::to_string(i)};
                }
            }
            return std::vector<std::string>{};
        });
    }

    SUBCASE("preserves sample count and geometry") {
        // constrain adds a field; it must not drop, reorder or edit samples.
        forEachFixture([](const std::string& name, const std::vector<Sample>& s) {
            const std::vector<ConstrainedSample> c = constrain(s, base());
            if (c.size() != s.size()) {
                return std::vector<std::string>{
                    name + ": length " + std::to_string(c.size()) + " != " +
                    std::to_string(s.size())};
            }
            for (size_t i = 0; i < s.size(); i++) {
                if (!testbits::sameBits(s[i].x, c[i].s.x) ||
                    !testbits::sameBits(s[i].y, c[i].s.y) ||
                    !testbits::sameBits(s[i].kappa, c[i].s.kappa) ||
                    !testbits::sameBits(s[i].ds, c[i].s.ds) ||
                    s[i].flags != c[i].s.flags) {
                    return std::vector<std::string>{
                        name + ": sample " + std::to_string(i) + " altered"};
                }
            }
            return std::vector<std::string>{};
        });
    }
}

// ═══ 2. CONTRACT PROPERTIES ══════════════════════════════════════════════════

TEST_CASE("constrain: straight line") {
    SUBCASE("ceiling == feedMax everywhere") {
        for (const ConstrainedSample& x : constrain(samplesFor(curves::straightLine()), base())) {
            CHECK(std::fabs(x.vCeiling - FEED) < 1e-6);
        }
    }
}

TEST_CASE("constrain: centripetal cap") {
    SUBCASE("holds at EVERY sample on every fixture") {
        // Was checked at one hand-picked mid-sample on one fixture. The cap is a
        // per-sample contract, so assert it per sample.
        forEachFixture([](const std::string& name, const std::vector<Sample>& s) {
            const std::vector<ConstrainedSample> c = constrain(s, base());
            double worst = 0;
            size_t count = 0;
            for (size_t i = 0; i < s.size(); i++) {
                if (s[i].kappa <= 1e-9) continue;
                const double lim = std::sqrt(A_MAX / s[i].kappa);
                if (c[i].vCeiling > lim + 1e-9) {
                    count++;
                    worst = std::fmax(worst, c[i].vCeiling / lim);
                }
            }
            if (count == 0) return std::vector<std::string>{};
            return std::vector<std::string>{
                name + ": " + std::to_string(count) + " sample(s), worst " +
                fmt(worst, 4) + "x the cap"};
        });
    }

    SUBCASE("r5 circle: kappa=0.2 -> v ~ sqrt(1000/0.2) ~ 70.7") {
        const double expected = std::sqrt(A_MAX / 0.2);
        const double mid = midCeiling(constrain(samplesFor(curves::quarterCircleR5()), base()));
        CHECK(std::fabs(mid - expected) / expected < 0.05);
    }

    SUBCASE("scales as sqrt(aMax)") {
        // Pins the sqrt. A cap written as aMax/kappa — the A-slew form, easy to
        // paste into the wrong branch — would scale linearly and give 4x here.
        const auto mid = [](double aMax) {
            ConstrainOptions o = base();
            o.aMax = aMax;
            return midCeiling(constrain(samplesFor(curves::quarterCircleR5()), o));
        };
        CHECK(std::fabs(mid(1000) / mid(250) - 2.0) < 1e-6);
    }

    SUBCASE("tighter circle -> lower cap") {
        const auto mid = [](const std::vector<CubicBezier>& cs) {
            return midCeiling(constrain(samplesFor(cs), base()));
        };
        CHECK(mid(curves::quarterCircleR5()) < mid(curves::quarterCircleR50()));
    }
}

TEST_CASE("constrain: A-slew cap") {
    SUBCASE("holds at EVERY sample on every fixture") {
        const double aRateDegS = 100.0;
        const double aRateRad = (aRateDegS * PI) / 180;
        ConstrainOptions o = base();
        o.aRateDegS = aRateDegS;
        forEachFixture([&](const std::string& name, const std::vector<Sample>& s) {
            const std::vector<ConstrainedSample> c = constrain(s, o);
            double worst = 0;
            size_t count = 0;
            for (size_t i = 0; i < s.size(); i++) {
                if (s[i].kappa <= 1e-9) continue;
                const double lim = aRateRad / s[i].kappa;
                if (c[i].vCeiling > lim + 1e-9) {
                    count++;
                    worst = std::fmax(worst, c[i].vCeiling / lim);
                }
            }
            if (count == 0) return std::vector<std::string>{};
            return std::vector<std::string>{
                name + ": " + std::to_string(count) + " sample(s), worst " +
                fmt(worst, 4) + "x the cap"};
        });
    }

    SUBCASE("slow A axis lowers the ceiling on a tight curve") {
        // And pins the FORM: this cap is linear in 1/kappa, not a square root,
        // so the expected value is rad(100)/0.2 directly.
        ConstrainOptions o = base();
        o.aRateDegS = 100.0;
        const double withA = midCeiling(constrain(samplesFor(curves::quarterCircleR5()), o));
        CHECK(withA < midCeiling(constrain(samplesFor(curves::quarterCircleR5()), base())));
        CHECK(std::fabs(withA - (PI * 100 / 180) / 0.2) / withA < 0.05);
    }
}

TEST_CASE("constrain: A-accel gradient cap") {
    const auto minOf = [](const ConstrainOptions& o) {
        double m = std::numeric_limits<double>::infinity();
        for (const ConstrainedSample& x : constrain(samplesFor(curves::sCurve()), o)) {
            m = std::fmin(m, x.vCeiling);
        }
        return m;
    };

    SUBCASE("changing curvature — tight a_accel lowers the min ceiling") {
        ConstrainOptions tight = base();
        tight.aAccelDegS2 = 50.0;
        CHECK(minOf(tight) < minOf(base()));
    }

    SUBCASE("constant curvature — cap inactive (dk/ds = 0)") {
        // A circular arc has no curvature gradient, so this cap must not bind at
        // all. If it did, every arc in every job would be slowed for nothing.
        const auto at = [](const ConstrainOptions& o) {
            return midCeiling(constrain(samplesFor(curves::quarterCircleR50()), o));
        };
        ConstrainOptions tight = base();
        tight.aAccelDegS2 = 50.0;
        CHECK(std::fabs(at(base()) - at(tight)) < 1e-9);
    }

    SUBCASE("binds at exactly sqrt(alpha/|k'|) where it is the active cap") {
        // ── added beyond the TypeScript, and here is why ──────────────────────
        //
        // The TypeScript asserts only monotonicity and a scaling law for this
        // cap, because `kappaPrime` is internal and it declined to re-derive it.
        // Mutation showed that leaves a hole: two mutants of kappaPrime — using
        // half the arc-length span, and dropping the guard that stops a finite
        // difference straddling a curve boundary — survive the ENTIRE rest of
        // this file. Both only ever LOWER a ceiling, so monotonicity is blind to
        // them by construction, and the scaling law is a RATIO, so any constant
        // factor on |k'| cancels out of it exactly.
        //
        // Their effect on a machine is real: the first slows every
        // curvature-varying move by sqrt(2) for nothing, and the second invents
        // an angular acceleration at every curve join out of a kappa
        // discontinuity that is an artefact of two curves meeting rather than
        // anything the A axis has to deliver.
        //
        // So this test states the formula independently and asserts the cap
        // where it BINDS — an equality, which is two-sided and therefore sees a
        // ceiling that is too low as readily as one that is too high.
        //
        // Honest limitation: |k'| is re-derived here by the same central
        // difference constrain uses, so this cannot catch the two of them
        // agreeing on a WRONG definition of dk/ds. It catches a wrong span, a
        // wrong index, or a missing discontinuity guard, which is what the
        // surviving mutants actually were.
        const double aAccelDegS2 = 50.0;
        const double aAccRad = (aAccelDegS2 * PI) / 180;
        ConstrainOptions o = base();
        o.aAccelDegS2 = aAccelDegS2;

        const std::vector<Sample> s = samplesFor(curves::sCurve());
        const std::vector<ConstrainedSample> c = constrain(s, o);
        const uint32_t KAPPA_BREAK = PATH_START | PATH_END | CURVE_BOUNDARY;

        size_t binding = 0;
        std::vector<std::string> bad;
        for (size_t i = 1; i + 1 < s.size(); i++) {
            // The span runs i-1 .. i+1; a flag marks the FIRST sample after a
            // discontinuity, so a flag at i or at i+1 means the span straddles
            // one and the difference is meaningless.
            if ((s[i].flags & KAPPA_BREAK) || (s[i + 1].flags & KAPPA_BREAK)) continue;
            const double span = s[i - 1].ds + s[i].ds;
            if (span < 1e-6) continue;
            const double kp = std::fabs(s[i + 1].kappa - s[i - 1].kappa) / span;
            if (kp <= 1e-9) continue;

            const double aAccelLim = std::sqrt(aAccRad / kp);
            // Only assert where this cap is strictly the smallest, so the
            // equality is not competing with feedMax or the centripetal term.
            double others = FEED;
            if (s[i].kappa > 1e-9) others = std::fmin(others, std::sqrt(A_MAX / s[i].kappa));
            if (aAccelLim >= others * 0.999) continue;

            binding++;
            if (std::fabs(c[i].vCeiling - aAccelLim) > 1e-9 && bad.size() < 4) {
                bad.push_back(std::to_string(i) + ": got " + fmt(c[i].vCeiling, 8) +
                              " expected " + fmt(aAccelLim, 8));
            }
        }
        // Guards against the whole test going vacuous if the s_curve ever stops
        // driving this cap — a silent pass would be worse than no test.
        CHECK_MESSAGE(binding > 10u, "cap bound at only " << binding << " sample(s)");
        std::string joined;
        for (size_t i = 0; i < bad.size(); i++) joined += (i ? "; " : "") + bad[i];
        CHECK(joined == "");
    }

    SUBCASE("a kappa jump ACROSS a curve join is not an angular acceleration") {
        // The companion to the test above, and the other half of the same hole:
        // that one skips boundary-straddling samples, so it cannot see this cap
        // being applied where it should not be.
        //
        // kappa is genuinely discontinuous where two curves meet — the sampled
        // jump is an artefact of the representation, not a rotation the A axis
        // has to perform in the arc length between the two samples. A finite
        // difference taken across that join divides a large kappa step by a
        // near-zero span and yields an enormous |k'|, which would clamp the
        // ceiling to near zero at every curve join in every job. The guard in
        // kappaPrime is what prevents it, and no other test in this file
        // notices when it is removed.
        //
        // Stated behaviourally rather than by inspecting kappaPrime: turning the
        // A-accel cap on must change NOTHING at a sample whose difference span
        // straddles a break.
        ConstrainOptions off = base();
        ConstrainOptions on = base();
        on.aAccelDegS2 = 50.0;
        const uint32_t KAPPA_BREAK = PATH_START | PATH_END | CURVE_BOUNDARY;

        size_t checked = 0;
        forEachFixture([&](const std::string& name, const std::vector<Sample>& s) {
            const std::vector<ConstrainedSample> a = constrain(s, off);
            const std::vector<ConstrainedSample> b = constrain(s, on);
            std::vector<std::string> bad;
            for (size_t i = 1; i + 1 < s.size(); i++) {
                if (!((s[i].flags & KAPPA_BREAK) || (s[i + 1].flags & KAPPA_BREAK))) continue;
                checked++;
                if (!testbits::sameBits(a[i].vCeiling, b[i].vCeiling) && bad.size() < 4) {
                    bad.push_back(name + "[" + std::to_string(i) + "]: " +
                                  fmt(a[i].vCeiling, 8) + " -> " + fmt(b[i].vCeiling, 8) +
                                  " across a kappa break");
                }
            }
            return bad;
        });
        CHECK_MESSAGE(checked > 5u, "only " << checked << " boundary sample(s) examined");
    }

    SUBCASE("scales as sqrt(aAccel) where the cap binds") {
        // The only assertion that pins the FORM of this cap rather than its
        // direction. v <= sqrt(alpha/|k'|), so 4x the angular-accel budget must
        // buy exactly 2x the speed. A cap implemented as alpha/|k'| — a plausible
        // transcription slip, and dimensionally wrong — would give 4x and fail.
        const auto m = [&](double aAccelDegS2) {
            ConstrainOptions o = base();
            o.aAccelDegS2 = aAccelDegS2;
            return minOf(o);
        };
        CHECK(std::fabs(m(50.0) / m(12.5) - 2.0) < 1e-3);
    }
}

// ── monotonicity: the min() chain, checked without re-deriving it ────────────

TEST_CASE("constrain: monotonicity") {
    SUBCASE("lowering feedMax never raises a ceiling") {
        ConstrainOptions t = base();
        t.feedMax = 40;
        assertNeverRaises("feedMax 80->40", base(), t);
    }

    SUBCASE("lowering aMax never raises a ceiling") {
        ConstrainOptions t = base();
        t.aMax = 250;
        assertNeverRaises("aMax 1000->250", base(), t);
    }

    SUBCASE("enabling the A-slew cap never raises a ceiling") {
        ConstrainOptions t = base();
        t.aRateDegS = 100;
        assertNeverRaises("aRateDegS off->100", base(), t);
    }

    SUBCASE("enabling the A-accel cap never raises a ceiling") {
        ConstrainOptions t = base();
        t.aAccelDegS2 = 50;
        assertNeverRaises("aAccelDegS2 off->50", base(), t);
    }

    SUBCASE("lowering the corner-stop threshold never raises a ceiling") {
        assertNeverRaises("cornerStop 90->20",
                          withCornerStop(base(), 90.0),
                          withCornerStop(base(), 20.0));
    }
}

// ── corner stop ──────────────────────────────────────────────────────────────

TEST_CASE("constrain: corner stop") {
    SUBCASE("sharp corner forces vCeiling = 0") {
        const std::vector<Sample> s = rightAngle();
        const std::vector<ConstrainedSample> c = constrain(s, withCornerStop(base(), 20.0));
        size_t bi = c.size();
        for (size_t i = 0; i < c.size(); i++) {
            if (c[i].s.flags & CURVE_BOUNDARY) { bi = i; break; }
        }
        REQUIRE(bi < c.size());
        CHECK(c[bi].vCeiling == 0.0);
    }

    SUBCASE("no corner stop when disabled — junction cap still applies") {
        const std::vector<ConstrainedSample> c = constrain(rightAngle(), base());
        size_t bi = c.size();
        for (size_t i = 0; i < c.size(); i++) {
            if (c[i].s.flags & CURVE_BOUNDARY) { bi = i; break; }
        }
        REQUIRE(bi < c.size());
        CHECK(c[bi].vCeiling > 0.0);
        CHECK(c[bi].vCeiling < FEED);
    }

    SUBCASE("a corner stop lands on the boundary sample only") {
        // discretize choreographs the lift-pivot-lower around this exact index.
        // If the zero ever spreads to a neighbour, the pivot is placed wrong.
        const std::vector<ConstrainedSample> c = constrain(rightAngle(), withCornerStop(base(), 20.0));
        std::vector<size_t> zeros, boundaries;
        for (size_t i = 0; i < c.size(); i++) {
            if (c[i].vCeiling == 0.0) zeros.push_back(i);
            if (c[i].s.flags & CURVE_BOUNDARY) boundaries.push_back(i);
        }
        CHECK(zeros == boundaries);
    }
}

TEST_CASE("constrain: junctionCap helper") {
    const double DEV = 0.05;

    SUBCASE("monotone — sharper turn lowers the cap; straight = feedMax") {
        const double straight = junctionCap(1.0, A_MAX, DEV, FEED);
        const double gentle = junctionCap(30.0, A_MAX, DEV, FEED);
        const double sharp = junctionCap(120.0, A_MAX, DEV, FEED);
        CHECK(straight >= gentle);
        CHECK(gentle >= sharp);
        CHECK(junctionCap(0.0, A_MAX, DEV, FEED) == FEED);
    }

    SUBCASE("a full reversal caps at zero") {
        // cos(180/2) = 0 -> the arc radius collapses. The tool cannot carry any
        // speed through a doubling-back join.
        CHECK(junctionCap(180.0, A_MAX, DEV, FEED) == 0.0);
    }

    SUBCASE("is symmetric in turn direction") {
        for (const double deg : {15.0, 45.0, 90.0, 150.0}) {
            CAPTURE(deg);
            CHECK(junctionCap(-deg, A_MAX, DEV, FEED) == junctionCap(deg, A_MAX, DEV, FEED));
        }
    }

    SUBCASE("a larger deviation budget allows more speed") {
        CHECK(junctionCap(45, A_MAX, 0.2, FEED) > junctionCap(45, A_MAX, 0.02, FEED));
    }

    SUBCASE("matches the closed form at 90 degrees") {
        // GRBL junction deviation: model the corner as an arc of radius
        //   r = d*cos(t/2) / (1 - cos(t/2))
        // and hold centripetal accel on it, v = sqrt(a*r). Computed here from the
        // definition rather than copied from the implementation, so a dropped
        // factor shows up as a number rather than as a direction.
        //
        // std::cos is deliberate HERE, unlike in constrain.cpp which must use
        // jsCos: this is an independent re-derivation, and pi/4 is small enough
        // that the two agree to well inside the tolerance. Using the library's
        // own jsCos would make the check partly self-referential.
        const double halfCos = std::cos(PI / 4);
        const double r = (DEV * halfCos) / (1 - halfCos);
        CHECK(std::fabs(junctionCap(90, A_MAX, DEV, FEED) - std::sqrt(A_MAX * r)) < 1e-9);
    }

    SUBCASE("scales as sqrt(deviation) below the feed clamp") {
        // v ~ sqrt(a*r) and r ~ deviation, so 4x the budget is 2x the speed. A
        // sharp turn is used so neither result is clamped at feedMax.
        const double lo = junctionCap(150, A_MAX, 0.01, FEED);
        const double hi = junctionCap(150, A_MAX, 0.04, FEED);
        CHECK(hi < FEED);
        CHECK(std::fabs(hi / lo - 2.0) < 1e-6);
    }
}

// ═══ 3. THE CUSP — what constrain actually does ══════════════════════════════

TEST_CASE("constrain: cusp handling") {
    SUBCASE("F2 (FIXED): stops at an intra-curve tangent reversal") {
        // Was: the corner-stop branch was gated on CURVE_BOUNDARY, which flatten
        // only sets at curve JOINS. A 178deg reversal INSIDE a single curve was
        // never considered for a corner stop — while discretize's ungated dtheta
        // check treated it as one and inserted a lift-pivot-lower there. The two
        // stages disagreed about what a corner is.
        const std::vector<Sample> s = samplesFor(curves::cusp());
        const std::vector<ConstrainedSample> c = constrain(s, withCornerStop(base(), 20.0));
        size_t boundaries = 0, zeros = 0;
        for (size_t i = 0; i < c.size(); i++) {
            if (s[i].flags & CURVE_BOUNDARY) boundaries++;
            if (c[i].vCeiling == 0.0) zeros++;
        }
        CHECK(boundaries == 0u);
        CHECK(zeros > 0u);
    }

    SUBCASE("F2 (FIXED): the stop lands on the sample the pivot happens at") {
        // Off-by-one guard, and it is not hypothetical: the finding test for this
        // in discretize.test.ts checked the sample BEFORE the jump while printing
        // the flag of the sample AFTER it, so it under-reported the defect. The
        // corner is at the LATER sample of the pair.
        const std::vector<Sample> s = samplesFor(curves::cusp());
        const std::vector<ConstrainedSample> c = constrain(s, withCornerStop(base(), 20.0));
        size_t found = 0;
        for (size_t i = 1; i < s.size(); i++) {
            if (std::fabs(angleDelta(s[i - 1].theta, s[i].theta)) >= 20.0) {
                found++;
                CAPTURE(i);
                CHECK(c[i].vCeiling == 0.0);
            }
        }
        CHECK(found > 0u);
    }

    SUBCASE("C1 (FIXED): a ceiling below vMin becomes a stop, not a crawl") {
        // Was: the A caps drove the ceiling to ~3e-3 mm/s — 166x BELOW
        // quality.vMin (0.5 mm/s), the floor discretize clamps the interval to.
        // The planned and executed profiles diverged by two orders of magnitude
        // there, and every timeline derived from the plan went with them.
        ConstrainOptions o = base();
        o.aRateDegS = 100;
        o.aAccelDegS2 = 50;
        o.vMin = quality::V_MIN;
        const std::vector<ConstrainedSample> c = constrain(samplesFor(curves::cusp()), o);
        double minNonZero = std::numeric_limits<double>::infinity();
        bool anyZero = false;
        for (const ConstrainedSample& x : c) {
            if (x.vCeiling > 0) minNonZero = std::fmin(minNonZero, x.vCeiling);
            if (x.vCeiling == 0.0) anyZero = true;
        }
        CHECK(minNonZero >= quality::V_MIN);
        CHECK(anyZero);
    }

    SUBCASE("C1: the floor is opt-in — absent vMin leaves the old crawl") {
        // The stage takes no config and invents no defaults: a caller that does
        // not state a floor does not get one. This is what keeps constrain usable
        // outside the production bridge, and it is why the value is passed rather
        // than imported.
        ConstrainOptions o = base();
        o.aRateDegS = 100;
        o.aAccelDegS2 = 50;
        double minNonZero = std::numeric_limits<double>::infinity();
        for (const ConstrainedSample& x : constrain(samplesFor(curves::cusp()), o)) {
            if (x.vCeiling > 0) minNonZero = std::fmin(minNonZero, x.vCeiling);
        }
        CHECK(minNonZero < quality::V_MIN / 100);
    }
}

// ═══ 4. REAL SVG ═════════════════════════════════════════════════════════════

TEST_CASE("constrain: real SVG") {
    const svgfix::Subpaths& repaired = svgfix::load("snake");
    REQUIRE_MESSAGE(!repaired.empty(),
                    "test/data/svg_fixtures.txt missing — regenerate with "
                    "`cd web && GEN_CPP_REF=1 npx vitest run test/port/cppRefFixtures`");
    const std::vector<Sample> s = flatten(repaired, quality::flattenOpts());

    SUBCASE("snake.svg — every cap holds and the output is usable") {
        ConstrainOptions o = withCornerStop(base(), 20.0);
        o.aRateDegS = 100;
        o.aAccelDegS2 = 50;
        const std::vector<ConstrainedSample> c = constrain(s, o);
        const double aRateRad = (100 * PI) / 180;
        std::vector<std::string> bad;
        for (size_t i = 0; i < s.size() && bad.size() < 6; i++) {
            const double v = c[i].vCeiling;
            if (!std::isfinite(v) || v < 0 || v > FEED + 1e-6) {
                bad.push_back(std::to_string(i) + ": v=" + fmt(v));
            }
            if (s[i].kappa > 1e-9) {
                if (v > std::sqrt(A_MAX / s[i].kappa) + 1e-9) {
                    bad.push_back(std::to_string(i) + ": centripetal");
                }
                if (v > aRateRad / s[i].kappa + 1e-9) {
                    bad.push_back(std::to_string(i) + ": A-slew");
                }
            }
        }
        std::string joined;
        for (size_t i = 0; i < bad.size(); i++) joined += (i ? "; " : "") + bad[i];
        CHECK(joined == "");
    }

    SUBCASE("snake.svg — only boundary samples are forced to zero") {
        const std::vector<ConstrainedSample> c = constrain(s, withCornerStop(base(), 20.0));
        for (size_t i = 0; i < c.size(); i++) {
            if (c[i].vCeiling != 0.0) continue;
            CAPTURE(i);
            CHECK((s[i].flags & (CURVE_BOUNDARY | PATH_START)) != 0u);
        }
    }
}

// ═══ 5. FORCED STOPS ═════════════════════════════════════════════════════════
// A stop the CALLER injects, for a reason the geometry knows nothing about —
// today, releasing a duty-limited tool's enable line before its budget expires
// (docs/tool_duty_limits.md §5 tier 2).

TEST_CASE("constrain: forcedStops") {
    const auto opts = [](std::set<size_t> stops) {
        ConstrainOptions o = base();
        o.forcedStops = std::move(stops);
        return o;
    };

    SUBCASE("zeroes the ceiling at the named sample and nowhere else") {
        const std::vector<Sample> s = straightRun();
        const size_t idx = s.size() / 2;
        const std::vector<ConstrainedSample> c = constrain(s, opts({idx}));
        CHECK(c[idx].vCeiling == 0.0);
        for (size_t i = 0; i < c.size(); i++) {
            if (i == idx) continue;
            CAPTURE(i);
            CHECK(c[i].vCeiling > 0.0);
        }
    }

    SUBCASE("is a no-op when absent or empty — the byte-for-byte guarantee") {
        // Every existing caller passes nothing. If this ever diverges, the golden
        // snapshot moves for every tool, duty-limited or not. Bit-level, because
        // "byte-for-byte" is the actual claim.
        const std::vector<Sample> s = straightRun();
        const std::vector<ConstrainedSample> ref = constrain(s, opts({}));
        const std::vector<ConstrainedSample> alt = constrain(s, base());
        REQUIRE(alt.size() == ref.size());
        for (size_t i = 0; i < ref.size(); i++) {
            CHECK(testbits::sameBits(alt[i].vCeiling, ref[i].vCeiling));
        }
    }

    SUBCASE("beats every geometric cap, including a straight line at full feed") {
        // On a straight run nothing else constrains the sample, so a min()
        // against feedMax would leave it at 80 mm/s. This is the case that proves
        // the override is an override rather than one more term in the chain.
        const std::vector<Sample> s = straightRun();
        const size_t idx = s.size() / 2;
        CHECK(std::fabs(constrain(s, base())[idx].vCeiling - FEED) < 1e-6);
        CHECK(constrain(s, opts({idx}))[idx].vCeiling == 0.0);
    }

    SUBCASE("accepts several stops at once") {
        const std::vector<Sample> s = straightRun();
        const std::set<size_t> stops = {2, 5, 9};
        const std::vector<ConstrainedSample> c = constrain(s, opts(stops));
        for (const size_t i : stops) {
            CAPTURE(i);
            CHECK(c[i].vCeiling == 0.0);
        }
    }

    SUBCASE("ignores out-of-range indices rather than throwing") {
        // The caller measured a timeline from a PREVIOUS bake; a stale index is a
        // scheduling bug, not a crash. Failing soft keeps the pipeline's error
        // surface at the stage that can explain it.
        //
        // The TypeScript passes -1; here the set is size_t, so -1 arrives as
        // SIZE_MAX. That is the same test of the same guard — "an index past the
        // end is ignored" — and SIZE_MAX is if anything the harsher input, since
        // a signed-comparison slip would index catastrophically rather than
        // merely wrongly.
        const std::vector<Sample> s = straightRun();
        const std::vector<ConstrainedSample> ref = constrain(s, base());
        const std::vector<ConstrainedSample> c = constrain(
            s, opts({static_cast<size_t>(-1), s.size(), 9999}));
        REQUIRE(c.size() == ref.size());
        for (size_t i = 0; i < ref.size(); i++) {
            CHECK(testbits::sameBits(c[i].vCeiling, ref[i].vCeiling));
        }
    }

    SUBCASE("wins over a corner stop at the same index") {
        // Both produce 0, so this pins INTENT rather than arithmetic: the early
        // return must not be reordered below the corner branch during the port.
        const std::vector<Sample> s = rightAngle();
        size_t bi = s.size();
        for (size_t i = 0; i < s.size(); i++) {
            if (s[i].flags & CURVE_BOUNDARY) { bi = i; break; }
        }
        REQUIRE(bi < s.size());
        ConstrainOptions o = withCornerStop(base(), 20.0);
        o.forcedStops = {bi};
        CHECK(constrain(s, o)[bi].vCeiling == 0.0);
    }
}
