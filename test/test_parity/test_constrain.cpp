/**
 * test_constrain.cpp — differential test of motion::constrain against the TS.
 *
 * Bit-equality, no epsilon, same as the other two. Constrain is where the
 * config tiers meet the geometry, so most of what can go wrong here is a
 * MISREAD OPTION rather than a misread formula — an optional treated as
 * present, a gate inverted, a disabled switch that still applies its term. The
 * fixtures toggle each switch independently for that reason.
 *
 * Regenerate the vectors with:
 *     cd web && GEN_CPP_REF=1 npx vitest run test/port/cppRefConstrain
 */

#include <doctest.h>

#include "motion/constrain.h"
#include "motion/jsmath.h"

#include "support/bits.h"

#include <sstream>
#include <string>
#include <vector>

using namespace motion;
using testbits::fromHex;
using testbits::sameBits;
using testbits::toHex;

namespace {

struct RefCase {
    std::string name;
    ConstrainOptions opts;
    std::vector<Sample> in;
    std::vector<double> expected;
};

double nextHex(std::istringstream& ls) {
    std::string tok;
    ls >> tok;
    return fromHex(tok);
}

} // namespace

TEST_CASE("constrain is bit-identical to the TypeScript reference") {
    std::ifstream f = testbits::openRef("constrain_ref.txt");
    REQUIRE_MESSAGE(f.is_open(),
                    "constrain_ref.txt not tracked in git; generate it with "
                    "`cd web && GEN_CPP_REF=1 npx vitest run test/port/cppRefConstrain`");

    std::vector<RefCase> cases;
    size_t fnChecked = 0;
    size_t fnUnknown = 0;
    std::string line;

    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line);
        std::string kind;
        ls >> kind;

        if (kind == "case") {
            RefCase c;
            ls >> c.name;
            cases.push_back(c);
        } else if (kind == "opts") {
            ConstrainOptions& o = cases.back().opts;
            o.feedMax = nextHex(ls);
            o.aMax = nextHex(ls);
            o.junctionDeviation = nextHex(ls);
            o.aRateDegS = nextHex(ls);
            o.aAccelDegS2 = nextHex(ls);

            std::string tok;
            ls >> tok; // cornerStopAngleDeg, or "-" for undefined
            o.hasCornerStopAngle = tok != "-";
            o.cornerStopAngleDeg = o.hasCornerStopAngle ? fromHex(tok) : 0.0;

            o.vMin = nextHex(ls);

            ls >> tok; // forcedStops, comma-separated, or "-"
            if (tok != "-") {
                std::istringstream fs(tok);
                std::string idx;
                while (std::getline(fs, idx, ',')) {
                    o.forcedStops.insert(static_cast<size_t>(std::stoull(idx)));
                }
            }
        } else if (kind == "i") {
            Sample s{};
            s.x = nextHex(ls);
            s.y = nextHex(ls);
            s.theta = nextHex(ls);
            s.kappa = nextHex(ls);
            s.ds = nextHex(ls);
            ls >> s.flags;
            cases.back().in.push_back(s);
        } else if (kind == "o") {
            cases.back().expected.push_back(nextHex(ls));
        } else if (kind == "junctionCap" || kind == "jsCos") {
            // Same <fn> <nIn> <in..> <nOut> <out..> shape as geometry_ref.txt.
            size_t nIn = 0;
            ls >> nIn;
            std::vector<double> in(nIn);
            for (size_t k = 0; k < nIn; k++) in[k] = nextHex(ls);
            size_t nOut = 0;
            ls >> nOut;
            std::vector<double> want(nOut);
            for (size_t k = 0; k < nOut; k++) want[k] = nextHex(ls);

            double got = 0;
            if (kind == "junctionCap") {
                REQUIRE(nIn == 4);
                got = junctionCap(in[0], in[1], in[2], in[3]);
            } else {
                REQUIRE(nIn == 1);
                got = jsCos(in[0]);
            }
            REQUIRE(nOut == 1);
            if (!sameBits(got, want[0])) {
                fnUnknown++; // counted so a flood does not print 6000 lines
                REQUIRE_MESSAGE(sameBits(got, want[0]),
                                kind << "(" << toHex(in[0]) << ") cpp=" << toHex(got)
                                     << " ts=" << toHex(want[0]));
            }
            fnChecked++;
        }
        // "n" and "end" carry no state the parser needs.
    }

    // Guards against a truncated or half-parsed reference making this vacuous.
    REQUIRE(cases.size() >= 60);
    REQUIRE(fnChecked > 6000);
    REQUIRE(fnUnknown == 0);

    size_t totalCeilings = 0;
    for (const RefCase& c : cases) {
        CAPTURE(c.name);
        const std::vector<ConstrainedSample> got = constrain(c.in, c.opts);

        REQUIRE_MESSAGE(got.size() == c.expected.size(),
                        c.name << ": count cpp=" << got.size()
                               << " ts=" << c.expected.size());

        for (size_t i = 0; i < got.size(); i++) {
            // The stage is pure: the Sample must come through untouched.
            REQUIRE_MESSAGE(sameBits(got[i].s.kappa, c.in[i].kappa),
                            c.name << "[" << i << "]: sample mutated");
            REQUIRE_MESSAGE(got[i].s.flags == c.in[i].flags,
                            c.name << "[" << i << "]: flags mutated");
            REQUIRE_MESSAGE(sameBits(got[i].vCeiling, c.expected[i]),
                            c.name << "[" << i << "].vCeiling  cpp="
                                   << toHex(got[i].vCeiling)
                                   << "  ts=" << toHex(c.expected[i]));
        }
        totalCeilings += got.size();
    }

    MESSAGE("checked " << cases.size() << " cases, " << totalCeilings
                       << " ceilings, " << fnChecked << " fn cases");
    REQUIRE(totalCeilings > 5000);
}
