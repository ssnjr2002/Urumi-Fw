/**
 * flatten.h — stage 4: repaired Bezier subpaths -> flat Sample list.
 *
 * Transcribed from web/src/toolpath/flatten.ts.
 *
 * The representation drop the whole redesign turns on: after here the pipeline
 * no longer sees Bezier curves, only an arc-length sample stream carrying
 * PER-SAMPLE local curvature.
 *
 * Sampling is driven by three geometry-only limits (velocity is unknown here —
 * that is the point; planning happens downstream):
 *   1. chord deviation:  dt <= sqrt(8 * chordTol / |B''(t)|)
 *   2. spacing cap:      dt <= dsMax / |B'(t)|
 *   3. tangent step:     dt <= dthetaMax / (kappa * |B'(t)|)
 *
 * Pure stage: takes FlattenOptions (a subset of QualityConfig), never reads
 * config. The caller sources the values.
 */

#ifndef MOTION_FLATTEN_H
#define MOTION_FLATTEN_H

#include "motion/geometry.h"
#include "motion/sample.h"

#include <vector>

namespace motion {

struct FlattenOptions {
    double chordTol;
    double dsMax;
    double dthetaMax;
    double dtMax;
    double dtMin;
    /**
     * How many times a step may be halved when the step it predicted turns out
     * to have overshot a cap (audit F1/F7). Each halving at most doubles the
     * samples in that neighbourhood, so this is the knob that bounds how many
     * samples a cusp can cost — the thing a sample-count-bounded window on the
     * Pico actually cares about.
     *
     * The firmware ships one fixed value; a host doing offline work can raise
     * it to resolve pathological geometry more finely. `dtMin` still applies
     * underneath as a hard floor.
     *
     * 0 = no enforcement, i.e. the pre-F7 behaviour where the three caps were
     * predictors rather than bounds.
     */
    int maxRefine;
};

/**
 * Flatten repaired Bezier subpaths into one flat Sample list.
 *
 * Each inner list is one continuous pen-down stroke. PATH_START / PATH_END
 * bracket each subpath; CURVE_BOUNDARY marks intra-subpath curve joins. ds on
 * each sample is the chord to the next sample (0 on the final sample of each
 * subpath).
 */
std::vector<Sample> flatten(
    const std::vector<std::vector<CubicBezier>>& subpaths,
    const FlattenOptions& options);

} // namespace motion

#endif // MOTION_FLATTEN_H
