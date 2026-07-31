/**
 * quality.h — the shipped QualityConfig defaults, as the contract tests see them.
 *
 * The TypeScript tests call `qualityConfig()`; the C++ stages take no config at
 * all (every stage is pure and the caller sources the values), so the tests must
 * supply them. These are web/src/config/defaults.ts verbatim.
 *
 * Duplicated rather than generated ON PURPOSE. A generated copy would track
 * defaults.ts silently, and a contract test whose thresholds move with the code
 * under test is not a contract — the caps are supposed to be a commitment. If
 * defaults.ts changes, this file should have to be edited by a person who then
 * has to justify why every cap still holds.
 */

#ifndef TEST_SUPPORT_QUALITY_H
#define TEST_SUPPORT_QUALITY_H

#include "motion/flatten.h"

namespace quality {

constexpr double CHORD_TOL = 0.01;
constexpr double DS_MAX = 0.5;
constexpr double DTHETA_MAX = 2.0;
constexpr double DT_MAX = 0.05;
constexpr double DT_MIN = 1e-6;
constexpr int MAX_REFINE = 8;
constexpr double JUNCTION_DEVIATION = 0.05;
constexpr double V_MIN = 0.5;
constexpr double ANGLE_TOL = 5.0;
constexpr double GAP_TOL = 0.01;

/** The FlattenOptions every fixture is flattened with, unless a test varies one. */
inline motion::FlattenOptions flattenOpts(int maxRefine = MAX_REFINE) {
    return motion::FlattenOptions{CHORD_TOL, DS_MAX, DTHETA_MAX,
                                  DT_MAX, DT_MIN, maxRefine};
}

} // namespace quality

#endif // TEST_SUPPORT_QUALITY_H
