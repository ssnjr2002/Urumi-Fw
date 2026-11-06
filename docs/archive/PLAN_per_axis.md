# Plan: Per-Axis Planning

**Branch:** `microseg-host-drive`
**Date:** 2026-06-10
**Status:** Plan — Phase 0/1/2 not yet started

---

## Why now

The host pipeline works end-to-end for a **pen**, but the **drag-knife and
creasing tool — the primary tools — cannot be driven yet.** The blocker is the
A axis: `stage6` makes the XY tool honour `feed_max` on diagonals (the
geometry-aware interval fix, `0062f22`), but nothing limits how fast the A axis
rotates. On a tight curve the tangent spins fast, and with the real calibration
(A = 120 steps/deg, `214ed23`) the interval shrinks to a few CPU cycles —
~37 MHz of A stepping, physically impossible. `validate_plan` flags this as
`interval below floor` on the tangential (knife) plan.

For pen testing we currently sidestep it with `--no-tangential` (da = 0,
`cc54c46`). That is a stopgap. To cut, the planner must respect each axis's
**physical rate limit** — slowing the whole segment when an axis (A especially)
would otherwise overspeed.

This change also retires the **scalar bridge** the config has been warning about
since `eb5d4f9` / `214ed23`, and consumes `AxisConfig.invert` (`d7a2182`), which
will resolve the still-unverified **X-mirror** question from the pen-lift work.

---

## Current context (what already exists)

- **Config is per-axis** (`config.py`, `MachineConfig` / `AxisConfig`):
  - Real calibration: X/Y 160 steps/mm, Z 1200 steps/mm, A 120 steps/deg;
    node map X=1 Y=2 Z=3 A=4; `x.invert=True`.
  - `max_rate` / `accel` / `max_travel` are **carried but not consumed**.
    **Z and A `max_rate` are 0 (uncharacterised).**
  - A **scalar bridge** (`steps_per_mm`/`steps_per_deg` properties) exposes the
    legacy view and asserts X==Y, so non-square fails loudly.
- **stage5** plans tool velocity purely in **mm/s** — it never touches
  `steps_per_mm`. It is already axis-agnostic. Per-axis *acceleration* in stage5
  is a later refinement, out of scope here.
- **stage6** holds all the scalar coupling (6 sites): X/Y via scalar
  `steps_per_mm`, A via `steps_per_deg`, plus the interval math. Z lift already
  uses per-axis `z.steps_per_unit` (`cc54c46`).
- **Pico** owns machine state + position (`8f670d6`); manual ramped jogging
  exists (`26deae3`). Not affected by this change.
- **Verification debt carried in:** Z direction sign, no-travel-lines, and the
  X-mirror are unverified pending a mounted tool. Phase 1 (invert) is what makes
  the X-mirror check meaningful.

---

## Scope

Almost entirely `stage6`. `stage5` unchanged. No firmware changes.

### Phase 0 — Config calibration (user-supplied numbers)

Rate limiting divides by `max_rate × steps_per_unit`; a `max_rate` of 0 means
"no limit" (infinite). Before Phase 2 is meaningful, populate:

- `a.max_rate` — knife slew ceiling, **deg/s** (how fast the tangential axis can
  physically rotate).
- `z.max_rate` — Z raise/lower ceiling, **mm/s**.
- (X/Y already have `max_rate = 80`.)

Action: add placeholders in `_default_machine` with a loud comment; **user
replaces with measured values.** Until set, Phase 2 treats `max_rate = 0` as
"unlimited" for that axis (preserves current behaviour, no divide-by-zero).

### Phase 1 — Per-axis resolution + invert (retire the scalar bridge in stage6)

- X position → steps via `x.steps_per_unit`; Y via `y.steps_per_unit`
  (stage6 lines ~176–177, ~261–262).
- A via `a.steps_per_unit` (~185). Z lift already per-axis.
- Apply `axis.invert` to flip the sign of each commanded delta (dx/dy/dz/da).
- Remove stage6's dependence on the scalar `steps_per_mm` property.

**Effects:** fixes the X-mirror (consumes `x.invert`); enables non-square
machines. Low risk — on the square default the output is unchanged except the
intended X flip.

**Verify:** stage5/6 tests green; `validate_plan` parity on a square shape;
net-step signs flip as expected with invert; hardware toolless sanity.

### Phase 2 — Per-axis rate limiting in `_interval` (the knife unblocker)

For a segment with integer step deltas `(dX, dY, dZ, dA)` and segment time `T`:

```
axis step rate   r_i = |d_i| / T      must satisfy   r_i ≤ R_i
  where R_i = max_rate_i × steps_per_unit_i   (steps/s ceiling for axis i)

⇒  T ≥ T_min = max_i ( |d_i| / R_i )     over axes with R_i > 0

T_feed = governed by the XY tool feed (existing geometry-aware logic)
T_final = max(T_feed, T_min)
interval = T_final / major          (major = max |d_i|)
```

On a tight curve where A would overspeed, `T_min` (from A) dominates, so the
**whole segment slows** — A stays within its slew rate and XY slows with it.
That is the correct physical behaviour: the tool cannot corner faster than the
knife can rotate. Axes with `max_rate = 0` are skipped (treated as unlimited).

**Effects:** the `interval below floor` failures on the tangential plan
disappear; the knife plan becomes feasible.

**Known limitation (documented, not fixed here):** stage5 plans `v` without
knowing about A-rate slowdowns, so an A-limited segment runs slower than its
planned velocity — a one-directional, safe mismatch (never faster). Folding
per-axis rate/accel caps back into stage5's velocity profile is the future
refinement (and where stage5 finally goes per-axis too).

**Verify:** `validate_plan --tangential` on the fish passes interval bounds at
real A=120 once `a.max_rate` is set; sim shows segment times lengthen only where
A would have overspeed; hardware toolless on the knife path (watch A motor).

### Phase 3 — Soft-limit envelope (small, optional this round)

Consume `max_travel`: compute each path's extent and reject/flag any geometry
leaving the work envelope (`validate_plan` check + a host-side guard before
send). Independent of Phases 1–2; can follow or wait.

---

## Order & shippability

```
Phase 0 (config placeholders + user calibration)
   ↓
Phase 1 (resolution + invert)        — shippable alone; fixes X-mirror
   ↓
Phase 2 (per-axis rate limiting)     — makes the knife usable
   ↓
Phase 3 (envelope soft limits)       — safety, optional
```

Validate + hardware-toolless check after each phase.

---

## Parity note

All of this is deterministic integer/float math belonging to the
`machine` + `quality` parity spec the future C++ local-production port must
reproduce bit-for-bit. No nondeterminism introduced. Enforce `float`/`f`-suffixed
math when ported (no `double` on the M33).

---

## Out of scope (explicitly deferred)

- Per-axis **acceleration** in stage5 (velocity profile stays scalar `a_max`).
- Arc-length-accurate velocity model / **jerk** limiting (the ~1.7× a_max corner
  residual `validate_plan` reports at slack 2.0).
- A-axis **choreography**: pre-orientation at path start, reorient during jog,
  blade-offset compensation.
- Auto-lift at sharp corners (Z lift is between-subpaths only).
</content>
