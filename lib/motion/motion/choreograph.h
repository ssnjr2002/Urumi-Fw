/**
 * choreograph.h — non-cutting motion emitters (stateless, reusable).
 *
 * Transcribed from web/src/choreograph/choreograph.ts.
 *
 * Everything the machine does that is not cutting: travel jogs between
 * subpaths, Z lift and lower, ramped A rotation, lift-pivot-lower at a corner,
 * A pre-orientation at a path start. Each function takes the state it needs and
 * returns new state; nothing here holds a mutable internal. The caller —
 * discretize, or a future tool-change orchestrator — owns the walk state.
 *
 * It sits below discretize in the port even though it is nominally stage 8,
 * because discretize calls all of it and cannot be verified without it.
 *
 * The two profile-dependent entry points take `tangential` / `unwind` as plain
 * bools rather than a ToolProfile. The full profile is a config type with a
 * dozen fields this module never reads, and passing it would drag the config
 * layer across the port boundary to answer two yes/no questions.
 */

#ifndef MOTION_CHOREOGRAPH_H
#define MOTION_CHOREOGRAPH_H

#include "motion/axes.h"
#include "motion/microsegment.h"

#include <vector>

namespace motion {

/** One emitted piece of a ramp: `steps` major-axis steps clocked at `interval`. */
struct RampChunk {
    double steps = 0;
    double interval = 0;
};

/**
 * Cut a pure single-axis move of `N` steps into a trapezoidal speed profile:
 * accelerate v0 -> peak, cruise, decelerate peak -> v0, never exceeding
 * `accel`. Speeds in steps/s, accel in steps/s^2.
 *
 * Each chunk's interval comes from the EXACT time that chunk takes under
 * constant acceleration — dt = |v_end - v_start| / accel — not from the speed
 * sampled at one end of it. That is the whole of audit H1: sampling at the
 * chunk START is the slowest point of an accelerating chunk (conservative) and
 * the FASTEST point of a decelerating one (anti-conservative by 1.26-1.65x),
 * so one line produces an error of opposite sign on the two halves of the same
 * move. A mean derived from the kinematics has no side.
 *
 * Chunk boundaries are integer step counts, so the emitted move is exactly N
 * steps regardless of how many pieces it is cut into.
 */
std::vector<RampChunk> rampChunks(double N, double v0, double cruise,
                                  double accel, double fCpu);

/**
 * A ramped Z move (trapezoidal, via `rampChunks`). `dz` is in STEPS, signed;
 * invert is applied to the emitted values. Empty for dz = 0.
 *
 * Closes H3. Z used to be one constant-velocity segment, asking the axis for
 * its whole feed in zero distance — the defect H2 fixed for travel jogs and H1
 * for A.
 *
 * **Both targets are CLAMPED to the axis ceilings, not refused.** aMove refuses
 * an ABSENT limit (H4): a trapezoid cannot be built from "uncapped", and
 * guessing calibration is how you crash a machine. That does not extend to a
 * limit that is present and merely exceeded — there the machine's own number is
 * the answer, and using it is strictly safer than honouring the request. An
 * absent accel is still refused, for H4's original reason.
 */
std::vector<MicroSegment> zMove(double dz, const ResolvedAxes& axes, double zFeed,
                                double zAccel);

/** Z step count for a lift of `liftHeight` mm. 0 when the tool does not lift. */
double zStepCount(double liftHeight, const ResolvedAxes& axes);

/**
 * A ramped pure-A rotation (trapezoidal). `da` is in STEPS, signed; invert is
 * applied to the emitted value. Empty for da = 0.
 *
 * Throws if neither the slew target nor the A axis declares a feed and an
 * accel. A trapezoid cannot be built from "uncapped", and the code this
 * replaced substituted 180 deg/s and 2000 deg/s^2 silently — which made an
 * UNDECLARED A axis slew faster than a declared one (audit H4). Refusing and
 * naming the missing knob is the same policy the loader uses for
 * stepsPerUnit.
 */
std::vector<MicroSegment> aMove(double da, const ResolvedAxes& axes,
                                const OpTarget& slew);

/** Lift-pivot-lower: raise Z -> rotate A by `daTrue` steps -> lower Z. */
std::vector<MicroSegment> pivot(double daTrue, bool lift, double zSteps,
                                const ResolvedAxes& axes, double zFeed,
                                double zAccel, const OpTarget& slew);

/**
 * A ramped travel jog from (fromX, fromY) to (toX, toY), all in STEPS. Empty
 * when the rounded delta is zero.
 *
 * Ramped rather than one segment at full jog feed: the single-segment form
 * asked the machine for its whole travel speed in zero distance — 0 to 80 mm/s
 * instantly — against a configured x.maxAccel of 1000 mm/s^2 that the cutting
 * path respects everywhere (audit H2). The step totals are unchanged; only the
 * timeline is.
 */
std::vector<MicroSegment> travelJog(double fromX, double fromY, double toX,
                                    double toY, const ResolvedAxes& axes,
                                    double vMin, double jogFeed);

/** Result of a move that advances the tracked physical A position. */
struct AMoveResult {
    std::vector<MicroSegment> segments;
    double newAPhys = 0;
};

/**
 * Pre-orient A to the entry tangent before lowering to cut.
 *
 * unwind (a wired tool): rotate to the ABSOLUTE target, compensating for
 *   accumulated physical rotation, which keeps the cable within ~one turn.
 * non-unwind (free-spinning): rotate by the DELTA from the current tangent.
 *
 * A non-tangential tool returns no segments and an unchanged aPhys.
 */
AMoveResult preOrient(double entryTheta, double currentTheta, double currentAPhys,
                      const ResolvedAxes& axes, bool tangential, bool unwind,
                      const OpTarget& slew);

/** Move A to an absolute angle in degrees. For A-home and revolver slots. */
AMoveResult aMoveTo(double targetDeg, double currentAPhys,
                    const ResolvedAxes& axes, const OpTarget& slew);

/**
 * XY jog compensating for a head offset change, in mm. Emitted in steps, with
 * invert applied and ramped like any other travel move. Empty when the two
 * heads share an offset.
 */
std::vector<MicroSegment> headOffsetJog(double fromXOffset, double fromYOffset,
                                        double toXOffset, double toYOffset,
                                        const ResolvedAxes& axes, double vMin,
                                        double jogFeed);

} // namespace motion

#endif // MOTION_CHOREOGRAPH_H
