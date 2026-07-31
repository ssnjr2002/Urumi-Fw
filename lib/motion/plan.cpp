/**
 * plan.cpp — stage 6, transcribed from web/src/toolpath/plan.ts.
 *
 * BIT-EXACT transcription, not an improvement. See docs/planner_audit.md,
 * "Numeric porting rule".
 */

#include "motion/plan.h"

#include "motion/geometry.h"
#include "motion/jsmath.h"

#include <cmath>
#include <stdexcept>
#include <string>

namespace motion {

double segAccel(const Sample& s0, const Sample& s1, const PlanOptions& options) {
    const double dx = s1.x - s0.x;
    const double dy = s1.y - s0.y;
    // jsHypot, not std::hypot — V8's Math.hypot is a different algorithm.
    const double d = jsHypot(dx, dy);
    if (d < 1e-12) return options.aMax;

    const double ux = std::fabs(dx) / d;
    const double uy = std::fabs(dy) / d;

    // Mirrors the TypeScript's `cands` array plus `Math.min(...cands)`. Folding
    // with jsMin in push order reproduces Math.min's left-to-right result; the
    // ORDER matters and must match, because min over doubles is not
    // associative under signed zero.
    bool any = false;
    double best = 0;
    const auto push = [&](double c) {
        best = any ? jsMin(best, c) : c;
        any = true;
    };

    if (options.xAccel > 0 && ux > 1e-9) push(options.xAccel / ux);
    if (options.yAccel > 0 && uy > 1e-9) push(options.yAccel / uy);
    if (options.aAccelDegS2 > 0) {
        const double kap = jsMax(s0.kappa, s1.kappa);
        if (kap > 1e-9) push((options.aAccelDegS2 * PI) / 180 / kap);
    }
    // A tool that commands a lower path-accel caps the per-axis-derived limit.
    if (options.pathAccel > 0) push(options.pathAccel);

    return any ? best : options.aMax;
}

std::vector<PlannedSample> plan(
    const std::vector<ConstrainedSample>& samples,
    const PlanOptions& options) {

    // Every sample must belong to a bracketed subpath, or the sweeps below
    // silently skip it and its vCeiling is returned verbatim — full feed from a
    // standing start, no error (audit P2). flatten always brackets, but plan is
    // an exported pure stage and the port gives it callers that are not flatten
    // (jog, streamed tiles).
    const std::vector<Range> ranges = subpathRanges(samples);
    size_t next = 0;
    for (const Range& r : ranges) {
        if (r.first != next) {
            throw std::runtime_error(
                "plan: samples [" + std::to_string(next) + ", " +
                std::to_string(r.first - 1) +
                "] are outside any PATH_START/PATH_END bracket and would be left unplanned");
        }
        next = r.second + 1;
    }
    if (next != samples.size()) {
        throw std::runtime_error(
            "plan: samples [" + std::to_string(next) + ", " +
            std::to_string(samples.size() - 1) +
            "] are outside any PATH_START/PATH_END bracket and would be left unplanned" +
            (samples.empty() ? "" : " (unterminated subpath — missing PATH_END?)"));
    }

    // Work on a mutable v array indexed by sample position; fold into
    // PlannedSample at the end. The input is never touched.
    std::vector<double> v(samples.size());
    for (size_t i = 0; i < samples.size(); i++) v[i] = samples[i].vCeiling;

    for (const Range& r : ranges) {
        const size_t lo = r.first;
        const size_t hi = r.second;

        // Per-segment TANGENTIAL accel; segment i links i and i+1.
        std::vector<double> aSeg(samples.size(), 0.0);
        for (size_t i = lo; i < hi; i++) {
            aSeg[i] = segAccel(samples[i].s, samples[i + 1].s, options);
        }

        // ── the acceleration budget is one budget, not two (audit P1) ────────
        //
        // An axis supplies the VECTOR sum of the tangential term (dv/dt,
        // bounded here) and the centripetal term (v^2*kappa, bounded by
        // constrain's ceiling). Each stage bounded its own component at aMax
        // and nothing bounded the sum, so the analytic worst case was
        // sqrt(2)*aMax and it was essentially attained. Neither stage was wrong
        // alone — it is an interface defect, visible only in composition.
        //
        // The centripetal load is computed from the CEILING, not from a first
        // pass's v: using the planned v would be tighter and would cost
        // monotonicity ("more budget never plans slower"), and this form keeps
        // plan at two O(n) passes rather than four (P5).
        if (options.aMax > 0) {
            for (size_t i = lo; i < hi; i++) {
                const ConstrainedSample& a0 = samples[i];
                const ConstrainedSample& a1 = samples[i + 1];
                const double ac = jsMax(a0.vCeiling * a0.vCeiling * a0.s.kappa,
                                        a1.vCeiling * a1.vCeiling * a1.s.kappa);
                const double free =
                    std::sqrt(jsMax(0.0, options.aMax * options.aMax - ac * ac));
                if (free < aSeg[i]) aSeg[i] = free;
            }
        }

        // ── the two feasibility sweeps ───────────────────────────────────────
        // init from ceilings; pin the endpoints to rest
        for (size_t i = lo; i <= hi; i++) v[i] = samples[i].vCeiling;
        v[lo] = 0;
        v[hi] = 0;

        // backward: ensure we can brake to each downstream speed
        for (size_t i = hi; i-- > lo;) {
            const double ds = samples[i].s.ds;
            const double reachable = std::sqrt(v[i + 1] * v[i + 1] + 2 * aSeg[i] * ds);
            if (reachable < v[i]) v[i] = reachable;
        }

        // forward: ensure we can accelerate up to each speed
        for (size_t i = lo + 1; i <= hi; i++) {
            const double ds = samples[i - 1].s.ds;
            const double reachable = std::sqrt(v[i - 1] * v[i - 1] + 2 * aSeg[i - 1] * ds);
            if (reachable < v[i]) v[i] = reachable;
        }

        // endpoints stay pinned (the forward pass may have lifted hi off 0)
        v[lo] = 0;
        v[hi] = 0;
    }

    std::vector<PlannedSample> out;
    out.reserve(samples.size());
    for (size_t i = 0; i < samples.size(); i++) {
        out.push_back({samples[i].s, samples[i].vCeiling, v[i]});
    }
    return out;
}

} // namespace motion
