/**
 * executor.h — runs the planner's blocks in time.
 *
 * tick() advances by dt through the claimed block's profile, releasing and
 * claiming as blocks finish; leftover time carries into the next block. It
 * returns the position along the path; Z and A stay at the block's start.
 *
 * hold() brakes along the path at each block's own acceleration. Since the
 * committed plan is at or above that braking curve everywhere and ends at rest,
 * a hold stops no later than the plan would. resume() replans from rest where
 * the hold stopped; abort() holds and then empties the ring.
 *
 * Runs on the consumer side: it calls claim() and release(), and only calls
 * restartFrom(), replan() and commit() from resume() and abort(), once stopped.
 *
 * The claimed block runs from the executor's own copy of its profile. adopt()
 * takes a Piece the planner staged; it takes over at its t0, and the block
 * then ends where that piece does.
 */

#ifndef PLANNER_EXECUTOR_H
#define PLANNER_EXECUTOR_H

#include "planner/planner.h"

namespace planner {

class Executor {
public:
    enum class State { Running, Holding, Held };

    /** At rest at `pos`; call alongside Planner::reset(). */
    void reset(Pos pos);

    /**
     * Take a staged Piece, without the lock. Call before needsRing(): the piece
     * may end the block sooner.
     */
    void adopt(Planner& p);

    Pos tick(Planner& p, float dt);

    /**
     * True if tick(dt) may claim, release or reset the ring, so the caller
     * must hold its lock. Every other tick touches only the executor and the
     * claimed block, which the producer never writes.
     */
    bool needsRing(float dt) const;

    /** Start braking. No effect unless running. */
    void hold();
    /** Leave a finished hold: replan from rest and run again. */
    void resume(Planner& p);
    /** Hold, then discard every queued block once stopped. */
    void abort();

    State state() const { return state_; }
    Pos position() const { return pos_; }
    float speed() const { return v_; }   // mm/s along the path
    /** Seconds into the claimed block, published after each tick. */
    float clock() const { return clock_; }
    /** Pieces found only after their t0 had passed; each started a hold. */
    uint32_t lateAdoptions() const { return late_; }
#ifdef PLANNER_BENCH
    uint32_t adoptions = 0;
#endif

private:
    void finishBlock(Planner& p);

    State state_ = State::Running;
    bool aborting_ = false;
    const Block* cur_ = nullptr;
    Piece piece_;          // what runs now
    Piece next_;           // takes over at next_.t0
    bool has_next_ = false;
    float t_ = 0;   // s since cur_ was claimed, while running
    float s_ = 0;   // mm into cur_
    float v_ = 0;
    Pos pos_;
    volatile float clock_ = 0;
    uint32_t late_ = 0;
};

}  // namespace planner

#endif
