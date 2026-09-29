/**
 * executor.h — runs the planner's blocks in time.
 *
 * tick() advances by dt through the claimed block's profile, releasing and
 * claiming as blocks finish; leftover time carries into the next block. It
 * returns the XY position along the path.
 *
 * hold() brakes along the path at each block's own acceleration. Since the
 * committed plan is at or above that braking curve everywhere and ends at rest,
 * a hold stops no later than the plan would. resume() replans from rest where
 * the hold stopped; abort() holds and then empties the ring.
 *
 * Runs on the consumer side: it calls claim() and release(), and only calls
 * restartFrom(), replan() and commit() from resume() and abort(), once stopped.
 */

#ifndef PLANNER_EXECUTOR_H
#define PLANNER_EXECUTOR_H

#include "planner/planner.h"

namespace planner {

class Executor {
public:
    enum class State { Running, Holding, Held };

    /** At rest at `pos`; call alongside Planner::reset(). */
    void reset(Vec2 pos);

    Vec2 tick(Planner& p, float dt);

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
    Vec2 position() const { return pos_; }
    float speed() const { return v_; }   // mm/s along the path

private:
    void finishBlock(Planner& p);

    State state_ = State::Running;
    bool aborting_ = false;
    const Block* cur_ = nullptr;
    float t_ = 0;   // s into cur_'s profile, while running
    float s_ = 0;   // mm into cur_
    float v_ = 0;
    Vec2 pos_;
};

}  // namespace planner

#endif
