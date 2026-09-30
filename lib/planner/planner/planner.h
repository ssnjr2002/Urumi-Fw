/**
 * planner.h — the block ring and its look-ahead.
 *
 * The producer pushes lines or Béziers and replans; the consumer claims the oldest block,
 * runs it, and releases it. Single-threaded: a caller that splits producer and
 * consumer across cores shares one Planner and locks around push(), commit(),
 * claim() and release(). replan() needs no lock: it reads only block paths,
 * which the consumer never writes, and a claim or release during it makes the
 * following commit() refuse.
 *
 * Look-ahead is two calls so the lock can stay short (seed §15). replan()
 * computes the speeds into scratch and touches nothing the consumer reads.
 * commit() copies them into the ring, and refuses if the ring changed since
 * replan(): after a claim the plan starts from a block that is no longer the
 * one running.
 *
 * The claimed block's exit may still rise (grbl's prep horizon). Both calls
 * take the consumer's clock `t`, seconds into the claimed block. replan()
 * plans the rest of that block from a horizon `t_h`, a little ahead of `t`,
 * and commit() offers the consumer a Piece that takes over there, through a
 * staging slot the consumer reads without the lock (takeStaged()). commit()
 * refuses unless the consumer is more than `guard` short of `t_h`, `guard`
 * being the longest tick that can start before the consumer next calls
 * takeStaged(). Only the claimed block's profile changes, never its geometry.
 *
 * Invariants the consumer relies on: a staged Piece matches what it runs up
 * to `t_h`; the block after the claimed one enters at the claimed block's
 * committed exit; the newest block always exits at rest, so a ring that drains
 * stops gracefully.
 */

#ifndef PLANNER_PLANNER_H
#define PLANNER_PLANNER_H

#include "planner/bezier.h"
#include "planner/line.h"
#include "planner/trapezoid.h"

#include <stdint.h>

#ifndef PLANNER_RING_SIZE
#define PLANNER_RING_SIZE 64
#endif

namespace planner {

struct Block {
    enum Kind : uint8_t { LINE, BEZIER };
    Kind kind = LINE;
    Path path;                 // length is what remains after s0
    float s0 = 0;              // mm of the geometry already run (a resume trim)
    Vec2 origin;               // LINE: start point
    Bezier bez;                // BEZIER: the curve
    float max_entry_sqr = 0;   // junction limit with the previous block
    float entry_sqr = 0;       // committed plan
    float exit_sqr = 0;
    Trapezoid profile;
};

/** A block's profile from `t0` s after its claim, starting `s0` mm into it. */
struct Piece {
    float t0 = 0;
    float s0 = 0;
    Trapezoid profile;
    float end() const { return t0 + profile.duration(); }
};

class Planner {
public:
    static constexpr int kSize = PLANNER_RING_SIZE;

    /** Empty the ring; the machine is at rest at `pos`. */
    void reset(Vec2 pos);

    /**
     * Queue a line from the end of the last one to `target`. Returns false if
     * the ring is full. A zero-length move queues nothing and returns true.
     */
    bool push(Vec2 target, float feed, const AxisLimits& limits, float deviation);
    /** Queue an analysed Bézier; `b.p[0]` must be end(). False if full. */
    bool pushBezier(const Bezier& b, float feed, const AxisLimits& limits, float deviation);

    /** Horizon `ahead` of the consumer's clock, and the commit guard, in s. */
    void setTiming(float ahead, float guard) { ahead_ = ahead; guard_ = guard; }

    /** `t`: the consumer's clock, read without the lock. */
    void replan(float t = 0);
    /**
     * False if the plan is stale or its horizon too close to `t`, read again
     * under the lock; the caller replans and commits again.
     */
    bool commit(float t = 0);

    /** The oldest block, now running; null if one is already claimed or none is queued. */
    const Block* claim();
    /** Free the claimed block. */
    void release();
    /** Consumer, no lock: take a staged Piece for the claimed block, if any. */
    bool takeStaged(Piece& out);

    /**
     * The machine stopped `s` mm into the claimed block, or at the start of
     * the oldest block if none is claimed. The claimed block is unclaimed and
     * trimmed to start there, and the next plan enters from rest. Replan and
     * commit before claiming again.
     */
    void restartFrom(float s);

    int count() const { return count_; }
    bool full() const { return count_ == kSize; }
    bool claimed() const { return claimed_; }
    /** Where the last queued block ends. */
    Vec2 end() const { return end_; }

private:
    int index(int i) const { return (tail_ + i) % kSize; }
    int firstUnclaimed() const { return claimed_ ? 1 : 0; }
    bool pushBlock(Block::Kind kind, const Path& path, float deviation);
    void clearOffer();

    Block ring_[kSize];
    int tail_ = 0;
    int count_ = 0;
    bool claimed_ = false;
    Vec2 end_;

    // Exit speed of the most recently claimed block: the entry of the next.
    float pinned_entry_sqr_ = 0;
    // Bumped by every push, claim and release.
    uint32_t epoch_ = 0;

    float ahead_ = 0.005f;
    float guard_ = 0.0012f;

    // What the consumer runs in the claimed block: `run_` from the claim, then
    // `offer_` from its t0 once one is committed.
    Piece run_;
    Piece offer_;
    bool has_offer_ = false;
    // The handoff. The producer writes `staged_` only while the flag is clear.
    Piece staged_;
    volatile bool staged_flag_ = false;

    // replan() output by ring position, valid for commit() while epoch_ matches.
    float plan_entry_sqr_[kSize];
    float plan_exit_sqr_[kSize];
    Trapezoid plan_profile_[kSize];
    uint32_t plan_epoch_ = 0;
    bool plan_valid_ = false;
    bool plan_wait_ = false;    // horizon within guard of a pending switch
    bool plan_offer_ = false;   // plan_piece_ raises the claimed block's exit
    Piece plan_piece_;
};

}  // namespace planner

#endif
