/**
 * axes.h — the machine calibration the emitting stages need, and nothing else.
 *
 * This is NOT a port of web/src/config/. The config layer — schema, loader,
 * validator, tool catalogue, the tool->machine->default fallback chain — is
 * host stack and stays in TypeScript. What crosses is only the resolved
 * numbers that the step math cannot be written without: how many steps per mm
 * each axis takes, which way it is wired, how fast it may go, and the clock.
 *
 * Stages 4-6 needed none of this, because flatten/constrain/plan work in mm
 * and mm/s. Stage 7 is where the pipeline stops being geometry and starts
 * being a machine, so it is the first stage that has to know what a step is.
 *
 * `discretize` in the TypeScript takes MachineConfig + ToolProfile +
 * QualityConfig and resolves them itself (resolvedAxes, resolveTargets, the
 * `overrides ?? profile ?? machine` chain). The port takes those results
 * instead — see DiscretizeOptions in discretize.h. That chain is `??`
 * operators over config, not motion math, and duplicating it here would be
 * duplicating the one part of the stage that is not about motion.
 */

#ifndef MOTION_AXES_H
#define MOTION_AXES_H

namespace motion {

/**
 * One axis's calibration.
 *
 * `maxFeed` and `maxAccel` are PHYSICAL ceilings in the axis's own unit
 * (mm/s and mm/s^2 for X/Y/Z, deg/s and deg/s^2 for A). **0 means uncapped**,
 * which is the config layer's convention throughout and is load-bearing in
 * three places: interval()'s per-axis rate floor skips an axis with R <= 0,
 * xyJog refuses to ramp when neither X nor Y declares an accel, and aMove
 * refuses to slew A without a feed and an accel. A 0 that is read as a real
 * ceiling instead of "unset" produces a division by zero or a stopped machine,
 * so it is never defaulted silently.
 *
 * `invert` is applied at EMIT, to the outgoing step delta, never to the
 * position accumulators. Everything upstream of the emit works in the logical
 * frame; only the wire sees the wiring.
 */
struct AxisConfig {
    double stepsPerUnit = 0;
    double maxFeed = 0;   // 0 = uncapped
    double maxAccel = 0;  // 0 = uncapped
    bool invert = false;
};

/**
 * The four live axes plus the step clock. Z and A belong to the active head;
 * which head that is has already been resolved by the caller.
 */
struct ResolvedAxes {
    AxisConfig x;
    AxisConfig y;
    AxisConfig z;
    AxisConfig a;
    double fCpu = 0;
};

/**
 * An operation's requested feed/accel, with BOTH fields genuinely optional.
 *
 * The presence flags are not defensive style — absent and 0 mean opposite
 * things here, exactly as they do for constrain's cornerStopAngleDeg. In
 * aMove the expression is `slew?.feed ?? axes.a.maxFeed` followed by
 * `if (!(feed > 0)) throw`: an ABSENT feed falls through to the axis ceiling
 * and the move proceeds, while a feed of 0 is used and then throws. Collapsing
 * them onto a 0 sentinel would turn "no slew target configured" — the default
 * machine's state — into a hard error on every corner pivot.
 */
struct OpTarget {
    bool hasFeed = false;
    double feed = 0;
    bool hasAccel = false;
    double accel = 0;
};

} // namespace motion

#endif // MOTION_AXES_H
