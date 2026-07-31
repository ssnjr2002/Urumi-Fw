#include "motion/choreograph.h"

#include "motion/geometry.h"
#include "motion/jsmath.h"

#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>

namespace motion {

namespace {

/**
 * How many pieces each ramp is cut into. The ramp is exact at every chunk
 * BOUNDARY regardless of this number — it only sets how finely the speed
 * staircase approximates the continuous ramp, and so how much the axis is
 * asked to jerk at each boundary. 16 keeps a full-speed ramp under ~1/16th of
 * a step change per boundary while costing ~32 segments for the whole move.
 */
constexpr int RAMP_CHUNKS = 16;

/** Junction speed for a standalone move: slow, not standstill. */
constexpr double JUNCTION_V = 50;

} // namespace

std::vector<RampChunk> rampChunks(double N, double v0, double cruise,
                                  double accel, double fCpu) {
    std::vector<RampChunk> out;
    if (N <= 0) return out;

    // Ramp length, triangular-clamped when there is no room to reach cruise.
    double dAcc = (cruise * cruise - v0 * v0) / (2 * accel);
    if (2 * dAcc > N) dAcc = N / 2;
    const double peak = std::sqrt(v0 * v0 + 2 * accel * dAcc);

    /** Speed of the continuous profile at step distance n. */
    const auto vAt = [&](double n) -> double {
        if (n <= dAcc) return std::sqrt(v0 * v0 + 2 * accel * n);
        if (n >= N - dAcc) return std::sqrt(jsMax(v0 * v0, v0 * v0 + 2 * accel * (N - n)));
        return peak;
    };

    // Boundaries: equal speed increments up the ramp, one piece across the
    // cruise (dv = 0 there, so a single chunk is already exact), mirrored down.
    //
    // std::set stands in for the TypeScript's Set + numeric sort: both
    // deduplicate and the C++ container is already ordered, so the explicit
    // sort has no counterpart. The values are non-negative integers, so the
    // signed-zero disagreement between the two containers' notions of equality
    // cannot arise.
    std::set<double> marks;
    marks.insert(0);
    marks.insert(N);
    const double dv = (peak - v0) / RAMP_CHUNKS;
    if (dv > 0) {
        for (int i = 1; i <= RAMP_CHUNKS; i++) {
            const double v = v0 + i * dv;
            const double n = (v * v - v0 * v0) / (2 * accel);
            marks.insert(jsMin(jsRound(n), N));
            marks.insert(jsMax(N - jsRound(n), 0));
        }
    }

    const std::vector<double> bounds(marks.begin(), marks.end());
    for (size_t i = 0; i + 1 < bounds.size(); i++) {
        const double a = bounds[i];
        const double b = bounds[i + 1];
        const double steps = b - a;
        if (steps <= 0) continue;
        const double vA = vAt(a);
        const double vB = vAt(b);
        // Exact duration: constant-accel over the chunk, or constant speed when
        // the two ends agree (the cruise piece, and any degenerate ramp piece).
        const double dt = std::fabs(vB - vA) > 1e-9
                              ? std::fabs(vB - vA) / accel
                              : steps / jsMax(vA, 1e-9);
        const double iv = jsMax(1, jsMin(jsRound((fCpu * dt) / steps), fCpu));
        RampChunk c;
        c.steps = steps;
        c.interval = iv;
        out.push_back(c);
    }
    return out;
}

std::vector<MicroSegment> zMove(double dz, const ResolvedAxes& axes, double zFeed,
                                double zAccel) {
    const double N = std::fabs(std::trunc(dz));
    if (N == 0) return {};

    const double spu = axes.z.stepsPerUnit;
    // Clamp to the axis, never above it. A 0 ceiling means "undeclared", which
    // is not a licence to exceed — it is the absence of a number to clamp to.
    const double feed = axes.z.maxFeed > 0 ? jsMin(zFeed, axes.z.maxFeed) : zFeed;
    const double accel = axes.z.maxAccel > 0 ? jsMin(zAccel, axes.z.maxAccel) : zAccel;
    if (!(feed > 0) || !(accel > 0)) {
        std::string missing;
        if (!(feed > 0)) missing = "feed";
        if (!(accel > 0)) missing += missing.empty() ? "accel" : " and accel";
        throw std::runtime_error(
            "zMove: cannot move Z by " + std::to_string(static_cast<long long>(N)) +
            " steps — no " + missing +
            " limit. Set machine.z.feed / machine.heads[].z.maxAccel, or pass an explicit target.");
    }

    const double cruise = jsMax(feed * spu, 1);
    const double rate = jsMax(accel * spu, 1);
    const double v0 = jsMin(cruise, JUNCTION_V);
    const double sign = (dz > 0 ? 1 : -1) * (axes.z.invert ? -1 : 1);

    std::vector<MicroSegment> out;
    for (const RampChunk& c : rampChunks(N, v0, cruise, rate, axes.fCpu)) {
        out.push_back(microSegment(0, 0, sign * c.steps, 0, c.interval, MICRO_LIFT));
    }
    return out;
}

double zStepCount(double liftHeight, const ResolvedAxes& axes) {
    if (liftHeight <= 0) return 0;
    return jsRound(liftHeight * axes.z.stepsPerUnit);
}

std::vector<MicroSegment> aMove(double da, const ResolvedAxes& axes,
                                const OpTarget& slew) {
    const double N = std::fabs(std::trunc(da));
    if (N == 0) return {};

    // Standalone-A slew target (machine-owned), falling back to the A axis
    // ceiling. See axes.h on why absent and 0 must stay distinguishable here.
    const double aSpd = axes.a.stepsPerUnit;
    const double feed = slew.hasFeed ? slew.feed : axes.a.maxFeed;
    const double rate = slew.hasAccel ? slew.accel : axes.a.maxAccel;
    if (!(feed > 0) || !(rate > 0)) {
        std::string missing;
        if (!(feed > 0)) missing = "feed";
        if (!(rate > 0)) missing += missing.empty() ? "accel" : " and accel";
        throw std::runtime_error(
            "aMove: cannot rotate A by " + std::to_string(static_cast<long long>(N)) +
            " steps — no " + missing +
            " limit. Set machine.heads[].a.maxFeed/maxAccel, or pass an explicit slew target.");
    }
    const double cruise = jsMax(feed * aSpd, 1);
    const double accel = jsMax(rate * aSpd, 1);
    const double v0 = jsMin(cruise, JUNCTION_V);

    const double sign = (da > 0 ? 1 : -1) * (axes.a.invert ? -1 : 1);

    std::vector<MicroSegment> out;
    for (const RampChunk& c : rampChunks(N, v0, cruise, accel, axes.fCpu)) {
        out.push_back(microSegment(0, 0, 0, sign * c.steps, c.interval, MICRO_JOG));
    }
    return out;
}

std::vector<MicroSegment> pivot(double daTrue, bool lift, double zSteps,
                                const ResolvedAxes& axes, double zFeed,
                                double zAccel, const OpTarget& slew) {
    std::vector<MicroSegment> out;
    const auto append = [&out](const std::vector<MicroSegment>& v) {
        out.insert(out.end(), v.begin(), v.end());
    };
    if (lift) append(zMove(+zSteps, axes, zFeed, zAccel));
    append(aMove(daTrue, axes, slew));
    if (lift) append(zMove(-zSteps, axes, zFeed, zAccel));
    return out;
}

namespace {

/**
 * A straight XY move of (dx, dy) STEPS as a ramped travel jog. Shared by
 * travelJog and headOffsetJog.
 *
 * The per-axis deltas are distributed proportionally with float accumulators
 * rounded at emit, so the chunks sum to exactly (dx, dy) with no drift — the
 * same telescoping discretize uses on the cutting path.
 */
std::vector<MicroSegment> xyJog(double dx, double dy, const ResolvedAxes& axes,
                                double vMin, double jogFeed) {
    if (dx == 0 && dy == 0) return {};

    // Cruise speed comes from interval() exactly as it would for a cutting
    // segment, so the jog's top speed and every per-axis feed floor keep their
    // existing meaning.
    const double ivCruise = interval(jogFeed, axes, vMin, dx, dy, 0, 0);
    const double major = jsMax(std::fabs(dx), std::fabs(dy));
    const double cruise = axes.fCpu / ivCruise; // major-axis steps/s

    // XY acceleration ceiling, converted from mm/s^2 to major-axis steps/s^2
    // along THIS path. Whichever axis is tighter owns the move.
    const double inf = std::numeric_limits<double>::infinity();
    const double lenMm = jsHypot(dx / axes.x.stepsPerUnit, dy / axes.y.stepsPerUnit);
    const double accelMm = jsMin(axes.x.maxAccel > 0 ? axes.x.maxAccel : inf,
                                 axes.y.maxAccel > 0 ? axes.y.maxAccel : inf);
    if (!(accelMm > 0) || !std::isfinite(accelMm) || lenMm <= 0) {
        // No declared XY accel means there is nothing to ramp against. Refuse
        // rather than silently slam, the same policy aMove uses for A (H4).
        throw std::runtime_error(
            "travel jog of " + std::to_string(static_cast<long long>(major)) +
            " steps: no XY acceleration limit. Set machine.x.maxAccel and machine.y.maxAccel.");
    }
    const double accel = (accelMm * major) / lenMm;

    // Junction speed: the same standstill-ish entry/exit aMove uses, so a jog
    // starts and ends slow instead of at feed.
    const double v0 = jsMin(cruise, JUNCTION_V);

    std::vector<MicroSegment> out;
    double doneMajor = 0;
    double accX = 0;
    double accY = 0;
    for (const RampChunk& c : rampChunks(major, v0, cruise, accel, axes.fCpu)) {
        doneMajor += c.steps;
        const double f = doneMajor / major;
        const double tgtX = dx * f;
        const double tgtY = dy * f;
        const double sx = jsRound(tgtX) - jsRound(accX);
        const double sy = jsRound(tgtY) - jsRound(accY);
        accX = tgtX;
        accY = tgtY;
        if (sx == 0 && sy == 0) continue;
        out.push_back(microSegment(axes.x.invert ? -sx : sx,
                                   axes.y.invert ? -sy : sy,
                                   0, 0, c.interval, MICRO_JOG));
    }
    return out;
}

} // namespace

std::vector<MicroSegment> travelJog(double fromX, double fromY, double toX,
                                    double toY, const ResolvedAxes& axes,
                                    double vMin, double jogFeed) {
    const double dx = jsRound(toX) - jsRound(fromX);
    const double dy = jsRound(toY) - jsRound(fromY);
    return xyJog(dx, dy, axes, vMin, jogFeed);
}

AMoveResult preOrient(double entryTheta, double currentTheta, double currentAPhys,
                      const ResolvedAxes& axes, bool tangential, bool unwind,
                      const OpTarget& slew) {
    AMoveResult r;
    r.newAPhys = currentAPhys;
    if (!tangential) return r;

    const double aSpd = axes.a.stepsPerUnit;
    double daTrue;
    if (unwind) {
        const double target = jsRound(entryTheta * aSpd);
        daTrue = target - currentAPhys;
    } else {
        daTrue = jsRound(angleDelta(currentTheta, entryTheta) * aSpd);
    }

    if (daTrue == 0) return r;

    r.segments = aMove(daTrue, axes, slew);
    r.newAPhys = currentAPhys + daTrue;
    return r;
}

AMoveResult aMoveTo(double targetDeg, double currentAPhys,
                    const ResolvedAxes& axes, const OpTarget& slew) {
    AMoveResult r;
    r.newAPhys = currentAPhys;
    const double targetSteps = jsRound(targetDeg * axes.a.stepsPerUnit);
    const double daTrue = targetSteps - currentAPhys;
    if (daTrue == 0) return r;
    r.segments = aMove(daTrue, axes, slew);
    r.newAPhys = currentAPhys + daTrue;
    return r;
}

std::vector<MicroSegment> headOffsetJog(double fromXOffset, double fromYOffset,
                                        double toXOffset, double toYOffset,
                                        const ResolvedAxes& axes, double vMin,
                                        double jogFeed) {
    const double dxMm = toXOffset - fromXOffset;
    const double dyMm = toYOffset - fromYOffset;
    if (std::fabs(dxMm) < 1e-9 && std::fabs(dyMm) < 1e-9) return {};

    const double dxSteps = jsRound(dxMm * axes.x.stepsPerUnit);
    const double dySteps = jsRound(dyMm * axes.y.stepsPerUnit);
    return xyJog(dxSteps, dySteps, axes, vMin, jogFeed);
}

} // namespace motion
