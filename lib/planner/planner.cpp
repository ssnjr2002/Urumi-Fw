#include "planner/planner.h"
#include "planner/ram.h"

#include <math.h>

namespace planner {

void Planner::reset(Vec2 pos) {
    tail_ = 0;
    count_ = 0;
    claimed_ = false;
    end_ = pos;
    pinned_entry_sqr_ = 0;
    plan_valid_ = false;
}

bool Planner::push(Vec2 target, float feed, const AxisLimits& limits, float deviation) {
    const Line ln = makeLine(end_, target, feed, limits);
    if (ln.length <= 0) return true;
    if (full()) return false;
    ring_[index(count_)].origin = ln.p0;
    return pushBlock(pathOf(ln), deviation);
}

bool Planner::pushBlock(const Path& path, float deviation) {
    Block& b = ring_[index(count_)];
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
    end_ = path.end;
    return true;
}

void Planner::replan() {
    const int first = firstUnclaimed();

    // Reverse: the newest block stops; each entry is what can still stop in time.
    float next_entry = 0;
    for (int i = count_ - 1; i >= first; i--) {
        const Block& b = ring_[index(i)];
        plan_exit_sqr_[i] = next_entry;
        const float reachable = next_entry + 2.0f * b.path.accel * b.path.length;
        plan_entry_sqr_[i] = fminf(fminf(b.max_entry_sqr, b.path.v_max_sqr), reachable);
        next_entry = plan_entry_sqr_[i];
    }

    // Forward: from the pinned entry, each exit is what can be reached.
    float entry = pinned_entry_sqr_;
    for (int i = first; i < count_; i++) {
        const Block& b = ring_[index(i)];
        plan_entry_sqr_[i] = entry;
        const float reachable = entry + 2.0f * b.path.accel * b.path.length;
        plan_exit_sqr_[i] = fminf(plan_exit_sqr_[i], reachable);
        plan_profile_[i] = makeTrapezoid(b.path.length, b.path.accel, entry,
                                         b.path.v_max_sqr, plan_exit_sqr_[i]);
        entry = plan_exit_sqr_[i];
    }

    plan_epoch_ = epoch_;
    plan_valid_ = true;
}

bool Planner::commit() {
    if (!plan_valid_ || plan_epoch_ != epoch_) return false;
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
    return &b;
}

PLANNER_RAM void Planner::release() {
    if (!claimed_) return;
    claimed_ = false;
    tail_ = (tail_ + 1) % kSize;
    count_--;
    epoch_++;
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
}

}  // namespace planner
