# Plan: Pipeline Redesign — Sample-Centric Stages

**Branch:** `pipeline-redesign` (from `microseg-host-drive`)
**Date:** 2026-06-23
**Status:** Plan only — no code written yet

This document records a single architectural decision: **plan velocity per
arc-length sample, not per Bézier tile.** It supersedes the planner internals of
`PLAN_svg_tile_motion.md` §3.4 while leaving the wire protocol, the host/firmware
handoff, and the premortem risk analysis intact. Read those two plans first; this
is a revision note, not a replacement.

---

## 1. The core idea: tile vs sample

A **tile** is one whole cubic Bézier — the unit the SVG hands us, 4 control
points, anywhere from 0.5 mm to 80 mm long. The current pipeline (and
`PLAN_svg_tile_motion` §3.4) keeps tiles alive all the way to the end and plans
velocity at tile granularity: one cruise speed per curve, adjusted only at tile
boundaries.

A **sample** is one point *along* a flattened curve: position, unit tangent,
local curvature κ, and Δs to the next point. One tile flattens into many samples
— a long gentle curve into ~10, a tight feature into hundreds.

**Why the unit matters.** A tile is an arbitrary chunk of geometry; a single
Bézier can contain both a straight run and a hairpin. Per-tile planning must pick
*one* speed for the whole curve, so it picks the slow one — the hairpin taxes the
straight part. This is the exact bug we hit on hardware: one degenerate endpoint
(`|B'|→0`, a curvature spike over ~zero arc length) dragged a 17.8 mm curve to a
0.30 mm/s crawl because `kappa_max` is a whole-curve maximum. We patched it with
`stage4._kappa_max_moving` (exclude near-stationary samples). That patch is a
symptom of the wrong planning unit.

It breaks the other way too: fine detail (text, logos) is hundreds of
sub-millimetre tiles. Per-tile, each gets its own accel-up/decel-down profile and
the machine never reaches cruise — it chatters. The entire **merge-group**
machinery in `PLAN_svg_tile_motion` (§3.4 step 2, §6.1, §6.6 triangular-profile)
exists only to glue short tiles back together. It is a patch for choosing tiles
as the unit.

**Per-sample planning** flattens everything into one point stream, assigns a
speed to each point from the *local* curvature there, and runs the
forward/backward velocity passes across the whole stream. The straight part of
that 17.8 mm curve runs fast; only the few samples at the hairpin slow down.
Short tiles need no special handling — they are just more points in the list.
`_kappa_max_moving` deletes. The merge-group code deletes.

> Analogy: per-tile is one speed limit per *road* (a 50 km road gets one number
> even if it has a sharp bend midway). Per-sample is a limit per *100 m* — fast on
> the straights, slow only through the bend. Same roads, finer resolution, and you
> no longer need a rule like "merge short roads so the limit makes sense."

---

## 2. Redesigned stages (flat: stage n feeds stage n+1)

The organizing principle: **each stage lowers the representation exactly one
level, and never reaches back up.** No stage re-derives κ from Béziers at the end.

| # | Stage | Input → Output | Notes |
|---|---|---|---|
| 1 | **Parse** | SVG → cubic Béziers (subpaths) | today's stage1 |
| 2 | **Normalize** | curves → curves (mm, Y-flipped) | today's stage2 |
| 3 | **Repair** | curves → curves (C1) | today's stage3 — last stage where Béziers are the unit |
| 4 | **Flatten** | curves → **samples** | arc-length-spaced points carrying pos, tangent θ, **local** κ, Δs, flags. Beziers gone after here. |
| 5 | **Constrain** | samples → samples + `v_ceiling[i]` | per-sample local ceiling: feed, centripetal `√(a_lat/κ)`, A-slew `a_rate/κ`, junction-deviation, 0 at corner-stops. Local, parallel, trivially portable. |
| 6 | **Plan** | samples + ceiling → samples + `v[i]` | the look-ahead: backward decel sweep + forward accel sweep over the whole subpath. Per-axis accel enters here. Replaces today's stage5 **and** the trapezoid math in stage6. |
| 7 | **Choreograph** | samples + v → toolpath ops | tool-aware (`ToolProfile`): lift between subpaths, lift-pivot-lower at corners/cusps, A pre-orientation, A unwind. Today's `build_toolpath`. Wraps the v=0 corner-stops from stage 6. |
| 8 | **Discretize** | toolpath ops → MicroSegments | per-axis `steps_per_unit`, invert, Bresenham, `interval = f_cpu/(v·steps)`, per-axis rate clamp. Only stage that touches `steps_per_unit` — scalar bridge dies here. |
| 9 | **Serialize** | MicroSegments → wire packets | 26-byte pack, CRC8, seq stamp. Today's `host/serialise.py`. |

**The two splits that create the new seams:**

- Today's **stage 6** does flatten + choreograph + discretize at once → becomes
  **4, 7, 8** (with 5/6 between flatten and choreograph).
- Today's **stage 5** does constrain + plan at once → becomes **5 + 6**.

No new *work* is added — only seams. The same parity-spec computation runs; it is
just cut at honest boundaries.

---

## 3. The look-ahead planner (stage 6 detail)

Three passes over the flattened sample stream of a subpath:

**Pass 1 — per-sample ceiling** (this is stage 5, listed here for context):
for each sample `i`, `v_ceiling[i] = min(` feed_max, `√(a_lat/κ_i)`,
`a_rate/κ_i`, junction-deviation cap, `0` at corner-stop `)`. Uses **local** κ,
never a curve-wide max — so the degenerate spike caps one sample, not a curve.

**Pass 2 — backward (decel feasibility)**, last sample → first:
`v[i] = min(v_ceiling[i], √(v[i+1]² + 2·a·Δs))`. Guarantees you brake enough,
early enough, before every corner and tight feature — globally.

**Pass 3 — forward (accel feasibility)**, first → last:
`v[i] = min(v[i], √(v[i-1]² + 2·a·Δs))`. Guarantees no speed jump the machine
can't accelerate into.

After both sweeps every junction velocity is simultaneously reachable-from-behind
and stoppable-ahead. The **acceleration-continuity residual** we currently carry
(~448 vs 2·a_max=200, velocity steps at curve junctions) vanishes by construction
— junctions are no longer planning boundaries.

**Per-axis acceleration** drops in here for free: in passes 2/3 use
`a = min over present axes of (axis.accel projected onto this segment's
direction)` instead of the scalar `a_max`. That retires the `MotionConfig.a_max`
scalar and the `MachineConfig.steps_per_mm` assert-on-mismatch bridge.

**Porting note:** the 3-pass sweep is *easier* to port to C++ than the current
per-curve closed-form trapezoid (`stage6._velocity_at_s`). This redesign reduces
porting debt for local production, it does not add it.

---

## 4. What this does NOT change

- **Host/firmware handoff stays between stage 3 and stage 4.** The wire packet
  remains repaired Béziers (`SplineTile`, `PLAN_svg_tile_motion` §2.2). Host runs
  1–3; firmware (local-production mode) flattens and plans from stage 4 on. "Stage
  4 is the handoff" survives intact.
- **Wire protocol unchanged** — MicroSegment (0xAB), SplineTile (0xAD/0xAB),
  ToolConfig (0xAC), ACK/NACK/STATUS all as specified.
- **ATtiny firmware unchanged** — four identical stream-driven nodes.
- **Premortem risks survive, re-expressed in arc-length, not invalidated:**
  - **P1 (lookahead depth)** — window sized as `d_stop = v²/2a` in mm of path, not
    "16 tiles." This is exactly what P1's mitigation already demanded.
  - **P2 (commit-too-early)** — still fundamental: the commit pointer must lag the
    plan pointer by the decel distance. Re-run the backward sweep over the
    uncommitted span as samples arrive.
  - **P3 (accel quantization)** — *structurally* cured: per-sample planning means
    no steppy plateaus, so the `dt ≤ Δv_max/(a·|ds/dt|)` mitigation becomes native
    rather than a bolt-on. (`QualityConfig.dv_max` is already this knob.)
  - **P4 (interval ÷0 at v→0)** — keep the `v_min` clamp; corner dwell is via
    Z-lift, not infinite interval.
  - **P6 (blade offset)** — already `ToolProfile.offset_mm` + `OFFSET_TOLERANCE_MM`.
  - **P7 (A pre-orientation)** — already shipped; lives in stage 7 (choreograph).

---

## 5. Doc impact summary

| Doc / section | Status |
|---|---|
| `microseg_host_drive` — wire formats, architecture, build order | unchanged |
| `microseg_host_drive` — "Pico runs stages 4–6" → "stages 4–8" | renumber |
| `microseg_host_drive` — "pipeline (stages 1–6) is ground truth" → "1–9" | renumber |
| `svg_tile_motion` §2 packets, §4 ATtiny, §7 protocol | unchanged |
| `svg_tile_motion` §3.4 step 2 (merge groups) | **deleted** |
| `svg_tile_motion` §3.4 step 4 (per-tile pass) | **rewritten** as per-sample 3-pass (§3 above) |
| `svg_tile_motion` §3.4 step 5 (inline lift) | **split** into plan (v=0 samples) + choreograph stage |
| `svg_tile_motion` §6.1, §6.6 (merge / triangular) | **mostly deleted**; min-detail-feed survives as a per-sample floor |
| `svg_tile_motion` §10 premortem P1/P2/P3 | re-expressed in arc-length (§4 above) |

**Headline:** this is not a new architecture. Same host/firmware split, same
look-ahead the plans called for — but the planning quantum becomes the
**arc-length sample** instead of the **tile**. That one choice retires the
merge-group machinery and the accel-quantization mitigation, and is the only part
of `svg_tile_motion` §3.4 that needs a genuine rewrite rather than a renumber.

---

## 6. Migration order (when implementation begins)

The current code already works on hardware (the fish cuts). Do **not** rewrite in
place — keep the shipped path as the regression baseline.

1. **Flatten stage (4)** — extract sampling out of today's stage6 into a `Sample`
   stream with per-sample local κ. Validate the flattened stream reproduces
   today's geometry (positions, total arc length) within tolerance.
2. **Constrain (5)** — per-sample ceiling. Unit-test against known curves
   (circle → constant ceiling; straight → feed_max).
3. **Plan (6)** — the 3-pass look-ahead. Validate in sim it reproduces or beats
   the fish timing (today ~166 s) with no accel-continuity violations. This is the
   de-risking gate before touching the working stage5/6.
4. **Choreograph (7) + Discretize (8)** — split today's `build_toolpath` /
   microsegment tail along the new seam. Re-run the full hardware regression
   (square corners, fish) and confirm A unwind still ends at the expected angle.
5. Delete `_kappa_max_moving`, the merge-group code, the scalar bridge, and the
   `MotionConfig.a_max` scalar once the per-sample path passes hardware.

`Sample` dataclass is the spine of stages 4–7; get its fields right first
(pos_x, pos_y, theta, kappa, ds, flags) before building the stages around it.
