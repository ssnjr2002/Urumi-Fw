/**
 * test_flatten.cpp — differential test of motion::flatten against the TypeScript.
 *
 * Bit-equality, no epsilon, for the same reason as test_geometry.cpp. This one
 * matters more: flatten decides HOW MANY samples exist, so a divergence here is
 * not a rounding difference in a field — it changes the length of everything
 * downstream and moves every byte of the golden.
 *
 * Regenerate the vectors with:
 *     cd web && GEN_CPP_REF=1 npx vitest run test/port/cppRefFlatten
 */

#include <doctest.h>

#include "motion/flatten.h"

#include "bits.h"

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
    FlattenOptions opts{};
    std::vector<std::vector<CubicBezier>> subpaths;
    std::vector<Sample> expected;
};

double nextHex(std::istringstream& ls) {
    std::string tok;
    ls >> tok;
    return fromHex(tok);
}

} // namespace

TEST_CASE("flatten is bit-identical to the TypeScript reference") {
    std::ifstream f = testbits::openRef("flatten_ref.txt");
    REQUIRE_MESSAGE(f.good(),
                    "flatten_ref.txt not found — regenerate with "
                    "`cd web && GEN_CPP_REF=1 npx vitest run test/port/cppRefFlatten`");

    std::vector<RefCase> cases;
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
            FlattenOptions& o = cases.back().opts;
            o.chordTol = nextHex(ls);
            o.dsMax = nextHex(ls);
            o.dthetaMax = nextHex(ls);
            o.dtMax = nextHex(ls);
            o.dtMin = nextHex(ls);
            ls >> o.maxRefine;
        } else if (kind == "sp") {
            size_t n = 0;
            ls >> n;
            std::vector<CubicBezier> sp;
            sp.reserve(n);
            for (size_t k = 0; k < n; k++) {
                CubicBezier b{};
                b.p0.x = nextHex(ls); b.p0.y = nextHex(ls);
                b.p1.x = nextHex(ls); b.p1.y = nextHex(ls);
                b.p2.x = nextHex(ls); b.p2.y = nextHex(ls);
                b.p3.x = nextHex(ls); b.p3.y = nextHex(ls);
                sp.push_back(b);
            }
            cases.back().subpaths.push_back(sp);
        } else if (kind == "s") {
            Sample s{};
            s.x = nextHex(ls);
            s.y = nextHex(ls);
            s.theta = nextHex(ls);
            s.kappa = nextHex(ls);
            s.ds = nextHex(ls);
            ls >> s.flags;
            cases.back().expected.push_back(s);
        }
        // "n" and "end" carry no state the parser needs.
    }

    REQUIRE(cases.size() >= 20);

    size_t totalSamples = 0;
    for (const RefCase& c : cases) {
        CAPTURE(c.name);
        const std::vector<Sample> got = flatten(c.subpaths, c.opts);

        // Length first, and as a hard stop: a count mismatch means the stepping
        // loop diverged, and every field diff after it would be noise.
        REQUIRE_MESSAGE(got.size() == c.expected.size(),
                        c.name << ": sample count cpp=" << got.size()
                               << " ts=" << c.expected.size());

        for (size_t i = 0; i < got.size(); i++) {
            const Sample& a = got[i];
            const Sample& b = c.expected[i];
            const std::string names[] = {"x", "y", "theta", "kappa", "ds"};
            const double av[] = {a.x, a.y, a.theta, a.kappa, a.ds};
            const double bv[] = {b.x, b.y, b.theta, b.kappa, b.ds};
            for (int k = 0; k < 5; k++) {
                REQUIRE_MESSAGE(sameBits(av[k], bv[k]),
                                c.name << "[" << i << "]." << names[k]
                                       << "  cpp=" << toHex(av[k])
                                       << "  ts=" << toHex(bv[k]));
            }
            REQUIRE_MESSAGE(a.flags == b.flags,
                            c.name << "[" << i << "].flags  cpp=" << a.flags
                                   << "  ts=" << b.flags);
        }
        totalSamples += got.size();
    }

    MESSAGE("checked " << cases.size() << " cases, " << totalSamples << " samples");
    // Guards against a truncated reference file making the whole test vacuous.
    REQUIRE(totalSamples > 6000);
}
