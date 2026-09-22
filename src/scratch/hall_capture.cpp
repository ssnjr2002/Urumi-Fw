// hall_capture.cpp — standalone A-axis Hall capture rig. NOT part of the node
// firmware, NOT on the RS485 bus, and deliberately throwaway.
//
// Purpose: collect step-tagged Hall samples from the rotary A axis so the index
// centre-finding algorithm can be chosen from real data on a PC, instead of
// guessed at in firmware. Once an estimator wins, it gets written properly
// against the node core and THIS FILE GETS DELETED. Nothing here is a design
// commitment — not the buffering (there is none), not the transfer format, not
// the command set.
//
// Why standalone: the only thing firmware uniquely provides is the position
// tag — sampling the ADC synchronously with steps, which only the thing issuing
// steps can do. Everything downstream (baseline, threshold, centroid, whatever
// wins) is arithmetic that can happen anywhere. Off-bus, samples stream out
// live over USART as they are acquired, which removes buffer sizing, chunked
// transfers, and any "which window do I capture" trigger decision. Capture
// twenty revolutions straight through and sort it out in Python.
//
// Time-sampled logging (e.g. free-running ADC dumped over USB) does NOT work
// for this: it has to be resampled into the angle domain afterwards, and that
// resampling is acutely sensitive to timing jitter — exactly the axis being
// measured. One sample per step sidesteps it rather than correcting it.
//
// Wiring assumptions (db_node4, AVR128DB32, DRV8825):
//   PD1  A1324 Hall analog output  (AIN1)
//   PD4  STEP   PD5 DIR   PD6 ENABLE (active LOW)
//   PA4/PA6/PA7  DRV8825 M0/M1/M2 — NOT connected on this board; the microstep
//                mode is strapped in solder (1/16), so these are left alone
//   PD2  laser gate on this board (-DNODE_HAS_LASER) — held LOW here, see below
//
// Serial protocol (fixed-width ASCII so per-sample cost is constant — variable
// width would jitter the step interval and smear the speed tests):
//   > r <interval_us> <steps> [preroll]   stream one sample per step; `preroll`
//                                         steps are taken but not emitted, so
//                                         the capture starts at settled speed
//   > d <0|1>                   set direction for subsequent runs
//   > e <0|1>                   driver enable/disable
//   > ?                         status
// Output:
//   # BEGIN interval_us=1000 steps=128000 dir=0 micro=16 accbits=13
//   000000,04091
//   000001,04088
//   # END n=128000
#include <Arduino.h>

#ifndef CAP_SERIAL
#define CAP_SERIAL Serial1          // AVR-DB: USART1 on PC0/PC1 (USART2 is RS485)
#endif
#ifndef CAP_BAUD
#define CAP_BAUD 500000
#endif

// Microstepping. NOT software-selectable on this board: DRV8825 M0/M1/M2 are
// strapped in solder and are not routed to the MCU, so nothing here can change
// the mode. This is a declaration of what the hardware is set to, carried into
// the capture header purely so a CSV records the conditions it was taken under.
//
// The board is strapped to 1/16. Note that db_node4's DRV_MICROSTEPPING still
// defaults to 32, which does not match the hardware — see stepper.h.
#ifndef CAP_MICROSTEPPING
#define CAP_MICROSTEPPING 16
#endif

// ADC accumulation. DxCore's analogReadEnh() oversamples in hardware (the Dx
// ADC's SAMPNUM) and rescales to the requested resolution — free noise
// reduction for the cost of extra conversion time. 13 bits = 4x accumulation.
// The averaging is over TIME, so at speed it smears slightly over ANGLE: at
// 1000 steps/s that is well under a tenth of a step, i.e. irrelevant. Raise it
// if the captures look noisy; drop to 12 (no accumulation) if it costs too much
// time at the fastest sweep.
#ifndef CAP_ADC_BITS
#define CAP_ADC_BITS 13
#endif

#define HALL_PIN   PIN_PD1
#define STEP_PIN   PIN_PD4
#define DIR_PIN    PIN_PD5
#define EN_PIN     PIN_PD6

// Direct port access for the step pulse — the pin toggle wants to be two stores,
// not two digitalWrite() calls, so the pulse width is what it says it is.
#define STEP_HIGH() (PORTD.OUTSET = PIN4_bm)
#define STEP_LOW()  (PORTD.OUTCLR = PIN4_bm)

// DRV8825: 1.9µs minimum step high and low. 3µs matches HAL_STEP_PULSE_CCMP in
// the real HAL, so the driver sees an identical pulse either way.
#define STEP_PULSE_US 3

static uint8_t g_dir = 0;
static bool    g_enabled = false;

// Absolute step counter since reset. Every step in this file goes through
// stepOnce(), so this is the one place position is tracked and the homing
// result can be reported in a frame that survives across trials — which is
// what makes repeatability measurable at all.
static int32_t g_pos = 0;

static inline void stepOnce() {
    STEP_HIGH();
    delayMicroseconds(STEP_PULSE_US);
    STEP_LOW();
    g_pos += g_dir ? -1 : 1;
}

// ─── Driver plumbing ────────────────────────────────────────────────────────

// No setMicrostepping() here on purpose. M0/M1/M2 are strapped in solder and
// are not wired to the MCU, so driving PA4/PA6/PA7 would change nothing about
// the driver while asserting three pins whose actual net is unknown to this
// build. Leaving them untouched is both honest and safer.

static void driverEnable(bool on) {
    // DRV8825 enable is active LOW (matches HAL_MOTOR_ENABLE in stepper.h).
    digitalWrite(EN_PIN, on ? LOW : HIGH);
    g_enabled = on;
}

// ─── Capture ────────────────────────────────────────────────────────────────

// One sample per step, taken immediately AFTER the step pulse completes, so
// sample i is the field at the position reached by step i. No pipelining: the
// read is blocking, which at any sweep rate worth using costs a small fraction
// of the step interval and keeps the position tag exact rather than
// off-by-one-step.
//
// The loop paces off an absolute micros() deadline rather than delaying by a
// fixed amount, so the time spent converting and printing is absorbed instead
// of accumulating into a drifting, slowly-decreasing sweep speed.
static void runCapture(uint32_t intervalUs, uint32_t steps, uint32_t preroll) {
    if (!g_enabled) {
        CAP_SERIAL.println(F("# ERR driver disabled — 'e 1' first"));
        return;
    }

    digitalWrite(DIR_PIN, g_dir ? HIGH : LOW);
    delayMicroseconds(5);            // DIR setup, generous vs the DRV8825's 650ns

    CAP_SERIAL.print(F("# BEGIN interval_us="));
    CAP_SERIAL.print(intervalUs);
    CAP_SERIAL.print(F(" steps="));
    CAP_SERIAL.print(steps);
    CAP_SERIAL.print(F(" dir="));
    CAP_SERIAL.print(g_dir);
    CAP_SERIAL.print(F(" micro="));
    CAP_SERIAL.print(CAP_MICROSTEPPING);
    CAP_SERIAL.print(F(" accbits="));
    CAP_SERIAL.print(CAP_ADC_BITS);
    CAP_SERIAL.print(F(" preroll="));
    CAP_SERIAL.println(preroll);

    char line[16];
    uint32_t next = micros();

    // Pre-roll: step at the capture rate but emit nothing. The sweep otherwise
    // starts from standstill, so the opening samples carry the belt's start-up
    // transient — and the magnet sits on the LOAD side of that belt, which is
    // precisely the compliance the transient lives in. Pre-rolling means every
    // emitted sample is at settled constant speed.
    //
    // Set it to 0 deliberately when the goal is to MEASURE the transient rather
    // than exclude it — but then start the axis away from the magnet, or the
    // settling is superimposed on a dip and cannot be read.
    for (uint32_t i = 0; i < preroll; i++) {
        stepOnce();
        (void)analogReadEnh(HALL_PIN, CAP_ADC_BITS);   // keep ADC cadence identical
        next += intervalUs;
        while ((int32_t)(micros() - next) < 0) { /* pace */ }
    }

    for (uint32_t i = 0; i < steps; i++) {
        stepOnce();

        const int32_t v = analogReadEnh(HALL_PIN, CAP_ADC_BITS);

        // Fixed width on purpose: constant formatting cost per sample keeps the
        // step interval constant, which the settling-time and speed-dependence
        // analyses both depend on.
        snprintf(line, sizeof line, "%06lu,%05ld", (unsigned long)i, (long)v);
        CAP_SERIAL.println(line);

        next += intervalUs;
        while ((int32_t)(micros() - next) < 0) { /* pace */ }
    }

    CAP_SERIAL.print(F("# END n="));
    CAP_SERIAL.println(steps);
}

// ─── Homing ─────────────────────────────────────────────────────────────────
//
// This is the part that is NOT throwaway in spirit, even though the file is:
// whatever runs here is what has to run on the node, so it is written under
// node constraints — fixed point, one bounded buffer, no second pass over the
// sweep — rather than as a transcription of the Python.
//
// est_mirror won the bake-off (hall_analyze.py): it locates the dip's axis of
// symmetry by correlating the dip against its own reverse, and the
// autoconvolution of a bump centred at c peaks at 2c. It assumes symmetry and
// nothing else — no template, no shape model, no depth calibration — which is
// why it beat the matched filter on real data, where the dip width wanders
// 704-735 samples lap to lap and no single template fits every lap.
//
// Decimated 4:1. The dip is ~730 steps wide, so 4:1 still leaves ~180 points
// across it, and it cuts the O(n^2) autoconvolution 16x. The bottom is flat
// within noise for +-100 steps anyway — all the position information is in the
// flanks, and decimation does not touch those.
#define HOME_DECIM      4
// 400 decimated = 1600 steps of room. The dip runs ~900 steps wide measured at
// the HOME_EXIT threshold (much wider than the 730 quoted at 40% depth, since
// 300 counts is only 14% of the way down), and pre and post add 40 each, so a
// 320-sample buffer overflowed and truncated the tail instead of letting the
// symmetric-tail rule end the window. Harmless as it happened -- the cut was
// out on the flat shoulder -- but it meant the overflow guard was doing the
// job the exit logic was written to do, which is the kind of silent fallback
// that stops being harmless the moment the dip shape changes.
#define HOME_WIN      400
#define HOME_PRE       40     // decimated samples of pre-trigger context
#define HOME_ENTER    400     // counts below baseline to call it a dip. Noise
                              // is ~23 counts and the dip is ~2069 deep, so
                              // this sits ~17 sigma clear of one and well
                              // inside the other.
#define HOME_EXIT     300     // hysteresis, so noise on the flank cannot
                              // re-trigger the exit test

static int16_t g_win[HOME_WIN];
static int16_t g_g[HOME_WIN];

// Symmetry axis of the buffered dip, in decimated-sample units. Fixed point
// throughout except the final vertex interpolation.
static float mirrorCentre(uint16_t n, int32_t baseline) {
    // g = depth below baseline, clipped at zero so the flat shoulders
    // contribute nothing, and scaled down so the autoconvolution stays inside
    // int32: worst case n * (2069>>2)^2 is about 7e7, against a 2.1e9 ceiling.
    for (uint16_t i = 0; i < n; i++) {
        int32_t d = baseline - g_win[i];
        if (d < 0) d = 0;
        g_g[i] = (int16_t)(d >> 2);
    }

    // AC[k] = sum_i g[i]*g[k-i]. Keep a 3-deep history so the peak and both
    // its neighbours are available for the vertex fit without a second pass.
    int32_t h0 = 0, h1 = 0;
    int32_t best = -1, ba = 0, bb = 0, bc = 0;
    uint16_t bk = 0;
    const uint16_t kmax = (uint16_t)(2 * n - 1);

    for (uint16_t k = 0; k < kmax; k++) {
        const uint16_t lo = (k >= n) ? (uint16_t)(k - n + 1) : 0;
        const uint16_t hi = (k < n) ? k : (uint16_t)(n - 1);
        int32_t acc = 0;
        for (uint16_t i = lo; i <= hi; i++) acc += (int32_t)g_g[i] * g_g[k - i];

        if (k >= 2 && h1 > best) { best = h1; bk = (uint16_t)(k - 1); ba = h0; bb = h1; bc = acc; }
        h0 = h1; h1 = acc;
    }
    if (best <= 0) return -1.0f;

    const int32_t den = ba - 2 * bb + bc;
    const float delta = den ? (0.5f * (float)(ba - bc) / (float)den) : 0.0f;
    return ((float)bk + delta) * 0.5f;   // peak at 2c
}

// Sweep until one COMPLETE dip has passed, then report where its centre was.
//
// Note the shape of this: it cannot stop when it detects the index, because an
// analog dip's centre is only knowable after passing it. That is the structural
// difference from limit-switch homing, where the switch edge IS the position.
// One dip, starting from wherever the axis is now. Factored out of runHome so
// the multi-lap version can call it repeatedly inside one continuous sweep.
//
// Baseline is carried in and out: it is a running maximum over the WHOLE sweep,
// so later laps inherit the best estimate so far instead of rebuilding it. The
// feature is a DIP, so the largest field seen is the away-from-magnet level,
// and taking a max means the sweep may START on the magnet without poisoning
// the reference — which a leading average would do. It biases high by a couple
// of counts of noise, but that bias is identical every lap, so it cancels out
// of repeatability entirely.
static bool sweepOneDip(uint32_t intervalUs, uint32_t maxSteps, int32_t sign,
                        int32_t* baselineIO, float* idxOut,
                        bool report, bool emitWindow) {
    int32_t baseline = *baselineIO;

    int16_t  ring[HOME_PRE];
    uint16_t rn = 0, rhead = 0;
    uint16_t nwin = 0, preN = 0, post = 0;
    int32_t  winStart = 0;
    bool     inDip = false, exiting = false, done = false, overflow = false;

    uint32_t next = micros();
    uint32_t i = 0;
    for (; i < maxSteps && !done; i++) {
        stepOnce();
        const int32_t v = analogReadEnh(HALL_PIN, CAP_ADC_BITS);
        if (v > baseline) baseline = v;

        if ((i % HOME_DECIM) == 0) {
            if (!inDip) {
                if (baseline - v > HOME_ENTER) {
                    inDip = true;
                    preN = rn;
                    winStart = g_pos - (int32_t)rn * HOME_DECIM * sign;
                    for (uint16_t k = 0; k < rn; k++)
                        g_win[nwin++] = ring[(uint16_t)((rhead + HOME_PRE - rn + k) % HOME_PRE)];
                } else {
                    ring[rhead] = (int16_t)v;
                    rhead = (uint16_t)((rhead + 1) % HOME_PRE);
                    if (rn < HOME_PRE) rn++;
                }
            }
            if (inDip) {
                if (nwin < HOME_WIN) { g_win[nwin++] = (int16_t)v; }
                else { done = true; overflow = true; }   // dip wider than the buffer
                if (!exiting) {
                    if (baseline - v < HOME_EXIT) { exiting = true; post = 0; }
                } else if (++post >= preN) {
                    done = true;                      // as much tail as head
                }
            }
        }

        next += intervalUs;
        while ((int32_t)(micros() - next) < 0) { /* pace */ }
    }

    *baselineIO = baseline;

    if (!inDip || !exiting) {
        CAP_SERIAL.print(F("# HOME found=0 swept=")); CAP_SERIAL.println(i);
        return false;
    }

    const float c = mirrorCentre(nwin, baseline);
    if (c < 0) { CAP_SERIAL.println(F("# HOME found=0 reason=degenerate")); return false; }

    // Back to absolute step coordinates. Decimated sample j sits at
    // winStart + j*HOME_DECIM*sign, so a fractional j interpolates the same way.
    const float idxf = (float)winStart + c * (float)(HOME_DECIM * sign);
    const int32_t idx = (int32_t)lroundf(idxf);
    *idxOut = idxf;

    if (!report) return true;

    CAP_SERIAL.print(F("# HOME found=1 index="));   CAP_SERIAL.print(idx);
    CAP_SERIAL.print(F(" centre="));                CAP_SERIAL.print(idxf, 2);
    CAP_SERIAL.print(F(" baseline="));              CAP_SERIAL.print(baseline);
    CAP_SERIAL.print(F(" win="));                   CAP_SERIAL.print(nwin);
    CAP_SERIAL.print(F(" pre="));                   CAP_SERIAL.print(preN);
    CAP_SERIAL.print(F(" ovf="));                   CAP_SERIAL.print(overflow ? 1 : 0);
    CAP_SERIAL.print(F(" swept="));                 CAP_SERIAL.print(i);
    CAP_SERIAL.print(F(" pos="));                   CAP_SERIAL.println(g_pos);

    // Emitting the same window the node just reduced is the point of doing this
    // on the scratch rig: the PC can run the float est_mirror over identical
    // samples, so any disagreement is purely the fixed-point/decimated
    // implementation and not the mechanism.
    if (emitWindow) {
        CAP_SERIAL.print(F("# WIN start=")); CAP_SERIAL.print(winStart);
        CAP_SERIAL.print(F(" decim="));      CAP_SERIAL.print(HOME_DECIM);
        CAP_SERIAL.print(F(" sign="));       CAP_SERIAL.print(sign);
        CAP_SERIAL.print(F(" n="));          CAP_SERIAL.println(nwin);
        for (uint16_t k = 0; k < nwin; k++) CAP_SERIAL.println(g_win[k]);
        CAP_SERIAL.println(F("# WIN end"));
    }
    return true;
}

static bool runHome(uint32_t intervalUs, uint32_t maxSteps, bool emitWindow) {
    if (!g_enabled) { CAP_SERIAL.println(F("# ERR driver disabled — 'e 1' first")); return false; }
    digitalWrite(DIR_PIN, g_dir ? HIGH : LOW);
    delayMicroseconds(5);

    int32_t baseline = 0;
    float   idx = 0;
    return sweepOneDip(intervalUs, maxSteps, g_dir ? -1 : 1,
                       &baseline, &idx, true, emitWindow);
}

// ─── Multi-lap homing — MEASURED AND REJECTED ───────────────────────────────
//
// DO NOT USE THIS AS A HOMING PATH. It is kept, and kept working, only as a
// documented negative result, because the reasoning behind it is seductive and
// somebody will otherwise re-derive it and re-implement it.
//
// Bench result, 6 trials x 9 laps against the single-lap index from the SAME
// sweep (a paired comparison, so no run-to-run mechanical difference):
//
//     raw sd  57.8 steps (1.26 deg)      <- plain single-lap homing
//     ref sd  80.3 steps (1.75 deg)      <- with this "correction" applied
//     0.7x, i.e. a REGRESSION
//
// One trial had a bad endpoint (spr 16481 against ~16497 elsewhere), but
// dropping it does not rescue the method: 59.1 vs 88.7. The technique is
// broken, not unlucky.
//
// Why it fails is worth understanding, because the arithmetic below is not
// wrong -- its premise is. Cancellation requires the belt error to repeat with
// period EXACTLY equal to `laps`. Measured across four independent datasets the
// period is 8.80, 8.90, 9.00, 9.06 laps: never an integer. At period 8.8 a
// 9-lap span accumulates a phase error of 2*pi*(1 - 9/8.8) ~= -8 degrees, and
// what leaks through is the same order of magnitude as the belt swing itself,
// with no guaranteed sign. Observed corrections ran -29 to +47 steps rather
// than converging toward zero. Sampling at integer lap counts turns an exact
// cancellation into a coin flip.
//
// The fix is to fit the period as a free parameter instead of assuming it,
// which needs 15-20+ laps to constrain and is not AVR-sized work. That belongs
// on the PC as a calibration pass producing a stored table -- see hall_revs.py
// and the periodicity scan.
//
// The original reasoning, preserved because the algebra is sound and only the
// premise fails:
//
// Single-lap homing repeats to 4.2 steps (0.09 deg), but the index's position
// in MOTOR steps wanders by up to +-46 steps with belt phase, on a period
// measured at 8.80/8.90/9.00/9.06 laps across four independent datasets. That
// wander is not estimator noise and no estimator can remove it: the index is a
// fixed OUTPUT angle, and it is the motor count at which it appears that moves.
//
// It can be cancelled arithmetically, though, because a periodic error summed
// over exactly one full period is zero. Over LAPS+1 indices:
//
//   spr  = (idx[LAPS] - idx[0]) / LAPS
//          endpoint difference across exactly one period, so the belt term
//          appears identically at both ends and subtracts out. (A least-squares
//          slope does NOT have this property -- a sinusoid over one period has
//          zero mean but non-zero correlation with a ramp, so the fit is
//          biased. This is why the 9-lap baseline beat the 13-lap one.)
//
//   r[k] = idx[k] - idx[0] - k*spr          = e[k] - e[0]
//   ref  = idx[0] + mean(r[0..LAPS-1])      = idx[0] - e[0]
//          one full period of residuals averages the belt error to zero,
//          leaving the index position with the belt term removed -- and
//          averaging LAPS of them divides the 4.2-step noise by sqrt(LAPS).
//
// The catch is time: this is one continuous sweep of LAPS+1 revolutions. Right
// for a calibration pass whose answer gets stored, far too slow to home with
// routinely.
//
// Note the sweep pauses each lap while mirrorCentre() runs, roughly 50 ms.
//
// That pause was assumed harmless here — it is a stop, not a reversal, so no
// backlash is taken, and the next dip is a full revolution away, long settled
// by the time it matters. MEASURED, THAT ASSUMPTION IS WRONG, and it is the
// single largest thing standing between this node and the accuracy the same
// algorithm reaches on a PC.
//
// Held-out per-lap scatter against the length of the deliberate extra stop,
// via the dwell argument to `p`:
//
//     dwell      0 ms    250 ms   1000 ms
//     scatter    21.6     59.3     113.3  steps
//
// A continuous sweep — hall_capture.py, which never stops and preroll's 2000
// steps so it begins at settled speed — supports 5.2 steps on the same fit.
// This node, stopping once per lap, supports 10-16.
//
// The mechanism is the compliant belt: the magnet is on the LOAD side of it, so
// while the motor holds position the belt relaxes toward a new equilibrium and
// takes the magnet with it. Longer stop, further relaxation. The direction is
// not consistent between runs, so this is scatter rather than a systematic
// creep that could be calibrated out.
//
// The fix is not a better estimator, it is to stop stopping: chunk
// mirrorCentre across step intervals so the sweep never halts. At 400 us/step
// there are roughly 9600 CPU cycles of slack per step against ~800k cycles of
// autoconvolution per dip, so spreading it over a couple of hundred steps costs
// well under half the available slack.
#define HOME_MAX_LAPS 16

static void runHomeMulti(uint32_t intervalUs, uint32_t laps, uint32_t budgetPerLap) {
    if (!g_enabled) { CAP_SERIAL.println(F("# ERR driver disabled — 'e 1' first")); return; }
    if (laps < 1 || laps > HOME_MAX_LAPS) {
        CAP_SERIAL.print(F("# ERR laps must be 1..")); CAP_SERIAL.println(HOME_MAX_LAPS);
        return;
    }

    digitalWrite(DIR_PIN, g_dir ? HIGH : LOW);
    delayMicroseconds(5);
    const int32_t sign = g_dir ? -1 : 1;

    static float idx[HOME_MAX_LAPS + 1];
    int32_t baseline = 0;

    for (uint32_t k = 0; k <= laps; k++) {
        if (!sweepOneDip(intervalUs, budgetPerLap, sign, &baseline, &idx[k], false, false)) {
            CAP_SERIAL.print(F("# HOMEN found=0 lap=")); CAP_SERIAL.println(k);
            return;
        }
    }

    const float spr = (idx[laps] - idx[0]) / (float)laps;

    float sum = 0;
    for (uint32_t k = 0; k < laps; k++) sum += idx[k] - idx[0] - (float)k * spr;
    const float ref = idx[0] + sum / (float)laps;

    // Scatter of the per-lap residuals about their mean, reported so the caller
    // can see the belt swing this run actually cancelled rather than assume it.
    float ss = 0;
    const float mean = sum / (float)laps;
    for (uint32_t k = 0; k < laps; k++) {
        const float d = (idx[k] - idx[0] - (float)k * spr) - mean;
        ss += d * d;
    }
    const float sd = (laps > 1) ? sqrtf(ss / (float)(laps - 1)) : 0.0f;

    CAP_SERIAL.println(F("# WARN multi-lap correction measured WORSE than "
                         "single-lap (0.7x) — negative result, see source"));
    CAP_SERIAL.print(F("# HOMEN found=1 laps="));  CAP_SERIAL.print(laps);
    CAP_SERIAL.print(F(" spr="));                  CAP_SERIAL.print(spr, 2);
    CAP_SERIAL.print(F(" ref="));                  CAP_SERIAL.print(ref, 2);
    CAP_SERIAL.print(F(" raw="));                  CAP_SERIAL.print(idx[0], 2);
    CAP_SERIAL.print(F(" corr="));                 CAP_SERIAL.print(ref - idx[0], 2);
    CAP_SERIAL.print(F(" residsd="));              CAP_SERIAL.print(sd, 2);
    CAP_SERIAL.print(F(" pos="));                  CAP_SERIAL.println(g_pos);

    for (uint32_t k = 0; k <= laps; k++) {
        CAP_SERIAL.print(F("# LAP ")); CAP_SERIAL.print(k);
        CAP_SERIAL.print(' ');         CAP_SERIAL.println(idx[k], 2);
    }
}

// ─── Belt-phase correction ──────────────────────────────────────────────────
//
// The index is a fixed OUTPUT angle, but the MOTOR step count at which it
// appears swings by tens of steps on a period of ~9 laps. That is the belt loop
// aliasing: the belt advances a non-integer fraction of a loop per output
// revolution, so its phase creeps lap to lap rather than repeating. Periodic
// and deterministic means correctable, and uncorrected it is the largest error
// left on the axis once the config scale factor is fixed.
//
// Homing does NOT tell you the belt phase. The index is one output angle and
// the belt only returns to the same phase every ~9 revolutions, which is not an
// integer, so knowing the angle leaves the phase unknown. It has to be measured
// from several consecutive index sightings, and the measured threshold is FOUR:
// below that the phase is genuinely unresolved, because a cosine is even about
// its peak and a short window cannot tell which side of the peak it is on. A
// wrong-signed correction is worse than none, which is exactly what 2- and
// 3-lap windows scored on the bench.
//
// Everything here is integer except building the cosine table once at startup.
// Position is carried in Q8 (steps * 256) throughout: the sub-step precision
// matters, because the whole correction is worth ~0.8 deg and the residual it
// is chasing is ~0.13 deg.
#define BELT_SPR_Q8    4223437   // 16497.8 steps/rev  <<8, measured over 60 laps
#define BELT_PERIOD_Q8    2304   // 9.00 laps <<8
#define BELT_A_FWD          44   // steps; direction-dependent, and the
#define BELT_A_REV          30   // difference is real — 31/29-lap captures
#define BELT_WIN_DEF         6   // laps of phase-finding window
#define BELT_COS_N         256

static int16_t g_cos[BELT_COS_N];      // cos scaled by 4096, one full cycle
static bool    g_cosReady = false;

static void beltInit() {
    if (g_cosReady) return;
    for (uint16_t i = 0; i < BELT_COS_N; i++)
        g_cos[i] = (int16_t)lrintf(cosf(2.0f * (float)M_PI * (float)i
                                        / (float)BELT_COS_N) * 4096.0f);
    g_cosReady = true;
}

// Table index for lap k, in units of 1/256 of a belt cycle. k*65536/P_q8 works
// out to k*256/P exactly, which is the cycle position scaled to the table.
static inline uint8_t beltIdx(uint32_t k, uint8_t phase) {
    return (uint8_t)(((int32_t)k * 65536L / BELT_PERIOD_Q8) + (int32_t)phase);
}

// A*cos(...) in Q8. g_cos is scaled 4096, so A*4096>>4 lands on A*256.
static inline int32_t beltWaveQ8(uint32_t k, uint8_t phase, int16_t amp) {
    return ((int32_t)amp * (int32_t)g_cos[beltIdx(k, phase)]) >> 4;
}

// Grid-search the phase with amplitude and steps/rev held FIXED.
//
// Fixed, not fitted, and that is the whole trick. Letting a short window also
// estimate the slope made 2-4 lap windows WORSE than no correction: a slope
// through 3 points extrapolated across 12 is wild, and the wildness swamps the
// 44-step signal being corrected. Pinning the amplitude matters for the same
// reason — a free amplitude shrinks toward zero to fit noise, which is how a
// short window fools itself into looking converged.
static uint8_t beltSolvePhase(const int32_t* rq8, uint32_t n, int16_t amp,
                              int32_t* offOut) {
    beltInit();
    uint8_t  bestP = 0;
    int32_t  bestOff = 0;
    uint32_t bestErr = 0xFFFFFFFFUL;

    for (uint16_t p = 0; p < BELT_COS_N; p++) {
        int32_t sum = 0;
        for (uint32_t k = 0; k < n; k++)
            sum += rq8[k] - beltWaveQ8(k, (uint8_t)p, amp);
        const int32_t off = sum / (int32_t)n;

        uint32_t err = 0;
        for (uint32_t k = 0; k < n; k++) {
            // >>4 before squaring: a Q8 residual of 100 steps squares to 6.5e8,
            // and a few of those overflow int32. Q4 keeps it safe and is still
            // far finer than anything the comparison can resolve.
            const int32_t e = (rq8[k] - beltWaveQ8(k, (uint8_t)p, amp) - off) >> 4;
            err += (uint32_t)(e * e);
        }
        if (err < bestErr) { bestErr = err; bestP = (uint8_t)p; bestOff = off; }
    }
    if (offOut) *offOut = bestOff;
    return bestP;
}

// `p` — sweep `laps` laps, solve the phase from the first `win` of them, then
// predict every lap AFTER the window and report what it actually did. The
// prediction laps are never seen by the solver, so the residual printed is an
// honest out-of-sample number rather than a fit quality.
static void runPhase(uint32_t intervalUs, uint32_t laps, uint32_t budgetPerLap,
                     uint32_t win, uint32_t dwellMs) {
    if (!g_enabled) { CAP_SERIAL.println(F("# ERR driver disabled — 'e 1' first")); return; }
    if (laps < 1 || laps > HOME_MAX_LAPS) {
        CAP_SERIAL.print(F("# ERR laps must be 1..")); CAP_SERIAL.println(HOME_MAX_LAPS);
        return;
    }
    if (win < 2 || win + 1 > laps) {
        CAP_SERIAL.println(F("# ERR need 2 <= win < laps")); return;
    }

    digitalWrite(DIR_PIN, g_dir ? HIGH : LOW);
    delayMicroseconds(5);
    const int32_t sign = g_dir ? -1 : 1;
    const int16_t amp  = g_dir ? BELT_A_REV : BELT_A_FWD;

    static float   idx[HOME_MAX_LAPS + 1];
    static int32_t rq8[HOME_MAX_LAPS + 1];
    int32_t baseline = 0;

    for (uint32_t k = 0; k <= laps; k++) {
        if (!sweepOneDip(intervalUs, budgetPerLap, sign, &baseline, &idx[k], false, false)) {
            CAP_SERIAL.print(F("# PHASE found=0 lap=")); CAP_SERIAL.println(k);
            return;
        }
        // Deliberate extra stop, for testing whether the per-lap pause is what
        // costs the node its accuracy. mirrorCentre already stops the axis for
        // ~50 ms; if that stop is the mechanism, lengthening it should make the
        // per-lap scatter worse in proportion. If scatter does not care, the
        // stop is innocent and the error is somewhere else entirely.
        if (dwellMs) delay(dwellMs);
        // Ramp removed here, in Q8, so the solver only ever sees the residual.
        rq8[k] = (int32_t)lrintf(idx[k] * 256.0f)
               - (int32_t)((BELT_SPR_Q8 / 1) * (int32_t)k);
    }

    int32_t off = 0;
    const uint8_t ph = beltSolvePhase(rq8, win, amp, &off);

    CAP_SERIAL.print(F("# PHASE found=1 laps="));  CAP_SERIAL.print(laps);
    CAP_SERIAL.print(F(" win="));                  CAP_SERIAL.print(win);
    CAP_SERIAL.print(F(" dir="));                  CAP_SERIAL.print(g_dir);
    CAP_SERIAL.print(F(" amp="));                  CAP_SERIAL.print(amp);
    CAP_SERIAL.print(F(" dwellms="));              CAP_SERIAL.print(dwellMs);
    CAP_SERIAL.print(F(" phase="));                CAP_SERIAL.print(ph);
    CAP_SERIAL.print(F(" offq8="));                CAP_SERIAL.print(off);
    CAP_SERIAL.print(F(" pos="));                  CAP_SERIAL.println(g_pos);

    // Held-out laps: corrected residual against uncorrected, the only
    // comparison that says whether any of this was worth doing.
    float sc = 0, su = 0;
    uint32_t nsc = 0;
    for (uint32_t k = 0; k <= laps; k++) {
        const int32_t w  = beltWaveQ8(k, ph, amp);
        const int32_t ec = rq8[k] - w - off;      // corrected
        const int32_t eu = rq8[k] - off;          // ramp removed only
        const bool held  = (k >= win);
        if (held) { sc += (float)ec * ec; su += (float)eu * eu; nsc++; }
        CAP_SERIAL.print(F("# PLAP "));  CAP_SERIAL.print(k);
        CAP_SERIAL.print(' ');           CAP_SERIAL.print(idx[k], 2);
        CAP_SERIAL.print(F(" r="));      CAP_SERIAL.print(rq8[k] / 256.0f, 2);
        CAP_SERIAL.print(F(" w="));      CAP_SERIAL.print(w / 256.0f, 2);
        CAP_SERIAL.print(F(" ec="));     CAP_SERIAL.print(ec / 256.0f, 2);
        CAP_SERIAL.print(F(" eu="));     CAP_SERIAL.print(eu / 256.0f, 2);
        CAP_SERIAL.println(held ? F(" held=1") : F(" held=0"));
    }
    if (nsc) {
        const float rc = sqrtf(sc / (float)nsc) / 256.0f;
        const float ru = sqrtf(su / (float)nsc) / 256.0f;
        CAP_SERIAL.print(F("# PRMS n="));       CAP_SERIAL.print(nsc);
        CAP_SERIAL.print(F(" corrected="));     CAP_SERIAL.print(rc, 2);
        CAP_SERIAL.print(F(" uncorrected="));   CAP_SERIAL.print(ru, 2);
        CAP_SERIAL.print(F(" gain="));          CAP_SERIAL.print(rc > 0 ? ru / rc : 0.0f, 2);
        CAP_SERIAL.print(F(" degc="));          CAP_SERIAL.println(rc * 360.0f / 16497.8f, 4);
    }
}

// Relative move, for putting the axis at a known offset before a homing trial
// and for commanded-angle tests. Signed: negative moves the other way.
static void runMove(int32_t steps, uint32_t intervalUs) {
    if (!g_enabled) { CAP_SERIAL.println(F("# ERR driver disabled — 'e 1' first")); return; }

    const uint8_t saved = g_dir;
    g_dir = (steps < 0) ? 1 : 0;
    digitalWrite(DIR_PIN, g_dir ? HIGH : LOW);
    delayMicroseconds(5);

    uint32_t n = (uint32_t)((steps < 0) ? -steps : steps);
    uint32_t next = micros();
    while (n--) {
        stepOnce();
        next += intervalUs;
        while ((int32_t)(micros() - next) < 0) { /* pace */ }
    }
    g_dir = saved;
    CAP_SERIAL.print(F("# MOVE pos=")); CAP_SERIAL.println(g_pos);
}

// ─── Command line ───────────────────────────────────────────────────────────

static void handleLine(char* s) {
    while (*s == ' ') s++;
    const char cmd = *s;
    if (!cmd) return;
    s++;

    switch (cmd) {
        case 'r': {
            char* end;
            const uint32_t iv = strtoul(s, &end, 10);
            if (end == s) { CAP_SERIAL.println(F("# ERR usage: r <interval_us> <steps> [preroll]")); return; }
            s = end;
            const uint32_t n = strtoul(s, &end, 10);
            if (end == s || iv == 0 || n == 0) {
                CAP_SERIAL.println(F("# ERR usage: r <interval_us> <steps> [preroll]")); return;
            }
            s = end;
            const uint32_t pre = strtoul(s, &end, 10);   // optional; 0 if absent
            runCapture(iv, n, (end == s) ? 0 : pre);
            break;
        }
        case 'h': {
            char* end;
            const uint32_t iv = strtoul(s, &end, 10);
            if (end == s) { CAP_SERIAL.println(F("# ERR usage: h <interval_us> <max_steps> [emit_window]")); return; }
            s = end;
            const uint32_t n = strtoul(s, &end, 10);
            if (end == s || iv == 0 || n == 0) {
                CAP_SERIAL.println(F("# ERR usage: h <interval_us> <max_steps> [emit_window]")); return;
            }
            s = end;
            const uint32_t emit = strtoul(s, &end, 10);
            runHome(iv, n, (end != s) && emit);
            break;
        }
        case 'H': {
            char* end;
            const uint32_t iv = strtoul(s, &end, 10);
            if (end == s) { CAP_SERIAL.println(F("# ERR usage: H <interval_us> <laps> <budget_per_lap>")); return; }
            s = end;
            const uint32_t L = strtoul(s, &end, 10);
            if (end == s) { CAP_SERIAL.println(F("# ERR usage: H <interval_us> <laps> <budget_per_lap>")); return; }
            s = end;
            const uint32_t b = strtoul(s, &end, 10);
            if (end == s || iv == 0 || b == 0) {
                CAP_SERIAL.println(F("# ERR usage: H <interval_us> <laps> <budget_per_lap>")); return;
            }
            runHomeMulti(iv, L, b);
            break;
        }
        case 'p': {
            char* end = s + 1;
            const uint32_t iv = strtoul(end, &end, 10);
            const uint32_t L  = strtoul(end, &end, 10);
            char* b1 = end;
            uint32_t b = strtoul(end, &end, 10);
            if (end == b1) b = 21447;
            char* w1 = end;
            uint32_t w = strtoul(end, &end, 10);
            if (end == w1) w = BELT_WIN_DEF;
            char* d1 = end;
            uint32_t dw = strtoul(end, &end, 10);
            if (end == d1) dw = 0;
            runPhase(iv ? iv : 400, L ? L : 12, b, w, dw);
            break;
        }
        case 'm': {
            char* end;
            const int32_t n = strtol(s, &end, 10);
            if (end == s) { CAP_SERIAL.println(F("# ERR usage: m <steps> <interval_us>")); return; }
            s = end;
            const uint32_t iv = strtoul(s, &end, 10);
            if (end == s || iv == 0) { CAP_SERIAL.println(F("# ERR usage: m <steps> <interval_us>")); return; }
            runMove(n, iv);
            break;
        }
        case 'z':
            g_pos = 0;
            CAP_SERIAL.print(F("# pos=")); CAP_SERIAL.println(g_pos);
            break;
        case 'd':
            g_dir = (strtoul(s, nullptr, 10) != 0) ? 1 : 0;
            CAP_SERIAL.print(F("# dir=")); CAP_SERIAL.println(g_dir);
            break;
        case 'e':
            driverEnable(strtoul(s, nullptr, 10) != 0);
            CAP_SERIAL.print(F("# en=")); CAP_SERIAL.println(g_enabled ? 1 : 0);
            break;
        case '?':
            CAP_SERIAL.print(F("# dir=")); CAP_SERIAL.print(g_dir);
            CAP_SERIAL.print(F(" en=")); CAP_SERIAL.print(g_enabled ? 1 : 0);
            CAP_SERIAL.print(F(" pos=")); CAP_SERIAL.print(g_pos);
            CAP_SERIAL.print(F(" hall=")); CAP_SERIAL.println(analogReadEnh(HALL_PIN, CAP_ADC_BITS));
            break;
        default:
            CAP_SERIAL.println(F("# ERR cmds: r <interval_us> <steps> [preroll] | "
                                 "h <interval_us> <max_steps> [emit_window] | "
                                 "p <interval_us> <laps> [budget] [win] [dwell_ms] | "
                                 "m <steps> <interval_us> | z | d <0|1> | e <0|1> | ?"));
            break;
    }
}

void setup() {
    CAP_SERIAL.begin(CAP_BAUD);

    pinMode(STEP_PIN, OUTPUT);  STEP_LOW();
    pinMode(DIR_PIN,  OUTPUT);  digitalWrite(DIR_PIN, LOW);
    pinMode(EN_PIN,   OUTPUT);

    // db_node4 is the laser-carrying board (-DNODE_HAS_LASER reclaims PD2 as a
    // digital laser gate). This scratch build has no laser logic at all, so pin
    // it LOW explicitly rather than leaving it floating — a scratch tool must
    // not be able to fire the laser.
    pinMode(PIN_PD2, OUTPUT);
    digitalWrite(PIN_PD2, LOW);

    driverEnable(false);

    // Reference the ADC to VDD, not the internal reference. The A1324 is
    // ratiometric — its quiescent output is 50% of ITS supply and its
    // sensitivity scales with supply too, so the output is a fixed FRACTION of
    // VDD rather than a fixed voltage. With VDD as the ADC reference that
    // fraction is what gets measured and supply variation cancels outright;
    // against a fixed internal reference, every supply wobble lands directly in
    // the reading — on a rail shared with stepper drivers. See docs/homing.md.
    analogReference(VDD);
    pinMode(HALL_PIN, INPUT);

    // Throw away the first conversions. The first read after a reference change
    // is taken before the reference has settled and comes back low — it showed
    // up in the first capture as a single sample ~90 counts BELOW the true
    // minimum of a real dip, which is exactly the sort of outlier that poisons a
    // global depth estimate and every fraction-of-depth threshold derived from it.
    for (uint8_t i = 0; i < 4; i++) (void)analogReadEnh(HALL_PIN, CAP_ADC_BITS);

    CAP_SERIAL.println();
    CAP_SERIAL.println(F("# hall_capture ready — r <interval_us> <steps> | d <0|1> | e <0|1> | ?"));
}

void loop() {
    static char buf[48];
    static uint8_t n = 0;

    while (CAP_SERIAL.available()) {
        const char c = (char)CAP_SERIAL.read();
        if (c == '\n' || c == '\r') {
            if (n) { buf[n] = '\0'; handleLine(buf); n = 0; }
        } else if (n < sizeof buf - 1) {
            buf[n++] = c;
        }
    }
}
