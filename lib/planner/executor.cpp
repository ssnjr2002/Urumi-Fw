#include "planner/executor.h"
#include "planner/ram.h"

#include <math.h>

namespace planner {

void Executor::reset(Vec2 pos) {
    state_ = State::Running;
    aborting_ = false;
    cur_ = nullptr;
    t_ = 0;
    s_ = 0;
    v_ = 0;
    pos_ = pos;
}

// `s` mm into what remains of the block.
PLANNER_RAM static Vec2 pointAt(const Block& b, float s) {
    const float g = b.s0 + s;
    if (b.kind == Block::BEZIER) return bezierPoint(b.bez, bezierT(b.bez, g));
    return {b.origin.x + b.path.dir_start.x * g, b.origin.y + b.path.dir_start.y * g};
}

PLANNER_RAM void Executor::finishBlock(Planner& p) {
    pos_ = cur_->path.end;
    p.release();
    cur_ = nullptr;
    t_ = 0;
    s_ = 0;
}

PLANNER_RAM Vec2 Executor::tick(Planner& p, float dt) {
    while (dt > 0 && state_ != State::Held) {
        if (!cur_) {
            // At rest between blocks, a hold is already complete.
            if (state_ == State::Holding && v_ <= 0) { state_ = State::Held; break; }
            cur_ = p.claim();
            if (!cur_) { v_ = 0; if (state_ == State::Holding) state_ = State::Held; break; }
            t_ = 0;
            s_ = 0;
        }

        if (state_ == State::Running) {
            const Trapezoid& pr = cur_->profile;
            const float rem = pr.duration() - t_;
            if (dt < rem) {
                t_ += dt;
                s_ = pr.position(t_);
                v_ = pr.velocity(t_);
                dt = 0;
            } else {
                dt -= rem;
                v_ = pr.v_exit;
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
    return pos_;
}

PLANNER_RAM bool Executor::needsRing(float dt) const {
    if (aborting_) return true;
    if (!cur_) return state_ != State::Held;
    if (state_ == State::Running) return dt >= cur_->profile.duration() - t_;
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
