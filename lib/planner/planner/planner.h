/**
 * planner.h — the block ring and its look-ahead.
 *
 * The producer pushes lines and replans; the consumer claims the oldest block,
 * runs it, and releases it. Single-threaded: a caller that splits producer and
 * consumer across cores shares one Planner and locks around push(), commit(),
 * claim() and release(). replan() needs no lock: it reads only block lines,
 * which the consumer never writes, and a claim or release during it makes the
 * following commit() refuse.
 *
 * Look-ahead is two calls so the lock can stay short (seed §15). replan()
 * computes every unclaimed block's speeds into scratch and touches nothing the
 * consumer reads. commit() copies them into the ring, and refuses if the ring
 * changed since replan(): after a claim the plan's first entry is pinned to a
 * block that is no longer the one running.
 *
 * Invariants the consumer relies on: the claimed block is never modified; the
 * block after it enters at the claimed block's committed exit; the newest block
 * always exits at rest, so a ring that drains stops gracefully.
 */

#ifndef PLANNER_PLANNER_H
#define PLANNER_PLANNER_H

#include "planner/line.h"
#include "planner/trapezoid.h"

#include <stdint.h>

#ifndef PLANNER_RING_SIZE
#define PLANNER_RING_SIZE 64
#endif

namespace planner {

struct Block {
    Line line;
    float max_entry_sqr = 0;   // junction limit with the previous block
    float entry_sqr = 0;       // committed plan
    float exit_sqr = 0;
    Trapezoid profile;
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

    void replan();
    /** False if the plan is stale; the caller replans and commits again. */
    bool commit();

    /** The oldest block, now running; null if one is already claimed or none is queued. */
    const Block* claim();
    /** Free the claimed block. */
    void release();

    int count() const { return count_; }
    bool full() const { return count_ == kSize; }
    bool claimed() const { return claimed_; }
    /** Where the last queued line ends. */
    Vec2 end() const { return end_; }

private:
    int index(int i) const { return (tail_ + i) % kSize; }
    int firstUnclaimed() const { return claimed_ ? 1 : 0; }

    Block ring_[kSize];
    int tail_ = 0;
    int count_ = 0;
    bool claimed_ = false;
    Vec2 end_;

    // Exit speed of the most recently claimed block: the entry of the next.
    float pinned_entry_sqr_ = 0;
    // Bumped by every push, claim and release.
    uint32_t epoch_ = 0;

    // replan() output by ring position, valid for commit() while epoch_ matches.
    float plan_entry_sqr_[kSize];
    float plan_exit_sqr_[kSize];
    Trapezoid plan_profile_[kSize];
    uint32_t plan_epoch_ = 0;
    bool plan_valid_ = false;
};

}  // namespace planner

#endif
