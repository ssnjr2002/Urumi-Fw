/**
 * sample.h — the spine of the pipeline (stages 4-8).
 *
 * Transcribed from web/src/toolpath/sample.ts.
 *
 * A Sample is one point along a flattened toolpath: position, tangent, local
 * curvature, and the arc-length step to the NEXT sample. After flatten the
 * whole job is a single flat Sample list; every downstream stage operates on
 * that instead of on Bezier tiles.
 *
 * Sample carries only GEOMETRY — no velocity fields. Downstream stages produce
 * richer types: ConstrainedSample (adds vCeiling) and PlannedSample (adds v).
 * Each stage's output type documents its guarantee.
 *
 * Angle convention: theta is in DEGREES; curvature kappa is in 1/mm.
 */

#ifndef MOTION_SAMPLE_H
#define MOTION_SAMPLE_H

#include <cstdint>

namespace motion {

struct Sample {
    double x;        // mm, machine frame
    double y;        // mm, machine frame
    double theta;    // tangent angle, degrees
    double kappa;    // local curvature, 1/mm (>= 0)
    double ds;       // arc length to next sample, mm (0 at subpath end)
    uint32_t flags;  // PATH_START | PATH_END | CURVE_BOUNDARY
};

// ── provenance flags ─────────────────────────────────────────────────────────
// These mark WHERE a sample came from; the tool-dependent CORNER decision (is a
// curve-boundary tangent jump sharp enough to lift-pivot?) is made downstream
// where the ToolProfile is known, not here.

constexpr uint32_t PATH_START = 0x01;      // first sample of a subpath
constexpr uint32_t PATH_END = 0x02;        // last sample of a subpath
constexpr uint32_t CURVE_BOUNDARY = 0x04;  // first sample of a curve following
                                           // another in the same subpath. The
                                           // previous sample is the prior
                                           // curve's t=1; the two share a
                                           // position but may differ in tangent
                                           // — that difference IS the corner
                                           // signal.

} // namespace motion

#endif // MOTION_SAMPLE_H
