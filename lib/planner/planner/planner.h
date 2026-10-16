/**
 * planner.h — the block ring and its look-ahead.
 *
 * The producer pushes lines or Béziers and replans; the consumer claims the oldest block,
 * runs it, and releases it. Single-threaded: a caller that splits producer and
 * consumer across cores shares one Planner and locks around pushLine(),
 * pushBezier(), commit(), claim() and release(). replan() needs no lock: it
 * reads only block paths, which the consumer never writes, and a claim or
 * release during it makes the following commit() refuse.
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
    enum Kind : uint8_t { LINE, BEZIER, EVENT };
    Kind kind = LINE;
    uint8_t event = 0;         // EVENT: the caller's code, opaque here
    Path path;                 // length is what remains after s0
    float s0 = 0;              // mm of the geometry already run (a resume trim)
    Pos origin;                // start point
    Pos target;                // end point
    Bezier bez;                // BEZIER: the curve
    float max_entry_sqr = 0;   // junction limit with the previous block
    float entry_sqr = 0;       // committed plan
    float exit_sqr = 0;
    Trapezoid profile;
    uint16_t layer = 0;        // the record it runs, or travels to (setTag)
    uint32_t record = 0;
    uint16_t break_dist = 0;   // the record's nextBreakDist, mm (setTag)
    float resume_s = 0;        // a LINE or EVENT's: mm into the record a pause resumes at
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
    void reset(Pos pos);

    /**
     * Queue a line from the end of the last one to `target`, moving one axis
     * set (axesOf). Returns false if the ring is full or the line mixes sets.
     * A zero-length move queues nothing and returns true.
     */
    bool pushMove(const Pos& target, float feed, const AxisLimits& limits, float deviation,
                  const PathCap& cap = PathCap());
    /** An XY line; Z and A stay where the last block ends. */
    bool pushLine(Vec2 target, float feed, const AxisLimits& limits, float deviation,
                  const PathCap& cap = PathCap()) {
        Pos to = end_;
        to.setXy(target);
        return pushMove(to, feed, limits, deviation, cap);
    }
    /** Queue an analysed Bézier; `b.p[0]` must be end(). False if full. */
    bool pushBezier(const Bezier& b, float feed, const AxisLimits& limits, float deviation,
                    const PathCap& cap = PathCap()) {
        return pushBezierFrom(b, 0, feed, limits, deviation, cap);
    }
    /**
     * The same Bézier from `from` mm along it, whose point there must be end().
     * Nothing is queued if nothing is left of it.
     */
    bool pushBezierFrom(const Bezier& b, float from, float feed, const AxisLimits& limits,
                        float deviation, const PathCap& cap = PathCap());

    /**
     * Queue an event: no motion, at rest on both sides. The executor stops at
     * it until the consumer calls Executor::eventDone(). False if full.
     */
    bool pushEvent(uint8_t code);

    /** The record the blocks pushed from now on run, its nextBreakDist, and resume_s. */
    void setTag(uint16_t layer, uint32_t record, uint16_t breakDist = 0, float resumeS = 0) {
        tag_layer_ = layer; tag_record_ = record; tag_break_dist_ = breakDist;
        tag_resume_s_ = resumeS;
    }

    /**
     * Queue blocks ahead of the oldest: pushes from beginFront(`at`) to
     * endFront() start at `at` and run first, from rest. Only with no block
     * claimed (after restartFrom()); the caller checks there is room.
     */
    void beginFront(const Pos& at);
    void endFront();

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

#ifdef PLANNER_BENCH
    // Commits that staged a piece, and refusals by cause.
    struct Counts { uint32_t offers, waits, untaken, too_close; };
    Counts counts = {};
#endif

    int count() const { return count_; }
    bool full() const { return count_ == kSize; }
    bool claimed() const { return claimed_; }
    /** The oldest block, claimed or not; null if none is queued. */
    const Block* oldest() const { return count_ ? &ring_[tail_] : nullptr; }
    /** Where the last queued block ends. */
    Pos end() const { return end_; }
    /** Seconds of motion in the blocks not yet claimed, as last committed. */
    float queuedTime() const {
        float t = 0;
        for (int i = firstUnclaimed(); i < count_; i++) t += ring_[index(i)].profile.duration();
        return t;
    }

private:
    int index(int i) const { return (tail_ + i) % kSize; }
    int firstUnclaimed() const { return claimed_ ? 1 : 0; }
    bool pushBlock(Block::Kind kind, const Path& path, const Pos& target, float deviation);
    void clearOffer();

    Block ring_[kSize];
    int tail_ = 0;
    int count_ = 0;
    bool claimed_ = false;
    Pos end_;
    uint16_t tag_layer_ = 0;
    uint32_t tag_record_ = 0;
    uint16_t tag_break_dist_ = 0;
    float tag_resume_s_ = 0;

    // beginFront(): the ring's count and end before the front pushes.
    int front_mark_ = 0;
    Pos front_end_;

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
