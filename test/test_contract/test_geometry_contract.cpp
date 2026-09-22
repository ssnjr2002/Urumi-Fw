/**
 * test_geometry_contract.cpp — CONTRACT tests for geometry.h, ported from
 * web/test/toolpath/geometry.test.ts.
 *
 * 28 assertions' worth of "does the vector algebra and the Bezier math mean what
 * it says", stated as closed forms and identities rather than as pinned values.
 * Companion to test_parity/test_geometry.cpp, which asks the entirely different
 * question of whether the bits match V8.
 *
 * Why closed forms matter more here than anywhere else in the port: geometry is
 * the bottom of the stack, so a sign error or a dropped factor of 3 in
 * bezierDeriv1 does not fail here — it fails four stages later as a velocity
 * that is wrong by an amount nobody can attribute. Every test below states the
 * derivative or the identity independently (B'(0) = 3(p1-p0), B''(0) =
 * 6(p2-2p1+p0)) rather than calling the function twice.
 */

#include <doctest.h>

#include "motion/geometry.h"

#include <cmath>
#include <initializer_list>

using namespace motion;

namespace {

/** The TypeScript's approxPt, same default tolerance. */
bool approxPt(Pt a, Pt b, double tol = 1e-9) {
    return std::fabs(a.x - b.x) < tol && std::fabs(a.y - b.y) < tol;
}

Pt p(double x, double y) { return Pt{x, y}; }

} // namespace

// ── construction ─────────────────────────────────────────────────────────────

TEST_CASE("geometry: construction") {
    SUBCASE("cubic() builds the 4-point record") {
        const CubicBezier c = cubic(p(0, 0), p(1, 2), p(3, 4), p(5, 6));
        CHECK(approxPt(c.p0, p(0, 0)));
        CHECK(approxPt(c.p3, p(5, 6)));
    }

    SUBCASE("lineToCubic() puts control points at 1/3 and 2/3") {
        const CubicBezier c = lineToCubic(p(0, 0), p(9, 0));
        CHECK(approxPt(c.p1, p(3, 0)));
        CHECK(approxPt(c.p2, p(6, 0)));
    }

    SUBCASE("KAPPA is the quarter-circle approximation constant") {
        CHECK(std::fabs(KAPPA - 0.5522847498) < 1e-10);
    }
}

// ── vector algebra ───────────────────────────────────────────────────────────

TEST_CASE("geometry: vector algebra") {
    SUBCASE("sub / add / scale") {
        CHECK(approxPt(sub(p(5, 7), p(2, 1)), p(3, 6)));
        CHECK(approxPt(add(p(5, 7), p(2, 1)), p(7, 8)));
        CHECK(approxPt(scale(p(3, 4), 2), p(6, 8)));
    }

    SUBCASE("length of a 3-4-5 vector is 5") {
        // Exact, not approximate: 3, 4 and 5 are all representable and hypot is
        // correctly rounded here, so an epsilon would only hide a bad formula.
        CHECK(length(p(3, 4)) == 5.0);
    }

    SUBCASE("normalize: unit vector of (3,4) is (0.6, 0.8)") {
        CHECK(approxPt(normalize(p(3, 4)), p(0.6, 0.8)));
    }

    SUBCASE("normalize: near-zero vector returns (0,0)") {
        // The guard that stops a degenerate control polygon becoming NaN and
        // travelling silently into theta.
        CHECK(approxPt(normalize(p(1e-13, 0)), p(0, 0)));
        CHECK(approxPt(normalize(p(0, 0)), p(0, 0)));
    }

    SUBCASE("angleBetweenDeg: parallel = 0") {
        CHECK(std::fabs(angleBetweenDeg(p(1, 0), p(2, 0)) - 0.0) < 1e-6);
    }

    SUBCASE("angleBetweenDeg: anti-parallel = 180") {
        CHECK(std::fabs(angleBetweenDeg(p(1, 0), p(-1, 0)) - 180.0) < 1e-6);
    }

    SUBCASE("angleBetweenDeg: perpendicular = 90") {
        CHECK(std::fabs(angleBetweenDeg(p(1, 0), p(0, 1)) - 90.0) < 1e-6);
    }

    SUBCASE("angleBetweenDeg: clamps dot to [-1, 1] for near-parallel floats") {
        // u dot u can land a hair above 1 in floating point, and acos(1+eps) is
        // NaN. The TypeScript asserts "does not throw"; in C++ the failure mode
        // is quieter and worse — a NaN that propagates — so assert finiteness.
        const Pt u = normalize(p(3, 4));
        const double a = angleBetweenDeg(u, u);
        CHECK(std::isfinite(a));
        CHECK(std::fabs(a - 0.0) < 1e-6);
    }
}

// ── endpoint tangents ────────────────────────────────────────────────────────

TEST_CASE("geometry: endpoint tangents") {
    SUBCASE("exitTangent of a rightward curve = (1, 0)") {
        const CubicBezier c = cubic(p(0, 0), p(5, 0), p(8, 0), p(10, 0));
        CHECK(approxPt(exitTangent(c), p(1, 0)));
    }

    SUBCASE("entryTangent of an upward curve = (0, 1)") {
        const CubicBezier c = cubic(p(0, 0), p(0, 5), p(0, 8), p(0, 10));
        CHECK(approxPt(entryTangent(c), p(0, 1)));
    }

    SUBCASE("endpoint tangents are unit vectors") {
        const CubicBezier c = cubic(p(1, 2), p(4, 6), p(8, 5), p(12, 9));
        CHECK(std::fabs(length(exitTangent(c)) - 1.0) < 1e-9);
        CHECK(std::fabs(length(entryTangent(c)) - 1.0) < 1e-9);
    }

    SUBCASE("degenerate (p2==p3) exitTangent returns (0,0)") {
        const CubicBezier c = cubic(p(0, 0), p(1, 0), p(5, 5), p(5, 5));
        CHECK(approxPt(exitTangent(c), p(0, 0)));
    }
}

// ── bezierPoint ──────────────────────────────────────────────────────────────

TEST_CASE("geometry: bezierPoint") {
    const CubicBezier c = cubic(p(0, 0), p(1, 2), p(3, 4), p(5, 6));

    SUBCASE("B(0) == p0") { CHECK(approxPt(bezierPoint(c, 0), c.p0)); }
    SUBCASE("B(1) == p3") { CHECK(approxPt(bezierPoint(c, 1), c.p3)); }

    SUBCASE("B(0.5) is the curve midpoint") {
        // De Casteljau by hand on (0,0)(1,2)(3,4)(5,6):
        //   mid01=(0.5,1) mid12=(2,3) mid23=(4,5)
        //   mid012=(1.25,2) mid123=(3,4)  ->  B(0.5) = (2.125, 3)
        CHECK(approxPt(bezierPoint(c, 0.5), p(2.125, 3)));
    }

    SUBCASE("straight line cubic: B(0.5) is the linear midpoint") {
        CHECK(approxPt(bezierPoint(lineToCubic(p(0, 0), p(10, 0)), 0.5), p(5, 0)));
    }
}

// ── derivatives ──────────────────────────────────────────────────────────────

TEST_CASE("geometry: bezierDeriv1") {
    const CubicBezier c = cubic(p(0, 0), p(1, 2), p(3, 4), p(5, 6));

    SUBCASE("B'(0) = 3*(p1 - p0)") {
        CHECK(approxPt(bezierDeriv1(c, 0), p(3, 6)));
    }

    SUBCASE("B'(1) = 3*(p3 - p2)") {
        CHECK(approxPt(bezierDeriv1(c, 1), p(6, 6)));
    }

    SUBCASE("B'(t) of a straight-line cubic is constant = p3 - p0") {
        // lineToCubic makes all three control differences equal to (p3-p0)/3, so
        // B'(t) = (p3-p0)*(mt+t)^2 = (p3-p0) for every t. Checked at several t
        // rather than one, because "constant" is the claim.
        const CubicBezier line = lineToCubic(p(0, 0), p(10, 0));
        for (const double t : {0.0, 0.25, 0.5, 0.75, 1.0}) {
            CAPTURE(t);
            CHECK(approxPt(bezierDeriv1(line, t), p(10, 0)));
        }
    }
}

TEST_CASE("geometry: bezierDeriv2") {
    SUBCASE("B''(t) of a straight-line cubic is zero") {
        CHECK(approxPt(bezierDeriv2(lineToCubic(p(0, 0), p(10, 0)), 0.5), p(0, 0)));
    }

    SUBCASE("B''(0) = 6*(p2 - 2*p1 + p0)") {
        // 6 * (3 - 2*1 + 0) = 6.  This is the term flatten's chord-deviation cap
        // divides by, so a wrong factor here shows up as a sampling density that
        // is wrong by that factor and nothing else complains.
        CHECK(approxPt(bezierDeriv2(cubic(p(0, 0), p(1, 0), p(3, 0), p(6, 0)), 0), p(6, 0)));
    }
}

// ── curvature ────────────────────────────────────────────────────────────────

TEST_CASE("geometry: curvature") {
    SUBCASE("straight line cubic: curvature ~ 0 everywhere") {
        const CubicBezier line = lineToCubic(p(0, 0), p(10, 0));
        for (const double t : {0.0, 0.25, 0.5, 0.75, 1.0}) {
            CAPTURE(t);
            CHECK(std::fabs(curvature(line, t)) < 1e-9);
        }
    }

    SUBCASE("curvature is non-negative") {
        const CubicBezier c = cubic(p(0, 0), p(1, 2), p(3, 4), p(5, 6));
        for (const double t : {0.0, 0.25, 0.5, 0.75, 1.0}) {
            CAPTURE(t);
            CHECK(curvature(c, t) >= 0.0);
        }
    }

    SUBCASE("circular-arc cubic at t=0.5 matches 1/r") {
        // Unit-circle quarter arc, so kappa should be ~1 at the midpoint. The
        // KAPPA cubic only approximates a true arc; the ~0.6% error is the
        // approximation's, not the math's — hence the loose band.
        const CubicBezier quarter = cubic(p(1, 0), p(1, KAPPA), p(KAPPA, 1), p(0, 1));
        CHECK(std::fabs(curvature(quarter, 0.5) - 1.0) < 0.05);
    }

    SUBCASE("curvature returns 0 for near-zero speed (degenerate)") {
        // p0=p1=p2=p3 -> |B'| = 0, and kappa is |B'xB''|/|B'|^3. Without the
        // guard this is 0/0. Exact 0 asserted, not approximate: the guard either
        // fires or the result is NaN, and there is nothing in between.
        const CubicBezier degenerate = cubic(p(5, 5), p(5, 5), p(5, 5), p(5, 5));
        CHECK(curvature(degenerate, 0.5) == 0.0);
    }
}
