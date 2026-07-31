/**
 * test_plan.cpp — differential test of motion::plan against the TypeScript.
 *
 * Bit-equality, no epsilon. The companion to test_plan_contract.cpp, and the
 * division of labour between them is deliberate:
 *
 *   this file                 catches numeric divergence, and nothing else. It
 *                             will fail on any optimisation, correct or not.
 *   test_plan_contract.cpp    catches semantic defects, and survives rewrites.
 *
 * Since the reorder (docs/port_workflow.md) the contract tests run first, so by
 * the time this file is consulted the stage is already known to plan feasible,
 * bounded, endpoint-pinned profiles. A failure here is therefore a rounding
 * question, not a physics one — which is exactly the diagnosis that used to
 * cost the most to establish.
 *
 * Regenerate the vectors with:
 *     cd web && GEN_CPP_REF=1 npx vitest run test/port/cppRefPlan
 */

#include <doctest.h>

#include "motion/plan.h"

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
    PlanOptions opts;
    std::vector<ConstrainedSample> in;
    std::vector<double> expected;
};

double nextHex(std::istringstream& ls) {
    std::string tok;
    ls >> tok;
    return fromHex(tok);
}

} // namespace

TEST_CASE("plan is bit-identical to the TypeScript reference") {
    std::ifstream f = testbits::openRef("plan_ref.txt");
    REQUIRE_MESSAGE(f.good(),
                    "plan_ref.txt not found — regenerate with "
                    "`cd web && GEN_CPP_REF=1 npx vitest run test/port/cppRefPlan`");

    std::vector<RefCase> cases;
    size_t fnChecked = 0;
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
            PlanOptions& o = cases.back().opts;
            o.xAccel = nextHex(ls);
            o.yAccel = nextHex(ls);
            o.aAccelDegS2 = nextHex(ls);
            o.aMax = nextHex(ls);
            o.pathAccel = nextHex(ls);
        } else if (kind == "i") {
            ConstrainedSample c{};
            c.s.x = nextHex(ls);
            c.s.y = nextHex(ls);
            c.s.theta = nextHex(ls);
            c.s.kappa = nextHex(ls);
            c.s.ds = nextHex(ls);
            ls >> c.s.flags;
            c.vCeiling = nextHex(ls);
            cases.back().in.push_back(c);
        } else if (kind == "o") {
            cases.back().expected.push_back(nextHex(ls));
        } else if (kind == "segAccel") {
            size_t nIn = 0;
            ls >> nIn;
            REQUIRE(nIn == 8);
            Sample s0{};
            Sample s1{};
            PlanOptions o{};
            s0.x = nextHex(ls); s0.y = nextHex(ls); s0.kappa = nextHex(ls);
            s1.x = nextHex(ls); s1.y = nextHex(ls); s1.kappa = nextHex(ls);
            o.xAccel = nextHex(ls); o.yAccel = nextHex(ls);
            size_t nMid = 0;
            ls >> nMid;
            REQUIRE(nMid == 3);
            o.aAccelDegS2 = nextHex(ls); o.aMax = nextHex(ls); o.pathAccel = nextHex(ls);
            size_t nOut = 0;
            ls >> nOut;
            REQUIRE(nOut == 1);
            const double want = nextHex(ls);
            const double got = segAccel(s0, s1, o);
            REQUIRE_MESSAGE(sameBits(got, want),
                            "segAccel cpp=" << toHex(got) << " ts=" << toHex(want));
            fnChecked++;
        }
    }

    // Guards against a truncated or half-parsed reference making this vacuous.
    REQUIRE(cases.size() >= 60);
    REQUIRE(fnChecked > 1400);

    size_t totalSpeeds = 0;
    for (const RefCase& c : cases) {
        CAPTURE(c.name);
        const std::vector<PlannedSample> got = plan(c.in, c.opts);

        REQUIRE_MESSAGE(got.size() == c.expected.size(),
                        c.name << ": count cpp=" << got.size()
                               << " ts=" << c.expected.size());

        for (size_t i = 0; i < got.size(); i++) {
            // The stage is pure: geometry and ceiling must come through intact.
            REQUIRE_MESSAGE(sameBits(got[i].vCeiling, c.in[i].vCeiling),
                            c.name << "[" << i << "]: ceiling mutated");
            REQUIRE_MESSAGE(sameBits(got[i].v, c.expected[i]),
                            c.name << "[" << i << "].v  cpp=" << toHex(got[i].v)
                                   << "  ts=" << toHex(c.expected[i]));
        }
        totalSpeeds += got.size();
    }

    MESSAGE("checked " << cases.size() << " cases, " << totalSpeeds
                       << " speeds, " << fnChecked << " segAccel cases");
    REQUIRE(totalSpeeds > 5000);
}
