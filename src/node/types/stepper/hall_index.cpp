// hall_index.cpp — rotary index finding.
//
// See hall_index.h. A constant in ADC counts describes the sensor and magnet,
// the same on every head. A constant in steps would describe the gearing, which
// differs between heads, so there are none: the sweep measures them.
#include <Arduino.h>
#include <math.h>
#include "hall_index.h"
#include "common.h"

#ifdef HAS_HALL_INDEX

// ─── Count-domain: properties of the sensor, shared across heads ─────────────
#define HOME_ENTER    400   // counts below baseline to call it a dip: far above
                            // the noise, far inside the dip (rotary_a_axis.md).
#define HOME_EXIT     300   // hysteresis, so noise on a flank cannot re-trigger
                            // the exit test.

// ─── Sample-domain: RAM budget and window shape, NOT claims about mechanism ──
#define HOME_WIN      400   // capture buffer, in SAMPLES. 800 bytes of a 16 KB
                            // part. How many STEPS it spans is derived below.
#define HOME_PRE       40   // samples of pre-trigger context, head and tail.
#define CLEAR_MIN  HOME_PRE // consecutive clear samples required before a dip
                            // may be declared.

// Crossings before the sweep answers. Two prove the dip repeats, so it is the
// index and not a transient; three prove it twice, so two laps that disagree
// show a slip.
#define HALL_CROSSINGS  3
#if HALL_CROSSINGS < 2
#error "HALL_CROSSINGS < 2 cannot establish periodicity -- see the note above"
#endif

// Intervals may disagree by this fraction of their mean before the sweep is
// called a slip: belt compliance is well under it, a skipped step or stall far
// over.
#define LAP_SPREAD_SHIFT  3   // tolerance = mean >> 3, i.e. 12.5%

// One buffer: the resolve's first pass transforms it in place.
static int16_t g_win[HOME_WIN];

// ─── Sweep state ─────────────────────────────────────────────────────────────
// Written at the arm in loop context, then owned by the ISR until it reports
// done, as LegState in leg.cpp.
static volatile bool     s_seeded;
static volatile int32_t  s_baseline;
static volatile uint16_t s_clearRun;    // consecutive samples clear of a dip
static volatile bool     s_inDip;
static volatile uint8_t  s_nCross;      // crossings COMPLETED so far
static volatile int32_t  s_enterPos;
static volatile int32_t  s_minPos;
static volatile int16_t  s_minVal;
static volatile int8_t   s_sign;
static volatile int16_t  s_lastRaw;

// Only the last crossing is buffered. The earlier ones record their entry, exit
// and minimum: enough for the decimation, the lap length and the slip check.
static volatile bool     s_capturing;
static volatile uint16_t s_decim, s_dcnt;
static volatile uint16_t s_nwin, s_preN, s_post;
static volatile bool     s_exiting, s_overflow;
static volatile int32_t  s_winStart;

static int16_t  s_ring[HOME_PRE];
static uint16_t s_rn, s_rhead;

typedef struct { int32_t enterPos, exitPos, minPos; } Crossing;
static Crossing s_cross[HALL_CROSSINGS];

static int32_t s_index       = 0;
static int32_t s_stepsPerRev = 0;
static int32_t s_lapSpread   = 0;
static uint8_t s_cause       = ROTARY_IDX_NONE;

// Resolve is a state machine because it spans loop() passes.
#define RS_IDLE 0
#define RS_RUN  1
#define RS_DONE 2
static uint8_t  s_rs      = RS_IDLE;
static uint16_t s_k, s_kmax;
static int32_t  s_h0, s_h1;
static int32_t  s_best, s_ba, s_bb, s_bc;
static uint16_t s_bk;

// ─── ADC ─────────────────────────────────────────────────────────────────────
// Free-running and read, not triggered, so a sample in the pulser ISR is one
// register read. A conversion completes about every 36 us at 24 MHz.
//
// The sample is up to one conversion old. The lag is constant, so it offsets
// the centre by a fixed amount, which homing absorbs; it flips sign with the
// direction of travel.
//
// SAMPNUM_ACC4 + >>1 reproduces analogReadEnh(pin, 13) exactly: four 12-bit
// conversions accumulate to a 14-bit sum, and the shift rescales to 13 bits.
// That matters because HOME_ENTER and HOME_EXIT are in 13-bit counts.
void hallIndexSetup(void) {
    pinMode(HAL_HALL_PIN, INPUT);

    // VDD as the reference: the A1324's output is a fraction of its supply, so
    // supply variation cancels. Set through the core's setter (VREF.ADC0REF).
    analogReference(VDD);

    ADC0.CTRLC  = ADC_PRESC_DIV16_gc;
    ADC0.CTRLB  = ADC_SAMPNUM_ACC4_gc;
    ADC0.MUXPOS = HAL_HALL_MUXPOS;
    ADC0.CTRLA  = ADC_ENABLE_bm | ADC_RESSEL_12BIT_gc | ADC_FREERUN_bm;
    ADC0.COMMAND = ADC_STCONV_bm;      // kick it once; it re-triggers itself

    // Discard the first conversions: the first after a reference change reads
    // low and would poison the baseline.
    for (uint8_t i = 0; i < 8; i++) {
        while (!(ADC0.INTFLAGS & ADC_RESRDY_bm)) { }
        (void)ADC0.RES;
        ADC0.INTFLAGS = ADC_RESRDY_bm;
    }
}

static inline int16_t hallRead(void) {
    return (int16_t)(ADC0.RES >> 1);   // 14-bit accumulation → 13 bits
}

void hallIndexArm(int8_t sign, int32_t posNow) {
    (void)posNow;
    s_seeded   = false;
    s_baseline = 0;
    s_clearRun = 0;
    s_inDip    = false;
    s_nCross   = 0;
    s_sign     = sign;

    s_capturing = false;
    s_decim = 1; s_dcnt = 0;
    s_nwin = 0; s_preN = 0; s_post = 0;
    s_exiting = false; s_overflow = false;
    s_winStart = 0;
    s_rn = 0; s_rhead = 0;

    s_index = 0; s_stepsPerRev = 0; s_lapSpread = 0;
    s_cause = ROTARY_IDX_NONE;
    // Abandon any resolve still slicing: this sweep overwrites its buffer.
    s_rs    = RS_IDLE;
}

// Size the capture from the crossing just surveyed: the buffer must fit the dip
// plus a head and a tail of HOME_PRE, so one sample covers at least
// width/(HOME_WIN - 2*HOME_PRE) steps. The 32-bit divide runs once per sweep.
static void armCapture(void) {
    const uint8_t last = (uint8_t)(s_nCross - 1);
    int32_t w = s_cross[last].exitPos - s_cross[last].enterPos;
    if (w < 0) w = -w;

    const uint16_t room = HOME_WIN - 2 * HOME_PRE;
    uint16_t d = (uint16_t)(((uint32_t)w + room - 1) / room);
    if (d < 1) d = 1;

    s_decim = d;
    s_dcnt  = 0;
    s_rn    = 0;  s_rhead = 0;
    s_nwin  = 0;  s_preN  = 0;  s_post = 0;
    s_exiting   = false;
    s_capturing = true;
}

// One step, from the pulser ISR. Returns true when the sweep has its answer and
// the move should end.
bool hallIndexSample(int32_t posNow) {
    const int16_t v = hallRead();
    s_lastRaw = v;

    // The baseline is the highest reading, seeded from the first sample: the
    // feature is a dip. A seed taken on the magnet corrects itself, and
    // CLEAR_MIN keeps that partial dip from counting.
    if (!s_seeded) { s_baseline = v; s_seeded = true; }
    else if (v > s_baseline) s_baseline = v;

    const int32_t below = s_baseline - v;

    // Post-roll: as much tail as head, so the window is symmetric about the dip,
    // as the mirror estimator assumes.
    if (s_exiting) {
        if (++s_dcnt >= s_decim) {
            s_dcnt = 0;
            if (s_nwin >= HOME_WIN) { s_overflow = true; return true; }
            g_win[s_nwin++] = v;
            if (++s_post >= s_preN) return true;
        }
        return false;
    }

    if (s_inDip) {
        if (v < s_minVal) { s_minVal = v; s_minPos = posNow; }

        if (s_capturing && ++s_dcnt >= s_decim) {
            s_dcnt = 0;
            if (s_nwin >= HOME_WIN) { s_overflow = true; return true; }
            g_win[s_nwin++] = v;
        }

        if (below < HOME_EXIT) {
            s_cross[s_nCross].enterPos = s_enterPos;
            s_cross[s_nCross].exitPos  = posNow;
            s_cross[s_nCross].minPos   = s_minPos;
            s_nCross++;
            s_inDip    = false;
            s_clearRun = 0;

            if (s_capturing) { s_exiting = true; s_post = 0; s_dcnt = 0; return false; }
            if (s_nCross >= HALL_CROSSINGS)     return true;   // capture never armed
            if (s_nCross == HALL_CROSSINGS - 1) armCapture();
        }
        return false;
    }

    // ── Clear of a dip ───────────────────────────────────────────────────────
    if (below < HOME_EXIT && s_clearRun < 0xFFFF) s_clearRun++;

    if (s_capturing && ++s_dcnt >= s_decim) {
        s_dcnt = 0;
        s_ring[s_rhead] = v;
        s_rhead = (uint16_t)((s_rhead + 1) % HOME_PRE);
        if (s_rn < HOME_PRE) s_rn++;
    }

    // A dip counts only after a run of clear samples: a sweep that starts on
    // the magnet does not count that dip, and the pre-trigger ring is full at
    // the entry.
    if (below > HOME_ENTER && s_clearRun >= CLEAR_MIN) {
        s_inDip    = true;
        s_enterPos = posNow;
        s_minVal   = v;
        s_minPos   = posNow;
        if (s_capturing) {
            s_nwin     = 0;
            s_preN     = s_rn;
            // The ring holds rn decimated samples taken BEFORE this one, so the
            // window begins rn*decim steps back along the direction of travel.
            s_winStart = posNow - (int32_t)s_rn * (int32_t)s_decim * (int32_t)s_sign;
            for (uint16_t k = 0; k < s_rn; k++)
                g_win[s_nwin++] = s_ring[(uint16_t)((s_rhead + HOME_PRE - s_rn + k) % HOME_PRE)];
            s_dcnt = 0;
        }
    }
    return false;
}

// ─── Resolve: symmetry axis of the buffered dip, SLICED ─────────────────────
//
// est_mirror: correlate the dip against its own reverse; the autoconvolution of
// a bump centred at c peaks at 2c. It assumes only symmetry (docs/rotary_a_axis.md
// compares it with the others). The dip's flat bottom makes the minimum good
// only for the lap length, never for the datum. Fixed point except the final
// vertex interpolation.
//
// Sliced: the correlation is O(n^2), ~170 ms at HOME_WIN, and commands are
// dispatched from loop(), so in one pass the node would miss the controller's
// polls.

// Multiply-accumulates per hallIndexResolveStep() call, ~2 ms, well inside a
// poll interval. Whole lags are finished, so a pass may overshoot by up to n.
#define RESOLVE_MACS 2048

// Everything cheap, and every answer that needs no correlation; those leave
// s_rs at RS_DONE, so the first hallIndexResolveStep() returns true.
void hallIndexResolveBegin(void) {
    s_rs = RS_DONE;

    if (s_overflow) { s_cause = ROTARY_IDX_OVERFLOW; return; }

    // Short of crossings: NOTFOUND, told apart by hallIndexCrossings().
    if (s_nCross < HALL_CROSSINGS || !s_exiting || s_post < s_preN) {
        s_cause = ROTARY_IDX_NOTFOUND; return;
    }

    // Lap length, from the crossings.
    int32_t sum = 0, dmin = INT32_MAX, dmax = INT32_MIN;
    for (uint8_t i = 1; i < HALL_CROSSINGS; i++) {
        int32_t d = s_cross[i].minPos - s_cross[i - 1].minPos;
        if (d < 0) d = -d;
        sum += d;
        if (d < dmin) dmin = d;
        if (d > dmax) dmax = d;
    }
    s_stepsPerRev = sum / (HALL_CROSSINGS - 1);
    s_lapSpread   = dmax - dmin;

    // Depth below baseline, clipped at zero so the flat shoulders contribute
    // nothing, and scaled down so the autoconvolution stays inside int32:
    // worst case n * (2732>>2)^2 is about 1.9e8 against a 2.1e9 ceiling.
    //
    // One pass over the window, O(n), so it stays here rather than being sliced
    // -- and it MUST complete before the first correlation slice reads g_win.
    for (uint16_t i = 0; i < s_nwin; i++) {
        int32_t d = s_baseline - g_win[i];
        if (d < 0) d = 0;
        g_win[i] = (int16_t)(d >> 2);
    }

    s_k    = 0;
    s_kmax = (uint16_t)(2 * s_nwin - 1);
    s_h0   = 0;
    s_h1   = 0;
    s_best = -1;
    s_bk   = 0;
    s_ba   = 0;
    s_bb   = 0;
    s_bc   = 0;
    s_rs   = RS_RUN;
}

// One bounded slice. Returns true when the answer (or the refusal) is final.
bool hallIndexResolveStep(void) {
    if (s_rs != RS_RUN) return true;

    // AC[k] = sum_i g[i]*g[k-i]. A 3-deep history keeps the peak and both its
    // neighbours available for the vertex fit without a second pass -- which is
    // also why the history has to survive across slices rather than being
    // rebuilt: the peak may sit on either side of a slice boundary.
    int32_t budget = RESOLVE_MACS;
    while (s_k < s_kmax) {
        const uint16_t n  = s_nwin;
        const uint16_t lo = (s_k >= n) ? (uint16_t)(s_k - n + 1) : 0;
        const uint16_t hi = (s_k < n) ? s_k : (uint16_t)(n - 1);
        int32_t acc = 0;
        for (uint16_t i = lo; i <= hi; i++) acc += (int32_t)g_win[i] * g_win[s_k - i];

        if (s_k >= 2 && s_h1 > s_best) {
            s_best = s_h1; s_bk = (uint16_t)(s_k - 1);
            s_ba = s_h0; s_bb = s_h1; s_bc = acc;
        }
        s_h0 = s_h1; s_h1 = acc;
        s_k++;

        // Charged AFTER the lag completes: a lag is indivisible (its history
        // update is not restartable), so the budget bounds the slice rather
        // than cutting one short.
        budget -= (int32_t)(hi - lo + 1);
        if (budget <= 0) return false;
    }

    s_rs = RS_DONE;

    if (s_best <= 0) { s_cause = ROTARY_IDX_DEGENERATE; return true; }

    const int32_t den = s_ba - 2 * s_bb + s_bc;
    const float delta = den ? (0.5f * (float)(s_ba - s_bc) / (float)den) : 0.0f;
    const float c     = ((float)s_bk + delta) * 0.5f;   // peak at 2c

    // Back to absolute step coordinates. Decimated sample j sits at
    // winStart + j*decim*sign, so a fractional j interpolates the same way.
    const float idxf = (float)s_winStart + c * (float)((int32_t)s_decim * (int32_t)s_sign);
    s_index = (int32_t)lroundf(idxf);

    // Refuse rather than warn. A datum measured across a slip is wrong BY the
    // slip, and it would be recorded as an ordinary-looking coordinate with
    // nothing downstream able to tell.
    s_cause = (s_lapSpread > (s_stepsPerRev >> LAP_SPREAD_SHIFT))
                ? ROTARY_IDX_SLIP : ROTARY_IDX_OK;
    return true;
}

int32_t hallIndexPos(void)        { return s_index; }
uint8_t hallIndexCause(void)      { return s_cause; }
int32_t hallIndexStepsPerRev(void){ return s_stepsPerRev; }
int32_t hallIndexLapSpread(void)  { return s_lapSpread; }
uint8_t hallIndexCrossings(void)  { return s_nCross; }

// Live sensor reading, taken NOW rather than remembered from the sweep. The
// bring-up instrument: turn the axis and watch it move. A value that does not
// respond to the magnet says the sensor is not on this pin.
int16_t hallIndexRaw(void)        { return hallRead(); }

// Running maximum from the last sweep: the away-from-magnet level the thresholds
// were judged against. With the raw value it gives the dip DEPTH, which is what
// says whether HOME_ENTER is set sensibly for this board.
int16_t hallIndexBaseline(void)   { return (int16_t)s_baseline; }

#endif  // HAS_HALL_INDEX
