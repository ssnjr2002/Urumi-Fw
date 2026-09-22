/**
 * Contract tests for stage 7, discretize — ported from
 * web/test/toolpath/discretize.test.ts.
 *
 * The bottom of the pipeline: what comes out of here goes on the wire, so a
 * defect here is a defect on metal. Two groups, as with the earlier stages:
 *
 *   INVARIANTS          hold for every input; a failure is a bug.
 *   CONTRACT PROPERTIES what the stage's doc comment claims.
 *
 * Method note carried across from the TypeScript, and the reason these tests
 * are worth porting rather than trusting the differential: `emittedSeconds`
 * re-derives wall time from `interval` and step counts the way the FIRMWARE
 * will, not the way discretize computed it. That independence is how D2 and D3
 * were found, and a bit-comparison against the TypeScript could not have found
 * either — both languages would have agreed on the same wrong timeline.
 *
 * Fixture loops aggregate through forEachFixture and fail once, so the worst
 * case cannot hide behind the first.
 */

#include "doctest.h"

#include "motion/choreograph.h"
#include "motion/discretize.h"
#include "motion/geometry.h"
#include "motion/jsmath.h"
#include "motion/plan.h"
#include "support/bits.h"
#include "support/curves.h"
#include "support/machine.h"
#include "support/quality.h"
#include "support/svgfix.h"

#include <cmath>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

using motion::CubicBezier;
using motion::DiscretizeOptions;
using motion::MicroSegment;
using motion::PlannedSample;
using motion::Pt;
using motion::angleDelta;
using motion::discretize;
using motion::MICRO_JOG;
using motion::MICRO_LIFT;
using motion::MICRO_PATH_END;

namespace {

using Subpaths = std::vector<std::vector<CubicBezier>>;

/**
 * Maximal runs of consecutive Z-moving segments, with each run's signed total.
 *
 * A lift is a RAMP now (H3), not one segment, so "the lower before the stroke"
 * is a contiguous group rather than a single index. Grouping keeps these tests
 * stating the property — down, then up, matched — instead of counting emitter
 * internals that the ramp granularity is free to change.
 */
struct ZRun { size_t from = 0, to = 0; double dz = 0; };

std::vector<ZRun> zRuns(const std::vector<MicroSegment>& segs) {
    std::vector<ZRun> runs;
    for (size_t i = 0; i < segs.size(); i++) {
        if (segs[i].dz == 0) continue;
        ZRun r;
        r.from = i;
        while (i < segs.size() && segs[i].dz != 0) r.dz += segs[i++].dz;
        r.to = i - 1;
        runs.push_back(r);
    }
    return runs;
}


CubicBezier line(double x0, double y0, double x1, double y1) {
    return motion::lineToCubic(Pt{x0, y0}, Pt{x1, y1});
}

std::string fmt(double v, int prec = 4) {
    std::ostringstream os;
    os.precision(prec);
    os << std::fixed << v;
    return os.str();
}

// ── measurement helpers (deliberately firmware-shaped, not discretize-shaped) ──

/** Steps the firmware will clock on this segment: the major axis. */
double major(const MicroSegment& s) {
    return std::fmax(std::fmax(std::fabs(s.dx), std::fabs(s.dy)),
                     std::fmax(std::fabs(s.dz), std::fabs(s.da)));
}

/**
 * Wall time the FIRMWARE will spend on these segments: interval cycles per
 * major-axis step, times steps, over the clock. Derived the way the executor
 * derives it — not from the planned v that produced it.
 */
double emittedSeconds(const std::vector<MicroSegment>& segs) {
    double t = 0;
    for (const MicroSegment& s : segs) t += (s.interval * major(s)) / machine::axes().fCpu;
    return t;
}

/** Time the PLAN says the cut takes, with the same vMin floor interval applies. */
double plannedSeconds(const std::vector<PlannedSample>& p) {
    double t = 0;
    for (const motion::Range& r : motion::subpathRanges(p)) {
        for (size_t i = r.first; i < r.second; i++) {
            t += p[i].s.ds / std::fmax(0.5 * (p[i].v + p[i + 1].v), quality::V_MIN);
        }
    }
    return t;
}

/** Non-cutting motion: travel jogs, Z lifts, pivots, pre-orientation. */
bool isChoreography(const MicroSegment& s) {
    return (s.flags & MICRO_JOG) != 0 || (s.flags & MICRO_LIFT) != 0 || s.dz != 0;
}

std::vector<MicroSegment> cutting(const std::vector<MicroSegment>& segs) {
    std::vector<MicroSegment> out;
    for (const MicroSegment& s : segs) {
        if (!isChoreography(s)) out.push_back(s);
    }
    return out;
}

struct Net {
    double dx = 0;
    double dy = 0;
    double da = 0;
};

Net net(const std::vector<MicroSegment>& segs) {
    Net n;
    for (const MicroSegment& s : segs) {
        n.dx += s.dx;
        n.dy += s.dy;
        n.da += s.da;
    }
    return n;
}

/**
 * Where the geometry says the tool must end up, in emitted steps.
 *
 * Re-derived from flatten's first and last sample rather than from anything
 * discretize produced, which is what makes the XY-conservation test a check on
 * the accumulator telescoping rather than a restatement of it.
 */
std::pair<double, double> expectedXY(const Subpaths& subpaths) {
    const std::vector<motion::Sample> s = motion::flatten(subpaths, quality::flattenOpts());
    const motion::ResolvedAxes& ax = machine::axes();
    double dx = motion::jsRound(s.back().x * ax.x.stepsPerUnit) -
                motion::jsRound(s.front().x * ax.x.stepsPerUnit);
    double dy = motion::jsRound(s.back().y * ax.y.stepsPerUnit) -
                motion::jsRound(s.front().y * ax.y.stepsPerUnit);
    if (ax.x.invert) dx = -dx;
    if (ax.y.invert) dy = -dy;
    return {dx, dy};
}

/**
 * Re-derive discretize's corner rule from the planned stream, returning the
 * index of the sample the PIVOT HAPPENS AT.
 *
 * discretize walks pairs (i, i+1) and pivots after arriving at i+1, so the
 * sample that must be at rest is the LATER one. Returning `i` here while the
 * caller prints the flags of `i + 1` is what under-reported D4 in the
 * TypeScript: at the cusp the approach sample read 4.84e-3 mm/s and the sample
 * that actually pivots read 2.14e-2, 4.4x worse than the number the finding was
 * first filed with.
 */
std::vector<size_t> cornerIndices(const std::vector<PlannedSample>& p,
                                  const DiscretizeOptions& tool) {
    std::vector<size_t> out;
    if (!tool.tangential) return out;
    for (const motion::Range& r : motion::subpathRanges(p)) {
        double theta = p[r.first].s.theta;
        for (size_t i = r.first; i < r.second; i++) {
            if (std::fabs(angleDelta(theta, p[i + 1].s.theta)) >= tool.cornerAngleDeg) {
                out.push_back(i + 1);
            }
            theta = p[i + 1].s.theta;
        }
    }
    return out;
}

/** Aggregate over every fixture and fail ONCE, so the worst case is visible. */
void forEachFixture(
    const std::function<std::vector<std::string>(const std::string&,
                                                 const std::vector<CubicBezier>&)>& probe) {
    std::vector<std::string> violations;
    for (const curves::Case& c : curves::casesWithCusp()) {
        const std::vector<std::string> v = probe(c.first, *c.second);
        violations.insert(violations.end(), v.begin(), v.end());
    }
    if (!violations.empty()) {
        std::ostringstream os;
        os << violations.size() << " violation(s):";
        for (const std::string& v : violations) os << "\n  " << v;
        FAIL(os.str());
    }
}

/** The two tools every "both tools" test sweeps. */
const std::vector<std::pair<std::string, DiscretizeOptions>>& tools() {
    static const std::vector<std::pair<std::string, DiscretizeOptions>> v = {
        {"knife", machine::knife()}, {"pen", machine::pen()}};
    return v;
}

} // namespace

// ═════════════════════════════════════════════════════════════════════════════
// INVARIANTS
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("discretize INVARIANT: purity and determinism") {
    SUBCASE("does not mutate its input") {
        const std::vector<PlannedSample> p = machine::planFor({curves::sCurve()}, machine::knife());
        std::vector<PlannedSample> copy = p;
        discretize(copy, machine::knife());
        REQUIRE(copy.size() == p.size());
        for (size_t i = 0; i < p.size(); i++) {
            CAPTURE(i);
            CHECK(testbits::sameBits(copy[i].s.x, p[i].s.x));
            CHECK(testbits::sameBits(copy[i].s.y, p[i].s.y));
            CHECK(testbits::sameBits(copy[i].s.theta, p[i].s.theta));
            CHECK(testbits::sameBits(copy[i].s.kappa, p[i].s.kappa));
            CHECK(testbits::sameBits(copy[i].s.ds, p[i].s.ds));
            CHECK(copy[i].s.flags == p[i].s.flags);
            CHECK(testbits::sameBits(copy[i].vCeiling, p[i].vCeiling));
            CHECK(testbits::sameBits(copy[i].v, p[i].v));
        }
    }

    SUBCASE("is deterministic") {
        // Bit-level, not approximate: two runs of a pure function have no
        // licence to differ at all.
        const std::vector<PlannedSample> p =
            machine::planFor({curves::fullCircleR30()}, machine::knife());
        const std::vector<MicroSegment> a = discretize(p, machine::knife());
        const std::vector<MicroSegment> b = discretize(p, machine::knife());
        REQUIRE(a.size() == b.size());
        for (size_t i = 0; i < a.size(); i++) {
            CAPTURE(i);
            CHECK(testbits::sameBits(a[i].dx, b[i].dx));
            CHECK(testbits::sameBits(a[i].dy, b[i].dy));
            CHECK(testbits::sameBits(a[i].dz, b[i].dz));
            CHECK(testbits::sameBits(a[i].da, b[i].da));
            CHECK(testbits::sameBits(a[i].interval, b[i].interval));
            CHECK(a[i].flags == b[i].flags);
        }
    }
}

TEST_CASE("discretize INVARIANT: XY conservation") {
    // The decisive property: whatever the segment density, the float
    // accumulators telescope to round(last) - round(first), with invert applied.
    SUBCASE("net XY lands on the geometric endpoint, every fixture, both tools") {
        forEachFixture([](const std::string& name, const std::vector<CubicBezier>& cs) {
            std::vector<std::string> bad;
            for (const auto& t : tools()) {
                const Net n = net(machine::prep({cs}, t.second));
                const std::pair<double, double> e = expectedXY({cs});
                if (n.dx != e.first || n.dy != e.second) {
                    bad.push_back(name + "/" + t.first + ": got (" + fmt(n.dx, 0) + "," +
                                  fmt(n.dy, 0) + ") want (" + fmt(e.first, 0) + "," +
                                  fmt(e.second, 0) + ")");
                }
            }
            return bad;
        });
    }

    SUBCASE("holds across multiple subpaths, including the travel jogs between them") {
        const Subpaths sp = {curves::straightLine(), curves::quarterCircleR5(),
                             {line(200, 40, 260, 90)}};
        const Net n = net(machine::prep(sp, machine::knife()));
        const std::pair<double, double> e = expectedXY(sp);
        CHECK(n.dx == e.first);
        CHECK(n.dy == e.second);
    }

    SUBCASE("holds on real artwork") {
        // The TypeScript runs the SVG through its parser and enforceC1 here;
        // the port has neither, so it consumes the repaired subpaths those two
        // produced (support/svgfix.h) and does its own flattening from there.
        const svgfix::Subpaths& sp = svgfix::load("snake");
        REQUIRE_MESSAGE(!sp.empty(),
                        "test/data/svg_fixtures.txt missing — regenerate with "
                        "GEN_CPP_REF=1 npx vitest run test/port/cppRefFixtures");
        const Net n = net(machine::prep(sp, machine::knife()));
        const std::pair<double, double> e = expectedXY(sp);
        CHECK(n.dx == e.first);
        CHECK(n.dy == e.second);
    }
}

TEST_CASE("discretize INVARIANT: A conservation for a tangential tool") {
    const double aSpd = machine::axes().a.stepsPerUnit;
    const double aInv = machine::axes().a.invert ? -1 : 1;

    SUBCASE("net A equals the entry orientation plus the total tracked turn") {
        // Tracking, pre-orientation and corner pivots must telescope to exactly
        // the geometry's total turn. A drift here is a knife pointing the wrong
        // way, which no downstream stage can detect.
        forEachFixture([&](const std::string& name, const std::vector<CubicBezier>& cs) {
            const std::vector<PlannedSample> p = machine::planFor({cs}, machine::knife());
            double netA = 0;
            for (const MicroSegment& s : machine::prep({cs}, machine::knife())) {
                netA += s.da * aInv;
            }
            netA /= aSpd;
            double turn = 0;
            for (const motion::Range& r : motion::subpathRanges(p)) {
                double th = p[r.first].s.theta;
                for (size_t i = r.first; i < r.second; i++) {
                    turn += angleDelta(th, p[i + 1].s.theta);
                    th = p[i + 1].s.theta;
                }
            }
            const double want = p.front().s.theta + turn;
            const double slack = 2 / aSpd; // da is rounded at every emit
            std::vector<std::string> bad;
            if (std::fabs(netA - want) > slack) {
                bad.push_back(name + ": net A " + fmt(netA) + "deg, want " + fmt(want) + "deg");
            }
            return bad;
        });
    }

    SUBCASE("unwind keeps physical A bounded over repeated closed loops") {
        const Subpaths three = {curves::fullCircleR30(), curves::fullCircleR30(),
                                curves::fullCircleR30()};
        double phys = 0;
        double peak = 0;
        for (const MicroSegment& s : machine::prep(three, machine::knife())) {
            phys += s.da * aInv;
            peak = std::fmax(peak, std::fabs(phys));
        }
        CHECK(peak < 540 * aSpd);
    }

    SUBCASE("unwind stays correct when the winding came from corner pivots") {
        // The circles above wind A entirely through TRACKING. A square winds it
        // entirely through corner PIVOTS, which update aPhys on a separate code
        // path. Repeating the square lets a mis-tracked aPhys compound into the
        // next subpath's pre-orientation instead of cancelling within one.
        const std::vector<CubicBezier> square = {line(0, 0, 20, 0), line(20, 0, 20, 20),
                                                 line(20, 20, 0, 20), line(0, 20, 0, 0)};
        double phys = 0;
        double peak = 0;
        for (const MicroSegment& s : machine::prep({square, square, square}, machine::knife())) {
            phys += s.da * aInv;
            peak = std::fmax(peak, std::fabs(phys));
        }
        CHECK(peak < 540 * aSpd);
    }
}

TEST_CASE("discretize INVARIANT: every emitted segment is executable") {
    SUBCASE("interval is an integer in [1, fCpu] on every segment") {
        const double fCpu = machine::axes().fCpu;
        forEachFixture([&](const std::string& name, const std::vector<CubicBezier>& cs) {
            std::vector<std::string> bad;
            for (const auto& t : tools()) {
                const std::vector<MicroSegment> segs = machine::prep({cs}, t.second);
                for (size_t i = 0; i < segs.size() && bad.size() < 3; i++) {
                    const double iv = segs[i].interval;
                    if (iv != std::floor(iv) || iv < 1 || iv > fCpu) {
                        bad.push_back(name + "/" + t.first + ": seg " + std::to_string(i) +
                                      " interval " + fmt(iv, 1));
                    }
                }
            }
            return bad;
        });
    }

    SUBCASE("all step deltas are integers") {
        forEachFixture([](const std::string& name, const std::vector<CubicBezier>& cs) {
            std::vector<std::string> bad;
            const std::vector<MicroSegment> segs = machine::prep({cs}, machine::knife());
            for (size_t i = 0; i < segs.size() && bad.size() < 3; i++) {
                const MicroSegment& s = segs[i];
                const double d[4] = {s.dx, s.dy, s.dz, s.da};
                for (double x : d) {
                    if (x != std::floor(x)) {
                        bad.push_back(name + ": seg " + std::to_string(i) + " non-integer delta");
                        break;
                    }
                }
            }
            return bad;
        });
    }
}

TEST_CASE("discretize INVARIANT: PATH_END marks each subpath exactly once") {
    SUBCASE("one MICRO_PATH_END per subpath, on a segment that moves") {
        const Subpaths sp = {curves::straightLine(), curves::quarterCircleR5()};
        const std::vector<MicroSegment> segs = machine::prep(sp, machine::knife());
        size_t ends = 0;
        for (const MicroSegment& s : segs) {
            if (s.flags & MICRO_PATH_END) {
                ends++;
                CHECK(major(s) > 0);
            }
        }
        CHECK(ends == sp.size());
    }

    SUBCASE("the final cutting segment of a single subpath carries it") {
        const std::vector<MicroSegment> segs =
            cutting(machine::prep({curves::straightLine()}, machine::knife()));
        REQUIRE(!segs.empty());
        CHECK((segs.back().flags & MICRO_PATH_END) != 0);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// CONTRACT PROPERTIES — tool behaviour
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("discretize: a non-tangential tool") {
    SUBCASE("never rotates A and never lifts") {
        forEachFixture([](const std::string& name, const std::vector<CubicBezier>& cs) {
            std::vector<std::string> bad;
            for (const MicroSegment& s : machine::prep({cs}, machine::pen())) {
                if (bad.size() >= 2) break;
                if (s.da != 0) bad.push_back(name + ": da=" + fmt(s.da, 0));
                if (s.flags & MICRO_LIFT) bad.push_back(name + ": MICRO_LIFT set");
            }
            return bad;
        });
    }

    SUBCASE("rounds a 90-degree corner at speed instead of stopping for it") {
        // The junction-deviation path: a pen has no blade to reorient, so a
        // sharp join is cornered, not lift-pivoted. Pins that PEN does NOT
        // inherit the knife's stop.
        const Subpaths corner = {{line(0, 0, 20, 0), line(20, 0, 20, 20)}};
        const std::vector<PlannedSample> p = machine::planFor(corner, machine::pen());
        size_t bi = 0;
        bool found = false;
        for (size_t i = 0; i < p.size(); i++) {
            if (p[i].s.flags & motion::CURVE_BOUNDARY) {
                bi = i;
                found = true;
                break;
            }
        }
        REQUIRE(found);
        CHECK(bi > 0);
        CHECK(p[bi].v > 1.0);
        for (const MicroSegment& s : machine::prep(corner, machine::pen())) {
            CHECK(s.da == 0);
        }
    }
}

TEST_CASE("discretize: a tangential tool at a corner") {
    SUBCASE("emits a pure-A pivot at a 90-degree join") {
        const Subpaths corner = {{line(0, 0, 20, 0), line(20, 0, 20, 20)}};
        const std::vector<MicroSegment> segs = machine::prep(corner, machine::knife());
        double turned = 0;
        size_t pivots = 0;
        const double aInv = machine::axes().a.invert ? -1 : 1;
        for (const MicroSegment& s : segs) {
            if ((s.flags & MICRO_JOG) && s.da != 0 && s.dx == 0 && s.dy == 0) {
                pivots++;
                turned += s.da * aInv;
            }
        }
        CHECK(pivots > 0);
        // and the pivot turns through the full corner
        CHECK(std::fabs(turned / machine::axes().a.stepsPerUnit) > 80);
    }

    SUBCASE("does not pivot where the tangent turns smoothly") {
        // A quarter circle turns 90 degrees in total but never more than
        // dthetaMax at once, so it must be tracked continuously, not pivoted.
        const std::vector<PlannedSample> p =
            machine::planFor({curves::quarterCircleR50()}, machine::knife());
        CHECK(cornerIndices(p, machine::knife()).empty());
    }
}

TEST_CASE("discretize: per-axis invert is applied to every emitted delta") {
    // The bench machine has x.invert = true but y.invert = false, so the
    // fixtures alone cannot tell "Y invert applied" from "Y invert ignored".
    // Flip each axis explicitly and require the emitted deltas to negate.
    const std::vector<PlannedSample> p = machine::planFor({curves::sCurve()}, machine::pen());

    SUBCASE("flipping x.invert negates every dx and nothing else") {
        DiscretizeOptions off = machine::pen();
        off.axes.x.invert = false;
        DiscretizeOptions on = machine::pen();
        on.axes.x.invert = true;
        const std::vector<MicroSegment> a = discretize(p, off);
        const std::vector<MicroSegment> b = discretize(p, on);
        REQUIRE(a.size() == b.size());
        for (size_t i = 0; i < a.size(); i++) {
            CAPTURE(i);
            CHECK(b[i].dx == -a[i].dx);
            CHECK(b[i].dy == a[i].dy);
        }
    }

    SUBCASE("flipping y.invert negates every dy and nothing else") {
        DiscretizeOptions off = machine::pen();
        off.axes.y.invert = false;
        DiscretizeOptions on = machine::pen();
        on.axes.y.invert = true;
        const std::vector<MicroSegment> a = discretize(p, off);
        const std::vector<MicroSegment> b = discretize(p, on);
        REQUIRE(a.size() == b.size());
        for (size_t i = 0; i < a.size(); i++) {
            CAPTURE(i);
            CHECK(b[i].dy == -a[i].dy);
            CHECK(b[i].dx == a[i].dx);
        }
    }
}

TEST_CASE("discretize: Z lift choreography") {
    // Every shipped tool profile has liftHeight = 0, so in the default config
    // `lift` is false and NOTHING in the Z path executes — no lower-to-cut, no
    // raise-at-end, no lift inside a corner pivot. The lift-pivot-lower that the
    // whole corner design rests on has never actually lifted under test. These
    // drive it through the documented liftHeight override.
    const double LIFT = 2.0;
    const auto lifted = [&](const Subpaths& sp, DiscretizeOptions t) {
        const std::vector<PlannedSample> p = machine::planFor(sp, t);
        t.liftHeight = LIFT;
        return discretize(p, t);
    };

    SUBCASE("lowers before the stroke and raises after it, by the same step count") {
        const std::vector<MicroSegment> segs = lifted({curves::straightLine()}, machine::knife());
        const std::vector<ZRun> runs = zRuns(segs);
        REQUIRE(runs.size() == 2);
        CHECK(runs[0].dz + runs[1].dz == 0); // returns to travel height
        CHECK(std::fabs(runs[0].dz) == motion::jsRound(LIFT * machine::axes().z.stepsPerUnit));
        CHECK(runs[0].dz == -runs[1].dz);    // down first, up last
    }

    SUBCASE("net Z is zero over many subpaths — every lower is matched by a raise") {
        const Subpaths sp = {curves::straightLine(), curves::quarterCircleR5(), curves::cusp()};
        double sum = 0;
        for (const MicroSegment& s : lifted(sp, machine::knife())) sum += s.dz;
        CHECK(sum == 0);
    }

    SUBCASE("a corner pivot lifts, turns, and lowers again") {
        const Subpaths corner = {{line(0, 0, 20, 0), line(20, 0, 20, 20)}};
        const std::vector<MicroSegment> segs = lifted(corner, machine::knife());
        const std::vector<ZRun> runs = zRuns(segs);
        // four runs: lower to cut, the pivot's lift and lower, raise after
        REQUIRE(runs.size() == 4);
        CHECK(runs[1].dz == -runs[2].dz); // the pivot's pair cancels
        // and a pure-A rotation happens between the lift and the lower
        bool rotated = false;
        for (size_t i = runs[1].to + 1; i < runs[2].from; i++) {
            CHECK(segs[i].dx == 0);
            CHECK(segs[i].dy == 0);
            CHECK(segs[i].dz == 0);
            if (segs[i].da != 0) rotated = true;
        }
        CHECK(rotated);
    }

    SUBCASE("XY conservation is unaffected by lifting") {
        const Net n = net(lifted({curves::sCurve()}, machine::knife()));
        const std::pair<double, double> e = expectedXY({curves::sCurve()});
        CHECK(n.dx == e.first);
        CHECK(n.dy == e.second);
    }
}

TEST_CASE("discretize: velocity-aware subdivision") {
    SUBCASE("skips interior sub-steps that move nothing") {
        // The interior-skip guard working as intended. Interior only — the
        // guard's two former exemptions are D1's subject.
        DiscretizeOptions dense = machine::pen();
        const std::vector<PlannedSample> p =
            machine::planFor({curves::longGentleArc()}, machine::pen());
        dense.dvMax = 0.05;
        for (const MicroSegment& s : discretize(p, dense)) {
            if (major(s) == 0) CHECK((s.flags & MICRO_PATH_END) != 0);
        }
    }

    SUBCASE("the subdivision rule itself never leaves a pair above dvMax") {
        // Re-derives k the way the stage does, so it can only ever check the
        // ARITHMETIC of the rule — that `ceil` and the 256 clamp compose into a
        // realised dv within budget. It cannot check that the stage USES this
        // rule; that is the subcase below, and the two are not interchangeable.
        // Mutation proved it: replacing the stage's `ceil` with `floor` left
        // this test green, because the test's own `ceil` was still doing the
        // work. Same shape as FINDING C3 — a check that re-derives the quantity
        // it is auditing agrees with the implementation by construction.
        forEachFixture([](const std::string& name, const std::vector<CubicBezier>& cs) {
            const std::vector<PlannedSample> p = machine::planFor({cs}, machine::knife());
            std::vector<std::string> bad;
            for (const motion::Range& r : motion::subpathRanges(p)) {
                for (size_t i = r.first; i < r.second && bad.size() < 3; i++) {
                    const double dv = std::fabs(p[i + 1].v - p[i].v);
                    const double k = std::fmin(256.0, std::fmax(1.0, std::ceil(dv / quality::DV_MAX)));
                    if (dv / k > quality::DV_MAX * 1.001) {
                        bad.push_back(name + ": pair " + std::to_string(i) + " realised dv " +
                                      fmt(dv / k, 3) + " > " + fmt(quality::DV_MAX, 3));
                    }
                }
            }
            return bad;
        });
    }

    SUBCASE("and the segments it actually emits hold that speed budget") {
        // The two-sided version, measured from the EMITTED stream. Each
        // segment's speed is re-derived the way the firmware will execute it —
        // XY distance over (interval x major steps / fCpu) — with no reference
        // to k, to dvMax, or to anything discretize computed. A rule the stage
        // does not follow shows up here and nowhere else.
        //
        // PEN only, and the two rate-floor fixtures excluded: where interval()'s
        // per-axis floor binds (D3) the executed speed is deliberately NOT the
        // planned one, and this measurement would be reading that instead.
        const motion::ResolvedAxes& ax = machine::axes();
        forEachFixture([&](const std::string& name, const std::vector<CubicBezier>& cs) {
            std::vector<std::string> bad;
            if (name == "cusp" || name == "near_cusp") return bad;
            const std::vector<MicroSegment> segs = cutting(machine::prep({cs}, machine::pen()));
            double prev = -1;
            size_t measured = 0;
            for (size_t i = 0; i < segs.size() && bad.size() < 3; i++) {
                const double dist = motion::jsHypot(segs[i].dx / ax.x.stepsPerUnit,
                                                    segs[i].dy / ax.y.stepsPerUnit);
                const double dt = (segs[i].interval * major(segs[i])) / ax.fCpu;
                // A segment's interval is ONE integer for the whole segment, so
                // the speed this derives is quantised to roughly 1/major. Below
                // ~10 steps that quantisation is larger than the budget being
                // measured and the reading is noise — which is exactly what the
                // ends of a path are, where the tool is leaving or returning to
                // rest one step at a time. Measuring them anyway reported 6 mm/s
                // "jumps" that are an artefact of the integer, not motion.
                if (dist <= 0 || dt <= 0 || major(segs[i]) < 10) {
                    prev = -1;
                    continue;
                }
                const double v = dist / dt;
                measured++;
                // 1.5x slack for the residual quantisation. The mutants this
                // must catch move the speed by 2x or more, so the slack costs
                // nothing that matters.
                if (prev >= 0 && std::fabs(v - prev) > quality::DV_MAX * 1.5) {
                    bad.push_back(name + ": segment " + std::to_string(i) + " jumps " +
                                  fmt(std::fabs(v - prev), 3) + " mm/s, budget " +
                                  fmt(quality::DV_MAX, 3));
                }
                prev = v;
            }
            // A filter that quietly excluded everything would make this pass
            // vacuously, which is the failure mode the filter itself invites.
            // Only demanded of fixtures long enough to have interior segments at
            // all — `short_curve` is 3 mm of arc and every one of its segments is
            // a few steps, so it has nothing this measurement can read. That is a
            // property of the fixture, not a gap, and it is stated as a length
            // condition rather than an exemption by name so a fixture that
            // SHRINKS into this category is not silently excused.
            if (segs.size() >= 50 && measured < 20) {
                bad.push_back(name + ": only " + std::to_string(measured) +
                              " segment(s) were long enough to measure");
            }
            return bad;
        });
    }

    SUBCASE("driving dvMax to zero does not drive the segment count to infinity") {
        // Wire volume is bounded by the GEOMETRY, not by the subdivision knob.
        // A pair spans at most dsMax of arc and dthetaMax of turn, so it has at
        // most dsMax*spu = 80 XY steps and dthetaMax*aSpd = 104 A steps to give;
        // every sub-step beyond that rounds to no motion and is skipped. So the
        // emitted count must SATURATE as dvMax falls, and an operator who sets
        // dvMax absurdly low gets a slow bake, not an unsendable stream.
        //
        // Measured on this fixture: 609 segments at the shipped dvMax=3,
        // 22,426 at 1e-3, 23,099 at 1e-4 — a 10x tightening buying 3%.
        const std::vector<PlannedSample> p =
            machine::planFor({curves::sCurve()}, machine::knife());
        const auto countAt = [&](double dvMax) {
            DiscretizeOptions t = machine::knife();
            t.dvMax = dvMax;
            return discretize(p, t).size();
        };
        const size_t pairs = p.size() - 1;
        const size_t coarse = countAt(1e-3);
        const size_t fine = countAt(1e-4);
        CHECK_MESSAGE(fine < coarse * 1.2,
                      "10x finer dvMax grew the stream from " << coarse << " to " << fine
                                                              << " segments — not saturating");
        // and the bound is the geometric one, not an artefact of nothing happening
        CHECK_MESSAGE(fine < pairs * 128,
                      "emitted " << fine << " segments for " << pairs << " pairs");
        CHECK_MESSAGE(fine > pairs * 8,
                      "only " << fine << " segments for " << pairs
                              << " pairs — dvMax is not driving subdivision at all");
    }

    SUBCASE("a cruise at constant speed is not subdivided") {
        // k=1 on cruise is what keeps segment counts sane; if this regresses the
        // wire volume explodes without improving anything.
        const std::vector<MicroSegment> mid =
            cutting(machine::prep({curves::longGentleArc()}, machine::pen()));
        const std::vector<PlannedSample> p =
            machine::planFor({curves::longGentleArc()}, machine::pen());
        size_t cruisePairs = 0;
        for (const motion::Range& r : motion::subpathRanges(p)) {
            for (size_t i = r.first; i < r.second; i++) {
                if (std::fabs(p[i + 1].v - p[i].v) < quality::DV_MAX) cruisePairs++;
            }
        }
        CHECK(cruisePairs > 100u);
        CHECK(static_cast<double>(mid.size()) < static_cast<double>(p.size()) * 1.5);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// FINDINGS — each pins a defect recorded in docs/planner_audit.md.
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("discretize D1 (FIXED): no empty segment carrying a one-second interval") {
    // The interior-skip guard used to exempt two cases: the subpath's final
    // sub-step, and a corner's last sub-step. Both can have every delta zero —
    // and interval()'s `if (major == 0) return fCpu` then hands the empty
    // segment the largest interval representable:
    //
    //     dx=dy=dz=da=0, interval = 150,000,000 = a full second at fCpu.
    //
    // Both were reachable without strange geometry: `cusp` with a KNIFE hit the
    // corner case, and `long_gentle_arc` with a PEN at dvMax = 0.05 hit the
    // final one — so the marker that ENDS every path was itself the empty one.
    //
    // FIXED: every zero-motion sub-step is skipped. PATH_END is re-homed onto
    // the last segment the subpath actually emitted — the same position in the
    // stream, minus the empty second. The one-PATH_END-per-subpath invariant
    // above pins that it survives; these pin WHERE it lands.
    SUBCASE("emits no segment with zero motion on any axis") {
        forEachFixture([](const std::string& name, const std::vector<CubicBezier>& cs) {
            std::vector<std::string> bad;
            for (const auto& t : tools()) {
                const std::vector<MicroSegment> segs = machine::prep({cs}, t.second);
                for (size_t i = 0; i < segs.size(); i++) {
                    if (major(segs[i]) == 0) {
                        bad.push_back(name + "/" + t.first + ": seg " + std::to_string(i) +
                                      " all-zero, interval " + fmt(segs[i].interval, 0) + " (" +
                                      fmt(segs[i].interval / machine::axes().fCpu, 3) + "s)");
                    }
                }
            }
            return bad;
        });
    }

    SUBCASE("reaches the PATH_END marker on ordinary geometry, not just a cusp") {
        // The exemption that mattered most: this is a pen on a gentle arc.
        DiscretizeOptions dense = machine::pen();
        dense.dvMax = 0.05;
        const std::vector<PlannedSample> p =
            machine::planFor({curves::longGentleArc()}, machine::pen());
        size_t empty = 0;
        for (const MicroSegment& s : discretize(p, dense)) {
            if (major(s) == 0) empty++;
        }
        CHECK(empty == 0u);
    }

    SUBCASE("PATH_END lands on a segment that moves, on every fixture and tool") {
        // The relocation's actual contract. Without this, skipping the final
        // sub-step could be 'fixed' by dropping the marker onto anything.
        forEachFixture([](const std::string& name, const std::vector<CubicBezier>& cs) {
            std::vector<std::string> bad;
            for (const auto& t : tools()) {
                const std::vector<MicroSegment> segs = machine::prep({cs}, t.second);
                for (size_t i = 0; i < segs.size(); i++) {
                    if ((segs[i].flags & MICRO_PATH_END) != 0 && major(segs[i]) == 0) {
                        bad.push_back(name + "/" + t.first + ": PATH_END on empty seg " +
                                      std::to_string(i));
                    }
                }
            }
            return bad;
        });
    }

    SUBCASE("keeps PATH_END on the last cutting segment when the final sub-step moves") {
        // The common case must be untouched by the relocation: when the final
        // sub-step does move, the marker rides it, exactly as before.
        const std::vector<MicroSegment> cut =
            cutting(machine::prep({{line(0, 0, 40, 0)}}, machine::pen()));
        REQUIRE(cut.size() > 1);
        CHECK((cut.back().flags & MICRO_PATH_END) == MICRO_PATH_END);
        for (size_t i = 0; i + 1 < cut.size(); i++) {
            CAPTURE(i);
            CHECK((cut[i].flags & MICRO_PATH_END) == 0u);
        }
    }

    SUBCASE("marks the last CUTTING segment when a subpath ends on a corner") {
        // A subpath whose final pair is a corner emits pivot (and Z-raise)
        // segments AFTER the cut ends. So "last segment emitted" and "last
        // cutting segment" genuinely differ here, and PATH_END belongs to the
        // cut. This is what stops the D1 relocation from drifting onto the
        // choreography that follows.
        const Subpaths tinyTail = {{line(0, 0, 10, 0), line(10, 0, 10.001, 0.001)}};
        const std::vector<MicroSegment> segs = machine::prep(tinyTail, machine::knife());
        size_t at = segs.size();
        for (size_t i = 0; i < segs.size(); i++) {
            if (segs[i].flags & MICRO_PATH_END) {
                at = i;
                break;
            }
        }
        REQUIRE(at < segs.size());
        CHECK(segs.size() - 1 - at > 0u); // choreography follows it
        CHECK(major(segs[at]) > 0);
        // and everything after it is non-cutting (lift / pivot / lower / raise)
        for (size_t i = at + 1; i < segs.size(); i++) {
            CAPTURE(i);
            CHECK(isChoreography(segs[i]));
        }
    }

    SUBCASE("moves PATH_END back one segment when the final sub-step is empty") {
        // The relocation firing, isolated: the dense PEN arc is the case where
        // the last sub-step rounds to no motion. The marker must be on the
        // segment before it, and that segment must be a real move.
        DiscretizeOptions dense = machine::pen();
        dense.dvMax = 0.05;
        const std::vector<PlannedSample> p =
            machine::planFor({curves::longGentleArc()}, machine::pen());
        const std::vector<MicroSegment> cut = cutting(discretize(p, dense));
        REQUIRE(!cut.empty());
        CHECK((cut.back().flags & MICRO_PATH_END) == MICRO_PATH_END);
        CHECK(major(cut.back()) > 0);
    }
}

TEST_CASE("discretize D2 (FIXED): sub-segment speed follows constant acceleration") {
    // The stage used to interpolate sub-segment speed linearly in ARC LENGTH,
    // v(f) = v0 + (v1 - v0)*f. Under constant acceleration — exactly what plan's
    // sweeps produce — speed is not linear in distance:
    //     v(f) = sqrt(v0^2 + f*(v1^2 - v0^2)).
    //
    // Consequences, measured:
    //   - At k=1 the pair-level mean (v0+v1)/2 is EXACTLY right, so the emitted
    //     time was exact: ratio 1.0000.
    //   - Every subdivision replaced that one exact estimate with k wrong ones,
    //     and the error grew monotonically the harder it subdivided:
    //         10mm line, dvMax = inf / 24 / 6 / 3 / 0.75
    //                    1.000 / 1.103 / 1.268 / 1.361 / 1.510
    //     Subdivision exists to improve fidelity (premortem P3). For timing it
    //     did the opposite, and the knob meant to buy accuracy was the one
    //     costing it.
    //
    // FIXED. These tests were written RED against the linear model and inverting
    // them is the whole record of the fix: subdivision must be timing-neutral.
    //
    // The exempt set is not a judgement call: cusp and near_cusp are exactly the
    // two fixtures whose plan asks the A axis for more than its rate ceiling
    // (16.82x and 1.02x), which is the precondition for interval()'s floor to
    // stretch a segment. Every fixture where D3 cannot fire is exact.
    SUBCASE("emitted cut time matches the exact constant-accel time") {
        forEachFixture([](const std::string& name, const std::vector<CubicBezier>& cs) {
            std::vector<std::string> bad;
            if (name == "cusp" || name == "near_cusp") return bad; // asserted under D3
            const std::vector<PlannedSample> p = machine::planFor({cs}, machine::knife());
            const double ratio =
                emittedSeconds(cutting(machine::prep({cs}, machine::knife()))) / plannedSeconds(p);
            if (ratio > 1.02) {
                bad.push_back(name + ": emitted " + fmt(ratio, 3) + "x the exact cut time");
            }
            return bad;
        });
    }

    SUBCASE("subdivision is timing-neutral: dvMax buys fidelity without costing time") {
        // The isolation that identified the cause, now inverted. Runs a PEN, so
        // no A axis: any error would be in the sub-segment model alone, not in
        // interval(), not in step rounding.
        const Subpaths ten = {{line(0, 0, 10, 0)}};
        const std::vector<PlannedSample> p = machine::planFor(ten, machine::pen());
        const double exact = plannedSeconds(p);
        const auto ratioAt = [&](double dvMax) {
            DiscretizeOptions t = machine::pen();
            t.dvMax = dvMax;
            return emittedSeconds(discretize(p, t)) / exact;
        };
        CHECK(std::fabs(ratioAt(1e9) - 1.0) < 5e-4);  // k=1 everywhere
        CHECK(std::fabs(ratioAt(6) - 1.0) < 5e-3);    // was 1.268
        CHECK(std::fabs(ratioAt(0.75) - 1.0) < 5e-3); // was 1.510 — the harder it
        // subdivided the worse it got; monotonic degradation is gone.
    }

    SUBCASE("times a ramp-dominated path as accurately as a cruising one") {
        // Long paths cruise, so the old ramp error was diluted; short ones are
        // all ramp. That asymmetry is why the golden fixtures (long SVG paths)
        // never showed D2 and a 10mm line did. It must no longer exist.
        const auto ratioFor = [](double L) {
            const Subpaths sp = {{line(0, 0, L, 0)}};
            const std::vector<PlannedSample> p = machine::planFor(sp, machine::pen());
            return emittedSeconds(discretize(p, machine::pen())) / plannedSeconds(p);
        };
        CHECK(std::fabs(ratioFor(10) - 1.0) < 5e-3); // was 1.361
        CHECK(std::fabs(ratioFor(500) - 1.0) < 5e-3);
    }

    SUBCASE("leaving rest is finite without leaning on vMin") {
        // The linear model's time integral over a pair is ds*ln(v1/v0)/(v1-v0),
        // which diverges as v0 -> 0: quality.vMin was the only reason a ramp off
        // a standstill produced a finite number, and that made an ACCURACY clamp
        // load-bearing for TERMINATION. Under sqrt interpolation the mean is
        // (v0+v1)/2 with v0 = 0 handled exactly, so slashing vMin by 1000x must
        // barely move the emitted time.
        const Subpaths ten = {{line(0, 0, 10, 0)}};
        const std::vector<PlannedSample> p = machine::planFor(ten, machine::pen());
        const auto at = [&](double vMin) {
            DiscretizeOptions t = machine::pen();
            t.vMin = vMin;
            return emittedSeconds(discretize(p, t));
        };
        CHECK(std::fabs(at(quality::V_MIN / 1000) / at(quality::V_MIN) - 1.0) < 5e-3);
    }
}

TEST_CASE("discretize D3 (OPEN): interval's rate floor is a second speed governor") {
    // interval() floors each segment's duration so no axis exceeds
    // maxFeed * stepsPerUnit. The floor is correct and necessary — but it is
    // applied AFTER planning, and nothing upstream knows it fired. Where it
    // binds, the executed timeline is slower than the planned one and every
    // quantity derived from the plan's timeline is wrong with it.
    //
    // That matters most for stage 9: docs/tool_duty_limits.md §5 schedules the
    // knife's enable-line resets against PLANNED durations. An error in the
    // window is a knife that runs past its budget.
    //
    // The chain the finding was originally filed with — flatten's tangent cap
    // overshooting (F7), constrain under-capping v from kappa, and plan then
    // asking A for up to 16.8x its rate ceiling — NO LONGER HOLDS at its top
    // end. The first subcase below measures the A demand directly and it is
    // within the ceiling on every fixture (worst 1.01x on `cusp`), in both
    // languages. The stretch is real but its cause is no longer "the plan
    // overdrives A", and the audit entry should be re-derived rather than
    // re-quoted. Filed here as an open question, not as a known cause.
    //
    // The TypeScript states the surviving half as a RED test. A permanently-red
    // suite trains people to ignore it, so the port states it as a PIN ON THE
    // DEFECT: it asserts the stretch is still present, on exactly the two
    // fixtures where it is, and goes red the day D3 is fixed — which is the
    // signal to invert it, exactly as D1 and D2 above were inverted.
    SUBCASE("the plan never asks the A axis for more than its rate ceiling") {
        // Green, and green in the TypeScript too. Kept because it is the load
        // -bearing half of D3's stated cause: if this ever goes red again, the
        // stretch below has an explanation, and if it stays green it does not.
        const double aCeil = machine::axes().a.maxFeed;
        forEachFixture([&](const std::string& name, const std::vector<CubicBezier>& cs) {
            const std::vector<PlannedSample> p = machine::planFor({cs}, machine::knife());
            double worst = 0;
            for (const motion::Range& r : motion::subpathRanges(p)) {
                for (size_t i = r.first; i < r.second; i++) {
                    const PlannedSample& a = p[i];
                    const PlannedSample& b = p[i + 1];
                    if (a.s.ds < 1e-9 || a.v < 1e-9) continue;
                    const double dt = a.s.ds / (0.5 * (a.v + b.v));
                    worst = std::fmax(worst, std::fabs(angleDelta(a.s.theta, b.s.theta)) / dt);
                }
            }
            std::vector<std::string> bad;
            if (worst > aCeil * 1.01) {
                bad.push_back(name + ": plan asks A for " + fmt(worst, 0) + " deg/s (" +
                              fmt(worst / aCeil, 2) + "x the " + fmt(aCeil, 0) + " ceiling)");
            }
            return bad;
        });
    }

    SUBCASE("emitted cut time still exceeds the plan, on cusp and near_cusp only") {
        // D2's fix took every other fixture to 1.000x; these two did not move.
        // The band is two-sided on purpose. An upper bound alone would be
        // satisfied by the defect getting arbitrarily WORSE, which is the same
        // one-sided blindness FINDING C3 found in constrain's tests — a pin on
        // an open defect has to pin its magnitude, not just its existence.
        forEachFixture([](const std::string& name, const std::vector<CubicBezier>& cs) {
            std::vector<std::string> bad;
            if (!(name == "cusp" || name == "near_cusp")) return bad;
            const std::vector<PlannedSample> p = machine::planFor({cs}, machine::knife());
            const double ratio =
                emittedSeconds(cutting(machine::prep({cs}, machine::knife()))) / plannedSeconds(p);
            // Measured: cusp 1.101, near_cusp 1.867 — the same numbers the
            // TypeScript's red test reports, which is what makes this a port of
            // the finding rather than a restatement of it.
            const double want = (name == "cusp") ? 1.101 : 1.867;
            if (ratio <= 1.02) {
                bad.push_back(name + ": emitted " + fmt(ratio, 3) +
                              "x — D3 appears FIXED; invert this test");
            } else if (std::fabs(ratio - want) > 0.01) {
                bad.push_back(name + ": emitted " + fmt(ratio, 3) + "x, D3 was filed at " +
                              fmt(want, 3) + "x — the defect moved, re-measure it");
            }
            return bad;
        });
    }

    SUBCASE("documents the root cause: actual turn exceeds what kappa predicts") {
        // constrain's A-slew cap is rad(aRate)/kappa, which is only sound if the
        // sample-to-sample turn equals kappa*ds. It does not. Delete this only
        // together with F7.
        const std::vector<PlannedSample> p = machine::planFor({curves::cusp()}, machine::knife());
        double worst = 0;
        for (const motion::Range& r : motion::subpathRanges(p)) {
            for (size_t i = r.first; i < r.second; i++) {
                const double predicted = (p[i].s.kappa * p[i].s.ds * 180) / motion::PI;
                if (predicted > 1e-9) {
                    worst = std::fmax(worst,
                                      std::fabs(angleDelta(p[i].s.theta, p[i + 1].s.theta)) /
                                          predicted);
                }
            }
        }
        CHECK(worst > 4);
    }
}

TEST_CASE("discretize D4: the corner rule is ungated, unlike constrain's") {
    // constrain gates its corner-stop on (flags & CURVE_BOUNDARY); discretize
    // gates on nothing. So discretize will lift-pivot at an INTRA-curve tangent
    // jump that constrain never stopped for, contradicting this stage's own
    // header ("velocity planning already brought the tool to v=0 at every
    // corner").
    //
    // Measured, the gap is currently narrow: the only geometry that reaches it
    // is a cusp, where the curvature caps happen to have crawled v down anyway
    // (4.8e-3 mm/s on `cusp`). So today the precondition holds BY ACCIDENT, not
    // by construction — and interval() floors that crawl up to vMin = 0.5 mm/s
    // regardless, so the pivot does execute while moving.
    //
    // Filed rather than dismissed because the accident is F1's doing: a cusp is
    // exactly where flatten's tangent cap is skipped. Fix F1 so the marcher
    // resolves cusps properly and this stops being a cusp-only case.
    //
    // The test is GREEN on the current fixture set, in both languages — the gap
    // is a latent one, reachable only by geometry none of these fixtures
    // contains. That is precisely why it is worth keeping: it is the check that
    // will notice when a future flatten change makes the case reachable.
    SUBCASE("every corner it pivots at was stopped for by constrain") {
        forEachFixture([](const std::string& name, const std::vector<CubicBezier>& cs) {
            const std::vector<PlannedSample> p = machine::planFor({cs}, machine::knife());
            std::vector<std::string> bad;
            for (size_t i : cornerIndices(p, machine::knife())) {
                if (p[i].vCeiling != 0 && bad.size() < 3) {
                    std::ostringstream os;
                    os << name << ": corner at sample " << i << " has vCeiling "
                       << p[i].vCeiling << ", not 0";
                    bad.push_back(os.str());
                }
            }
            return bad;
        });
    }
}
