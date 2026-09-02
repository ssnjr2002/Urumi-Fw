// stepper.cpp — stepper node type.
// Owns the RX ISR (so the time-critical stream path inlines with no register
// spill), the step-pulse one-shot timer, position tracking, and the core↔type
// hooks (node_type / node_setup / node_set_enabled / node_handle_command).
//
// All board-specific symbols (USART instance, step/dir ports, timer peripheral,
// motor enable polarity) come from the HAL contract via board.h +
// stepper/stepper.h → board/hal/. After preprocessing these are direct register
// accesses — zero indirection, safe inside ISRs.
#include <Arduino.h>
#include "board.h"
#include "stepper/stepper.h"
#include "common.h"
#include "node_hooks.h"
#include "rs485/frame.h"
#include "hall_index.h"

// drivers_init() is declared by hal_stepper.h (via motor.h). A weak no-op
// default lives in board/hal/motor.cpp (always compiled); a board with real
// driver init (AVR128DB32 TMC/DRV) provides a strong override in its
// drivers.cpp. Call unconditionally — no null check needed.

// ─── Stream slot (runtime-assigned via CMD_ENGAGE) ──────────────────────────
// The stream byte is four 2-bit slots (bit(2n)=step, bit(2n+1)=dir). Which slot
// this node reads is NO LONGER derived from NODE_ID — it is assigned at runtime
// by the Pico's axis map (docs/engage_and_axis_map.md §4). The node boots
// DISENGAGED (slot == SLOT_NONE): masks are 0, so it ignores every stream byte
// and its position freezes until an ENGAGE binds it to a slot.
enum Slot : uint8_t {
    SLOT_X = 0,
    SLOT_Y,
    SLOT_Z,
    SLOT_A,
    SLOT_NONE = 0xFF,
};

// ─── Stepper state ──────────────────────────────────────────────────────────
// slot/masks change at runtime (ENGAGE handler, main-loop context) and are read
// in the RX ISR → volatile.
static volatile int32_t absolutePosition = 0;
static volatile uint8_t slot             = SLOT_NONE;
static volatile uint8_t stepBitMask      = 0;
static volatile uint8_t dirBitMask       = 0;
static bool             currentDir       = false;

#ifdef HAS_LIMIT_SWITCH
// ─── Limit gate ──────────────────────────────────────────────────
// Compiled only on boards that actually have a switch wired. The gate is
// unconditional and stateless with respect to direction: while the switch reads
// asserted, this node refuses EVERY stream step, both ways. It is not a homing
// feature and does not care whether a homing move is in progress — a machine
// that has run onto a hard stop must stop, whatever put it there.
//
// Refusing both directions means the stream can never drive off the switch
// again. That is deliberate: recovery is a homing RETRACT (docs/homing.md §1),
// which is supervised and step-budgeted, rather than a stream the Pico is
// emitting open-loop with no idea the axis is pinned.
//
// The coupling with the pulser runs one way only. The pulser NEVER reads these
// — it samples the pin directly when a CMD_HOME arrives, and that one read
// picks its mode: clear → seek (stop when the level asserts), asserted →
// retract (ignore the switch, run the step budget out). Deciding from the pin
// rather than from the latch is what makes a boot with the axis already parked
// on its switch retract correctly on the FIRST command, with no sentinel and no
// wasted move. The pulser only ever WRITES here, and only in one case: clearing
// limitLatched after a retract that both ran its budget and left the pin clear.
//
// TIME BASE. The accumulator counts stream BYTES, not steps, because the byte
// rate is fixed by the baud rate and the step rate is not: a stream byte is 11
// bit-times (start + 9 data + stop) at RS485_BAUD, so each one is a known,
// constant tick with no timer read inside the ISR. Steps would have measured
// distance, not time, and a slow axis would take minutes to reach a threshold a
// fast one crossed in half a second.
#define LIMIT_STREAM_BYTES_PER_SEC ((uint32_t)RS485_BAUD / 11u)
#define LIMIT_LATCH_MS             500u
#define LIMIT_LATCH_BYTES          (LIMIT_STREAM_BYTES_PER_SEC * LIMIT_LATCH_MS / 1000u)

// Consecutive ASSERTED pulser samples before a seek believes its switch.
//
// The stream path above rejects glitches and the homing pulser did not: it
// halted on a single port read, so one transient assert -- a motor cable
// coupling into the switch line, a drag chain flexing -- stopped a seek dead
// with its budget barely touched. Worse, it stopped SILENTLY: nothing latched,
// so by the time the supervisor polled 25 ms later the pin had released and the
// leg was indistinguishable from one that never found its switch at all. It
// reported "switch never reached within 211200 steps" for an axis that had
// stopped at 26241 and was nowhere near anything.
//
// Three samples, not the stream path's 500 ms: this is a distance, and it is
// paid on every genuine trip. At the floor interval that is three steps -- tens
// of microns -- against a seek whose whole purpose is to arrive at this switch.
// The stream path can afford to be slow because it is deciding whether a
// refusal becomes STICKY, not whether to refuse.
#define HOMING_LIMIT_SAMPLES       3

// Monotonic — counts every stream byte ever seen while asserted, and is NEVER
// reset. It is the lifetime diagnostic: a switch that keeps chattering racks up
// a total even though no single run ever latched, which is exactly the
// intermittent fault that is otherwise invisible from the bus. The current run
// is (total − base), with base snapshotted each time the switch releases, so
// the run comparison costs the diagnostic nothing.
static volatile uint32_t limitBytesAsserted = 0;
static volatile uint32_t limitRunBase       = 0;
static volatile bool     limitLatched       = false;
#endif  // HAS_LIMIT_SWITCH

#ifdef HAS_HOMING
// ─── Homing pulser state (CMD_HOME) ─────────────────────────────────────────
// Written once at arm time in loop context, then owned by the pulser ISR until
// it stops. `active` is the handshake between the two: loop context must not
// touch the rest while it is set.
//
// `retract` is decided by ONE read of the limit pin at arm time and never
// revisited (docs/homing.md 1.2). It is not carried in the payload and is not
// remembered across commands. On a rotary build there is no pin and no mode: it
// is forced false, and the ISR's terminator is a completed dip instead.
struct HomingState {
    uint16_t interval;    // current step interval, TCA0 ticks
    uint16_t floorTicks;  // fastest interval this move is allowed to reach
    uint16_t rampStep;    // ticks shaved per step until floorTicks; 0 = no ramp
    uint32_t remaining;   // runaway budget, in steps
    bool     dir;         // wire dir bit for the whole move
    bool     retract;     // true = ignore the switch; false = stop when it asserts
    uint8_t  limitRun;    // consecutive asserted samples, for the debounce above
};
static volatile HomingState homing  = {0, 0, 0, 0, false, false};
static volatile bool        homingActive = false;

// TCA0 ticks per microsecond, from the board's own F_CPU with the div8
// prescaler: 3 on the 24MHz DB32, 2 (truncated from 2.5) on a 20MHz ATtiny.
// This is why CMD_HOME carries microseconds — the difference dies here and never
// reaches the master or the config schema.
#define HOMING_TICKS_PER_US ((F_CPU / 1000000UL) / 8UL)

// Set by the pulser ISR when a SEEK stopped because its switch genuinely
// asserted (debounced), as opposed to running its budget out. homingFinish()
// turns it into limitLatched.
//
// Without this a seek's success was inferred from the pin still reading
// asserted whenever the supervisor happened to poll, which is a different
// question -- it makes a real trip that bounces look like a leg that found
// nothing, and there is no way to tell the two apart after the fact.
static volatile bool homingHitLimit = false;

// Set by the pulser ISR when it stops, consumed once by node_loop(). The ISR
// cannot do the finishing itself: clearing the latch and publishing the flags
// both go through node_set_flag(), which read-modify-writes a byte the core also
// owns and is therefore loop-context only.
static volatile bool homingFinished = false;

// ─── Leg span ────────────────────────────────────────────────────────────────
// How far the last COMPLETED homing leg actually moved: the counter at the arm,
// and the difference once it stops. Reported in the status tail, so every leg
// is self-describing.
//
// One leg, not a sequence. An earlier design tried to span a seek/retract PAIR
// and immediately ran aground on the fact that a full home is two pairs and the
// node cannot tell which one it is in -- it sees individual legs and has no
// sequence context at all. Per-leg is the primitive the node can actually
// answer for; composing legs into "distance from the far stop to the datum" is
// the master's job, and it has the leg boundaries to do it with.
//
// It matters that this lives here rather than on the master, even though the
// master could subtract two CMD_NODE_STATUS reads and get the same number: the
// BENCH path has no master sequencer. Every value in docs/homing.md §7 was
// found by driving one node directly with the raw console `home` command, and
// that is how the axes get brought up. Bracketing is unavailable there; this is
// not.
//
// Signed and in raw steps -- the sign catches a leg that ran the wrong way, and
// steps stay steps because stepsPerUnit is host config that no node has ever
// seen. Both directions of the subtraction survive a failed leg on purpose:
// where a leg stopped IS the diagnostic when it stopped somewhere unexpected.
static int32_t homingSpanFrom  = 0;
static int32_t homingSpanSteps = 0;

static int32_t readPositionAtomic(void);
static uint8_t homingArm(bool dir, bool retract, uint16_t startUs, uint16_t floorUs,
                         uint16_t rampSteps, uint32_t maxSteps);
static void homingHalt(void);
static void homingFinish(void);
#endif  // HAS_HOMING

// ─── Hooks ──────────────────────────────────────────────────────────────────
// Guard against an env that compiles this type dir with the wrong identity flag.
#ifdef NODE_TYPE
static_assert(NODE_TYPE == NODE_TYPE_STEPPER,
              "stepper.cpp compiled with a non-stepper -DNODE_TYPE");
#endif

uint8_t node_type(void) { return NODE_TYPE_STEPPER; }

void node_setup(void) {
    pinMode(HAL_STEP_PIN, OUTPUT); digitalWrite(HAL_STEP_PIN, LOW);
    pinMode(HAL_DIR_PIN,  OUTPUT); digitalWrite(HAL_DIR_PIN,  LOW);
    pinMode(HAL_EN_PIN,   OUTPUT);

#ifdef HAS_LIMIT_SWITCH
    // Input with pull-up: the switch pulls to ground, so asserted reads LOW and
    // a broken wire reads asserted too — the safe way round.
    pinMode(HAL_LIMIT_SWITCH_PIN, INPUT_PULLUP);
#endif
#ifdef HAS_HALL_INDEX
    // Same pin, other kind of axis. No pull-up: this one is an analog input
    // driven by the Hall sensor, and a pull-up would fight it.
    hallIndexSetup();
#endif

    // Init driver (brings up SPI for TMC2660) BEFORE the first HAL_MOTOR_DISABLE,
    // which for TMC issues a toff() over SPI.
    drivers_init();
    HAL_MOTOR_DISABLE();

    // Step-pulse one-shot timer: pulls STEP low HAL_STEP_PULSE_CCMP cycles
    // after a step.
    HAL_STEP_TIMER_INST.CTRLB   = HAL_STEP_TIMER_CNTMODE;
    HAL_STEP_TIMER_INST.INTCTRL = HAL_STEP_TIMER_CAPT_bm;

    // No NODE_ID-derived slot — the node boots disengaged and ignores the stream
    // until CMD_ENGAGE binds it (slot/masks stay at their SLOT_NONE/0 defaults).

#ifdef NODE_HAS_LASER
    // Laser gate (only the one stepper node wired to a laser): boot OFF.
    pinMode(HAL_LASER_PIN, OUTPUT); digitalWrite(HAL_LASER_PIN, LOW);
#endif
}

// CMD_ENABLE / CMD_DISABLE effect: ENERGIZE ONLY — no stream role.
// The stream gate is the slot (ENGAGE), decoupled from holding torque (ENABLE):
// a parked dual-head axis is ENABLED (holds Z height) but DISENGAGED (ignores
// the stream). See docs/engage_and_axis_map.md §4.3.
void node_set_enabled(bool on) {
    if (on) {
        HAL_MOTOR_ENABLE();
    } else {
#ifdef HAS_HOMING
        // De-energising mid-home must kill the pulser, or TCA0 would keep
        // counting steps into a position the motor is no longer holding. This is
        // what makes the existing broadcast estop stop a home too, with no new
        // mechanism: CMD_DISABLE is already on the broadcast allowlist.
        homingHalt();
#endif
        HAL_MOTOR_DISABLE();
    }
}

// Stepper does all its work in the RX ISR. The one thing left for loop context
// is publishing the limit state into the shared flags byte: node_set_flag()
// read-modify-writes a byte the core also owns, so it must not be called from
// the ISR that produces the state. The ISR latches into its own volatiles and
// this mirrors them out.
void node_loop(void) {
#ifdef HAS_HOMING
    // Finish before publishing: homingFinish() can clear the latch, and the
    // flags below must describe the state the master will act on, not the one
    // that existed a microsecond before the move ended.
    //
    // On a rotary build this is also where the ~50 ms autoconvolution runs. It
    // blocks node_loop, which is fine: a stepper node does its bus work in the
    // RS485 RX ISR, so commands keep being answered throughout — the only thing
    // delayed is the flag publish, and NODE_FLAG_HOMING staying set until the
    // answer actually exists is the correct reading, not a lag to apologise for.
    if (homingFinished) homingFinish();
    node_set_flag(NODE_FLAG_HOMING, homingActive);
#endif
#ifdef HAS_LIMIT_SWITCH
    // Live pin OR latch — the master needs to see the flag while the axis is
    // sitting on the switch AND after a latch that a bounce-free release has
    // since cleared from the pin but not from the gate.
    node_set_flag(NODE_FLAG_LIMIT, HAL_LIMIT_ASSERTED() || limitLatched);
#endif
}

#ifdef HAS_HOMING
// ─── Arming a homing move (§1.4) ────────────────────────────────────────────
// Validates, converts to ticks, and hands the move to the pulser. Returns false
// to NAK — the master then knows the move never started, which is a different
// thing from a move that started and failed.
//
// Rejecting rather than clamping is deliberate. Every one of these is a config
// or arithmetic mistake on the host side, and a clamped homing move would run at
// a rate nobody asked for, into a hard stop, while reporting success.
static uint8_t homingArm(bool dir, bool retract, uint16_t startUs, uint16_t floorUs,
                         uint16_t rampSteps, uint32_t maxSteps) {
    // The one refusal that is not the host's fault and not permanent: the same
    // frame is correct, just early. Everything below it is arithmetic the host
    // got wrong and will keep getting wrong until it changes the numbers.
    if (homingActive)              return NAK_BUSY;   // one move at a time
    if (startUs == 0 || floorUs == 0) return NAK_BAD_ARG;
    if (floorUs > startUs)         return NAK_BAD_ARG;  // floor is the FASTER rate
    if (maxSteps == 0)             return NAK_BAD_ARG;  // no budget = no runaway guard

    const uint32_t startTicks = (uint32_t)startUs * HOMING_TICKS_PER_US;
    const uint32_t floorTicks = (uint32_t)floorUs * HOMING_TICKS_PER_US;
    if (startTicks > 0xFFFF || startTicks == 0) return NAK_BAD_ARG;  // TCA0 is 16-bit
    if (floorTicks == 0)                        return NAK_BAD_ARG;

    HomingState h;
    h.dir        = dir;
    h.retract    = retract;
    h.floorTicks = (uint16_t)floorTicks;
    h.remaining  = maxSteps;
    h.limitRun   = 0;
    // ramp_steps == 0 means no ramp: start at the cruise rate rather than
    // ramping over zero steps, which would be a divide by zero.
    h.interval   = rampSteps ? (uint16_t)startTicks : (uint16_t)floorTicks;
    h.rampStep   = rampSteps ? (uint16_t)((startTicks - floorTicks) / rampSteps) : 0;

    cli();
    homing         = h;
    homingActive   = true;
    homingHitLimit = false;
    // Span start. Captured inside the same cli() as the arm so it cannot be
    // taken a step late -- the pulser is enabled below, but the stream path can
    // still be advancing the counter right up to here.
    homingSpanFrom = absolutePosition;
#ifdef HAS_HALL_INDEX
    // Same cli() for the same reason. `sign` is what one pulser step adds to
    // the counter (see the ISR), and the dip window is mapped back to absolute
    // steps through it — get it backwards and the index lands mirrored about
    // the start, which is a plausible-looking wrong answer rather than a
    // failure.
    hallIndexArm(dir ? 1 : -1, absolutePosition);
#endif
    sei();

    // DIR is set here, once, in loop context — so the pulser ISR never pays the
    // DM542 setup guard the stream path pays with delayMicroseconds(5).
    if (dir) HAL_DIR_PORT.OUTSET = HAL_DIR_BM;
    else     HAL_DIR_PORT.OUTCLR = HAL_DIR_BM;
    currentDir = dir;
    delayMicroseconds(5);

    node_set_flag(NODE_FLAG_HOMING, true);

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
    // runs before setup() and unconditionally leaves TCA0 in SPLIT mode,
    // RUNNING, for analogWrite() — TCA_SPLIT_SPLITM_bm | DIV64 | ENABLE at this
    // clock. In split mode PER is not one 16-bit register: it is two
    // independent 8-bit registers (LPER/HPER) at the same addresses. Writing a
    // 16-bit interval through the SINGLE view without leaving split mode first
    // does not error — it silently splits into two ~30-tick periods, so every
    // move ran at roughly 250x the requested rate regardless of what interval
    // was asked for. That was the actual cause of every leg looking "jerky"
    // and the slow latch seek not being slow at all: the ramp math was never
    // reached by the bug, the base rate was already wrong before the ramp
    // began.
    //
    // CTRLD IS ENABLE-LOCKED — the datasheet's own words, and Microchip's
    // DxCore takeover guide confirms it (docs/homing.md links it): a write to
    // CTRLD while CTRLA.ENABLE is still set is silently DROPPED. DxCore leaves
    // TCA0 enabled from boot, so CTRLA must be cleared FIRST — disabling it —
    // before CTRLD is written, or the "fix" changes nothing and split mode
    // stays active with no error to show for it.
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

// Stop the pulser. Safe from either context and idempotent — the ISR calls it to
// end a move normally, CMD_DISABLE calls it to abort one.
//
// Deliberately leaves TCA0 in SINGLE mode rather than restoring DxCore's SPLIT
// startup state: nothing on a stepper build calls analogWrite() (only the knife
// type does, and build_src_filter compiles one type per binary), so there is
// nothing to hand the timer back to, and re-deriving DxCore's own PWM_TIMER_PERIOD
// / prescaler pairing here would be new surface for no reachable benefit.
static void homingHalt(void) {
    TCA0.SINGLE.CTRLA   = 0;
    TCA0.SINGLE.INTCTRL = 0;
    homingActive   = false;
    homingFinished = true;
}

// The loop-context half of stopping, run once per completed move.
static void homingFinish(void) {
    homingFinished = false;
    // Close the span. Unconditional: a leg that failed still went somewhere, and
    // that distance is exactly what a failure needs to be diagnosed -- a seek
    // that stopped 164 mm into a 1200 mm frame says something a "switch never
    // reached" verdict on its own does not (docs/homing.md §7.2).
    homingSpanSteps = readPositionAtomic() - homingSpanFrom;
#ifdef HAS_HALL_INDEX
    // Reduce the window the ISR buffered. This is the expensive half of a
    // rotary home and it deliberately happens HERE rather than in the pulser:
    // a dip's centre is only knowable after passing it, so there was never an
    // ISR-sized answer to compute.
    hallIndexResolve();
#endif
#ifdef HAS_LIMIT_SWITCH
    // Clearing the latch is the retract's ONLY write to the gate, and only when
    // it verifiably got clear of the switch: a retract that spent its whole
    // budget and is still asserted did not escape (under-budgeted, wrong
    // direction, or a stuck switch), and the latch must survive that. A seek is
    // never eligible — it ends sitting ON the switch by definition.
    if (homing.retract && !HAL_LIMIT_ASSERTED()) {
        limitLatched   = false;
        limitRunBase   = limitBytesAsserted;   // the next run starts from here
    }
    // A seek that stopped ON its switch latches, so the fact survives the pin
    // bouncing before the supervisor's next poll. This is what §1.5's "after a
    // seek, LIMIT set means found" actually rests on -- previously it rested on
    // the pin still being asserted at poll time, which is true when the axis is
    // parked against the switch and false the instant the trip is electrically
    // noisy, i.e. exactly when it matters.
    if (!homing.retract && homingHitLimit) limitLatched = true;
#endif
}

// ─── The pulser (§1.3) ──────────────────────────────────────────────────────
// One step per overflow. Deliberately lean: no floating point, no
// delayMicroseconds, no bus work. DIR was set once at arm time, so unlike the
// stream path there is no setup guard to spin on here.
ISR(TCA0_OVF_vect) {
    TCA0.SINGLE.INTFLAGS = TCA_SINGLE_OVF_bm;
    if (!homingActive) return;

    // Both stop conditions are checked BEFORE the step, so the move never takes
    // one more step past the thing that ended it. On a seek that matters
    // physically: the switch is the target, and overshooting it is travel into
    // the hard stop.
    //
    // A retract ignores the switch entirely — it starts on an asserted one, so
    // testing the level would stop it before it ever moved. Its only terminator
    // is the budget, which is therefore a distance, not a guard.
#ifdef HAS_LIMIT_SWITCH
    if (!homing.retract) {
        // Debounced, unlike the single port read this used to be. A run that
        // breaks before HOMING_LIMIT_SAMPLES was a glitch and the seek carries
        // on; one that reaches it is the switch, and is RECORDED as such rather
        // than left for the supervisor to re-read off a pin that may have
        // released by the time it looks.
        if (HAL_LIMIT_ASSERTED()) {
            if (++homing.limitRun >= HOMING_LIMIT_SAMPLES) {
                homingHitLimit = true;
                homingHalt();
                return;
            }
        } else {
            homing.limitRun = 0;
        }
    }
#endif
    if (homing.remaining == 0)                   { homingHalt(); return; }

    HAL_STEP_PORT.OUTSET = HAL_STEP_BM;
    absolutePosition += (homing.dir ? 1 : -1);   // one counter, one meaning (§4)
    HAL_STEP_TIMER_INST.CCMP  = HAL_STEP_PULSE_CCMP;
    HAL_STEP_TIMER_INST.CNT   = 0;
    HAL_STEP_TIMER_INST.CTRLA = HAL_STEP_TIMER_CLKSEL | HAL_STEP_TIMER_ENABLE_bm;

    homing.remaining--;

#ifdef HAS_HALL_INDEX
    // Sampled AFTER the step, so the sample belongs to the position just
    // reached — which is what makes the window's step tags exact rather than
    // off by one. Note this is the opposite order from the limit check above,
    // and necessarily so: a switch is a reason NOT to take the next step, while
    // a dip sample is a measurement OF the step just taken.
    if (hallIndexSample(absolutePosition)) { homingHalt(); return; }
#endif

    // Linear decay of the interval toward the floor. Not constant acceleration
    // (that falls as ~1/sqrt(n)), but gentler early, which is the direction that
    // matters for not stalling on pull-in.
    if (homing.rampStep && homing.interval > homing.floorTicks) {
        uint16_t next = homing.interval - homing.rampStep;
        if (next < homing.floorTicks) next = homing.floorTicks;  // never overshoot
        homing.interval = next;
        TCA0.SINGLE.PER = next - 1;
    }
}
#endif

static int32_t readPositionAtomic() {
    cli();
    int32_t pos = absolutePosition;
    sei();
    return pos;
}

// Type-specific status tail: [pos int32 BE][slot], plus [span int32 BE] on a
// board with a switch. Lets a host see what the node counted, which stream slot
// it is ENGAGE-bound to (0xFF = disengaged), and how far its last homing leg ran.
//
// The span is APPENDED, and only on HAS_LIMIT_SWITCH builds, so the tail is
// legitimately two different lengths across the bus. That is safe in both
// directions because the master decodes on `len >= NS_STEP_LEN` rather than
// equality: an old master ignores the extra four bytes, and a new one reading a
// switchless node simply finds no span rather than mis-parsing.
uint8_t node_status(uint8_t* buf) {
    int32_t pos = readPositionAtomic();
    buf[0] = (pos >> 24) & 0xFF;
    buf[1] = (pos >> 16) & 0xFF;
    buf[2] = (pos >> 8)  & 0xFF;
    buf[3] =  pos        & 0xFF;
    buf[4] = slot;
    // Which terminator this board has, DECLARED rather than inferred from how
    // long the rest of this tail turns out to be. Sent by every stepper,
    // including ones with neither -- see HOMING_KIND_* in common.h.
#if defined(HAS_HALL_INDEX)
    buf[5] = HOMING_KIND_INDEX;
#elif defined(HAS_LIMIT_SWITCH)
    buf[5] = HOMING_KIND_LIMIT;
#else
    buf[5] = HOMING_KIND_NONE;
#endif
#ifdef HAS_HOMING
    const int32_t span = homingSpanSteps;
    buf[6] = (span >> 24) & 0xFF;
    buf[7] = (span >> 16) & 0xFF;
    buf[8] = (span >> 8)  & 0xFF;
    buf[9] =  span        & 0xFF;
#ifdef HAS_HALL_INDEX
    // Appended again, by the same rule that appended the span: a longer tail on
    // the boards that have more to say. The index is NOT buf[0..3] — a rotary
    // sweep runs THROUGH its feature, so where the axis stopped and where the
    // index is are two different numbers and both are wanted.
    const int32_t idx = hallIndexPos();
    buf[10] = (idx >> 24) & 0xFF;
    buf[11] = (idx >> 16) & 0xFF;
    buf[12] = (idx >> 8)  & 0xFF;
    buf[13] =  idx        & 0xFF;
    buf[14] = hallIndexCause();
    // Live sensor value and the last sweep's baseline. Diagnostics, not part of
    // the homing answer -- but a NOTFOUND cannot otherwise be told apart from a
    // sensor that is not wired to this pin at all, and on a new board that is
    // the FIRST thing worth ruling out.
    const int16_t raw  = hallIndexRaw();
    const int16_t base = hallIndexBaseline();
    buf[15] = (raw  >> 8) & 0xFF;
    buf[16] =  raw        & 0xFF;
    buf[17] = (base >> 8) & 0xFF;
    buf[18] =  base       & 0xFF;
    // Lap length, measured by the sweep itself, and the count of index crossings
    // it managed. The lap length is what makes the answer portable: no host can
    // know it in advance on an unknown head. The crossing count is what makes a
    // FAILURE legible -- 0 means the magnet was never seen, and a short count
    // means the budget ran out before periodicity could be proven.
    const int32_t spr = hallIndexStepsPerRev();
    buf[19] = (spr >> 24) & 0xFF;
    buf[20] = (spr >> 16) & 0xFF;
    buf[21] = (spr >> 8)  & 0xFF;
    buf[22] =  spr        & 0xFF;
    buf[23] = hallIndexCrossings();
    return 24;
#else
    return 10;
#endif
#else
    return 6;
#endif
}

bool node_handle_command(const uint8_t* pkt, uint8_t len,
                         uint8_t* reply, uint8_t* replyLen) {
    (void)len;
    switch (pkt[1]) {
        case CMD_ENGAGE: {
            // payload [slot]: 0..3 bind to that stream slot, 0xFF = disengage.
            uint8_t s = pkt[3];
            if (s == SLOT_NONE) {
                stepBitMask = 0;
                dirBitMask  = 0;
            } else if (s <= SLOT_A) {
                stepBitMask = 1 << (s * 2);
                dirBitMask  = 1 << (s * 2 + 1);
            } else {
                return false;              // out-of-range slot → NAK, keep state
            }
            slot = s;
            // ACK carries slot, pos and energised state which currently happens 
            // to be exactly the same as the full state: [type][flags][pos][slot],
            // sampled after the bind. That makes an engage one atomic observation
            // of (bound, position, enabled) — a separate follow-up read could
            // straddle a node reboot and report a position for a slot the node no
            // longer holds. The echoed slot also self-verifies the bind.
            reply[0] = NODE_ID;
            reply[1] = CMD_ENGAGE;
            uint8_t n = buildNodeStatus(&reply[3]);
            reply[2] = n;
            *replyLen = 3 + n + 1;
            return true;
        }
#ifdef NODE_HAS_LASER
        case CMD_LASER: {
            // payload [state]: 1 = laser on, 0 = off. Compiled only on the laser
            // node; every other stepper NAKs this (falls through to return false).
            if (len < 5) return false;             // [id][cmd][1][state][crc]
            digitalWrite(HAL_LASER_PIN, pkt[3] ? HIGH : LOW);
            reply[0] = NODE_ID;
            reply[1] = CMD_LASER;
            reply[2] = 0;
            *replyLen = 4;
            return true;
        }
#endif
#ifdef HAS_HOMING
        case CMD_HOME: {
            // [id][cmd][len][11 payload][crc]. Compiled only where the node has
            // something that can END a move — a switch or a Hall index. Without
            // one there is no terminator at all, so such a node NAKs rather than
            // running open-loop into the stop.
            // BAD_ARG, not the dispatcher's UNSUPPORTED: the opcode IS
            // supported and the frame is simply the wrong length. Answering
            // "unsupported" here points the host at its firmware version when
            // the fault is in the bytes it just sent.
            if (len < 3 + CMD_HOME_PAYLOAD_LEN + 1) {
                node_reply_nak(CMD_HOME, NAK_BAD_ARG, reply, replyLen);
                return true;
            }

            const uint8_t* p = &pkt[3];
            const bool     dir             = (p[0] & 0x01) != 0;
            const uint16_t startUs   = ((uint16_t)p[1] << 8) | p[2];
            const uint16_t floorUs   = ((uint16_t)p[3] << 8) | p[4];
            const uint16_t rampSteps = ((uint16_t)p[5] << 8) | p[6];
            const uint32_t maxSteps  = ((uint32_t)p[7]  << 24) |
                                       ((uint32_t)p[8]  << 16) |
                                       ((uint32_t)p[9]  <<  8) |
                                        (uint32_t)p[10];

#ifdef HAS_LIMIT_SWITCH
            const bool intendedRetract = (p[0] & 0x02) != 0;

            // THE mode decision, and the only place it is made: one pin read,
            // now. Sitting on the switch means the only useful move is off it.
            const bool retract = HAL_LIMIT_ASSERTED();

            // The intent bit does not feed the decision above -- it is checked
            // AGAINST it. A mismatch means the host's model of the switch state
            // has diverged from reality, and arming anyway would run the
            // declared budget under the WRONG semantics: a host expecting a
            // seek sized its budget as a runaway cap for a move the switch was
            // meant to cut short, and a retract ignores the switch and runs
            // that same budget to completion (include/common.h, CMD_HOME). NAK
            // here, before anything moves, rather than silently execute a move
            // the host did not intend.
            //
            // A reasoned NAK, not the generic `return false`: NAK_UNSUPPORTED
            // would read as "this node does not do CMD_HOME", which is false —
            // it does, just not with THIS command's premise. NAK_INTENT_MISMATCH
            // tells the host to re-read the switch before retrying rather than
            // to suspect its wiring or payload framing.
            if (retract != intendedRetract) {
                node_reply_nak(CMD_HOME, NAK_INTENT_MISMATCH, reply, replyLen);
                return true;
            }
#else
            // Rotary: no pin, so no mode and nothing to disagree about. The
            // intent bit is IGNORED rather than given a second meaning here
            // (include/common.h, CMD_HOME) — there is exactly one kind of
            // rotary leg, a sweep, and `retract` false is what runs it.
            const bool retract = false;
#endif

            const uint8_t why = homingArm(dir, retract, startUs, floorUs,
                                          rampSteps, maxSteps);
            if (why) {
                node_reply_nak(CMD_HOME, why, reply, replyLen);
                return true;
            }

            // Ack with full status, like CMD_ENGAGE: one atomic observation of
            // (homing, limit, position) taken after the arm, so the supervisor
            // never has to infer the starting point from a separate read that
            // could straddle the first steps.
            reply[0] = NODE_ID;
            reply[1] = CMD_HOME;
            uint8_t n = buildNodeStatus(&reply[3]);
            reply[2] = n;
            *replyLen = 3 + n + 1;
            return true;
        }
#endif
        case CMD_GET_POS: {
            // Same payload as CMD_NODE_STATUS / the ENGAGE ack — position never
            // travels in a shape of its own, so there is one parser on the host
            // side and one place to extend. Kept as a distinct verb only because
            // the direct UPDI debug console asks for it by name.
            reply[0] = NODE_ID;
            reply[1] = CMD_GET_POS;
            uint8_t n = buildNodeStatus(&reply[3]);
            reply[2] = n;
            *replyLen = 3 + n + 1;
            return true;
        }
        default:
            return false;
    }
}

// ─── RX ISR — command framing + stream stepping ─────────────────────────────
ISR(HAL_USART_RXC_vect) {
    uint8_t status = HAL_USART_INST.RXDATAH;
    uint8_t b      = HAL_USART_INST.RXDATAL;

    if (status & 0x01) {            // 9th bit = 1 → command frame
        frame_command_byte(b);
        return;
    }

    frame_stream_reset();           // 9th bit → stream byte
    if (slot == SLOT_NONE) return;  // disengaged → ignore stream, freeze position

#ifdef HAS_HOMING
    // A home owns the axis outright, and this path must not touch it. The
    // collision is not hypothetical or rare: busQuiesce() prefaces EVERY command
    // frame with a NOP stream byte, and a zero byte has dirBitMask clear, so it
    // reads as "direction 0" here and drives DIR low. The supervisor polls the
    // homing node every HOMING_POLL_MS, so the first poll after the arm would
    // yank DIR out from under the pulser and every step after it ran the wrong
    // way -- while absolutePosition, which the pulser derives from homing.dir,
    // kept counting the direction that was ASKED for. The counter and the shaft
    // disagreed, and only the counter was visible over the bus.
    //
    // Returning before the limit accumulator as well: during a home the pulser's
    // own pin read is the authority on the switch, and letting NOP bytes advance
    // limitBytesAsserted would move the baseline homingFinish() judges a retract
    // against. The DIR hazard is identical on a rotary build — the pulser owns
    // DIR there too — so this guard is NOT limit-specific and must not be
    // folded back under the switch's #ifdef.
    if (homingActive) return;
#endif

#ifdef HAS_LIMIT_SWITCH
    // One port read, no debounce, no branch on direction. Refusal is IMMEDIATE:
    // a real trip stops on the very next step, because waiting out the latch
    // window before refusing would let the axis run ~500 ms further into the
    // hard stop — thousands of steps. The accumulator below decides only whether
    // the refusal becomes STICKY, not whether it happens.
    const bool limAsserted = HAL_LIMIT_ASSERTED();
    if (limAsserted) {
        if (++limitBytesAsserted - limitRunBase >= LIMIT_LATCH_BYTES)
            limitLatched = true;    // sustained → a genuine trip, hold the gate
    } else {
        limitRunBase = limitBytesAsserted;   // released → start a new run
    }
    // A run that ends under the threshold was a glitch: the gate opens again by
    // itself on release, the job carries on having lost a few steps, and the
    // lifetime total records that it happened. A run that latched stays shut
    // until a successful RETRACT clears it — nothing in the stream path can, and
    // neither can a seek, which by definition ends sitting ON the switch.
    if (limAsserted || limitLatched) return;   // refuse the step
#endif

    bool stepReq = (b & stepBitMask) != 0;
    bool newDir  = (b & dirBitMask)  != 0;

    if (newDir != currentDir) {
        if (newDir) HAL_DIR_PORT.OUTSET = HAL_DIR_BM;
        else        HAL_DIR_PORT.OUTCLR = HAL_DIR_BM;
        currentDir = newDir;
        delayMicroseconds(5);       // DM542 DIR-before-STEP setup guard
    }

    if (stepReq) {
        HAL_STEP_PORT.OUTSET = HAL_STEP_BM;
        absolutePosition += (currentDir ? 1 : -1);
        HAL_STEP_TIMER_INST.CCMP  = HAL_STEP_PULSE_CCMP;
        HAL_STEP_TIMER_INST.CNT   = 0;
        HAL_STEP_TIMER_INST.CTRLA = HAL_STEP_TIMER_CLKSEL | HAL_STEP_TIMER_ENABLE_bm;
    }
}

// ─── Step-pulse timer — end of step pulse ───────────────────────────────────
ISR(HAL_STEP_TIMER_vect) {
    HAL_STEP_TIMER_INST.INTFLAGS = HAL_STEP_TIMER_CAPT_bm;
    HAL_STEP_PORT.OUTCLR = HAL_STEP_BM;
    HAL_STEP_TIMER_INST.CTRLA &= ~HAL_STEP_TIMER_ENABLE_bm;
}
