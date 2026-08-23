# Rotary A axis — measured behaviour

Everything here was measured on **node 4** (AVR128DB32, COM15) with the
`hall_capture` scratch firmware. The A axis drives through a deliberately
compliant, non-uniform 3D-printed belt. A magnet sits on the **output** side of
that belt — past the compliance — and a fixed A1324 Hall sensor on PD1 sees one
deep dip per output revolution.

That geometry is the whole story of this document. The sensor reports the true
output angle, which is what you want, but it reports it in a frame that the
motor step count only loosely predicts.

Bench tooling lives in `hall_bench/`. Firmware is `src/scratch/hall_capture.cpp`.

---

## 1. Constants

| quantity | value | notes |
|---|---|---|
| motor | 3200 steps/rev | 200 full steps, 1/16 microstepping soldered on the DRV8825 |
| output | **16497.8 ± 0.2 steps/rev** | 60 laps, two speeds, both directions |
| | **45.8272 steps/deg** | 0.021829 deg/step |
| reduction | 5.15559 | not a simple tooth ratio — see §5 |
| belt error period | ~8.81–9.00 laps | drifts, see §3 |
| belt error amplitude | 44.1 steps forward, 29.7 reverse | direction-dependent, real |
| backlash | ~1 step (0.03 deg) | measured directly, see §4 |
| homing precision | 4.2 steps (0.09 deg) | belt term removed |

`web/demo/config.json` currently carries `/heads[0]/a/stepsPerUnit = 45.46`
against the measured 45.8272 — a 0.79% error, so a commanded 360° turns about
357.2°. **This is still the largest error on the axis by an order of magnitude
and it is a one-line fix.** It is left alone here pending confirmation against a
physical mark on the belt.

### How steps/rev got ten times better without new data

Three obvious reductions of the same four captures disagreed by 6 steps:

    endpoint over the full 14-lap baseline    16491.4
    least-squares slope through all 15 dips   16495.3
    mean over 9-lap baselines                 16497.7

Each repeated internally to under a step, which made all three look precise and
at least two of them wrong. In fact all three were biased. A sinusoid spanning a
non-integer number of periods has non-zero correlation with a ramp, so *any*
slope drawn through those dip positions inherits some of the belt error — and
each reduction inherits a different amount. That is exactly the 6-step spread.

Fitting the belt term *alongside* the slope rather than averaging it away:

    pos[k] = a + b·k + c·cos(2πk/P) + d·sin(2πk/P),  scanning P

Across two speeds crossed with two directions: **16497.73, 16498.09, 16497.57,
16498.08 — sd 0.26**. The model also predicts the old biases correctly (−6.2 for
the endpoint, −2.6 for the plain slope), which is the check that says it is
right rather than merely tighter.

Confirmed later on an independent 31-lap capture: sub-windows give 16498.07 /
16497.64 / 16497.59, and forward vs reverse agree at **16497.77 vs 16497.78**.
Steps per revolution *must* be direction-independent, so that agreement is a
correctness check on the entire chain.

---

## 2. The index estimator

Candidates were ranked against the same captured dips (`hall_analyze.py`). The
absolute residual is inflated by belt error that is not the estimator's fault,
but every estimator sees identical belt error on identical data, so the ranking
holds; the pairwise table cancels it exactly.

**`est_mirror` won every run** (dev 0.98 / 0.76 / 1.07). It locates the dip's
symmetry axis by correlating the dip against its own reverse — autoconvolution
of a bump centred at *c* peaks at *2c*. It assumes only symmetry: no template,
no shape model, no depth calibration. That is why it beat the matched filter on
real data, where the dip width wanders 704–735 samples.

`argmin` and `parabolic` were ~10× worse, because the dip bottom is flat for
about ±100 steps.

The node runs this in fixed point, decimated 4:1. Node vs PC float over the very
same emitted window: **0.03–0.04 steps**. The arithmetic is not a limitation.

---

## 3. The belt error

The index is a fixed *output* angle and homing finds it every time. What wobbles
is the *motor step count* at which it appears — by ±44 steps (±1.0°), on a
period of about 9 laps.

Why 9 laps, when a once-per-revolution error would alias to DC: the dip is
sampled once per output revolution, so anything locked to output angle is
invisible. A period *longer* than one revolution requires something turning
slower than the output, and the belt loop is the only candidate. It advances a
non-integer fraction of a loop per output revolution, so its phase creeps lap to
lap rather than repeating.

Replicated across four independent captures at 8.80, 8.90, 9.00 and 9.06 laps,
and cross-validated between two different measurements of it: 45.9 steps of
*position* amplitude at period 9 implies 45.9·2·sin(π/8.9) = 32.1 steps of
*spacing* amplitude, against 32.7 measured.

**The amplitude is direction-dependent**: 44.1 steps forward, 29.7 reverse,
stable within each direction over 31 and 29 laps. An earlier 15-lap
disagreement (32.3 vs 26.2) was small-sample noise. A purely geometric error
would not care about direction; that this one does says part of it is tension or
lag, which is what a belt chosen to be compliant should do.

**There is a real second harmonic** at P/2, amplitude ~5 steps. Including it
cuts held-out error about 30% (12.1 → 8.4 steps). Worth having, not decisive.

**The period is not stable.** Within one continuous 31-lap capture the
single-sinusoid period moves 8.79 → 9.03 → 9.10, with amplitude falling
46.0 → 43.3. A belt meshing without slip *cannot* do that — its ratio to the
pulleys would be fixed by tooth counts. Fitting the second harmonic out does not
remove the wander, so this is genuine non-stationarity, consistent with creep on
a compliant belt. It is why long extrapolation degrades (0.26° across 27 laps)
while short-horizon correction does not.

---

## 4. Backlash — measured, and there is essentially none

The lap-based figure was 9.7 ± 5 steps (0.21°), too uncertain to compensate
with. It was also measuring the wrong thing: it differenced forward and reverse
index positions across whole revolutions, and since the belt amplitude is
direction-dependent (§3), that asymmetry lands directly in the difference and is
indistinguishable from lost motion.

The dip's **flank** offers a better instrument. Around ±450 steps from centre
the field changes ~4.6 counts/step, monotonically, against 23 counts of noise —
so for about 20° of the 360 the axis has genuine load-side position feedback.
Not enough to home with, but ideal for a short-range *relative* measurement,
which is what backlash is. The whole measurement then happens inside a few
hundred steps at one belt phase, so the belt contributes nothing.

    flank slope         -4.67 counts/step   (independent prediction: 4.6)
    reverse lost motion  1.30 steps (0.028 deg)  sd 0.91  sem 0.37
    forward lost motion -0.57 steps (0.012 deg)  sd 0.95  sem 0.39

**Do not apply backlash compensation.** Lost motion is about one step; forward
is statistically indistinguishable from zero. Applying the 9.7-step lap figure
would have injected a ~9-step error on every direction change, and it would have
looked plausible because the number came from real data.

Two limits: this is one angular position, and it is static (1000 µs/step, no
acceleration). Dynamic wind-up under a real accel profile is a different
quantity, unmeasured, and a compliant belt is exactly where it might matter.

---

## 5. There is no exact tooth ratio — stop looking for one

Tempting hypothesis: real pulleys have integer teeth, so 5.15559 should be a
simple rational. 165/32 = 5.15625 gives exactly 16500 steps/rev and sits within
the old ±3 error bar, and a 186-tooth belt predicts an 8.86-lap alias period
against ~8.81 measured. Three numbers appearing to agree.

**It does not survive the better steps/rev measurement.** 16500 is off by
−2.13 steps against a between-run sd of 0.26. Adding a belt second harmonic and
a term at 6.40 laps (where motor rotor error would alias under 165/32) moved the
slope by **0.01 steps** — so it is not slope bias. The rotor term also came out
no larger than the same fit at control periods with no physical meaning, so
there is no rotor signature either.

Searching all tooth sets jointly against ratio, alias period, and the constraint
that the pulleys must not intersect leaves exactly one survivor: 45T:232T with a
262T belt, requiring a non-standard motor pulley and a 148 mm output pulley.

The honest reading is that a deliberately compliant printed belt is not a clean
tooth-by-tooth kinematic constraint, so the ratio is a real number rather than a
fraction. **16497.8 ± 0.2 is the answer.** Counting teeth is still worth five
minutes as a falsification check — if they come back 32 and 165, something in
this chain is broken and it would be worth knowing.

---

## 6. Belt-phase correction

Periodic and deterministic means correctable. This is the largest remaining
error once the config scale factor is fixed.

**Homing does not tell you the belt phase.** The index is one output angle and
the belt only returns to the same phase every ~9 revolutions, which is not an
integer, so knowing the angle leaves the phase unknown. It must be measured from
several consecutive index sightings.

**The threshold is four laps**, and it is sharp. Below it the phase is genuinely
unresolved, because a cosine is even about its peak and a short window cannot
tell which side it is on — and a wrong-signed correction is worse than none.
The threshold reproduced exactly between the 15-lap and 31-lap sets and again on
hardware, which is what says it is a property of the problem rather than of one
dataset.

Two things matter in the solver, both counter-intuitive:

* **Hold steps/rev fixed.** Letting a short window re-estimate the slope made
  2–4 lap windows *worse than no correction*: a slope through 3 points
  extrapolated across 12 is wild, and the wildness swamps the 44-step signal.
* **Pin the amplitude.** Fitting `a·cos + b·sin` cannot enforce a known
  amplitude and will shrink the sinusoid toward zero to fit noise — which is how
  a short window fools itself into looking converged. Grid-search the phase
  instead.

Do not fit a phase once and extrapolate; the index passes once per lap for free,
so refresh it from a trailing window. Horizon barely matters — predicting 4 laps
ahead scores the same as 1 — so the limit is phase-estimation noise from a short
window, not drift over the horizon.

### On a PC, against continuous captures

    trailing 5 laps   0.150 deg      trailing 8 laps   0.115-0.131 deg
    trailing 6 laps   0.137 deg      uncorrected       0.9-1.0 deg

### On the node

Implemented in `hall_capture.cpp` as the `p` command: Q8 positions, a 256-entry
cosine table built once at startup, integer grid search over 256 phases,
`>>4` before squaring to keep the error sum inside int32. Flash 12.6%, RAM
16.4%; the search is imperceptible against a 6.6 s lap.

**The arithmetic is exact.** Node and PC pick the identical phase index (94 vs
94), agree on the offset to 0.008 steps and on per-lap residuals to 0.019 steps
(`hall_phase_node.py` runs the identical solve in float on the node's own
emitted residuals, so any disagreement would be arithmetic and nothing else).

**The accuracy does not transfer.** Six trials from random belt phases gave
0.21–0.43°, mean gain 2.4×, against 0.137° predicted.

---

## 7. Why the node cannot reach the PC's accuracy

This is the most useful thing the firmware exercise produced, and it would not
have been found by analysis alone.

Fit the node's own per-lap numbers a **free, perfectly-chosen** period and they
still only support 10–16 steps. The continuous PC capture supports 5.2 on the
same fit. So the stored period is nearly blameless — the correction is already
running at the ceiling its input data allows, and feeding it a better model
would change nothing. The limit is acquisition.

`hall_capture.py` never stops, and preroll's 2000 steps so the sweep begins at
settled speed — precisely because the magnet is on the load side of the belt.
The node stops ~50 ms at every dip to run `mirrorCentre`. A `dwell` argument was
added to `p` to test that rather than assert it:

    dwell      0 ms    250 ms   1000 ms
    scatter    21.6     59.3     113.3  steps

While the motor holds position the belt relaxes toward a new equilibrium and
carries the magnet with it. Longer stop, further relaxation. The direction is
**not** consistent between runs (−1.22 vs +2.06 steps/lap on successive trials),
so this is scatter, not a systematic creep that could be calibrated out.

`runHomeMulti` previously carried a comment asserting the per-lap stop was
harmless. That assumption is wrong and the comment now carries these numbers.

**The fix is not a better estimator — it is to stop stopping.** Chunk
`mirrorCentre` across step intervals so the sweep never halts. At 400 µs/step
there are roughly 9600 CPU cycles of slack per step against ~800k cycles of
autoconvolution per dip, so spreading it over a couple hundred steps costs well
under half the available slack.

---

## 8. Negative results, recorded so they are not re-proposed

**Multi-lap belt cancellation** (in `hall_capture.cpp`, kept and marked).
Summing residuals over exactly one full period should cancel a periodic error by
construction. Measured **0.7× — worse than single-lap homing**. The premise
fails, not the implementation: cancellation requires the period to equal an
integer number of laps, and the measured periods are 8.80 / 8.90 / 9.00 / 9.06 —
never integer. What leaks through is the same order as the belt swing itself.

**Least-squares slope over one period does not cancel a periodic error.** The
endpoint difference over exactly one period cancels exactly; a least-squares
slope does not, because a sinusoid over one period has zero mean but non-zero
correlation with a ramp. This is the same effect as §1 and it caught us twice.

**No motor-rotor signature.** A term at the period rotor eccentricity or cogging
would alias to buys an amplitude no larger than the same term at control periods
with no physical basis.

**A `DRV_MICROSTEPPING` mismatch was never a distance error.** The flag feeds
only the M0/M1/M2 pin writes in `drivers.cpp` and reaches no kinematics; on this
board those pins are not routed at all. The real scale error is the config
`stepsPerUnit` in §1.

---

## 9. Error budget, and what to do next

| source | magnitude | character |
|---|---|---|
| config `stepsPerUnit` | **2.8°/rev** | accumulates; one-line fix |
| belt error | ±1.0° | bounded, periodic, correctable to ~0.3° on-node |
| homing precision | 0.09° | floor |
| backlash | 0.03° | negligible, do not compensate |
| arithmetic | 0.0008° | not a limitation |

Recommended order:

1. **Fix `stepsPerUnit` to 45.8272**, after confirming against a physical mark.
   Worth more than everything else combined.
2. **Ship single-lap homing at 0.09°.** It works and is direction-independent.
3. **Leave belt correction as a scratch capability** until the axis is in real
   use. On-node it currently buys 0.9° → ~0.3°; the interleaved rewrite in §7
   might reach 0.15°, at the cost of restructuring the homing path that every
   future change has to live with. That trade is worth making only once 0.3° is
   demonstrably hurting something.

Unmeasured and worth knowing before trusting the axis under load: **dynamic
wind-up** during real acceleration profiles. Every compliance number here is
static. The flank technique in §4 measures it directly — park on the flank,
command an accel profile, watch the output lag.

### For the hardware team

The A1324 is ratiometric: its quiescent output is 50% of *its* supply, so any
VDD difference between sensor and ADC reference appears directly as apparent
angle. Everything above assumes that stays put.

If this axis ever needs to be better than ~0.1° without the homing dance, the
answer is a load-side absolute encoder — AS5600 (I²C, 12-bit) or AS5048A (SPI,
14-bit) reading the same magnet. That removes the belt from the measurement
entirely rather than modelling it, and makes §3, §6 and §7 all moot.
