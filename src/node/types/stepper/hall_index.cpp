// hall_index.cpp — rotary index finding.
//
// See hall_index.h for the contract. What follows is the rule that shapes every
// constant in this file, learned the hard way across three bench runs:
//
//   A constant in ADC COUNTS describes the sensor and the magnet. That is the
//   same A1324 at a similar airgap on every head, so it transfers.
//
//   A constant in STEPS describes the gearing. Gearing is NOT shared between
//   heads -- node 4 and node 5 are mechanically unrelated -- so a step-domain
//   constant measured on one head is a guess on any other.
//
// The first version of this file carried node 4's step-domain numbers (window
// span, decimation, minimum width, settle distance) to node 5 on faith. Each
// was wrong there by a different factor, and each produced a different and
// misleading failure. So there are no step-domain constants here any more. The
// sweep MEASURES them, on whatever head it happens to be running on.
#include <Arduino.h>
#include <math.h>
#include "hall_index.h"
#include "common.h"

#ifdef HAS_HALL_INDEX

// ─── Count-domain: properties of the sensor, shared across heads ─────────────
#define HOME_ENTER    400   // counts below baseline to call it a dip. Noise is
                            // ~23 counts and the dip measured 2069 deep on node
                            // 4 and 2732 on node 5, so this sits ~17 sigma clear
                            // of the noise and far inside both dips. The margin
                            // is ~60x, which is why this one is not delicate.
#define HOME_EXIT     300   // hysteresis, so noise on a flank cannot re-trigger
                            // the exit test.

// ─── Sample-domain: RAM budget and window shape, NOT claims about mechanism ──
#define HOME_WIN      400   // capture buffer, in SAMPLES. 800 bytes of a 16 KB
                            // part. How many STEPS it spans is derived below.
#define HOME_PRE       40   // samples of pre-trigger context, head and tail.
#define CLEAR_MIN  HOME_PRE // consecutive clear samples required before a dip
                            // may be declared.

// How many times the sweep must cross the index before it will answer.
//
// Two is the minimum that can prove anything: a single excursion is
// unfalsifiable -- it could be the magnet, a transient, a stall, or the driver
// switching, and nothing in a sub-lap sweep distinguishes them. Two crossings at
// a consistent separation is a PERIODIC feature, and a periodic feature on a
// rotary axis is the index. That inference needs no knowledge of gearing.
//
// Three is the minimum that can prove it TWICE, which is what makes slip
// detectable: two intervals that disagree mean the axis did not travel what it
// was told. Raise this for more confidence -- laps are cheap on an axis with no
// hard stop, and everything here is written against the count, not against 3.
#define HALL_CROSSINGS  3
#if HALL_CROSSINGS < 2
#error "HALL_CROSSINGS < 2 cannot establish periodicity -- see the note above"
#endif

// Intervals may disagree by this fraction of their mean before the sweep is
// called a slip. Dimensionless, so it is not a mechanical constant: belt
// compliance moves the index by a fraction of a degree, well under a percent of
// a revolution, while a skipped step or a stall moves it by far more.
#define LAP_SPREAD_SHIFT  3   // tolerance = mean >> 3, i.e. 12.5%

// ONE buffer, not the scratch rig's two. mirrorCentre's first pass reads win[i]
// and writes at the same index with no cross-talk, so it transforms in place.
static int16_t g_win[HOME_WIN];

// ─── Sweep state ─────────────────────────────────────────────────────────────
// Written at the arm in loop context, then owned by the ISR until it reports
// done — same handshake as HomingState next door.
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

// Capture — only the FINAL crossing is buffered. The earlier ones are surveyed:
// their entry, exit and argmin are recorded, which costs 12 bytes each and is
// everything needed to derive the decimation, the lap length and the slip check.
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

// ─── ADC ─────────────────────────────────────────────────────────────────────
// FREE-RUNNING, and read rather than triggered. The scratch rig called
// analogReadEnh() inline in its step loop, which blocks for the whole
// conversion; that is affordable in a dedicated loop and is not affordable in
// the pulser ISR, whose whole budget is a few microseconds. Free-running makes
// the per-step cost a register read of the most recent completed conversion.
//
// At DIV16 on a 24 MHz part a 4x-accumulated conversion completes about every
// 36 us, so even a 400 us step has roughly eleven conversions available and the
// ADC is nowhere near being the limit on samples per dip. Decimation is, which
// is why decimation is now derived rather than chosen.
//
// The cost is that the sample is up to one conversion old rather than taken
// exactly at the step. That lag is CONSTANT, so it displaces the measured centre
// by a fixed amount — a datum offset, not scatter, and homing against that datum
// absorbs it. It does NOT absorb if the axis is homed in both directions,
// because the lag flips sign with travel.
//
// SAMPNUM_ACC4 + >>1 reproduces analogReadEnh(pin, 13) exactly: four 12-bit
// conversions accumulate to a 14-bit sum, and the shift rescales to 13 bits.
// That matters because HOME_ENTER and HOME_EXIT are in 13-bit counts.
void hallIndexSetup(void) {
    pinMode(HAL_HALL_PIN, INPUT);

    // VDD as the reference, not the internal one. The A1324 is RATIOMETRIC: its
    // quiescent output is 50% of ITS supply and its sensitivity scales with
    // supply too, so the output is a fixed fraction of VDD rather than a fixed
    // voltage. Referenced to VDD that fraction is what gets measured and supply
    // variation cancels; against a fixed internal reference every supply wobble
    // lands directly in the reading — on a rail shared with stepper drivers.
    // On AVR-Dx the reference is VREF.ADC0REF, not a field of ADC0.CTRLC as it
    // was on the tinyAVR parts — go through the core's own setter.
    analogReference(VDD);

    ADC0.CTRLC  = ADC_PRESC_DIV16_gc;
    ADC0.CTRLB  = ADC_SAMPNUM_ACC4_gc;
    ADC0.MUXPOS = HAL_HALL_MUXPOS;
    ADC0.CTRLA  = ADC_ENABLE_bm | ADC_RESSEL_12BIT_gc | ADC_FREERUN_bm;
    ADC0.COMMAND = ADC_STCONV_bm;      // kick it once; it re-triggers itself

    // Throw away the first conversions. The first read after a reference change
    // is taken before the reference has settled and comes back low — on the
    // scratch rig that showed up as a single sample ~90 counts BELOW the true
    // minimum of a real dip, exactly the sort of outlier that poisons a baseline
    // and every threshold derived from it.
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
}

// Size the capture from the crossing just surveyed. THIS is the number that was
// a magic constant three attempts running: the buffer holds HOME_WIN samples and
// must fit the dip plus a head and a tail of HOME_PRE each, so one sample has to
// cover at least width/(HOME_WIN - 2*HOME_PRE) steps. Measured width in,
// decimation out — no assumption about how many steps a revolution holds.
//
// The 32-bit divide is slow on this part (~200 cycles, ~8 us at 24 MHz) but runs
// exactly once per sweep, at a crossing, not per step.
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

    // Baseline is a running MAXIMUM, SEEDED FROM THE FIRST SAMPLE. The feature
    // is a dip, so the largest field seen is the away-from-magnet level. Seeded
    // at zero instead, the max has to climb through every real value on the way
    // up, and during that climb `baseline - v` is large for every sample — the
    // detector then finds a dip wherever it happens to be looking. Seeding costs
    // nothing: worst case the seed is taken ON the magnet, and the max corrects
    // itself the moment the axis leaves it. The CLEAR_MIN gate below is what
    // stops that first partial dip from being counted.
    if (!s_seeded) { s_baseline = v; s_seeded = true; }
    else if (v > s_baseline) s_baseline = v;

    const int32_t below = s_baseline - v;

    // Post-roll: the final crossing has ended and we are buffering as much tail
    // as we buffered head, so the window is symmetric about the feature — which
    // is what the mirror estimator assumes.
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

    // A dip may only be DECLARED after a run of clear samples. Two jobs: it
    // discards a sweep that starts sitting on the magnet (there is no clear run
    // until the axis leaves it, so that truncated dip is never counted and never
    // corrupts the lap length), and it guarantees the pre-trigger ring is full
    // at the entry — the exit test compares the tail length against preN, so
    // entering with a short ring completes the window almost immediately.
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

// Symmetry axis of the buffered dip, in decimated-sample units.
//
// est_mirror: correlate the dip against its own reverse, since the
// autoconvolution of a bump centred at c peaks at 2c. It assumes symmetry and
// nothing else — no template, no shape model, no depth calibration — which is
// why it beat the matched filter on real data, where the dip width wanders lap
// to lap and no single template fits every lap. argmin and a parabolic fit were
// ~10x worse, because the dip bottom is flat for about +/-100 steps. That is
// also why argmin is used only for the LAP LENGTH here, where a coarse position
// is plenty, and never for the datum.
//
// Fixed point throughout except the final vertex interpolation. Node vs PC float
// over the very same window agreed to 0.03-0.04 steps (docs/rotary_a_axis.md).
static float mirrorCentre(uint16_t n, int32_t baseline) {
    // Depth below baseline, clipped at zero so the flat shoulders contribute
    // nothing, and scaled down so the autoconvolution stays inside int32:
    // worst case n * (2732>>2)^2 is about 1.9e8 against a 2.1e9 ceiling.
    for (uint16_t i = 0; i < n; i++) {
        int32_t d = baseline - g_win[i];
        if (d < 0) d = 0;
        g_win[i] = (int16_t)(d >> 2);
    }

    // AC[k] = sum_i g[i]*g[k-i]. Keep a 3-deep history so the peak and both its
    // neighbours are available for the vertex fit without a second pass.
    int32_t h0 = 0, h1 = 0;
    int32_t best = -1, ba = 0, bb = 0, bc = 0;
    uint16_t bk = 0;
    const uint16_t kmax = (uint16_t)(2 * n - 1);

    for (uint16_t k = 0; k < kmax; k++) {
        const uint16_t lo = (k >= n) ? (uint16_t)(k - n + 1) : 0;
        const uint16_t hi = (k < n) ? k : (uint16_t)(n - 1);
        int32_t acc = 0;
        for (uint16_t i = lo; i <= hi; i++) acc += (int32_t)g_win[i] * g_win[k - i];

        if (k >= 2 && h1 > best) { best = h1; bk = (uint16_t)(k - 1); ba = h0; bb = h1; bc = acc; }
        h0 = h1; h1 = acc;
    }
    if (best <= 0) return -1.0f;

    const int32_t den = ba - 2 * bb + bc;
    const float delta = den ? (0.5f * (float)(ba - bc) / (float)den) : 0.0f;
    return ((float)bk + delta) * 0.5f;   // peak at 2c
}

void hallIndexResolve(void) {
    if (s_overflow) { s_cause = ROTARY_IDX_OVERFLOW; return; }

    // NOTFOUND is the honest verdict for every way of arriving here without a
    // complete set of crossings, and hallIndexCrossings() is what distinguishes
    // them: 0 means the sensor never saw the magnet (or the axis never moved, or
    // there is nothing on the pin), and 1..K-1 means the budget ran out before
    // the sweep could prove periodicity. Those used to be indistinguishable.
    if (s_nCross < HALL_CROSSINGS || !s_exiting || s_post < s_preN) {
        s_cause = ROTARY_IDX_NOTFOUND; return;
    }

    // Lap length, straight off the crossings. This is the number the whole
    // exercise exists to produce: it is not knowable in advance on an unknown
    // head, and every step-domain quantity anyone else needs derives from it.
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

    const float c = mirrorCentre(s_nwin, s_baseline);
    if (c < 0) { s_cause = ROTARY_IDX_DEGENERATE; return; }

    // Back to absolute step coordinates. Decimated sample j sits at
    // winStart + j*decim*sign, so a fractional j interpolates the same way.
    const float idxf = (float)s_winStart + c * (float)((int32_t)s_decim * (int32_t)s_sign);
    s_index = (int32_t)lroundf(idxf);

    // Refuse rather than warn. A datum measured across a slip is wrong BY the
    // slip, and it would be recorded as an ordinary-looking coordinate with
    // nothing downstream able to tell.
    s_cause = (s_lapSpread > (s_stepsPerRev >> LAP_SPREAD_SHIFT))
                ? ROTARY_IDX_SLIP : ROTARY_IDX_OK;
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
