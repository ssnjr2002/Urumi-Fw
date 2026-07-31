/**
 * svgfix.h — loads the real-artwork geometry exported by
 * web/test/port/cppRefFixtures.test.ts.
 *
 * INPUT geometry only. Nothing in test/data/svg_fixtures.txt is an expected
 * answer — it is the output of the SVG parser and enforceC1, neither of which is
 * in the port's scope, standing in for a front end the C++ does not have. The
 * C++ then flattens and constrains these curves itself and its own caps are what
 * get asserted, so a contract test built on this file is not checking the
 * TypeScript's arithmetic in any form.
 *
 * If the file is missing, the tests that need it say so and fail rather than
 * quietly passing over zero fixtures.
 */

#ifndef TEST_SUPPORT_SVGFIX_H
#define TEST_SUPPORT_SVGFIX_H

#include "motion/geometry.h"
#include "support/bits.h"

#include <sstream>
#include <string>
#include <vector>

namespace svgfix {

using Subpaths = std::vector<std::vector<motion::CubicBezier>>;

/** Named artwork -> its repaired subpaths. Empty if the file is absent. */
inline const Subpaths& load(const std::string& name) {
    static std::vector<std::pair<std::string, Subpaths>> cache;
    for (const auto& e : cache) {
        if (e.first == name) return e.second;
    }

    Subpaths out;
    std::ifstream f = testbits::openRef("svg_fixtures.txt");
    if (f.good()) {
        std::string line;
        bool inFixture = false;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream ls(line);
            std::string kind;
            ls >> kind;
            if (kind == "fixture") {
                std::string who;
                ls >> who;
                inFixture = (who == name);
            } else if (kind == "end") {
                if (inFixture) break;
            } else if (inFixture && kind == "subpath") {
                out.emplace_back();
            } else if (inFixture && kind == "c") {
                double v[8];
                for (double& d : v) {
                    std::string tok;
                    ls >> tok;
                    d = testbits::fromHex(tok);
                }
                out.back().push_back(motion::CubicBezier{
                    {v[0], v[1]}, {v[2], v[3]}, {v[4], v[5]}, {v[6], v[7]}});
            }
        }
    }
    cache.push_back({name, out});
    return cache.back().second;
}

} // namespace svgfix

#endif // TEST_SUPPORT_SVGFIX_H
