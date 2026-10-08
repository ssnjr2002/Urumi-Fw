#include "planner/planner.h"
#include "planner/ram.h"

#include <math.h>

namespace planner {

// Orders the staging slot against its flag between the two sides.
PLANNER_RAM static inline void fence() { __atomic_thread_fence(__ATOMIC_SEQ_CST); }

void Planner::reset(Pos pos) {
    tail_ = 0;
    count_ = 0;
    claimed_ = false;
    end_ = pos;
    pinned_entry_sqr_ = 0;
    plan_valid_ = false;
    clearOffer();
}

PLANNER_RAM void Planner::clearOffer() {
    has_offer_ = false;
    staged_flag_ = false;
}

bool Planner::pushLine(Vec2 target, float feed, const AxisLimits& limits, float deviation) {
    const Line ln = makeLine(end_.xy(), target, feed, limits);
    if (ln.length <= 0) return true;
    if (full()) return false;
    return pushBlock(Block::LINE, pathOf(ln), deviation);
}

bool Planner::pushBezier(const Bezier& bz, float feed, const AxisLimits& limits, float deviation) {
    if (full()) return false;
    ring_[index(count_)].bez = bz;
    return pushBlock(Block::BEZIER, pathOf(bz, feed, limits), deviation);
}

bool Planner::pushBlock(Block::Kind kind, const Path& path, float deviation) {
    Block& b = ring_[index(count_)];
    b.kind = kind;
    b.origin = end_;
    b.path = path;
    b.s0 = 0;
    b.max_entry_sqr = count_ > 0
        ? junctionMaxSqr(ring_[index(count_ - 1)].path, path, deviation)
        : 0;
    // Committed at rest on both ends until the next commit: still consistent
    // with its predecessor, whose committed exit is also rest.
    b.entry_sqr = 0;
    b.exit_sqr = 0;
    b.profile = makeTrapezoid(path.length, path.accel, 0, path.v_max_sqr, 0);

    count_++;
    epoch_++;
    end_.setXy(path.end);
    return true;
}

void Planner::replan(float t) {
    // The epoch before anything else is read: a claim or release from here on
    // makes commit() refuse.
    plan_epoch_ = epoch_;
    plan_valid_ = true;
    plan_wait_ = false;
    plan_offer_ = false;
    const int first = firstUnclaimed();

    // The claimed block's rest, from a horizon: at the pending offer's start
    // while there is still time to replace it, else `ahead_` past `t` on
    // whichever piece is running then.
    bool from_horizon = false;
    float t_h = 0, s_h = 0, v_h_sqr = 0;
    if (claimed_) {
        if (has_offer_ && t < offer_.t0) {
            if (t + guard_ >= offer_.t0) { plan_wait_ = true; return; }
            t_h = offer_.t0;
            s_h = offer_.s0;
            v_h_sqr = offer_.profile.v_entry * offer_.profile.v_entry;
            from_horizon = true;
        } else {
            const Piece& base = has_offer_ ? offer_ : run_;
            t_h = t + ahead_;
            if (t_h < base.end()) {
                const float u = t_h - base.t0;
                s_h = base.s0 + base.profile.position(u);
                const float v = base.profile.velocity(u);
                v_h_sqr = v * v;
                from_horizon = true;
            }
        }
    }

    // Reverse: the newest block stops; each entry is what can still stop in time.
    float next_entry = 0;
    for (int i = count_ - 1; i >= first; i--) {
        const Block& b = ring_[index(i)];
        plan_exit_sqr_[i] = next_entry;
        const float reachable = next_entry + 2.0f * b.path.accel * b.path.length;
        plan_entry_sqr_[i] = fminf(fminf(b.max_entry_sqr, b.path.v_max_sqr), reachable);
        next_entry = plan_entry_sqr_[i];
    }

    // The claimed block's exit rises to what its rest can reach, or stays.
    float entry = pinned_entry_sqr_;
    if (from_horizon) {
        const Block& c = ring_[tail_];
        const float rest = c.path.length - s_h;
        const float exit_sqr = fminf(fminf(next_entry, c.path.v_max_sqr),
                                     v_h_sqr + 2.0f * c.path.accel * fmaxf(rest, 0.0f));
        if (rest > 0 && exit_sqr > c.exit_sqr * (1 + 1e-5f) + 1e-6f) {
            plan_offer_ = true;
            plan_exit_sqr_[0] = exit_sqr;
            plan_piece_.t0 = t_h;
            plan_piece_.s0 = s_h;
            plan_piece_.profile = makeTrapezoid(rest, c.path.accel, v_h_sqr, c.path.v_max_sqr, exit_sqr);
            entry = exit_sqr;
        }
    }

    // Forward: from the claimed block's exit, each exit is what can be reached.
    for (int i = first; i < count_; i++) {
        const Block& b = ring_[index(i)];
        plan_entry_sqr_[i] = entry;
        const float reachable = entry + 2.0f * b.path.accel * b.path.length;
        plan_exit_sqr_[i] = fminf(plan_exit_sqr_[i], reachable);
        plan_profile_[i] = makeTrapezoid(b.path.length, b.path.accel, entry,
                                         b.path.v_max_sqr, plan_exit_sqr_[i]);
        entry = plan_exit_sqr_[i];
    }
}

bool Planner::commit(float t) {
    if (!plan_valid_ || plan_epoch_ != epoch_) return false;
    // The consumer must take an offer before it reaches t0, and may still be
    // copying the last one.
    if (plan_wait_ || (plan_offer_ && (staged_flag_ || !(t + guard_ < plan_piece_.t0)))) {
#ifdef PLANNER_BENCH
        if (plan_wait_) counts.waits++;
        else if (staged_flag_) counts.untaken++;
        else counts.too_close++;
#endif
        plan_valid_ = false;
        return false;
    }
    if (plan_offer_) {
        staged_ = plan_piece_;
        fence();
        staged_flag_ = true;
#ifdef PLANNER_BENCH
        counts.offers++;
#endif
        offer_ = plan_piece_;
        has_offer_ = true;
        ring_[tail_].exit_sqr = plan_exit_sqr_[0];
        pinned_entry_sqr_ = plan_exit_sqr_[0];
    }
    for (int i = firstUnclaimed(); i < count_; i++) {
        Block& b = ring_[index(i)];
        b.entry_sqr = plan_entry_sqr_[i];
        b.exit_sqr = plan_exit_sqr_[i];
        b.profile = plan_profile_[i];
    }
    plan_valid_ = false;
    return true;
}

PLANNER_RAM const Block* Planner::claim() {
    if (claimed_ || count_ == 0) return nullptr;
    claimed_ = true;
    epoch_++;
    const Block& b = ring_[tail_];
    pinned_entry_sqr_ = b.exit_sqr;
    run_.t0 = 0;
    run_.s0 = 0;
    run_.profile = b.profile;
    clearOffer();
    return &b;
}

PLANNER_RAM void Planner::release() {
    if (!claimed_) return;
    claimed_ = false;
    tail_ = (tail_ + 1) % kSize;
    count_--;
    epoch_++;
    clearOffer();
}

PLANNER_RAM bool Planner::takeStaged(Piece& out) {
    if (!staged_flag_) return false;
    fence();
    out = staged_;
    fence();
    staged_flag_ = false;
    return true;
}

void Planner::restartFrom(float s) {
    if (claimed_) {
        claimed_ = false;
        Block& b = ring_[tail_];
        if (s >= b.path.length) {
            tail_ = (tail_ + 1) % kSize;
            count_--;
        } else if (s > 0) {
            b.s0 += s;
            b.path.length -= s;
        }
    }
    if (count_ > 0) {
        Block& b = ring_[tail_];
        b.max_entry_sqr = 0;
        b.entry_sqr = 0;
        b.exit_sqr = 0;
        b.profile = makeTrapezoid(b.path.length, b.path.accel, 0, b.path.v_max_sqr, 0);
    }
    pinned_entry_sqr_ = 0;
    plan_valid_ = false;
    epoch_++;
    clearOffer();
}

}  // namespace planner
