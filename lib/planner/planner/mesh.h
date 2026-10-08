/**
 * mesh.h — the bed mesh: a grid of heights under the tip, and its effect on a
 * block.
 *
 * Heights are int16 µm, + up, on an nx × ny grid at (x0 + i·dx, y0 + j·dy),
 * row-major (x fastest). Bilinear between points; outside the grid the edge
 * value carries on.
 *
 * A block's walk samples the offset along it at no more than half the grid
 * spacing: its range (for the soft-range check), its steepest slope (Z speed
 * and the Z share of the block's acceleration) and its largest change of
 * slope between samples (a grid-line crossing, taken as a junction over one
 * sample).
 */

#ifndef PLANNER_MESH_H
#define PLANNER_MESH_H

#include "planner/bezier.h"

#include <stdint.h>

namespace planner {

struct Mesh {
    int nx = 0, ny = 0;
    float x0 = 0, y0 = 0;       // mm, the first point
    float dx = 1, dy = 1;       // mm, spacing
    float inv_dx = 1, inv_dy = 1;
    const int16_t* z = nullptr; // µm, nx·ny

    /** Point the mesh at `heights`; false if the grid is under 2 × 2 or a spacing is not positive. */
    bool set(int nx_, int ny_, float x0_, float y0_, float dx_, float dy_, const int16_t* heights);
    bool valid() const { return z != nullptr; }
};

/** Height at (x, y), mm. */
float meshAt(const Mesh& m, float x, float y);

/** The offset along one block: meshAt(p(s) + tip) − ref. */
struct MeshWalk {
    float lo = 0, hi = 0;    // mm, the offset's range
    float slope = 0;         // max |dz/ds|
    float dslope = 0;        // max |change of dz/ds| between samples
    float step = 0;          // mm between samples
};

/** Walk a line from p0 to p1. */
MeshWalk meshWalkLine(const Mesh& m, Vec2 p0, Vec2 p1, Vec2 tip, float ref);
/** Walk an analysed Bézier. */
MeshWalk meshWalkBezier(const Mesh& m, const Bezier& b, Vec2 tip, float ref);

/**
 * Caps for Z along a walked block: v · slope ≤ z_feed; half of z_accel each
 * for the tangential share (accel · slope) and for slope changes
 * (v² · dslope / step).
 */
PathCap meshCap(const MeshWalk& w, float z_feed, float z_accel);

}  // namespace planner

#endif
