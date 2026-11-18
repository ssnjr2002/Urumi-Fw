// leg.cpp — the node's leg pulser on TCA0. See leg.h.
#include <Arduino.h>
#include "leg.h"
#include "common.h"
#include "node_hooks.h"
#include "hall_index.h"

#ifdef HAS_HOMING

#ifdef HAS_LIMIT_SWITCH
// Pressed samples in a row, one per step, before a seek or park believes its
// switch, so one transient does not stop it. Samples, not time: each one is a
// step paid on every real press.
#define LEG_LIMIT_SAMPLES 3
#endif

// TCA0 ticks per microsecond with the div8 prescaler: 3 at 24 MHz, 2 at 20 MHz
// (truncated). Legs arrive in microseconds, so the controller never needs the
// node's clock.
#define LEG_TICKS_PER_US ((F_CPU / 1000000UL) / 8UL)

// ─── Pulser state ───────────────────────────────────────────────────────────
// Written at arm time in loop context, then owned by the pulser ISR until it
// stops; loop context must not touch it while `legActive` is set.
struct LegState {
    uint16_t interval;    // current step interval, TCA0 ticks
    uint16_t floorTicks;  // fastest interval this move is allowed to reach
    uint16_t rampStep;    // ticks shaved per step until floorTicks; 0 = no ramp
    uint16_t startTicks;  // the interval a park ramps back down to
    uint16_t upSteps;     // steps a park spent ramping up, so it ramps down as many
    uint32_t remaining;   // runaway budget, in steps; a park's distance
    bool     dir;         // wire dir bit for the whole move
    LegMode  mode;
    uint8_t  limitRun;    // consecutive asserted samples, for the seek's debounce
};
static volatile LegState leg = {0, 0, 0, 0, 0, 0, false, LEG_SEEK, 0};
volatile bool legActive = false;

// Set by the pulser when a seek or park stopped on its switch (debounced), not
// its budget. legFinish() turns it into limitLatched.
static volatile bool legHitLimit = false;

// Set by the pulser when it stops, consumed once by legLoop(): the finish writes
// the flags, which only loop context may do.
static volatile bool legFinished = false;
#ifdef HAS_HALL_INDEX
// True from the moment the pulser stops until the sliced correlation has an
// answer. Not volatile: written and read only in loop context.
static bool legResolving = false;
#endif

// ─── Leg span ────────────────────────────────────────────────────────────────
// How far the last finished leg moved, in signed steps, failed or not, from
// the counter at the arm. Reported in the status tail.
static int32_t legSpanFrom  = 0;
static int32_t legSpan      = 0;

int32_t legSpanSteps(void) { return legSpan; }

static int32_t readPositionAtomic(void) {
    cli();
    int32_t pos = absolutePosition;
    sei();
    return pos;
}

// ─── Arming a leg ───────────────────────────────────────────────────────────
// Refuses rather than clamps: each refusal is a mistake on the controller's
// side, and a clamped leg would run at a rate nobody asked for.
uint8_t legArm(LegMode mode, bool dir, uint16_t startUs, uint16_t floorUs,
               uint16_t rampSteps, uint32_t maxSteps) {
    if (legActive)                    return NAK_BUSY;   // one leg at a time
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
    h.startTicks = (uint16_t)startTicks;
    h.upSteps    = 0;
    h.remaining  = maxSteps;
    h.limitRun   = 0;
    // rampSteps == 0: no ramp, start at the floor.
    h.interval   = rampSteps ? (uint16_t)startTicks : (uint16_t)floorTicks;
    h.rampStep   = rampSteps ? (uint16_t)((startTicks - floorTicks) / rampSteps) : 0;

    cli();
    leg         = h;
    legActive   = true;
    legHitLimit = false;
    // Span start, in the same cli() as the arm so it is not a step late.
    legSpanFrom = absolutePosition;
#ifdef HAS_HALL_INDEX
    // `sign` is what one pulser step adds to the counter; backwards, the index
    // lands mirrored about the start.
    if (mode == LEG_SWEEP) hallIndexArm(dir ? 1 : -1, absolutePosition);
#endif
    sei();

    // DIR once, here, so the pulser never waits for its setup time.
    if (dir) HAL_DIR_PORT.OUTSET = HAL_DIR_BM;
    else     HAL_DIR_PORT.OUTCLR = HAL_DIR_BM;
    currentDir = dir;
    delayMicroseconds(5);

    node_set_flag(NODE_FLAG_LEG, true);

    // Start TCA0: normal 16-bit mode, overflow interrupt only, PER the step
    // interval. TCB0 still shapes the pulse. No stepper build uses PWM, and
    // millis() is on a TCB, so TCA0 is free.
    //
    // The core leaves TCA0 running in split mode for analogWrite(), where PER
    // is two 8-bit registers, so split mode must be cleared (CTRLD). CTRLD
    // ignores writes while the timer is enabled, so CTRLA is cleared first.
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

// Safe from either context. TCA0 stays in single mode: no stepper build uses
// PWM.
void legHalt(void) {
    TCA0.SINGLE.CTRLA   = 0;
    TCA0.SINGLE.INTCTRL = 0;
    legActive   = false;
    legFinished = true;
}

// The loop-context half of stopping, run once per completed leg.
static void legFinish(void) {
    legFinished = false;
    // Close the span, failed or not: where a failed leg stopped is the
    // diagnostic.
    legSpan = readPositionAtomic() - legSpanFrom;
#ifdef HAS_HALL_INDEX
    // Resolve the window, sliced across legLoop() passes so the bus is served.
    if (leg.mode == LEG_SWEEP) {
        hallIndexResolveBegin();
        legResolving = true;
    }
#endif
#ifdef HAS_LIMIT_SWITCH
    // A retract clears the latch only if it ended clear of the switch.
    if (leg.mode == LEG_RETRACT && !HAL_LIMIT_ASSERTED()) {
        limitLatched   = false;
        limitRunBase   = limitBytesAsserted;   // the next run starts from here
    }
    // A seek or park that stopped on its switch latches, so the fact survives
    // the pin bouncing before the controller's next poll.
    if ((leg.mode == LEG_SEEK || leg.mode == LEG_PARK) && legHitLimit)
        limitLatched = true;
#endif
}

void legLoop(void) {
    // Finish before publishing: legFinish() can change the latch.
    if (legFinished) legFinish();
#ifdef HAS_HALL_INDEX
    // One bounded slice per pass: commands are dispatched from loop(), and the
    // whole correlation (~170 ms) would miss the controller's polls.
    if (legResolving && hallIndexResolveStep()) legResolving = false;
#endif
    // NODE_FLAG_LEG means no answer yet, so it covers the resolve too.
    node_set_flag(NODE_FLAG_LEG, legActive
#ifdef HAS_HALL_INDEX
                                 || legResolving
#endif
                  );
}

// ─── The pulser ─────────────────────────────────────────────────────────────
// One step per overflow. No floating point, no delays, no bus work.
ISR(TCA0_OVF_vect) {
    TCA0.SINGLE.INTFLAGS = TCA_SINGLE_OVF_bm;
    if (!legActive) return;

    // Stop conditions come before the step, so a leg never steps past what
    // ended it. A retract starts on its switch and ignores it; only its budget
    // ends it.
#ifdef HAS_LIMIT_SWITCH
    // A park starts clear of the switch, so meeting it is a fault to stop at.
    if (leg.mode == LEG_SEEK || leg.mode == LEG_PARK) {
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
    absolutePosition += (leg.dir ? 1 : -1);
    HAL_STEP_TIMER_INST.CCMP  = HAL_STEP_PULSE_CCMP;
    HAL_STEP_TIMER_INST.CNT   = 0;
    HAL_STEP_TIMER_INST.CTRLA = HAL_STEP_TIMER_CLKSEL | HAL_STEP_TIMER_ENABLE_bm;

    leg.remaining--;

#ifdef HAS_HALL_INDEX
    // After the step, so the sample belongs to the position just reached.
    if (leg.mode == LEG_SWEEP && hallIndexSample(absolutePosition)) { legHalt(); return; }
#endif

    // The interval decays linearly toward the floor, gentler than constant
    // acceleration at the start, where a motor stalls. A park ramps back down
    // over the steps it took to ramp up, so a short park is a triangle.
    if (leg.mode == LEG_PARK && leg.remaining <= leg.upSteps) {
        if (leg.rampStep) {
            uint16_t next = leg.interval + leg.rampStep;
            if (next > leg.startTicks) next = leg.startTicks;
            leg.interval = next;
            TCA0.SINGLE.PER = next - 1;
        }
    } else if (leg.rampStep && leg.interval > leg.floorTicks) {
        uint16_t next = leg.interval - leg.rampStep;
        if (next < leg.floorTicks) next = leg.floorTicks;  // never overshoot
        leg.interval = next;
        TCA0.SINGLE.PER = next - 1;
        if (leg.mode == LEG_PARK) leg.upSteps++;
    }
}

#endif  // HAS_HOMING
