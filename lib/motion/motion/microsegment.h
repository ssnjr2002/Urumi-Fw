/**
 * microsegment.h — the bottom-of-pipeline wire event and its emit helpers.
 *
 * Transcribed from web/src/wire/format/microsegment.ts.
 *
 * One MicroSegment is one step-timing event: per-axis integer step deltas plus
 * a clock interval for the major axis. Discretize and choreograph both produce
 * these; the serialiser packs them to the 26-byte wire format.
 *
 * Every field is `double`, including the four step counts, which are always
 * integral. That is deliberate and it is not laziness about types: the
 * TypeScript's fields are JS numbers, the port's contract is bit-equality with
 * the TypeScript, and an `int32_t` here would silently launder a divergence
 * that the differential exists to catch. The values are integers well inside
 * 2^53, so nothing is lost by carrying them exactly as the TypeScript does.
 */

#ifndef MOTION_MICROSEGMENT_H
#define MOTION_MICROSEGMENT_H

#include "motion/axes.h"

#include <cstdint>

namespace motion {

struct MicroSegment {
    double dx = 0;        // X steps (signed, integral)
    double dy = 0;        // Y steps
    double dz = 0;        // Z steps
    double da = 0;        // A steps (tangential rotation)
    double interval = 0;  // clock cycles for the major axis
    uint32_t flags = 0;
};

inline MicroSegment microSegment(double dx, double dy, double dz, double da,
                                 double interval, uint32_t flags = 0) {
    MicroSegment m;
    m.dx = dx;
    m.dy = dy;
    m.dz = dz;
    m.da = da;
    m.interval = interval;
    m.flags = flags;
    return m;
}

// ── flag constants ───────────────────────────────────────────────────────────
// ONE namespace shared with the wire (docs/wire_protocol.md). Low bits are
// firmware semantics; high bits are host planning hints the firmware ignores.
// JOG must NOT be 0x04 — that would alias every travel move onto PAUSE.

constexpr uint32_t MICRO_PATH_END = 0x01;
constexpr uint32_t MICRO_PAUSE = 0x04;
constexpr uint32_t MICRO_LIFT = 0x08;
constexpr uint32_t MICRO_JOG = 0x10;
constexpr uint32_t MICRO_DUTY_RELEASE = 0x20;
constexpr uint32_t MICRO_DUTY_ASSERT = 0x40;

/**
 * Clock cycles per major-axis step so the XY TOOL moves at `v` mm/s.
 *
 * The firmware times a segment by its MAJOR axis (most steps of any driven
 * axis), but the tool travels the XY hypotenuse, which is longer than the
 * major leg on a diagonal. Without the correction the realised tool speed
 * overshoots v by up to sqrt(2). Scaling by hypot(dx,dy)/major restores the
 * commanded feed and reduces to the plain major-axis rate on a pure axis move.
 *
 * Per-axis rate floor: the segment's duration is floored so no axis exceeds
 * maxFeed * stepsPerUnit. This is what keeps A within its slew rate on tight
 * curves — and it is also FINDING D3, because it fires after planning and
 * nothing upstream learns that it did.
 *
 * The four-argument overload governs the major axis directly at v, with no
 * geometry to correct against (the legacy path, and what choreograph's jog
 * uses to derive a cruise rate).
 */
double interval(double v, const ResolvedAxes& axes, double vMin);

double interval(double v, const ResolvedAxes& axes, double vMin,
                double dx, double dy, double dz, double da);

} // namespace motion

#endif // MOTION_MICROSEGMENT_H
