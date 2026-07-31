/**
 * curves.h — the shared curve fixtures, ported from
 * web/test/toolpath/curves.cases.ts.
 *
 * Same registry, same names, same control points. Keeping the names identical
 * matters more than it looks: when a C++ contract test and its TypeScript
 * original both fail on `near_cusp`, that is one finding, not two.
 *
 * CUSP is deliberately OUTSIDE the registry, exactly as in the TypeScript —
 * four test files iterate the registry, and a cusp in it would change several
 * stages' expectations at once, so a cusp regression could not be attributed to
 * one stage. Stages opt in by naming CUSP directly.
 */

#ifndef TEST_MOTION_CURVES_H
#define TEST_MOTION_CURVES_H

#include "motion/geometry.h"

#include <string>
#include <utility>
#include <vector>

namespace curves {

using motion::CubicBezier;
using motion::KAPPA;
using motion::Pt;

// motion::cubic already exists and is the same constructor; reuse it rather
// than shadowing, so the fixtures are built the way the library builds curves.
using motion::cubic;
inline Pt pt(double x, double y) { return Pt{x, y}; }

inline const std::vector<CubicBezier>& straightLine() {
    static const std::vector<CubicBezier> v = {
        cubic(pt(0, 0), pt(33.333, 0), pt(66.667, 0), pt(100, 0)),
    };
    return v;
}

inline const std::vector<CubicBezier>& quarterCircleR50() {
    static const double R = 50.0, H = 50.0 * KAPPA;
    static const std::vector<CubicBezier> v = {
        cubic(pt(R, 0), pt(R, H), pt(H, R), pt(0, R)),
    };
    return v;
}

inline const std::vector<CubicBezier>& quarterCircleR5() {
    static const double R = 5.0, H = 5.0 * KAPPA;
    static const std::vector<CubicBezier> v = {
        cubic(pt(R, 0), pt(R, H), pt(H, R), pt(0, R)),
    };
    return v;
}

inline const std::vector<CubicBezier>& sCurve() {
    static const std::vector<CubicBezier> v = {
        cubic(pt(0, 0), pt(20, 40), pt(40, 40), pt(60, 0)),
        cubic(pt(60, 0), pt(80, -40), pt(100, -40), pt(120, 0)),
    };
    return v;
}

inline const std::vector<CubicBezier>& shortCurve() {
    static const std::vector<CubicBezier> v = {
        cubic(pt(0, 0), pt(0.333, 0), pt(0.667, 0), pt(1.0, 0)),
    };
    return v;
}

inline const std::vector<CubicBezier>& longGentleArc() {
    static const std::vector<CubicBezier> v = {
        cubic(pt(0, 0), pt(166.667, 50), pt(333.333, 50), pt(500, 0)),
    };
    return v;
}

inline const std::vector<CubicBezier>& nearCusp() {
    static const std::vector<CubicBezier> v = {
        cubic(pt(0, 0), pt(40, 0), pt(41, 1), pt(1, 1)),
    };
    return v;
}

inline const std::vector<CubicBezier>& fullCircleR30() {
    static const double R = 30.0, H = 30.0 * KAPPA;
    static const std::vector<CubicBezier> v = {
        cubic(pt(R, 0), pt(R, H), pt(H, R), pt(0, R)),
        cubic(pt(0, R), pt(-H, R), pt(-R, H), pt(-R, 0)),
        cubic(pt(-R, 0), pt(-R, -H), pt(-H, -R), pt(0, -R)),
        cubic(pt(0, -R), pt(H, -R), pt(R, -H), pt(R, 0)),
    };
    return v;
}

/**
 * B'(0.5) == 0 exactly: the tangent REVERSES through a point of zero speed.
 * The geometry that breaks flatten's tangent cap (audit F1).
 */
inline const std::vector<CubicBezier>& cusp() {
    static const std::vector<CubicBezier> v = {
        cubic(pt(0, 0), pt(10, 0), pt(0, 5), pt(10, -5)),
    };
    return v;
}

using Case = std::pair<std::string, const std::vector<CubicBezier>*>;

/** The registry, in the TypeScript's declaration order. */
inline const std::vector<Case>& cases() {
    static const std::vector<Case> v = {
        {"straight_line", &straightLine()},
        {"quarter_circle_r50", &quarterCircleR50()},
        {"quarter_circle_r5", &quarterCircleR5()},
        {"s_curve", &sCurve()},
        {"short_curve", &shortCurve()},
        {"long_gentle_arc", &longGentleArc()},
        {"near_cusp", &nearCusp()},
        {"full_circle_r30", &fullCircleR30()},
    };
    return v;
}

/** The registry plus the cusp — what plan's tests iterate (GEOMETRY_CASES). */
inline const std::vector<Case>& casesWithCusp() {
    static const std::vector<Case> v = [] {
        std::vector<Case> t = cases();
        t.push_back({"cusp", &cusp()});
        return t;
    }();
    return v;
}

} // namespace curves

#endif // TEST_MOTION_CURVES_H
