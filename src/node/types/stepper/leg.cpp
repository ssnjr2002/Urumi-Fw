// leg.cpp — the node's leg pulser on TCA0. See leg.h.
#include <Arduino.h>
#include "leg.h"
#include "common.h"
#include "node_hooks.h"
#include "hall_index.h"

#ifdef HAS_HOMING

#ifdef HAS_LIMIT_SWITCH
// Consecutive ASSERTED pulser samples before a seek believes its switch.
//
// The stream path rejects glitches and the pulser once did not: it halted on a
// single port read, so one transient assert -- a motor cable coupling into the
// switch line, a drag chain flexing -- stopped a seek dead with its budget
// barely touched. Worse, it stopped SILENTLY: nothing latched, so by the time
// the supervisor polled 25 ms later the pin had released and the leg was
// indistinguishable from one that never found its switch at all. It reported
// "switch never reached within 211200 steps" for an axis that had stopped at
// 26241 and was nowhere near anything.
//
// Three samples, not the stream path's 500 ms: this is a distance, and it is
// paid on every genuine trip. At the floor interval that is three steps -- tens
// of microns -- against a seek whose whole purpose is to arrive at this switch.
// The stream path can afford to be slow because it is deciding whether a
// refusal becomes STICKY, not whether to refuse.
#define LEG_LIMIT_SAMPLES 3
#endif

// TCA0 ticks per microsecond, from the board's own F_CPU with the div8
// prescaler: 3 on the 24MHz DB32, 2 (truncated from 2.5) on a 20MHz ATtiny.
// This is why CMD_HOME_LEG carries microseconds — the difference dies here and
// never reaches the master or the config schema.
#define LEG_TICKS_PER_US ((F_CPU / 1000000UL) / 8UL)

// ─── Pulser state ───────────────────────────────────────────────────────────
// Written once at arm time in loop context, then owned by the pulser ISR until
// it stops. `legActive` is the handshake between the two: loop context must not
// touch the rest while it is set.
//
// A seek or retract is decided by ONE read of the limit pin at arm time and
// never revisited (docs/homing.md 1.2); a rotary build has no pin and only
// sweeps.
struct LegState {
    uint16_t interval;    // current step interval, TCA0 ticks
    uint16_t floorTicks;  // fastest interval this move is allowed to reach
    uint16_t rampStep;    // ticks shaved per step until floorTicks; 0 = no ramp
    uint32_t remaining;   // runaway budget, in steps
    bool     dir;         // wire dir bit for the whole move
    LegMode  mode;
    uint8_t  limitRun;    // consecutive asserted samples, for the seek's debounce
};
static volatile LegState leg = {0, 0, 0, 0, false, LEG_SEEK, 0};
volatile bool legActive = false;

// Set by the pulser ISR when a SEEK stopped because its switch genuinely
// asserted (debounced), as opposed to running its budget out. legFinish()
// turns it into limitLatched.
//
// Without this a seek's success was inferred from the pin still reading
// asserted whenever the supervisor happened to poll, which makes a real trip
// that bounces look like a leg that found nothing.
static volatile bool legHitLimit = false;

// Set by the pulser ISR when it stops, consumed once by legLoop(). The ISR
// cannot do the finishing itself: clearing the latch and publishing the flags
// both go through node_set_flag(), which read-modify-writes a byte the core also
// owns and is therefore loop-context only.
static volatile bool legFinished = false;
#ifdef HAS_HALL_INDEX
// True from the moment the pulser stops until the sliced correlation has an
// answer. Not volatile: written and read only in loop context.
static bool legResolving = false;
#endif

// ─── Leg span ────────────────────────────────────────────────────────────────
// How far the last COMPLETED leg actually moved: the counter at the arm, and
// the difference once it stops. Reported in the status tail, so every leg is
// self-describing.
//
// One leg, not a sequence: the node sees individual legs and has no sequence
// context, so composing legs into "distance from the far stop to the datum" is
// the master's job. It lives here rather than on the master because the BENCH
// path has no master sequencer -- every value in docs/homing.md §7 was found by
// driving one node directly.
//
// Signed and in raw steps -- the sign catches a leg that ran the wrong way.
// Both directions of the subtraction survive a failed leg on purpose: where a
// leg stopped IS the diagnostic when it stopped somewhere unexpected.
static int32_t legSpanFrom  = 0;
static int32_t legSpan      = 0;

int32_t legSpanSteps(void) { return legSpan; }

static int32_t readPositionAtomic(void) {
    cli();
    int32_t pos = absolutePosition;
    sei();
    return pos;
}

// ─── Arming a leg (§1.4) ────────────────────────────────────────────────────
// Rejecting rather than clamping is deliberate. Every refusal below is a config
// or arithmetic mistake on the host side, and a clamped leg would run at a rate
// nobody asked for, into a hard stop, while reporting success.
uint8_t legArm(LegMode mode, bool dir, uint16_t startUs, uint16_t floorUs,
               uint16_t rampSteps, uint32_t maxSteps) {
    // The one refusal that is not the host's fault and not permanent: the same
    // frame is correct, just early.
    if (legActive)                    return NAK_BUSY;   // one move at a time
    if (startUs == 0 || floorUs == 0) return NAK_BAD_ARG;
    if (floorUs > startUs)            return NAK_BAD_ARG;  // floor is the FASTER rate
    if (maxSteps == 0)                return NAK_BAD_ARG;  // no budget = no runaway guard

    const uint32_t startTicks = (uint32_t)startUs * LEG_TICKS_PER_US;
    const uint32_t floorTicks = (uint32_t)floorUs * LEG_TICKS_PER_US;
    if (startTicks > 0xFFFF || startTicks == 0) return NAK_BAD_ARG;  // TCA0 is 16-bit
    if (floorTicks == 0)                        return NAK_BAD_ARG;

    LegState h;
    h.dir        = dir;
    h.mode       = mode;
    h.floorTicks = (uint16_t)floorTicks;
    h.remaining  = maxSteps;
    h.limitRun   = 0;
    // ramp_steps == 0 means no ramp: start at the cruise rate rather than
    // ramping over zero steps, which would be a divide by zero.
    h.interval   = rampSteps ? (uint16_t)startTicks : (uint16_t)floorTicks;
    h.rampStep   = rampSteps ? (uint16_t)((startTicks - floorTicks) / rampSteps) : 0;

    cli();
    leg         = h;
    legActive   = true;
    legHitLimit = false;
    // Span start. Captured inside the same cli() as the arm so it cannot be
    // taken a step late -- the stream path can still be advancing the counter
    // right up to here.
    legSpanFrom = absolutePosition;
#ifdef HAS_HALL_INDEX
    // Same cli() for the same reason. `sign` is what one pulser step adds to
    // the counter (see the ISR), and the dip window is mapped back to absolute
    // steps through it — get it backwards and the index lands mirrored about
    // the start, which is a plausible-looking wrong answer rather than a
    // failure.
    if (mode == LEG_SWEEP) hallIndexArm(dir ? 1 : -1, absolutePosition);
#endif
    sei();

    // DIR is set here, once, in loop context — so the pulser ISR never pays the
    // DM542 setup guard the stream path pays with delayMicroseconds(5).
    if (dir) HAL_DIR_PORT.OUTSET = HAL_DIR_BM;
    else     HAL_DIR_PORT.OUTCLR = HAL_DIR_BM;
    currentDir = dir;
    delayMicroseconds(5);

    node_set_flag(NODE_FLAG_LEG, true);

    // Start TCA0. Normal mode, 16-bit, overflow interrupt only: PER is the step
    // interval and the ISR rewrites it as the ramp decays. TCB0 stays the
    // pulse-width one-shot for both this path and the stream path, so a step is
    // shaped identically however it was requested.
    //
    // TCA0 is otherwise unused on a stepper node. It backs analogWrite() PWM in
    // the core, which nothing here calls; millis() is on a TCB (DxCore default),
    // so taking TCA0 does not disturb timekeeping.
    //
    // CTRLD.SPLITM must be cleared to leave split mode. DxCore's init_TCA0()
    // runs before setup() and leaves TCA0 in SPLIT mode, RUNNING, for
    // analogWrite(). In split mode PER is two independent 8-bit registers at the
    // same addresses, so a 16-bit interval written through the SINGLE view
    // silently splits into two ~30-tick periods and every leg runs at roughly
    // 250x the requested rate.
    //
    // CTRLD IS ENABLE-LOCKED (datasheet; Microchip's DxCore takeover guide,
    // linked from docs/homing.md): a write to CTRLD while CTRLA.ENABLE is set is
    // silently DROPPED. So CTRLA is cleared FIRST.
    TCA0.SINGLE.CTRLA   = 0;                       // disable — unlocks CTRLD
    TCA0.SPLIT.CTRLD    = 0;                       // now this actually lands: exit split mode
    TCA0.SINGLE.CTRLB   = 0;                       // NORMAL (single 16-bit)
    TCA0.SINGLE.CNT     = 0;
    TCA0.SINGLE.PER     = h.interval - 1;          // PER+1 ticks per overflow
    TCA0.SINGLE.INTFLAGS = TCA_SINGLE_OVF_bm;      // discard any stale flag
    TCA0.SINGLE.INTCTRL = TCA_SINGLE_OVF_bm;
    TCA0.SINGLE.CTRLA   = TCA_SINGLE_CLKSEL_DIV8_gc | TCA_SINGLE_ENABLE_bm;
    return 0;                                      // armed and pulsing
}

// Safe from either context. Deliberately leaves TCA0 in SINGLE mode rather than
// restoring DxCore's SPLIT startup state: nothing on a stepper build calls
// analogWrite() (build_src_filter compiles one type per binary), so there is
// nothing to hand the timer back to.
void legHalt(void) {
    TCA0.SINGLE.CTRLA   = 0;
    TCA0.SINGLE.INTCTRL = 0;
    legActive   = false;
    legFinished = true;
}

// The loop-context half of stopping, run once per completed leg.
static void legFinish(void) {
    legFinished = false;
    // Close the span. Unconditional: a leg that failed still went somewhere, and
    // that distance is exactly what a failure needs to be diagnosed
    // (docs/homing.md §7.2).
    legSpan = readPositionAtomic() - legSpanFrom;
#ifdef HAS_HALL_INDEX
    // Reduce the window the ISR buffered. A dip's centre is only knowable after
    // passing it, so there was never an ISR-sized answer to compute. Begin()
    // only; the correlation is sliced across legLoop passes so the bus keeps
    // being served.
    if (leg.mode == LEG_SWEEP) {
        hallIndexResolveBegin();
        legResolving = true;
    }
#endif
#ifdef HAS_LIMIT_SWITCH
    // Clearing the latch is the retract's ONLY write to the gate, and only when
    // it verifiably got clear of the switch: a retract that spent its whole
    // budget and is still asserted did not escape (under-budgeted, wrong
    // direction, or a stuck switch), and the latch must survive that.
    if (leg.mode == LEG_RETRACT && !HAL_LIMIT_ASSERTED()) {
        limitLatched   = false;
        limitRunBase   = limitBytesAsserted;   // the next run starts from here
    }
    // A seek that stopped ON its switch latches, so the fact survives the pin
    // bouncing before the supervisor's next poll. §1.5's "after a seek, LIMIT
    // set means found" rests on this.
    if (leg.mode == LEG_SEEK && legHitLimit) limitLatched = true;
#endif
}

void legLoop(void) {
    // Finish before publishing: legFinish() can clear the latch, and the flags
    // must describe the state the master will act on.
    if (legFinished) legFinish();
#ifdef HAS_HALL_INDEX
    // The rotary correlation runs HERE, one bounded slice per pass, because a
    // node dispatches RS485 commands from loop() -- the RX ISR only queues them.
    // Run to completion in one go it took ~170 ms, during which this node
    // answered nothing and the supervisor failed the leg on 4 missed polls.
    if (legResolving && hallIndexResolveStep()) legResolving = false;
#endif
    // NODE_FLAG_LEG means "no answer yet", not "the motor is still turning".
    // The resolve is part of the sweep: publishing clear before the index exists
    // would hand the master the PREVIOUS sweep's cause. Core 0's own deadline
    // still bounds the whole thing.
    node_set_flag(NODE_FLAG_LEG, legActive
#ifdef HAS_HALL_INDEX
                                 || legResolving
#endif
                  );
}

// ─── The pulser (§1.3) ──────────────────────────────────────────────────────
// One step per overflow. Deliberately lean: no floating point, no
// delayMicroseconds, no bus work. DIR was set once at arm time, so unlike the
// stream path there is no setup guard to spin on here.
ISR(TCA0_OVF_vect) {
    TCA0.SINGLE.INTFLAGS = TCA_SINGLE_OVF_bm;
    if (!legActive) return;

    // Stop conditions are checked BEFORE the step, so the move never takes one
    // more step past the thing that ended it. On a seek that matters
    // physically: the switch is the target, and overshooting it is travel into
    // the hard stop.
    //
    // A retract ignores the switch entirely — it starts on an asserted one, so
    // testing the level would stop it before it ever moved. Its only terminator
    // is the budget, which is therefore a distance, not a guard.
#ifdef HAS_LIMIT_SWITCH
    if (leg.mode == LEG_SEEK) {
        // Debounced. A run that breaks before LEG_LIMIT_SAMPLES was a glitch
        // and the seek carries on; one that reaches it is the switch, and is
        // RECORDED as such rather than left for the supervisor to re-read off a
        // pin that may have released by the time it looks.
        if (HAL_LIMIT_ASSERTED()) {
            if (++leg.limitRun >= LEG_LIMIT_SAMPLES) {
                legHitLimit = true;
                legHalt();
                return;
            }
        } else {
            leg.limitRun = 0;
        }
    }
#endif
    if (leg.remaining == 0)                      { legHalt(); return; }

    HAL_STEP_PORT.OUTSET = HAL_STEP_BM;
    absolutePosition += (leg.dir ? 1 : -1);      // one counter, one meaning (§4)
    HAL_STEP_TIMER_INST.CCMP  = HAL_STEP_PULSE_CCMP;
    HAL_STEP_TIMER_INST.CNT   = 0;
    HAL_STEP_TIMER_INST.CTRLA = HAL_STEP_TIMER_CLKSEL | HAL_STEP_TIMER_ENABLE_bm;

    leg.remaining--;

#ifdef HAS_HALL_INDEX
    // Sampled AFTER the step, so the sample belongs to the position just
    // reached — which is what makes the window's step tags exact rather than
    // off by one. The opposite order from the limit check above, and
    // necessarily so: a switch is a reason NOT to take the next step, while a
    // dip sample is a measurement OF the step just taken.
    if (leg.mode == LEG_SWEEP && hallIndexSample(absolutePosition)) { legHalt(); return; }
#endif

    // Linear decay of the interval toward the floor. Not constant acceleration
    // (that falls as ~1/sqrt(n)), but gentler early, which is the direction that
    // matters for not stalling on pull-in.
    if (leg.rampStep && leg.interval > leg.floorTicks) {
        uint16_t next = leg.interval - leg.rampStep;
        if (next < leg.floorTicks) next = leg.floorTicks;  // never overshoot
        leg.interval = next;
        TCA0.SINGLE.PER = next - 1;
    }
}

#endif  // HAS_HOMING
