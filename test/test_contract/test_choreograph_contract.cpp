/**
 * Contract tests for stage 8, choreograph — ported from
 * web/test/choreograph/choreograph.test.ts.
 *
 * Everything the machine does that is not cutting: travel jogs, Z lift and
 * lower, ramped A rotation, lift-pivot-lower, A pre-orientation.
 *
 *   INVARIANTS          hold for every input, forever. A failure is a bug.
 *   CONTRACT PROPERTIES what callers are entitled to rely on.
 *
 * The kinematic checks deliberately reconstruct the motion the way the FIRMWARE
 * will execute it — |steps| clocked at `interval` cycles apiece — rather than
 * from the velocity the emitter believed it was writing. A check expressed in
 * the emitter's own terms cannot see the emitter's own error; that is how H1 was
 * found, and it is the same discipline that closed C3 and D6 in the earlier
 * stages.
 *
 * Two things this file carries that the TypeScript does not, both because the
 * differential cannot see them:
 *
 *   - `aMoveTo` and `headOffsetJog` have NO caller in the C++ port. discretize
 *     reaches neither, so the reference vectors exercise neither, and these
 *     tests are the only thing holding them. They are written to the same
 *     standard as the reachable ones rather than as smoke tests.
 *   - Z is ramped here (H3, fixed) and its accel bound is looser than A's; the
 *     slack is measured rather than guessed. See Z_ACCEL_TOL.
 */

#include "doctest.h"

#include "motion/choreograph.h"
#include "motion/geometry.h"
#include "motion/jsmath.h"
#include "motion/microsegment.h"
#include "support/machine.h"

#include <cmath>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using motion::AMoveResult;
using motion::MicroSegment;
using motion::OpTarget;
using motion::RampChunk;
using motion::ResolvedAxes;
using motion::aMove;
using motion::aMoveTo;
using motion::headOffsetJog;
using motion::pivot;
using motion::preOrient;
using motion::travelJog;
using motion::zMove;
using motion::zStepCount;

namespace {

using Segs = std::vector<MicroSegment>;

const OpTarget NO_SLEW = machine::noSlew();

/** The A accel ceiling aMove is working to, in steps/s^2. */
double aAccel() { return machine::axes().a.maxAccel * machine::axes().a.stepsPerUnit; }
/** The A feed ceiling, in steps/s. */
double aCruise() { return machine::axes().a.maxFeed * machine::axes().a.stepsPerUnit; }
/** Z engage feed and accel (mm/s, mm/s^2) — the machine's own ceilings. */
double zFeed() { return machine::axes().z.maxFeed; }
double zAccel() { return machine::axes().z.maxAccel; }

/**
 * Z's accel bound is looser than A's 1.001, and the slack is measured, not
 * guessed: rampChunks rounds every chunk boundary to an integer step, which
 * perturbs the exact-by-construction accel there. Measured worst is 1.0025 for
 * Z across 120..24000 steps and 1.0001 for A — Z's ramp is only ~200 steps
 * long, so integer marks are coarser relative to it. 1.005 keeps the bound
 * meaningful: the defects it guards (H1, H2) missed by 26-65%, not 0.25%.
 */
constexpr double Z_ACCEL_TOL = 1.005;

std::string fmt(double v, int prec = 2) {
    std::ostringstream os;
    os.precision(prec);
    os << std::fixed << v;
    return os.str();
}

// ── the two tool profiles, as the two bools this module actually reads ────────
// KNIFE is tangential and unwinds; CREASE is tangential and does not; PEN is
// not tangential. See choreograph.h on why the full ToolProfile does not cross.
constexpr bool KNIFE_TAN = true, KNIFE_UNWIND = true;
constexpr bool CREASE_TAN = true, CREASE_UNWIND = false;
constexpr bool PEN_TAN = false, PEN_UNWIND = false;

// ── executed-motion reconstruction (firmware's view, not the emitter's) ───────

struct Slice {
    double steps = 0;
    double v = 0;   // step rate the firmware will actually clock this slice at
    double dt = 0;
};

/** Major-axis step count of a segment. */
double major(const MicroSegment& s) {
    return std::fmax(std::fmax(std::fabs(s.dx), std::fabs(s.dy)),
                     std::fmax(std::fabs(s.dz), std::fabs(s.da)));
}

std::vector<Slice> slices(const Segs& segs, const ResolvedAxes& ax = machine::axes()) {
    std::vector<Slice> out;
    out.reserve(segs.size());
    for (const MicroSegment& s : segs) {
        Slice sl;
        sl.steps = major(s);
        sl.v = ax.fCpu / s.interval;
        sl.dt = (sl.steps * s.interval) / ax.fCpu;
        out.push_back(sl);
    }
    return out;
}

/**
 * Worst acceleration the emitted staircase demands, as a multiple of `limit`.
 *
 * A slice's rate is its mean over the slice, so it is the speed at the slice's
 * TIME MIDPOINT. The machine therefore has half of each adjacent slice to make
 * the change: the demand is |dv| / ((dt_prev + dt_next) / 2). This asks a
 * question about the emitted bytes alone; it never consults the emitter.
 *
 * The midpoint convention replaced a |dv| / dt_prev one, which is asymmetric by
 * construction: on an accelerating ramp the LONG slice precedes each boundary
 * and on a decelerating one the SHORT slice does, so it flatters climbs and
 * penalises descents on the very same profile.
 */
double worstAccelRatio(const std::vector<Slice>& sl, double limit) {
    double worst = 0;
    for (size_t i = 1; i < sl.size(); i++) {
        const double dt = (sl[i - 1].dt + sl[i].dt) / 2;
        worst = std::fmax(worst, std::fabs(sl[i].v - sl[i - 1].v) / dt / limit);
    }
    return worst;
}

double peakV(const std::vector<Slice>& sl) {
    double p = 0;
    for (const Slice& s : sl) p = std::fmax(p, s.v);
    return p;
}

size_t peakIndex(const std::vector<Slice>& sl) {
    const double p = peakV(sl);
    for (size_t i = 0; i < sl.size(); i++) {
        if (sl[i].v == p) return i;
    }
    return 0;
}

/** Split at the peak: everything up to it is the accel ramp, after it the decel. */
struct RampRatios { double up = 0, down = 0; };

RampRatios rampRatios(const std::vector<Slice>& sl, double limit) {
    const size_t iPeak = peakIndex(sl);
    const std::vector<Slice> upSl(sl.begin(), sl.begin() + static_cast<long>(iPeak) + 1);
    const std::vector<Slice> downSl(sl.begin() + static_cast<long>(iPeak), sl.end());
    return RampRatios{worstAccelRatio(upSl, limit), worstAccelRatio(downSl, limit)};
}

double totalSeconds(const std::vector<Slice>& sl) {
    double t = 0;
    for (const Slice& s : sl) t += s.dt;
    return t;
}

/**
 * Aggregate deltas over a multi-segment emission. Travel jogs are ramped, so
 * their geometry is a property of the SUM, not of any one segment.
 */
struct Delta { double dx = 0, dy = 0, dz = 0, da = 0; };

Delta sum(const Segs& segs) {
    Delta t;
    for (const MicroSegment& s : segs) {
        t.dx += s.dx;
        t.dy += s.dy;
        t.dz += s.dz;
        t.da += s.da;
    }
    return t;
}

double sumAbsA(const Segs& segs) {
    double t = 0;
    for (const MicroSegment& s : segs) t += std::fabs(s.da);
    return t;
}

/**
 * The bench machine with per-axis field overrides applied.
 *
 * The TypeScript equivalent (`remap`) has to rebuild the machine through
 * axisConfig/toolHead/resolvedAxes because those types are nested and
 * validated. ResolvedAxes here is already the flat RESULT of that resolution
 * (see axes.h), so patching a field on a copy is the whole operation — and it
 * is the same operation, because nothing downstream of resolution reads
 * anything else.
 */
ResolvedAxes patched(const std::function<void(ResolvedAxes&)>& fn) {
    ResolvedAxes ax = machine::axes();
    fn(ax);
    return ax;
}

/** A range of rotation sizes spanning triangular, short-trapezoid and long. */
const double A_SIZES[] = {52, 129, 258, 500, 1000, 2325, 4650, 9300, 18600};
constexpr size_t N_SIZES = sizeof(A_SIZES) / sizeof(A_SIZES[0]);

/** Aggregate over sizes and fail once, so the worst case cannot hide. */
void forEachSize(const std::function<std::string(double)>& probe) {
    std::vector<std::string> bad;
    for (double n : A_SIZES) {
        const std::string m = probe(n);
        if (!m.empty()) bad.push_back(m);
    }
    if (!bad.empty()) {
        std::string msg = std::to_string(bad.size()) + "/" + std::to_string(N_SIZES) + " sizes:";
        for (const std::string& b : bad) msg += "\n  " + b;
        FAIL(msg);
    }
}

bool isInt(double v) { return std::floor(v) == v; }

/** doctest has no toBeCloseTo; this is vitest's rule, |diff| < 0.5 * 10^-digits. */
bool closeTo(double a, double b, int digits) {
    return std::fabs(a - b) < 0.5 * std::pow(10.0, -digits);
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// INVARIANTS
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("choreograph INVARIANT: purity and determinism") {
    SUBCASE("aMove is deterministic") {
        const Segs a = aMove(2325, machine::axes(), NO_SLEW);
        const Segs b = aMove(2325, machine::axes(), NO_SLEW);
        REQUIRE(a.size() == b.size());
        for (size_t i = 0; i < a.size(); i++) {
            CHECK(a[i].da == b[i].da);
            CHECK(a[i].interval == b[i].interval);
            CHECK(a[i].flags == b[i].flags);
        }
    }

    SUBCASE("emitters do not mutate the axes they are given") {
        // The C++ signatures take `const ResolvedAxes&`, so this is enforced by
        // the compiler rather than by the test — which is a stronger guarantee
        // than the TypeScript's JSON round-trip, not a weaker one. The check is
        // kept because the guarantee is what the callers rely on, and a future
        // signature that drops the const would otherwise pass silently.
        const ResolvedAxes before = machine::axes();
        aMove(1000, machine::axes(), NO_SLEW);
        zMove(100, machine::axes(), zFeed(), zAccel());
        travelJog(0, 0, 500, 500, machine::axes(), 0.5, 80);
        pivot(500, true, 2400, machine::axes(), zFeed(), zAccel(), NO_SLEW);
        preOrient(90, 0, 0, machine::axes(), KNIFE_TAN, KNIFE_UNWIND, NO_SLEW);
        aMoveTo(90, 0, machine::axes(), NO_SLEW);
        const ResolvedAxes& after = machine::axes();
        CHECK(after.a.stepsPerUnit == before.a.stepsPerUnit);
        CHECK(after.a.maxFeed == before.a.maxFeed);
        CHECK(after.a.maxAccel == before.a.maxAccel);
        CHECK(after.x.stepsPerUnit == before.x.stepsPerUnit);
        CHECK(after.z.stepsPerUnit == before.z.stepsPerUnit);
        CHECK(after.fCpu == before.fCpu);
    }
}

TEST_CASE("choreograph INVARIANT: step conservation") {
    SUBCASE("aMove emits exactly |da| steps, every size") {
        forEachSize([](double n) {
            const double total = sumAbsA(aMove(n, machine::axes(), NO_SLEW));
            return total == n ? "" : "N=" + fmt(n, 0) + ": emitted " + fmt(total, 0);
        });
    }

    SUBCASE("aMove emits exactly |da| steps for negative da too") {
        forEachSize([](double n) {
            const double total = sumAbsA(aMove(-n, machine::axes(), NO_SLEW));
            return total == n ? "" : "N=-" + fmt(n, 0) + ": emitted " + fmt(total, 0);
        });
    }

    SUBCASE("aMove never emits a zero-motion segment") {
        forEachSize([](double n) {
            size_t zeros = 0;
            for (const MicroSegment& s : aMove(n, machine::axes(), NO_SLEW)) {
                if (s.da == 0) zeros++;
            }
            return zeros == 0 ? "" : "N=" + fmt(n, 0) + ": " + std::to_string(zeros) + " zero segments";
        });
    }

    SUBCASE("aMoveTo's reported newAPhys matches the steps it emitted") {
        const double cases[][2] = {{90, 0}, {0, 4650}, {-90, 1000}, {51.43, -200}};
        for (const auto& c : cases) {
            const AMoveResult r = aMoveTo(c[0], c[1], machine::axes(), NO_SLEW);
            CHECK(std::fabs(r.newAPhys - c[1]) == sumAbsA(r.segments));
        }
    }

    SUBCASE("preOrient's reported newAPhys matches the steps it emitted") {
        const double cases[][3] = {{90, 0, 0}, {0, 90, 4650}, {-45, 30, -100}};
        for (const auto& c : cases) {
            for (bool unwind : {KNIFE_UNWIND, CREASE_UNWIND}) {
                const AMoveResult r =
                    preOrient(c[0], c[1], c[2], machine::axes(), true, unwind, NO_SLEW);
                CHECK(std::fabs(r.newAPhys - c[2]) == sumAbsA(r.segments));
            }
        }
    }

    SUBCASE("pivot conserves Z: the lift and the lower cancel exactly") {
        CHECK(sum(pivot(1000, true, 2400, machine::axes(), zFeed(), zAccel(), NO_SLEW)).dz == 0);
    }
}

TEST_CASE("choreograph INVARIANT: wire encoding") {
    const ResolvedAxes& ax = machine::axes();

    SUBCASE("every emitted interval is in [1, fCpu]") {
        Segs all;
        for (const Segs& g : {aMove(18600, ax, NO_SLEW), aMove(1, ax, NO_SLEW),
                              pivot(1000, true, 2400, ax, zFeed(), zAccel(), NO_SLEW),
                              zMove(2400, ax, zFeed(), zAccel()),
                              travelJog(0, 0, 32000, 16000, ax, 0.5, 80)}) {
            all.insert(all.end(), g.begin(), g.end());
        }
        REQUIRE(all.size() > 0);
        for (const MicroSegment& s : all) {
            CHECK(s.interval >= 1);
            CHECK(s.interval <= ax.fCpu);
        }
    }

    SUBCASE("all deltas are integers") {
        Segs all;
        for (const Segs& g : {aMove(1234, ax, NO_SLEW),
                              pivot(567, true, 2400, ax, zFeed(), zAccel(), NO_SLEW),
                              travelJog(0.4, 0.6, 321.7, 89.2, ax, 0.5, 80)}) {
            all.insert(all.end(), g.begin(), g.end());
        }
        REQUIRE(all.size() > 0);
        for (const MicroSegment& s : all) {
            CHECK(isInt(s.dx));
            CHECK(isInt(s.dy));
            CHECK(isInt(s.dz));
            CHECK(isInt(s.da));
            CHECK(isInt(s.interval));
        }
    }

    SUBCASE("aMove emits pure A motion tagged MICRO_JOG") {
        for (const MicroSegment& s : aMove(2325, ax, NO_SLEW)) {
            CHECK(s.flags == motion::MICRO_JOG);
            CHECK(s.dx == 0);
            CHECK(s.dy == 0);
            CHECK(s.dz == 0);
        }
    }

    SUBCASE("zMove emits pure Z motion tagged MICRO_LIFT") {
        const Segs segs = zMove(2400, ax, zFeed(), zAccel());
        CHECK(segs.size() > 1); // ramped, not one slam (H3)
        for (const MicroSegment& m : segs) {
            CHECK(m.flags == motion::MICRO_LIFT);
            CHECK(m.dx == 0);
            CHECK(m.dy == 0);
            CHECK(m.da == 0);
        }
    }

    SUBCASE("travelJog emits pure XY motion tagged MICRO_JOG") {
        const Segs segs = travelJog(0, 0, 3200, 1600, ax, 0.5, 80);
        CHECK(segs.size() > 0);
        for (const MicroSegment& m : segs) {
            CHECK(m.flags == motion::MICRO_JOG);
            CHECK(m.dz == 0);
            CHECK(m.da == 0);
        }
    }
}

TEST_CASE("choreograph INVARIANT: axis inversion") {
    const ResolvedAxes& ax = machine::axes();
    const ResolvedAxes flipZ = patched([](ResolvedAxes& a) { a.z.invert = !a.z.invert; });
    const ResolvedAxes flipA = patched([](ResolvedAxes& a) { a.a.invert = !a.a.invert; });
    const ResolvedAxes flipX = patched([](ResolvedAxes& a) { a.x.invert = !a.x.invert; });
    const ResolvedAxes flipY = patched([](ResolvedAxes& a) { a.y.invert = !a.y.invert; });

    SUBCASE("flipping z.invert negates every emitted dz and changes nothing else") {
        const Segs a = zMove(2400, ax, zFeed(), zAccel());
        const Segs b = zMove(2400, flipZ, zFeed(), zAccel());
        REQUIRE(b.size() == a.size());
        for (size_t i = 0; i < a.size(); i++) {
            CHECK(b[i].dz == -a[i].dz);
            CHECK(b[i].interval == a[i].interval);
        }
    }

    SUBCASE("flipping a.invert negates every emitted da and changes nothing else") {
        const Segs A = aMove(2325, ax, NO_SLEW);
        const Segs B = aMove(2325, flipA, NO_SLEW);
        REQUIRE(B.size() == A.size());
        for (size_t i = 0; i < A.size(); i++) {
            CHECK(B[i].da == -A[i].da);
            CHECK(B[i].interval == A[i].interval);
        }
    }

    SUBCASE("flipping x.invert negates dx only; y.invert negates dy only") {
        const Delta base = sum(travelJog(0, 0, 3200, 1600, ax, 0.5, 80));
        const Delta fx = sum(travelJog(0, 0, 3200, 1600, flipX, 0.5, 80));
        const Delta fy = sum(travelJog(0, 0, 3200, 1600, flipY, 0.5, 80));
        CHECK(fx.dx == -base.dx);
        CHECK(fx.dy == base.dy);
        CHECK(fy.dx == base.dx);
        CHECK(fy.dy == -base.dy);
    }

    SUBCASE("inversion is presentation only — aMove's step count is unchanged") {
        forEachSize([&flipA](double n) {
            const double t = sumAbsA(aMove(n, flipA, NO_SLEW));
            return t == n ? "" : "N=" + fmt(n, 0) + ": " + fmt(t, 0);
        });
    }
}

TEST_CASE("choreograph INVARIANT: the no-op cases produce nothing") {
    const ResolvedAxes& ax = machine::axes();

    SUBCASE("aMove(0) is empty") {
        CHECK(aMove(0, ax, NO_SLEW).empty());
    }

    SUBCASE("aMove rounds toward zero: |da| < 1 emits nothing") {
        CHECK(aMove(0.7, ax, NO_SLEW).empty());
        CHECK(aMove(-0.7, ax, NO_SLEW).empty());
    }

    SUBCASE("zStepCount is 0 for a non-positive lift") {
        CHECK(zStepCount(0, ax) == 0);
        CHECK(zStepCount(-1, ax) == 0);
        CHECK(zStepCount(2.0, ax) == 2400);
    }

    SUBCASE("travelJog emits nothing when the rounded position does not change") {
        CHECK(travelJog(100, 100, 100, 100, ax, 0.5, 80).empty());
        CHECK(travelJog(100.1, 100.1, 100.3, 100.3, ax, 0.5, 80).empty());
    }

    SUBCASE("aMoveTo returns nothing when already at target") {
        const double at = motion::jsRound(90 * ax.a.stepsPerUnit);
        const AMoveResult r = aMoveTo(90, at, ax, NO_SLEW);
        CHECK(r.segments.empty());
        CHECK(r.newAPhys == at);
    }

    SUBCASE("headOffsetJog emits nothing for identical heads") {
        CHECK(headOffsetJog(-50, 0, -50, 0, ax, 0.5, 80).empty());
    }

    SUBCASE("headOffsetJog emits nothing when the offset delta rounds below one step") {
        CHECK(headOffsetJog(0, 0, 0.001, 0.001, ax, 0.5, 80).empty());
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// CONTRACT PROPERTIES
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("choreograph CONTRACT: preOrient's two modes") {
    const ResolvedAxes& ax = machine::axes();
    const double spu = ax.a.stepsPerUnit;

    SUBCASE("a non-tangential tool is never pre-oriented") {
        const AMoveResult r = preOrient(90, 0, 500, ax, PEN_TAN, PEN_UNWIND, NO_SLEW);
        CHECK(r.segments.empty());
        CHECK(r.newAPhys == 500);
    }

    SUBCASE("unwind targets an ABSOLUTE angle regardless of where A is") {
        const double target = motion::jsRound(90 * spu);
        for (double from : {0.0, 4650.0, -4650.0, 18600.0}) {
            CHECK(preOrient(90, 0, from, ax, KNIFE_TAN, KNIFE_UNWIND, NO_SLEW).newAPhys == target);
        }
    }

    SUBCASE("unwind bounds |aPhys| — A cannot wind away over many paths") {
        // Whatever the tool did while cutting, the next path re-datums A to the
        // entry tangent. |aPhys| after preOrient is bounded by 180*spu.
        const double bound = 180 * spu + 1;
        double phys = 0;
        for (double entry : {10.0, -170.0, 175.0, -5.0, 90.0, -90.0, 179.0, -179.0}) {
            phys = preOrient(entry, 0, phys, ax, KNIFE_TAN, KNIFE_UNWIND, NO_SLEW).newAPhys;
            CHECK(std::fabs(phys) <= bound);
            phys += 6000; // simulate a path that winds A hard while cutting
        }
    }

    SUBCASE("non-unwind rotates by the SHORTEST delta, never the long way") {
        // 170 -> -170 is +20 degrees, not -340.
        const AMoveResult r = preOrient(-170, 170, 0, ax, CREASE_TAN, CREASE_UNWIND, NO_SLEW);
        CHECK(closeTo(std::fabs(r.newAPhys), 20 * spu, 0));
    }

    SUBCASE("non-unwind rotates in the right DIRECTION, not merely the right amount") {
        // Not in the TypeScript, and the reason it is here: every other
        // non-unwind assertion in that file takes an absolute value, so
        // reversing the rotation is invisible to all of them. Swapping
        // angleDelta's two arguments — which negates it, since angleDelta(a,b)
        // is the rotation FROM a TO b — survived the whole ported suite. A
        // crease tool would have turned the wrong way on every path.
        //
        // Same shape as C3 and D6: the quantity under audit was measured
        // through an operation that destroys the thing being asserted.
        CHECK(preOrient(-170, 170, 0, ax, CREASE_TAN, CREASE_UNWIND, NO_SLEW).newAPhys ==
              motion::jsRound(20 * spu)); // +20, not -20
        CHECK(preOrient(170, -170, 0, ax, CREASE_TAN, CREASE_UNWIND, NO_SLEW).newAPhys ==
              motion::jsRound(-20 * spu)); // and the mirror image
        // ordinary, no wrap involved: 0 -> 45 must be a positive rotation
        CHECK(preOrient(45, 0, 0, ax, CREASE_TAN, CREASE_UNWIND, NO_SLEW).newAPhys > 0);
        CHECK(preOrient(-45, 0, 0, ax, CREASE_TAN, CREASE_UNWIND, NO_SLEW).newAPhys < 0);
    }

    SUBCASE("non-unwind never rotates more than 180 degrees") {
        for (double cur = -180; cur <= 180; cur += 17) {
            for (double entry = -180; entry <= 180; entry += 23) {
                const AMoveResult r =
                    preOrient(entry, cur, 0, ax, CREASE_TAN, CREASE_UNWIND, NO_SLEW);
                CHECK(std::fabs(r.newAPhys) <= 180 * spu + 1);
            }
        }
    }

    SUBCASE("non-unwind ignores accumulated aPhys; unwind consumes it") {
        const AMoveResult freeR = preOrient(90, 0, 9999, ax, CREASE_TAN, CREASE_UNWIND, NO_SLEW);
        const AMoveResult wired = preOrient(90, 0, 9999, ax, KNIFE_TAN, KNIFE_UNWIND, NO_SLEW);
        CHECK(freeR.newAPhys - 9999 ==
              preOrient(90, 0, 0, ax, CREASE_TAN, CREASE_UNWIND, NO_SLEW).newAPhys);
        CHECK(wired.newAPhys == motion::jsRound(90 * spu));
    }
}

TEST_CASE("choreograph CONTRACT: pivot ordering") {
    const ResolvedAxes& ax = machine::axes();

    SUBCASE("lift happens before the rotation and lower after it") {
        const Segs segs = pivot(1000, true, 2400, ax, zFeed(), zAccel(), NO_SLEW);
        std::vector<size_t> zIdx, aIdx;
        for (size_t i = 0; i < segs.size(); i++) {
            if (segs[i].dz != 0) zIdx.push_back(i);
            if (segs[i].da != 0) aIdx.push_back(i);
        }
        // Z is a ramp now, so "the lift" is many segments, not one. What the
        // caller relies on is the ORDER: all Z before the turn, all Z after it,
        // and none interleaved.
        REQUIRE(zIdx.size() >= 2);
        REQUIRE(aIdx.size() > 0);
        CHECK(zIdx.front() < aIdx.front());
        CHECK(zIdx.back() > aIdx.back());
        for (size_t i : zIdx) CHECK((i < aIdx.front() || i > aIdx.back()));
    }

    SUBCASE("no Z motion is emitted when lift is false") {
        for (const MicroSegment& s : pivot(1000, false, 0, ax, zFeed(), zAccel(), NO_SLEW)) {
            CHECK(s.dz == 0);
        }
    }

    SUBCASE("pivot's A motion is exactly aMove's") {
        Segs p;
        for (const MicroSegment& s : pivot(1000, true, 2400, ax, zFeed(), zAccel(), NO_SLEW)) {
            if (s.da != 0) p.push_back(s);
        }
        const Segs a = aMove(1000, ax, NO_SLEW);
        REQUIRE(p.size() == a.size());
        for (size_t i = 0; i < p.size(); i++) {
            CHECK(p[i].da == a[i].da);
            CHECK(p[i].interval == a[i].interval);
            CHECK(p[i].flags == a[i].flags);
        }
    }

    SUBCASE("a zero-rotation pivot with lift still lifts and lowers (and nothing else)") {
        const Segs segs = pivot(0, true, 2400, ax, zFeed(), zAccel(), NO_SLEW);
        REQUIRE(segs.size() >= 2);
        for (const MicroSegment& s : segs) CHECK(s.da == 0);
        // the lift and the lower are mirror ramps: they cancel, and together
        // they move exactly twice the lift height
        CHECK(sum(segs).dz == 0);
        double absZ = 0;
        for (const MicroSegment& s : segs) absZ += std::fabs(s.dz);
        CHECK(absZ == 2 * 2400);
    }
}

TEST_CASE("choreograph CONTRACT: travelJog and headOffsetJog geometry") {
    const ResolvedAxes& ax = machine::axes();

    SUBCASE("travelJog moves the rounded step delta, both signs") {
        const double cases[][4] = {
            {0, 0, 3200, 1600}, {3200, 1600, 0, 0}, {-500, 250, 500, -250},
        };
        for (const auto& c : cases) {
            const Delta m = sum(travelJog(c[0], c[1], c[2], c[3], ax, 0.5, 80));
            const double dx = motion::jsRound(c[2]) - motion::jsRound(c[0]);
            const double dy = motion::jsRound(c[3]) - motion::jsRound(c[1]);
            CHECK(m.dx == (ax.x.invert ? -dx : dx));
            CHECK(m.dy == (ax.y.invert ? -dy : dy));
        }
    }

    SUBCASE("travelJog rounds each endpoint rather than truncating") {
        // trunc would lose a step whenever the two endpoints straddle .5 the
        // same way, and the loss would accumulate across a whole job.
        CHECK(std::fabs(sum(travelJog(0.6, 0.6, 10.6, 10.6, ax, 0.5, 80)).dx) == 10);
        CHECK(std::fabs(sum(travelJog(0.4, 0.4, 10.6, 10.6, ax, 0.5, 80)).dx) == 11);
    }

    SUBCASE("travelJog is antisymmetric: there and back cancels") {
        const Delta there = sum(travelJog(0, 0, 3200, 1600, ax, 0.5, 80));
        const Delta back = sum(travelJog(3200, 1600, 0, 0, ax, 0.5, 80));
        CHECK(there.dx + back.dx == 0);
        CHECK(there.dy + back.dy == 0);
    }

    SUBCASE("headOffsetJog moves by (to - from), so the new head lands where the old was") {
        const Delta m = sum(headOffsetJog(-50, 0, 50, 10, ax, 0.5, 80));
        const double dx = motion::jsRound(100 * ax.x.stepsPerUnit);
        const double dy = motion::jsRound(10 * ax.y.stepsPerUnit);
        CHECK(m.dx == (ax.x.invert ? -dx : dx));
        CHECK(m.dy == (ax.y.invert ? -dy : dy));
    }

    SUBCASE("a head offset that changes only one axis still jogs") {
        // Not in the TypeScript: both of its headOffsetJog fixtures move BOTH
        // axes, so narrowing the "nothing to do" guard from && to || — which
        // discards any single-axis offset change — survived the ported suite.
        // A revolver whose two heads differ only in X would silently not
        // compensate, and every cut from it would be offset by the difference.
        CHECK(sum(headOffsetJog(-50, 0, 50, 0, ax, 0.5, 80)).dx != 0);
        CHECK(sum(headOffsetJog(0, -20, 0, 20, ax, 0.5, 80)).dy != 0);
    }

    SUBCASE("headOffsetJog is antisymmetric") {
        const Delta there = sum(headOffsetJog(-50, 3, 50, 10, ax, 0.5, 80));
        const Delta back = sum(headOffsetJog(50, 10, -50, 3, ax, 0.5, 80));
        CHECK(there.dx + back.dx == 0);
        CHECK(there.dy + back.dy == 0);
    }
}

TEST_CASE("choreograph CONTRACT: emitted timing matches the requested feed") {
    const ResolvedAxes& ax = machine::axes();

    SUBCASE("zMove takes at least liftHeight / zFeed seconds, plus its ramps") {
        // Was an equality against the constant-velocity ideal. Z ramps now
        // (H3), so the ideal is a FLOOR rather than a target: the move cannot
        // be faster than running the whole distance at feed, and the ramp
        // overhead is a fixed cost that shrinks as a fraction of a longer lift.
        const double cases[][3] = {{2, 10, 0.5}, {5, 10, 0.25}, {20, 10, 0.07}};
        for (const auto& c : cases) {
            const double steps = zStepCount(c[0], ax);
            const double seconds = totalSeconds(slices(zMove(steps, ax, c[1], zAccel())));
            const double ideal = c[0] / c[1];
            CHECK(seconds >= ideal * 0.999);
            CHECK(seconds / ideal - 1 < c[2]);
        }
    }

    SUBCASE("zMove ramps against the Z accel ceiling and comes back to rest") {
        // H3's actual content now that it is fixed: H1a and H1b, asked of Z.
        const std::vector<Slice> sl = slices(zMove(zStepCount(5, ax), ax, zFeed(), zAccel()));
        REQUIRE(sl.size() > 1);
        const double limit = zAccel() * ax.z.stepsPerUnit;
        CHECK(worstAccelRatio(sl, limit) <= Z_ACCEL_TOL);
        CHECK(closeTo(sl.back().v, sl.front().v, 0));      // symmetric
        CHECK(sl.back().v / sl.back().dt / limit <= 1.0);  // can stop in its last chunk
    }

    SUBCASE("travelJog takes distance / jogFeed seconds, plus its ramps") {
        // A ramped jog cannot be FASTER than the constant-feed ideal, and the
        // ramp overhead is a fixed time cost, so it shrinks as a fraction of a
        // longer move. Both halves matter: the first says the feed is still
        // respected as a ceiling, the second says ramping did not quietly
        // double the duration of ordinary travel.
        const double cases[][3] = {{200, 80, 0.05}, {50, 80, 0.2}, {200, 40, 0.03}};
        for (const auto& c : cases) {
            const double steps = c[0] * ax.x.stepsPerUnit;
            const double seconds = totalSeconds(slices(travelJog(0, 0, steps, 0, ax, 0.5, c[1])));
            const double ideal = c[0] / c[1];
            CHECK(seconds >= ideal * 0.999);
            CHECK(seconds / ideal - 1 < c[2]);
        }
    }

    SUBCASE("zStepCount rounds rather than truncates") {
        CHECK(zStepCount(1.7005, ax) == motion::jsRound(1.7005 * 1200)); // 2041, not 2040
    }

    SUBCASE("zMove's interval stays in range at absurd feeds") {
        // Both extremes still have to land inside the wire's expressible range.
        // The high end is now also clamped to the axis (below), but the range
        // property is what the SERIALISER depends on and it is worth stating
        // separately from the clamp — a future uncapped axis would skip the
        // clamp and must still not emit interval 0 or fCpu+1.
        for (double feed : {1e9, 1e-9, 0.5, 10.0}) {
            for (const MicroSegment& m : zMove(100, ax, feed, zAccel())) {
                CHECK(m.interval >= 1);
                CHECK(m.interval <= ax.fCpu);
            }
        }
    }

    SUBCASE("zMove CLAMPS an over-ceiling feed instead of honouring it") {
        // The config ships machine.z.feed = 20 against a z.maxFeed of 10, and
        // validate.ts has always warned that the excess is "(clamped)" — which
        // was not true until now: zMove divided by whatever it was handed and
        // never consulted the axis. Unlike the cutting path it does not go
        // through interval(), so the per-axis rate floor never saw it either.
        const std::vector<Slice> atCeiling = slices(zMove(1200, ax, zFeed(), zAccel()));
        const std::vector<Slice> asked = slices(zMove(1200, ax, 1e9, zAccel()));
        REQUIRE(!asked.empty());
        CHECK(closeTo(peakV(asked), peakV(atCeiling), 6));
        CHECK(peakV(asked) / ax.z.stepsPerUnit <= zFeed() * 1.001);
        // And the accel ceiling is clamped by the same rule. Stated as an
        // EQUALITY against the at-ceiling emission, not as an accel bound: an
        // unclamped 1e9 mm/s^2 collapses the ramp to a single chunk, and
        // worstAccelRatio over one segment has no boundaries to measure, so it
        // returns 0 and passes any bound vacuously — the same empty-measurement
        // trap the stage-7 subdivision test hit.
        const Segs fast = zMove(1200, ax, zFeed(), 1e9);
        const Segs atAccel = zMove(1200, ax, zFeed(), zAccel());
        REQUIRE(fast.size() == atAccel.size());
        REQUIRE(fast.size() > 1);
        for (size_t i = 0; i < fast.size(); i++) {
            CHECK(fast[i].dz == atAccel[i].dz);
            CHECK(fast[i].interval == atAccel[i].interval);
        }
    }

    SUBCASE("zMove rounds toward zero: |dz| < 1 emits nothing") {
        // Mirrors aMove's guard. Without it a fractional dz would produce a
        // ramp of zero steps; with it the caller gets an honest no-op.
        CHECK(zMove(0.7, ax, zFeed(), zAccel()).empty());
        CHECK(zMove(-0.7, ax, zFeed(), zAccel()).empty());
        CHECK(zMove(0, ax, zFeed(), zAccel()).empty());
    }

    SUBCASE("zMove refuses an undeclared accel rather than inventing one") {
        // Same policy as aMove (H4): a limit that is ABSENT is refused, one that
        // is present and exceeded is clamped. The two are different questions,
        // and the difference is whether the machine supplied a number at all.
        const ResolvedAxes noAccel = patched([](ResolvedAxes& a) { a.z.maxAccel = 0; });
        std::string msg;
        try {
            zMove(1200, noAccel, zFeed(), 0);
        } catch (const std::exception& e) {
            msg = e.what();
        }
        CHECK(msg.find("no accel limit") != std::string::npos);
    }

    SUBCASE("a long aMove actually reaches the A feed ceiling") {
        // Not just "stays under" — the cruise phase must BE the ceiling, or the
        // axis is being driven far below what the machine can do.
        CHECK(peakV(slices(aMove(18600, ax, NO_SLEW))) > aCruise() * 0.99);
    }

    SUBCASE("aMove's total time is not far ABOVE the analytic trapezoid either") {
        // Pairs with the lower bound below: together they pin the ramp to the
        // real limits, so a ramp built from the wrong units cannot pass.
        const double v0 = std::fmin(aCruise(), 50);
        forEachSize([v0](double n) {
            const double t = totalSeconds(slices(aMove(n, machine::axes(), NO_SLEW)));
            const double dAcc = (aCruise() * aCruise() - v0 * v0) / (2 * aAccel());
            const double ideal =
                2 * dAcc > n
                    ? (2 * (std::sqrt(v0 * v0 + aAccel() * n) - v0)) / aAccel()
                    : (2 * (aCruise() - v0)) / aAccel() + (n - 2 * dAcc) / aCruise();
            // 1.35 accommodates the coarsest case (N=52 runs in 5 chunks, at
            // 1.32x): chunk quantisation genuinely costs time on tiny moves. A
            // ramp built from the wrong units lands far outside this.
            return t <= ideal * 1.35
                       ? std::string()
                       : "N=" + fmt(n, 0) + ": " + fmt(t, 4) + "s vs ideal " + fmt(ideal, 4) + "s";
        });
    }

    SUBCASE("aMove's peak respects the triangular clamp on short moves") {
        // A move too short to reach cruise may only accelerate for half its
        // length, or it cannot stop in the other half.
        const double v0 = std::fmin(aCruise(), 50);
        forEachSize([v0](double n) {
            const double peak = peakV(slices(aMove(n, machine::axes(), NO_SLEW)));
            const double reachable = std::sqrt(v0 * v0 + 2 * aAccel() * (n / 2));
            return peak <= std::fmin(aCruise(), reachable) * 1.02
                       ? std::string()
                       : "N=" + fmt(n, 0) + ": peak " + fmt(peak, 0) +
                             " exceeds half-length reachable " + fmt(reachable, 0);
        });
    }

    SUBCASE("aMove decelerates at all — it does not end at its peak rate") {
        // Companion to H1b: this asks only whether a ramp-down EXISTS, not
        // whether it is steep enough. It used to be scoped to n >= 129, because
        // the shortest rotation did not ramp down at all (H1c); that exemption
        // is gone.
        forEachSize([](double n) {
            const std::vector<Slice> sl = slices(aMove(n, machine::axes(), NO_SLEW));
            if (sl.empty()) return "N=" + fmt(n, 0) + ": emitted nothing";
            const double peak = peakV(sl);
            const double vEnd = sl.back().v;
            return vEnd < peak * 0.95
                       ? std::string()
                       : "N=" + fmt(n, 0) + ": ends at " + fmt(vEnd, 0) + " of peak " + fmt(peak, 0);
        });
    }

    SUBCASE("aMove's segment count is bounded by the ramp, not by the steps") {
        // One segment per step would be correct motion and ruinous bandwidth: a
        // 360 degree turn is 18600 steps but must not be 18600 wire segments.
        // The bound is FLAT — two ramps of RAMP_CHUNKS pieces plus one cruise
        // piece — where it used to grow with the move. A full turn costs 33.
        forEachSize([](double n) {
            const size_t count = aMove(n, machine::axes(), NO_SLEW).size();
            return count <= 33 ? std::string()
                               : "N=" + fmt(n, 0) + ": " + std::to_string(count) + " segments (>33)";
        });

        // The bound above is ONE-SIDED, which is the C3 shape: halving
        // RAMP_CHUNKS makes every ramp coarser and passes it comfortably. The
        // accel property cannot see that either — it is scale-invariant, since
        // a chunk twice as long carries twice the speed change over twice the
        // time — so nothing in the TypeScript suite pins ramp FIDELITY at all.
        //
        // Pin it from below, on a move long enough to be a full trapezoid. This
        // is a bandwidth-vs-smoothness choice rather than a correctness one, so
        // the point is that changing it has to be deliberate.
        CHECK(aMove(18600, ax, NO_SLEW).size() >= 30);
    }

    SUBCASE("aMove never commands the A axis above its feed ceiling") {
        forEachSize([](double n) {
            const double peak = peakV(slices(aMove(n, machine::axes(), NO_SLEW)));
            return peak <= aCruise() * 1.001
                       ? std::string()
                       : "N=" + fmt(n, 0) + ": peak " + fmt(peak, 0) + " > ceiling " +
                             fmt(aCruise(), 0);
        });
    }

    SUBCASE("aMove's total time is at least the analytic trapezoid time") {
        // Quantisation may only ever make the move SLOWER than the ideal ramp.
        const double v0 = std::fmin(aCruise(), 50);
        forEachSize([v0](double n) {
            const double t = totalSeconds(slices(aMove(n, machine::axes(), NO_SLEW)));
            const double dAcc = (aCruise() * aCruise() - v0 * v0) / (2 * aAccel());
            const double ideal =
                2 * dAcc > n
                    ? (2 * (std::sqrt(v0 * v0 + aAccel() * n) - v0)) / aAccel()
                    : (2 * (aCruise() - v0)) / aAccel() + (n - 2 * dAcc) / aCruise();
            return t >= ideal * 0.999
                       ? std::string()
                       : "N=" + fmt(n, 0) + ": " + fmt(t, 4) + "s < ideal " + fmt(ideal, 4) + "s";
        });
    }
}

// ── the findings: each names the one it pins ─────────────────────────────────

TEST_CASE("choreograph CONTRACT: acceleration limits (FINDINGS)") {
    const ResolvedAxes& ax = machine::axes();

    SUBCASE("H1a (FIXED): both of aMove's ramps respect the A accel ceiling") {
        // Was: the accel ramp was fine (<=0.88x) and the decel ramp overshot on
        // every size, because `v` was sampled at each chunk's START — the
        // SLOWEST point of an accelerating chunk (conservative) and the FASTEST
        // of a decelerating one (anti-conservative). One line, opposite sign on
        // the two halves of the same move.
        //
        // Now each chunk's interval comes from the exact constant-accel time
        // across it, so the demand is the accel limit itself at every boundary.
        // The bound is 1.0 and the measurement lands ON it, not under it: that
        // is the design — the ramp is meant to use the whole ceiling. A slack
        // bound here would stop pinning anything.
        forEachSize([](double n) {
            const RampRatios r = rampRatios(slices(aMove(n, machine::axes(), NO_SLEW)), aAccel());
            const double worst = std::fmax(r.up, r.down);
            return worst <= 1.001
                       ? std::string()
                       : "N=" + fmt(n, 0) + ": demands " + fmt(worst) + "x the A accel limit";
        });
    }

    SUBCASE("H1b (FIXED): aMove can come to rest within its final chunk") {
        // Was: the move stopped dead from 8.85-39.36 deg/s, having never reached
        // its designed terminal velocity of 0.97 deg/s.
        //
        // Terminal velocity is not directly readable from the stream — the last
        // chunk's rate is its MEAN, and the profile's true end speed is v0. So
        // the property to assert is the one that matters physically: whatever
        // rate the final chunk commands, the axis must be able to reach zero
        // from it within that chunk's own duration.
        forEachSize([](double n) {
            const std::vector<Slice> sl = slices(aMove(n, machine::axes(), NO_SLEW));
            if (sl.empty()) return "N=" + fmt(n, 0) + ": emitted nothing";
            const Slice& last = sl.back();
            const double demand = last.v / last.dt / aAccel();
            return demand <= 1.0
                       ? std::string()
                       : "N=" + fmt(n, 0) + ": stopping from " +
                             fmt(last.v / machine::axes().a.stepsPerUnit) + " deg/s in " +
                             fmt(last.dt * 1000) + "ms demands " + fmt(demand) + "x the limit";
        });
    }

    SUBCASE("H1c (FIXED): the shortest rotations ramp down as well as up") {
        // H1's cause at its most vivid. A 1 degree pivot (N=52) used to run in
        // five chunks whose speeds only ever went UP (50 -> 457 -> 1018 -> 1761
        // -> 2034), ending at its own peak: the "decel" chunk was faster than
        // the cruise chunk before it, because the decel rate was read at the
        // chunk's start where the remaining distance is greatest.
        const std::vector<Slice> sl = slices(aMove(52, ax, NO_SLEW));
        REQUIRE(sl.size() > 2);
        const size_t iPeak = peakIndex(sl);
        CHECK(sl.back().v < peakV(sl));
        CHECK(iPeak < sl.size() - 1); // peaks before the end
        CHECK(iPeak > 0);             // and after the start
        // and the profile is symmetric: it comes back down to where it started
        CHECK(closeTo(sl.back().v, sl.front().v, 0));
    }

    SUBCASE("H2 (FIXED): travelJog ramps to its feed instead of stepping straight to it") {
        // Was: one segment at full jogFeed — 0 -> 80 mm/s in zero distance,
        // against a configured x.maxAccel of 1000 mm/s^2 that needed 3.2mm of
        // ramp. Now the jog is a trapezoid like any other move, so it must open
        // slow and never demand more than the axis has.
        for (double mm : {5.0, 20.0, 200.0}) {
            const std::vector<Slice> sl =
                slices(travelJog(0, 0, mm * ax.x.stepsPerUnit, 0, ax, 0.5, 80));
            // REQUIRE, not CHECK: doctest's CHECK continues after a failure, so
            // an emitter that returns nothing would reach sl.front() below and
            // take the whole runner down with a segfault — reporting nothing
            // about the other 68 cases. A mutation that skipped every chunk of a
            // pure-X jog did exactly that.
            REQUIRE(sl.size() > 1);
            const double limit = ax.x.maxAccel * ax.x.stepsPerUnit; // steps/s^2
            // It must open well below feed — but not at v0 itself: the first
            // chunk's rate is its mean, and one step at this accel already
            // carries the axis well past its junction speed.
            CHECK(sl.front().v / ax.x.stepsPerUnit < 80.0 / 4);
            CHECK(closeTo(sl.back().v, sl.front().v, 0)); // symmetric
            CHECK(worstAccelRatio(sl, limit) <= 1.001);
        }
    }

    SUBCASE("H2 (FIXED): the tighter of the two XY axes owns the jog's ramp") {
        // The fixture gives x and y the same maxAccel, so a jog cannot tell min
        // from max there. Skew them: a diagonal move must ramp against the
        // WEAKER axis, and must not get faster when only the stronger one rises.
        const ResolvedAxes weakY = patched([](ResolvedAxes& a) { a.y.maxAccel = 100; });
        const std::vector<Slice> sl =
            slices(travelJog(0, 0, 16000, 16000, weakY, 0.5, 80), weakY);
        const double secs = totalSeconds(sl);
        // measured against the weak axis's own ceiling, the ramp is legal
        CHECK(worstAccelRatio(sl, 100 * weakY.y.stepsPerUnit) <= 1.001);
        // and it is genuinely slower than the same jog on the stiff machine
        CHECK(secs > totalSeconds(slices(travelJog(0, 0, 16000, 16000, ax, 0.5, 80))));
        // raising only the stronger axis must change nothing
        const ResolvedAxes strongX = patched([](ResolvedAxes& a) {
            a.x.maxAccel = 100000;
            a.y.maxAccel = 100;
        });
        CHECK(closeTo(totalSeconds(slices(travelJog(0, 0, 16000, 16000, strongX, 0.5, 80), strongX)),
                      secs, 6));
    }

    SUBCASE("H2 (FIXED): a ramped jog still travels in a straight line") {
        // Ramping splits one segment into ~33, so the two axes are now stepped
        // in pieces and could stair-step off the diagonal. Each chunk's
        // cumulative position must stay on the ideal line to within a step.
        const double dx = 16000;
        const double dy = 7000;
        double cx = 0, cy = 0, worst = 0;
        for (const MicroSegment& s : travelJog(0, 0, dx, dy, ax, 0.5, 80)) {
            cx += ax.x.invert ? -s.dx : s.dx;
            cy += ax.y.invert ? -s.dy : s.dy;
            worst = std::fmax(worst, std::fabs(cy - (cx * dy) / dx));
        }
        CHECK(worst <= 1);
        CHECK(cx == dx); // and it lands exactly
        CHECK(cy == dy);
    }

    SUBCASE("H2 (FIXED): headOffsetJog ramps too — it is the same emitter") {
        const std::vector<Slice> sl = slices(headOffsetJog(-50, 0, 50, 10, ax, 0.5, 80));
        REQUIRE(sl.size() > 1);
        CHECK(worstAccelRatio(sl, ax.x.maxAccel * ax.x.stepsPerUnit) <= 1.001);
    }

    SUBCASE("H3 (FIXED): zMove ramps instead of slamming to zFeed") {
        // Was: one segment opening at 24000 steps/s — 0 to 20 mm/s in zero
        // distance, and 20 mm/s was itself twice the axis's declared 10 mm/s
        // ceiling. The same defect H2 fixed for travel jogs and H1 for A; Z was
        // last because z.maxAccel was a 0 placeholder and the module refused to
        // invent one (H4). It is now a declared, provisional 300 mm/s^2.
        //
        // Asserted the way H2 is: it must open well below feed, not at it.
        const Segs segs = zMove(zStepCount(2, ax), ax, zFeed(), zAccel());
        REQUIRE(segs.size() > 1);
        const std::vector<Slice> sl = slices(segs);
        CHECK(sl.front().v / ax.z.stepsPerUnit < zFeed() / 2);
        CHECK(worstAccelRatio(sl, zAccel() * ax.z.stepsPerUnit) <= Z_ACCEL_TOL);
    }
}

TEST_CASE("choreograph CONTRACT: fallbacks") {
    /** Run `fn` and return the exception text, or "" if it did not throw. */
    auto threw = [](const std::function<void()>& fn) -> std::string {
        try {
            fn();
        } catch (const std::exception& e) {
            return std::string(e.what());
        }
        return std::string();
    };

    SUBCASE("H4: aMove refuses to invent A limits for an under-specified machine") {
        // load.ts refuses to invent stepsPerUnit/invert/node because guessing
        // calibration is how you crash a machine. aMove used to invent
        // 180 deg/s and 2000 deg/s^2 when the A ceilings were 0 ("uncapped"),
        // silently — which made an undeclared axis run 1.8x FASTER than the real
        // machine's declared 100 deg/s. Same class of number, same policy now.
        const ResolvedAxes none = patched([](ResolvedAxes& a) {
            a.a.maxFeed = 0;
            a.a.maxAccel = 0;
        });
        const ResolvedAxes noFeed = patched([](ResolvedAxes& a) { a.a.maxFeed = 0; });
        const ResolvedAxes noAccel = patched([](ResolvedAxes& a) { a.a.maxAccel = 0; });

        const std::string m1 = threw([&none] { aMove(4650, none, NO_SLEW); });
        const std::string m2 = threw([&noFeed] { aMove(4650, noFeed, NO_SLEW); });
        const std::string m3 = threw([&noAccel] { aMove(4650, noAccel, NO_SLEW); });
        CHECK(m1.find("no feed and accel limit") != std::string::npos);
        CHECK(m2.find("no feed limit") != std::string::npos);
        CHECK(m3.find("no accel limit") != std::string::npos);
    }

    SUBCASE("H4: an explicit slew target satisfies an otherwise uncapped A axis") {
        // "Uncapped" is refused for lack of a number, not as a policy against
        // the axis — supplying the number by any route is enough.
        const ResolvedAxes uncapped = patched([](ResolvedAxes& a) {
            a.a.maxFeed = 0;
            a.a.maxAccel = 0;
        });
        OpTarget slew;
        slew.hasFeed = true;
        slew.feed = 100;
        slew.hasAccel = true;
        slew.accel = 500;
        CHECK(sumAbsA(aMove(4650, uncapped, slew)) == 4650);
    }

    SUBCASE("H4: a zero rotation on an uncapped axis is still a no-op, not a throw") {
        // Nothing to rotate needs no limits. Keeps an absent A axis (which
        // load.ts builds with 0 ceilings) from throwing on a no-op call.
        const ResolvedAxes uncapped = patched([](ResolvedAxes& a) {
            a.a.maxFeed = 0;
            a.a.maxAccel = 0;
        });
        CHECK(aMove(0, uncapped, NO_SLEW).empty());
    }
}
