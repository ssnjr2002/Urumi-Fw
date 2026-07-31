/**
 * test_geometry.cpp — differential test of lib/motion against the TypeScript.
 *
 * The port's contract is BIT-EQUALITY with web/src/toolpath/geometry.ts, not
 * closeness (docs/planner_audit.md, "Numeric porting rule"). So this reads the
 * reference vectors generated from the TypeScript itself and compares raw
 * IEEE-754 bit patterns — no epsilon anywhere in this file, deliberately. An
 * epsilon here would hide exactly the transcription slips it exists to catch:
 * a reassociated polynomial, an integer 2/3, hypot swapped for sqrt.
 *
 * Regenerate the vectors with:
 *     cd web && GEN_CPP_REF=1 npx vitest run test/port/cppRef
 *
 * A failure prints the case's inputs, so it can be reproduced in either
 * language directly.
 */

#include <doctest.h>

#include "motion/geometry.h"
#include "motion/jsmath.h"

#include "support/bits.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace motion;
using testbits::fromHex;
using testbits::toHex;
using testbits::sameBits;

namespace {


struct Case {
    std::string fn;
    std::vector<double> in;
    std::vector<double> out;
};

CubicBezier curveFrom(const std::vector<double>& v) {
    return CubicBezier{Pt{v[0], v[1]}, Pt{v[2], v[3]}, Pt{v[4], v[5]}, Pt{v[6], v[7]}};
}

/** Compute a case's outputs, or return false if the fn name is unknown. */
bool evaluate(const Case& c, std::vector<double>& got) {
    const std::vector<double>& i = c.in;
    got.clear();
    if (c.fn == "bezierPoint") {
        Pt p = bezierPoint(curveFrom(i), i[8]);
        got = {p.x, p.y};
    } else if (c.fn == "bezierDeriv1") {
        Pt p = bezierDeriv1(curveFrom(i), i[8]);
        got = {p.x, p.y};
    } else if (c.fn == "bezierDeriv2") {
        Pt p = bezierDeriv2(curveFrom(i), i[8]);
        got = {p.x, p.y};
    } else if (c.fn == "curvature") {
        got = {curvature(curveFrom(i), i[8])};
    } else if (c.fn == "exitTangent") {
        Pt p = exitTangent(curveFrom(i));
        got = {p.x, p.y};
    } else if (c.fn == "entryTangent") {
        Pt p = entryTangent(curveFrom(i));
        got = {p.x, p.y};
    } else if (c.fn == "length") {
        got = {length(Pt{i[0], i[1]})};
    } else if (c.fn == "normalize") {
        Pt p = normalize(Pt{i[0], i[1]});
        got = {p.x, p.y};
    } else if (c.fn == "angleBetweenDeg") {
        got = {angleBetweenDeg(Pt{i[0], i[1]}, Pt{i[2], i[3]})};
    } else if (c.fn == "angleDelta") {
        got = {angleDelta(i[0], i[1])};
    } else if (c.fn == "jsRound") {
        got = {jsRound(i[0])};
    } else if (c.fn == "jsAtan2") {
        got = {jsAtan2(i[0], i[1])};
    } else if (c.fn == "jsAcos") {
        got = {jsAcos(i[0])};
    } else if (c.fn == "jsHypot") {
        got = {jsHypot(i[0], i[1])};
    } else if (c.fn == "lineToCubic") {
        CubicBezier b = lineToCubic(Pt{i[0], i[1]}, Pt{i[2], i[3]});
        got = {b.p0.x, b.p0.y, b.p1.x, b.p1.y, b.p2.x, b.p2.y, b.p3.x, b.p3.y};
    } else if (c.fn == "quadToCubic") {
        CubicBezier b = quadToCubic(Pt{i[0], i[1]}, Pt{i[2], i[3]}, Pt{i[4], i[5]});
        got = {b.p0.x, b.p0.y, b.p1.x, b.p1.y, b.p2.x, b.p2.y, b.p3.x, b.p3.y};
    } else {
        return false;
    }
    return true;
}

std::string describe(const Case& c) {
    std::string s = c.fn + "(";
    for (size_t k = 0; k < c.in.size(); k++) {
        if (k) s += ", ";
        s += toHex(c.in[k]);
    }
    return s + ")";
}

} // namespace

TEST_CASE("geometry is bit-identical to the TypeScript reference") {
    std::ifstream f = testbits::openRef("geometry_ref.txt");
    REQUIRE_MESSAGE(f.good(),
                    "geometry_ref.txt not found — regenerate with "
                    "`cd web && GEN_CPP_REF=1 npx vitest run test/port/cppRef`");

    std::string line;
    int checked = 0, unknown = 0;
    std::vector<double> got;

    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line);
        Case c;
        size_t n = 0;
        std::string tok;
        ls >> c.fn >> n;
        for (size_t k = 0; k < n; k++) { ls >> tok; c.in.push_back(fromHex(tok)); }
        ls >> n;
        for (size_t k = 0; k < n; k++) { ls >> tok; c.out.push_back(fromHex(tok)); }

        if (!evaluate(c, got)) { unknown++; continue; }

        REQUIRE_MESSAGE(got.size() == c.out.size(), describe(c) << " arity");
        for (size_t k = 0; k < got.size(); k++) {
            // Not CHECK: one reassociated expression fails thousands of cases,
            // and the first failure is the informative one.
            REQUIRE_MESSAGE(sameBits(got[k], c.out[k]),
                            describe(c) << " out[" << k << "]  cpp=" << toHex(got[k])
                                        << "  ts=" << toHex(c.out[k]));
        }
        checked++;
    }

    MESSAGE("checked " << checked << " reference cases");
    // A silently-empty reference file would make this whole test vacuous.
    REQUIRE(checked > 700);
    REQUIRE(unknown == 0);
}
