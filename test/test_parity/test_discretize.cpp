/**
 * test_discretize.cpp — differential test of motion::discretize, and of the
 * choreograph and microsegment code it stands on, against the TypeScript.
 *
 * Bit-equality, no epsilon. The companion to test_discretize_contract.cpp:
 *
 *   this file                      catches numeric divergence, and nothing
 *                                  else. It will fail on any optimisation,
 *                                  correct or not.
 *   test_discretize_contract.cpp   catches semantic defects, and survives
 *                                  rewrites.
 *
 * Three record kinds, matching the generator: `interval` calls, `ramp` blocks
 * (rampChunks, choreograph's core), and `case` blocks (discretize itself, with
 * the input samples shared through `samples`/`use`).
 *
 * Stage 7 is the first whose output is integers rather than reals, which makes
 * a bit-comparison both easier to satisfy and easier to be complacent about: a
 * step delta agrees or it does not, and most of them will agree for reasons
 * that have nothing to do with the arithmetic being right. The `interval` and
 * `ramp` records exist because that is where the actual doubles are, and they
 * are compared at full precision.
 *
 * Regenerate the vectors with:
 *     cd web && GEN_CPP_REF=1 npx vitest run test/port/cppRefDiscretize
 */

#include <doctest.h>

#include "motion/choreograph.h"
#include "motion/discretize.h"
#include "motion/microsegment.h"

#include "support/bits.h"

#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace motion;
using testbits::fromHex;
using testbits::sameBits;
using testbits::toHex;

namespace {

double nextHex(std::istringstream& ls) {
    std::string tok;
    ls >> tok;
    return fromHex(tok);
}

struct RefCase {
    std::string name;
    std::string samplesKey;
    DiscretizeOptions opts;
    std::vector<MicroSegment> expected;
};

struct RefRamp {
    std::string name;
    double N = 0, v0 = 0, cruise = 0, accel = 0, fCpu = 0;
    std::vector<RampChunk> expected;
};

} // namespace

TEST_CASE("discretize is bit-identical to the TypeScript reference") {
    std::ifstream f = testbits::openRef("discretize_ref.txt");
    REQUIRE_MESSAGE(f.is_open(),
                    "discretize_ref.txt not tracked in git; generate it with "
                    "`cd web && GEN_CPP_REF=1 npx vitest run test/port/cppRefDiscretize`");

    std::map<std::string, std::vector<PlannedSample>> sampleSets;
    std::vector<RefCase> cases;
    std::vector<RefRamp> ramps;
    size_t fnChecked = 0;
    std::string line;

    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line);
        std::string kind;
        ls >> kind;

        if (kind == "interval") {
            // interval <nIn> v vMin dx dy dz da fCpu <nOut> expected
            int nIn = 0;
            ls >> nIn;
            const double v = nextHex(ls);
            const double vMin = nextHex(ls);
            const double dx = nextHex(ls);
            const double dy = nextHex(ls);
            const double dz = nextHex(ls);
            const double da = nextHex(ls);
            const double fCpu = nextHex(ls);
            int nOut = 0;
            ls >> nOut;
            const double want = nextHex(ls);

            // The axes an `interval` line runs against are the bench machine's,
            // which the generator does not repeat per line — only fCpu varies
            // and it is carried explicitly.
            ResolvedAxes ax;
            ax.x = AxisConfig{160.0, 80.0, 1000.0, true};
            ax.y = AxisConfig{160.0, 80.0, 1000.0, false};
            ax.z = AxisConfig{1200.0, 10.0, 0.0, true};
            ax.a = AxisConfig{51.667, 100.0, 2000.0, true};
            ax.fCpu = fCpu;

            const double got = interval(v, ax, vMin, dx, dy, dz, da);
            if (!sameBits(got, want)) {
                FAIL("interval(" << v << ", vMin=" << vMin << ", " << dx << "," << dy << ","
                                 << dz << "," << da << "): got " << toHex(got) << " want "
                                 << toHex(want));
            }
            fnChecked++;
        } else if (kind == "ramp") {
            RefRamp r;
            ls >> r.name;
            r.N = nextHex(ls);
            r.v0 = nextHex(ls);
            r.cruise = nextHex(ls);
            r.accel = nextHex(ls);
            r.fCpu = nextHex(ls);
            ramps.push_back(r);
        } else if (kind == "r") {
            RampChunk c;
            c.steps = nextHex(ls);
            c.interval = nextHex(ls);
            ramps.back().expected.push_back(c);
        } else if (kind == "samples") {
            std::string key;
            ls >> key;
            sampleSets[key] = {};
            // `n` and the `i` lines follow; the reader below keys off the last
            // opened set rather than re-parsing the header.
            std::string cur = key;
            while (std::getline(f, line)) {
                if (line == "end") break;
                std::istringstream is(line);
                std::string k;
                is >> k;
                if (k == "n") continue;
                if (k != "i") continue;
                PlannedSample p;
                p.s.x = nextHex(is);
                p.s.y = nextHex(is);
                p.s.theta = nextHex(is);
                p.s.kappa = nextHex(is);
                p.s.ds = nextHex(is);
                unsigned long fl = 0;
                is >> fl;
                p.s.flags = static_cast<uint32_t>(fl);
                p.vCeiling = nextHex(is);
                p.v = nextHex(is);
                sampleSets[cur].push_back(p);
            }
        } else if (kind == "case") {
            RefCase c;
            ls >> c.name;
            cases.push_back(c);
        } else if (kind == "use") {
            ls >> cases.back().samplesKey;
        } else if (kind == "axes") {
            ResolvedAxes& ax = cases.back().opts.axes;
            ax.x.stepsPerUnit = nextHex(ls);
            ax.x.maxFeed = nextHex(ls);
            ax.x.maxAccel = nextHex(ls);
            ax.y.stepsPerUnit = nextHex(ls);
            ax.y.maxFeed = nextHex(ls);
            ax.y.maxAccel = nextHex(ls);
            ax.z.stepsPerUnit = nextHex(ls);
            ax.z.maxFeed = nextHex(ls);
            ax.z.maxAccel = nextHex(ls);
            ax.a.stepsPerUnit = nextHex(ls);
            ax.a.maxFeed = nextHex(ls);
            ax.a.maxAccel = nextHex(ls);
            ax.fCpu = nextHex(ls);
        } else if (kind == "inv") {
            ResolvedAxes& ax = cases.back().opts.axes;
            int xi = 0, yi = 0, zi = 0, ai = 0;
            ls >> xi >> yi >> zi >> ai;
            ax.x.invert = xi != 0;
            ax.y.invert = yi != 0;
            ax.z.invert = zi != 0;
            ax.a.invert = ai != 0;
        } else if (kind == "tool") {
            DiscretizeOptions& o = cases.back().opts;
            int tan = 0, unw = 0;
            ls >> tan >> unw;
            o.tangential = tan != 0;
            o.unwind = unw != 0;
            o.cornerAngleDeg = nextHex(ls);
            o.offsetMm = nextHex(ls);
        } else if (kind == "trav") {
            DiscretizeOptions& o = cases.back().opts;
            o.dvMax = nextHex(ls);
            o.vMin = nextHex(ls);
            o.jogFeed = nextHex(ls);
            o.liftHeight = nextHex(ls);
            o.zFeed = nextHex(ls);
        } else if (kind == "slew") {
            OpTarget& s = cases.back().opts.slew;
            int hf = 0, ha = 0;
            ls >> hf;
            s.hasFeed = hf != 0;
            s.feed = nextHex(ls);
            ls >> ha;
            s.hasAccel = ha != 0;
            s.accel = nextHex(ls);
        } else if (kind == "o") {
            MicroSegment m;
            m.dx = nextHex(ls);
            m.dy = nextHex(ls);
            m.dz = nextHex(ls);
            m.da = nextHex(ls);
            m.interval = nextHex(ls);
            unsigned long fl = 0;
            ls >> fl;
            m.flags = static_cast<uint32_t>(fl);
            cases.back().expected.push_back(m);
        }
        // "end" and "n" outside a samples block need no action.
    }

    REQUIRE(fnChecked > 2000u);
    REQUIRE(ramps.size() > 400u);
    REQUIRE(cases.size() > 150u);

    // ── rampChunks ───────────────────────────────────────────────────────────
    for (const RefRamp& r : ramps) {
        const std::vector<RampChunk> got = rampChunks(r.N, r.v0, r.cruise, r.accel, r.fCpu);
        REQUIRE_MESSAGE(got.size() == r.expected.size(),
                        "ramp " << r.name << ": got " << got.size() << " chunks, want "
                                << r.expected.size());
        for (size_t i = 0; i < got.size(); i++) {
            if (!sameBits(got[i].steps, r.expected[i].steps) ||
                !sameBits(got[i].interval, r.expected[i].interval)) {
                FAIL("ramp " << r.name << " chunk " << i << ": got steps "
                             << toHex(got[i].steps) << " iv " << toHex(got[i].interval)
                             << " want steps " << toHex(r.expected[i].steps) << " iv "
                             << toHex(r.expected[i].interval));
            }
        }
    }

    // ── discretize ───────────────────────────────────────────────────────────
    size_t segments = 0;
    for (const RefCase& c : cases) {
        const auto it = sampleSets.find(c.samplesKey);
        REQUIRE_MESSAGE(it != sampleSets.end(),
                        "case " << c.name << " references unknown samples " << c.samplesKey);
        const std::vector<MicroSegment> got = discretize(it->second, c.opts);
        REQUIRE_MESSAGE(got.size() == c.expected.size(),
                        "case " << c.name << ": got " << got.size() << " segments, want "
                                << c.expected.size());
        for (size_t i = 0; i < got.size(); i++) {
            const MicroSegment& g = got[i];
            const MicroSegment& w = c.expected[i];
            if (!sameBits(g.dx, w.dx) || !sameBits(g.dy, w.dy) || !sameBits(g.dz, w.dz) ||
                !sameBits(g.da, w.da) || !sameBits(g.interval, w.interval) ||
                g.flags != w.flags) {
                FAIL("case " << c.name << " seg " << i << ":\n  got  dx=" << toHex(g.dx)
                             << " dy=" << toHex(g.dy) << " dz=" << toHex(g.dz)
                             << " da=" << toHex(g.da) << " iv=" << toHex(g.interval)
                             << " fl=" << g.flags << "\n  want dx=" << toHex(w.dx)
                             << " dy=" << toHex(w.dy) << " dz=" << toHex(w.dz)
                             << " da=" << toHex(w.da) << " iv=" << toHex(w.interval)
                             << " fl=" << w.flags);
            }
        }
        segments += got.size();
    }

    MESSAGE("discretize: " << cases.size() << " cases, " << segments << " segments, "
                           << ramps.size() << " ramps, " << fnChecked << " interval calls");
}
