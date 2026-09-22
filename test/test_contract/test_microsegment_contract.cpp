/**
 * Contract tests for microsegment — the MicroSegment record, the flag
 * constants, and interval() — ported from
 * web/test/wire/format/microsegment.test.ts.
 *
 * Scope note. The TypeScript file has 16 tests; 12 port and 4 do not. The four
 * that stay behind test the 26-byte wire SERIALISER, which is not in the port:
 * the C++ side of the wire lives in the firmware, and packing a MicroSegment to
 * bytes is that code's job, not this module's. What crosses is the record shape,
 * the flag numbering, and the timing function — the three things discretize and
 * choreograph both depend on and neither of them owns.
 *
 * The flag-constant tests look like tautologies and are not. These values are a
 * SHARED namespace with the firmware (docs/wire_protocol.md): the numbers are
 * the contract, not an implementation detail, and MICRO_JOG in particular must
 * never be 0x04 or every travel move aliases onto PAUSE. Pinning them here is
 * what makes a renumbering a test failure instead of a silent protocol break.
 */

#include "doctest.h"

#include "motion/microsegment.h"
#include "support/machine.h"
#include "support/quality.h"

#include <cmath>
#include <vector>

using motion::MicroSegment;
using motion::interval;
using motion::microSegment;

namespace {

/** The bench machine with a single A-axis feed ceiling swapped out. */
motion::ResolvedAxes withAFeed(double maxFeed) {
    motion::ResolvedAxes ax = machine::axes();
    ax.a.maxFeed = maxFeed;
    return ax;
}

} // namespace

TEST_CASE("microsegment: the record") {
    SUBCASE("microSegment() builds the 6-field record") {
        const MicroSegment m = microSegment(10, -5, 0, 3, 1500, motion::MICRO_JOG);
        CHECK(m.dx == 10);
        CHECK(m.dy == -5);
        CHECK(m.dz == 0);
        CHECK(m.da == 3);
        CHECK(m.interval == 1500);
        CHECK(m.flags == motion::MICRO_JOG);
    }

    SUBCASE("flags default to 0") {
        CHECK(microSegment(1, 2, 3, 4, 100).flags == 0u);
    }
}

TEST_CASE("microsegment: flag constants are the wire's numbering") {
    // Shared with the firmware. A change here is a protocol change.
    CHECK(motion::MICRO_PATH_END == 0x01u);
    CHECK(motion::MICRO_LIFT == 0x08u);
    CHECK(motion::MICRO_JOG == 0x10u);

    // Not in the TypeScript, and the reason the numbering is worth pinning at
    // all: JOG must not collide with PAUSE, or the firmware halts on every
    // travel move. Stated as the disjointness it actually needs, so a future
    // renumbering that preserves the property still passes.
    CHECK((motion::MICRO_JOG & motion::MICRO_PAUSE) == 0u);
}

TEST_CASE("microsegment: interval()") {
    const motion::ResolvedAxes& ax = machine::axes();
    const double vMin = quality::V_MIN;

    SUBCASE("pure-X move — no hypotenuse correction (hypot == major)") {
        // dx=160 steps = 1mm, v=80mm/s -> stepRate = 80*160 = 12800
        // segTime = max(1/80, 160/(80*160)) = 0.0125s
        // cycles = 0.0125/160 * 150e6 = 11718
        CHECK(interval(80, ax, vMin, 160, 0, 0, 0) == 11718);
    }

    SUBCASE("diagonal — hypotenuse correction makes interval longer than pure-X") {
        // dx=160, dy=160 -> distMm = sqrt(2) ~ 1.414mm, major=160
        // segTime = sqrt(2)/80 ~ 0.01768s (feed dominates the 0.0125s rate floor)
        // cycles = 0.01768/160 * 150e6 ~ 16572
        const double pureX = interval(80, ax, vMin, 160, 0, 0, 0);
        const double diag = interval(80, ax, vMin, 160, 160, 0, 0);
        CHECK(diag > pureX);
        CHECK(diag == 16572);
    }

    SUBCASE("pure-A rotation — no XY feed, rate floor from the A axis") {
        // distMm=0, so the A rate floor is the only governor:
        // tRate = 100/(100*51.667) ~ 0.01935s; cycles = tRate/100 * 150e6 ~ 29031
        const double iv = interval(80, ax, vMin, 0, 0, 0, 100);
        CHECK(iv > 0);
        CHECK(iv < ax.fCpu);
        // A is slower than X, so the same step count must take longer.
        CHECK(iv > interval(80, ax, vMin, 160, 0, 0, 0));
    }

    SUBCASE("per-axis rate limit — the A rate floor bites on tight rotation") {
        // A higher maxFeed permits a shorter interval.
        const double ivFast = interval(80, withAFeed(1000), vMin, 0, 0, 0, 200);
        const double ivSlow = interval(80, withAFeed(50), vMin, 0, 0, 0, 200);
        CHECK(ivFast < ivSlow);
    }

    SUBCASE("v=0 uses the vMin floor") {
        // vv = vMin = 0.5; distMm = 1mm, segTime = 2.0s
        // cycles = 2.0/160 * 150e6 = 1875000
        CHECK(interval(0, ax, vMin, 160, 0, 0, 0) == 1875000);
    }

    SUBCASE("major=0 (all deltas zero) returns fCpu") {
        CHECK(interval(80, ax, vMin, 0, 0, 0, 0) == ax.fCpu);
    }

    SUBCASE("the three-argument overload governs the major axis directly") {
        // No geometry to correct against: stepRate = 80*160 = 12800,
        // fCpu/stepRate = 11718. Same number as the pure-X case, for a
        // different reason — there the hypotenuse simply equalled the major leg.
        CHECK(interval(80, ax, vMin) == 11718);
    }

    SUBCASE("the three-argument overload floors at vMin too") {
        // Not in the TypeScript, which only ever exercises the legacy path at
        // full feed. Dropping the vMin floor from this overload therefore
        // changed nothing any test could see — yet at v=0 it is the difference
        // between a 2-second segment and a stalled one: without the floor the
        // step rate is 0, the guard in majorRate fires, and the caller gets a
        // one-second interval that has nothing to do with the requested motion.
        //
        // Same number as the geometry-aware overload at v=0, and for the same
        // reason: both floor first and divide second.
        CHECK(interval(0, ax, vMin) == 1875000);
        CHECK(interval(0, ax, vMin) == interval(vMin, ax, vMin));
    }

    SUBCASE("interval never exceeds fCpu") {
        for (double v : {0.0, 0.5, 1.0, 10.0, 80.0, 1000.0}) {
            CHECK(interval(v, ax, vMin, 160, 160, 100, 50) <= ax.fCpu);
        }
    }

    SUBCASE("interval is always >= 1") {
        for (double v : {0.0, 0.5, 1.0, 10.0, 80.0, 1000.0, 1e6}) {
            CHECK(interval(v, ax, vMin, 1, 1, 1, 1) >= 1);
        }
    }

    SUBCASE("interval is always an integer") {
        // Not in the TypeScript. It matters because the value goes on the wire
        // as an integer cycle count: a fractional interval would be truncated
        // by the serialiser rather than rejected, so the timing error would be
        // silent. Both overloads, across the range where the trunc/min/max
        // chain has different branches active.
        for (double v : {0.0, 0.5, 10.0, 80.0, 1e6}) {
            const double a = interval(v, ax, vMin);
            const double b = interval(v, ax, vMin, 160, 37, 0, 11);
            CHECK(std::floor(a) == a);
            CHECK(std::floor(b) == b);
        }
    }
}
