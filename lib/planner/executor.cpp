#include "planner/executor.h"
#include "planner/ram.h"

#include <math.h>

namespace planner {

void Executor::reset(Pos pos) {
    state_ = State::Running;
    aborting_ = false;
    cur_ = nullptr;
    has_next_ = false;
    t_ = 0;
    s_ = 0;
    v_ = 0;
    pos_ = pos;
    clock_ = 0;
}

// `s` mm into what remains of the block.
PLANNER_RAM static Pos pointAt(const Block& b, float s) {
    const float g = b.s0 + s;
    Pos p = b.origin;
    if (b.kind == Block::BEZIER) p.setXy(bezierPoint(b.bez, bezierT(b.bez, g)));
    else p.setXy({b.origin.x + b.path.dir_start.x * g, b.origin.y + b.path.dir_start.y * g});
    return p;
}

PLANNER_RAM void Executor::finishBlock(Planner& p) {
    pos_ = cur_->origin;
    pos_.setXy(cur_->path.end);
    p.release();
    cur_ = nullptr;
    has_next_ = false;
    t_ = 0;
    s_ = 0;
}

PLANNER_RAM void Executor::adopt(Planner& p) {
    Piece in;
    if (!p.takeStaged(in)) return;
    if (!cur_ || state_ != State::Running) return;   // a hold ignores it
    if (t_ >= in.t0) {
        late_++;
        hold();
        return;
    }
    next_ = in;
    has_next_ = true;
#ifdef PLANNER_BENCH
    adoptions++;
#endif
}

PLANNER_RAM Pos Executor::tick(Planner& p, float dt) {
    while (dt > 0 && state_ != State::Held) {
        if (!cur_) {
            // At rest between blocks, a hold is already complete.
            if (state_ == State::Holding && v_ <= 0) { state_ = State::Held; break; }
            cur_ = p.claim();
            if (!cur_) { v_ = 0; if (state_ == State::Holding) state_ = State::Held; break; }
            piece_.t0 = 0;
            piece_.s0 = 0;
            piece_.profile = cur_->profile;
            has_next_ = false;
            t_ = 0;
            s_ = 0;
        }

        if (state_ == State::Running) {
            if (has_next_ && t_ + dt >= next_.t0) {
                dt -= next_.t0 - t_;
                t_ = next_.t0;
                piece_ = next_;
                has_next_ = false;
                s_ = piece_.s0;
                v_ = piece_.profile.v_entry;
                continue;
            }
            const float rem = piece_.end() - t_;
            if (dt < rem) {
                t_ += dt;
                const float u = t_ - piece_.t0;
                s_ = piece_.s0 + piece_.profile.position(u);
                v_ = piece_.profile.velocity(u);
                dt = 0;
            } else {
                dt -= rem;
                v_ = piece_.profile.v_exit;
                finishBlock(p);
            }
            continue;
        }

        // Holding: constant deceleration at this block's accel.
        const float a = cur_->path.accel;
        const float rem_s = cur_->path.length - s_;
        const float s_stop = v_ * v_ / (2 * a);
        if (s_stop <= rem_s) {
            const float t_stop = v_ / a;
            if (t_stop <= dt) {
                s_ += s_stop;
                v_ = 0;
                state_ = State::Held;
                break;
            }
        } else {
            // Reaches the block end still moving.
            const float v_end = sqrtf(fmaxf(0.0f, v_ * v_ - 2 * a * rem_s));
            const float t_end = (v_ - v_end) / a;
            if (t_end <= dt) {
                dt -= t_end;
                v_ = v_end;
                finishBlock(p);
                continue;
            }
        }
        s_ += v_ * dt - 0.5f * a * dt * dt;
        v_ -= a * dt;
        dt = 0;
    }

    if (cur_) pos_ = pointAt(*cur_, s_);
    if (state_ == State::Held && aborting_) {
        p.reset(pos_);
        reset(pos_);
    }
    clock_ = cur_ ? t_ : 0;
    return pos_;
}

PLANNER_RAM bool Executor::needsRing(float dt) const {
    if (aborting_) return true;
    if (!cur_) return state_ != State::Held;
    if (state_ == State::Running) return dt >= (has_next_ ? next_.end() : piece_.end()) - t_;
    if (state_ == State::Held) return false;

    // Holding: does the braking curve reach the block end within dt?
    const float a = cur_->path.accel;
    const float rem_s = cur_->path.length - s_;
    if (v_ * v_ / (2 * a) <= rem_s) return false;
    const float v_end = sqrtf(fmaxf(0.0f, v_ * v_ - 2 * a * rem_s));
    return (v_ - v_end) / a <= dt;
}

void Executor::hold() {
    if (state_ == State::Running) state_ = State::Holding;
}

void Executor::resume(Planner& p) {
    if (state_ != State::Held) return;
    p.restartFrom(cur_ ? s_ : 0);
    cur_ = nullptr;
    has_next_ = false;
    t_ = 0;
    s_ = 0;
    v_ = 0;
    p.replan();
    p.commit();
    state_ = State::Running;
}

void Executor::abort() {
    // Completes on the tick that finds the machine held.
    aborting_ = true;
    hold();
}

}  // namespace planner
