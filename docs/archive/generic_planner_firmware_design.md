# Flatbed Cutter Firmware — Design Compilation

Firmware for a flatbed cutter with a tangential oscillating knife (and support for other tools such as a pen). Axes: X, Y (linear, bed plane), Z (tool up/down, mesh following), A (blade rotation). Target MCU: RP2350. Host: TypeScript, streaming over USB CDC ACM.

---

## 1. Design principles

- **Host = geometry, firmware = machine + time.** Anything needing a global view of the geometry is done on the host. Anything depending on machine parameters or real-time state (feed override, pause, buffer level, oscillator timing) is done in firmware.
- **Task space vs joint space.** The host never mentions axes. It describes what the tool does (follow this curve, break here, use tool N). The firmware maps that onto axes (kinematics).
- **Everything emerges from the XY path.** A comes from the tangent, Z from depth + mesh, corners from tangent breaks.
- **Tool-agnostic host.** Process knowledge (swivel vs lift, overcut, oscillator, start angle) lives in firmware tool profiles. SVG layer / stroke colour → tool number mapping is job data.
- **Positions are absolute at every layer.** Errors are corrected each tick, never accumulated.
- **Each conversion happens in exactly one place.** Host outputs mm; firmware converts to steps only in the follower.
- **Offsets are applied where they are exact** (see §14).

---

## 2. Geometry representation

- **Cubic Béziers only** for cutting geometry.
  - Lines: control points at 1/3 and 2/3 → constant parametric speed, `t(s)` exactly linear. Never put control points on endpoints (zero derivative → undefined tangent).
  - Quadratics: exact degree elevation (host).
  - Arcs: cubics of ≤ 90° (radial error ≈ 0.03 % of R).
- Béziers are **affine-invariant**: transforming the 4 control points transforms the curve exactly.
- **Splitting** (host, exact, de Casteljau). Reasons:

| Reason | Protects |
|---|---|
| Cusps, degenerate handles | Correctness (defined tangent, explicit corners) |
| Inflections (`B' × B'' = 0`, quadratic in t for a cubic — closed form) | Signed curvature sign change; A reverses rotation |
| Curvature extrema / curvature ratio (e.g. `κ_max/κ_min ≤ 2`) | Speed: cap only the tight portion |
| `t(s)` fit error | Smooth, even motion along the piece (rarely triggers) |

- Split points inside a curve are **G2** → no junction penalty.
- Host samples densely **only for analysis**; samples are discarded. Only split Béziers + bounds are sent.
- **Arc-length parameterisation:** host fits `t(s) = c1·s + c2·s² + c3·s³` per piece. Error in `t(s)` only shifts position *along* the exact curve (speed ripple), never off it. Fallback: incremental Newton + Gauss–Legendre in firmware.

Continuity vocabulary: G0 = position, G1 = tangent, G2 = curvature. (Not to be confused with G-code G0/G1.)

---

## 3. Host → firmware protocol

### Messages (job frame, mm, task space)

```ts
type Msg =
  | { kind: 'CONTOUR_BEGIN'; tool: number; x0: number; y0: number }
  | { kind: 'BEZIER'; p1: Vec2; p2: Vec2; p3: Vec2;   // p0 = previous endpoint
      length: number;        // mm, arc length
      kappaMax: number;      // 1/mm, max |κ| over piece (interval max)
      dkappaMax: number;     // 1/mm², max |dκ/ds|
      kappaStart: number;    // signed
      kappaEnd: number;      // signed
      ts: [number, number, number] }  // t(s) coefficients
  | { kind: 'BREAK' }        // G1 lost here; tool profile decides
  | { kind: 'CONTOUR_END' };
```

Travel between contours is implicit (move to the next contour's start, tool up).

### Binary BEZIER layout (little-endian)

| Field | Type | Bytes |
|---|---|---|
| Message type | u8 | 1 |
| Flags | u8 | 1 |
| Sequence number | u16 | 2 |
| p1, p2, p3 | 6 × f32 | 24 |
| length, κ_max, dκ_max, κ_start, κ_end | 5 × f32 | 20 |
| t(s) coefficients | 3 × f32 | 12 |
| CRC-16 | u16 | 2 |
| **Total** | | **62** |

- COBS framing → fits one 64-byte USB packet. Optionally repeat p0 for a continuity check.
- Float32 is sufficient (host: `DataView.setFloat32(..., true)`).
- USB CDC ACM baud rate is ignored; several hundred KB/s is achievable. Bandwidth is **not** the constraint (≈5000 blocks/s at 300 KB/s).

### Reliability & flow control

- CRC mismatch → stop, report seq. Duplicate seq → ignore. Gap → stop, report. Host resends from reported seq.
- **Backpressure:** when the command queue is full, stop reading USB; CDC pauses the host automatically.
- **Real-time commands** (feed hold, resume, feed override, abort) are a separate message class handled immediately by the parser, bypassing all queues.
- Firmware reports: acks, errors (with seq), progress (seq currently executing).

---

## 4. Architecture and core allocation (RP2350)

RP2350: 2 × Cortex-M33 @ 150 MHz, single-precision FPU (DCP for doubles exists but is slower — not needed), 520 KB SRAM, 12 PIO state machines.

```
USB ─► [RX FIFO] ─► parser ─► [command queue] ─► planner ─► [planner ring] ─► 1 kHz tick ─► staged incs ─► 50 kHz ISR ─► PIO ─► drivers
        core 0       core 0        core 0          core 0     core0→core1       core 1        core 1         core 1
```

**Core 0 (non-real-time):** USB, parsing, validation, transform, limits, tool-profile expansion, look-ahead, oscillator scheduling, limit switches, status/UI.

**Core 1 (real-time only):** 50 kHz step ISR (highest priority, hardware timer) and the 1 kHz tick (main loop woken by a flag the ISR sets when its sub-counter wraps; loop otherwise sleeps with `__wfi()`). Tick is triggered by the ISR → the two are phase-locked.

### Buffers

| Buffer | Between | Contents | Size |
|---|---|---|---|
| RX FIFO | USB → parser | Raw bytes | a few KB |
| Command queue | Parser → planner | Validated messages, job frame | ~64 msgs, ~4 KB |
| Planner ring | Core 0 → core 1 | Planned blocks, machine frame | 64–128 blocks, ~8–13 KB |

Small handoffs: pending BREAK slot (core 0), executor snapshot (core 1), staged increments + accumulator snapshot (core 1), PIO TX FIFO (4–8 words, hardware), TX buffer to host (few hundred bytes). Total ≈ 25 KB.

**Ring sizing:** must hold the stopping distance `v²/(2a)` (e.g. 500 mm/s, 5000 mm/s² → 25 mm) divided by typical block length.

---

## 5. Core 0: receive pipeline

1. **Framing:** accumulate until COBS delimiter; decode.
2. **CRC check.**
3. **Sequence check.**
4. **Decode** into struct (no endian conversion needed).
5. **Sanity checks:** finite floats; length > 0; κ_max, dκ_max ≥ 0; `t(s)` ≈ 1 at `s = length` and monotonic.
6. Push to command queue (job frame).

---

## 6. Core 0: block preparation

When the ring has space, the planner pops a message:

1. **Attach context:** `p0` = previous endpoint; tool from current contour.
2. **Transform to machine frame:** apply work offset + rotation to p1, p2, p3. Length, κ values and `t(s)` are invariant under rigid transforms (mirroring flips κ sign; scaling changes everything → never scale in firmware).
3. **Travel-limit check:** control points (convex hull) + blade-offset margin vs bed limits.
4. **End tangents:** start = `p1 − p0`, end = `p3 − p2`; unwrapped start/end blade angles.
5. **Speed cap** (see §7).
6. **Block acceleration** (see §7).
7. **Junction limit** with the previous block (see §8). Also sanity-check that tangents match on non-BREAK joins.
8. **Tool-profile expansion** (see §9).
9. Write expanded blocks + this block into the ring (wait if full → backpressure).
10. Rerun look-ahead (§10) and oscillator scheduler (§11).

### Block struct (sketch)

```c
typedef struct {
    uint8_t type;               // BEZIER, SWIVEL, LIFT, PLUNGE, TRAVEL, CMD, DWELL...
    uint8_t tool, flags;
    vec2    p[4];               // machine frame
    float   ts[3];              // t(s) coefficients
    float   length;             // mm (or rad / mm for single-axis blocks)
    float   kappa_max, dkappa_max, kappa_start, kappa_end;
    float   a_start, a_end;     // unwrapped blade angles
    // planner
    float   v_cap_sqr, max_entry_sqr, entry_sqr, exit_sqr;
    float   accel;
} block_t;
```

---

## 7. Speed and acceleration limits

A is slaved to the path: `A = tangent angle`, `ω = v·κ`, `α = a_t·κ + v²·dκ/ds`.

**Per-block speed cap** (worst point in the piece):

```
v_cap = min( feed_tool,
             ω_max / κ_max,              // A angular rate
             √(a_max / κ_max),           // XY centripetal
             √(α_frac · α_max / dκ_max)) // A angular accel from changing curvature
```

Use a fraction `α_frac < 1` so some angular-acceleration budget remains for tangential acceleration.

**Per-block acceleration:**

```
a_block = min( a_xy,  (α_max − v_cap² · dκ_max) / κ_max )
```

On a constant arc this reduces to `|a_t| ≤ α_max / |κ|` — on tight curves the blade's rotation limits how fast the machine can speed up / slow down.

**Notes**
- X, Y, A have separate motors → separate torque budgets. The coupling is kinematic (A must follow the tangent), not shared capacity.
- On a Cartesian machine the XY acceleration limit is a **box** (per axis); an inscribed circle is a simpler direction-independent approximation.
- Refinement (not yet implemented): tangential + centripetal share the XY budget ("friction circle"). Simple guard: reduce tangential acceleration on curved blocks.
- If A is belt-driven along the gantry, XY motion may rotate the blade → compensate A target from XY position.

---

## 8. Junctions

A junction is where one block ends and the next begins.

| Continuity | Junction speed |
|---|---|
| G0 (BREAK) | Tool profile: knife stops (swivel/lift); pen uses junction limit |
| G1 | Limited by curvature jump |
| G2 (split points) | No extra limit (only neighbours' caps) |

Signed curvature: + left, − right, 0 line. `Δκ = |κ_next,start − κ_prev,end|`.

- Line→line collinear: Δκ = 0.
- Line↔arc: Δκ = 1/R.
- Same-direction arcs: `|1/R₁ − 1/R₂|` (small).
- S-bend: `1/R₁ + 1/R₂` (largest).

**Blade-lag limit:** at a curvature step, A ramps at α; peak misalignment `e = Δω² / (2α)`. With tolerance `e_max`:

```
Δω_max = √(2 · α · e_max)
v_junction ≤ Δω_max / Δκ
```

Example: α = 2000 rad/s², e_max = 2° → Δω_max ≈ 11.8 rad/s → 118 mm/s entering R = 10 mm from a line.
If `Δω_max ≥ ω_max`, G1 junctions cost nothing extra in practice → G2 unnecessary.

The XY centripetal step needs a similar limit (`v² ≤ Δa_max / Δκ`), derived from an allowed path deviation.

---

## 9. Tool profiles

Each contour runs under its tool's profile. Each tool has an **XY tool offset** from the reference point.

### Knife (tangential, oscillating)
- **Contour start:** TRAVEL to p0 with A pre-rotating to start tangent (XY+A move together with knife up) → oscillator ON → dwell for spin-up → PLUNGE.
- **BREAK:** held pending until the next BEZIER arrives (angle needs next start tangent). Corner angle = next start tangent − previous end tangent.
  - Below swivel threshold (≈10–25°, material/blade dependent): SWIVEL in material (v → 0, rotate A only).
  - Above: overcut ≈ blade offset → LIFT → rotate → PLUNGE.
- **Symmetric blades:** 180° flip may shorten rotations.
- **Angles unwrapped** (continuous A, never ±180° wrap).
- **Contour end:** LIFT → oscillator OFF.
- **Blade offset** `d`: rotation axis leads/trails tip; applied per tick (`axis = P + d·T`).

### Pen
- A ignored/parked; BREAK = ordinary junction limit (no stop required); tool down = contact.

### Adding tools
New tool = new firmware profile; host unchanged.

---

## 10. Look-ahead planner

GRBL-style two-pass on the planner ring:

1. **Reverse pass** (newest → oldest unstarted): newest block exits at 0; `v_entry² ≤ v_exit² + 2·a·L`, capped by `max_entry` (junction limit) and `v_cap`.
2. **Forward pass:** `v_exit² ≤ v_entry² + 2·a·L`.
3. Stop early once speeds stop changing.
4. Each block → trapezoid (accel / cruise / decel), or triangle if short.

Rules:
- Last queued block always ends at 0 → ring draining = graceful stop (stutter, not overrun).
- Blocks being executed are never modified. The block after the executing one has its **entry pinned** to the executing snapshot's exit speed.
- Snapshot exit speeds are always safe: more look-ahead only raises later speeds.
- Single-axis blocks (SWIVEL, LIFT, PLUNGE) and CMD/DWELL blocks are barriers (entry/exit 0).

Future: S-curve (jerk-limited) profiles if ringing appears.

---

## 11. Oscillator timeout scheduling

Constraint: external knife controller auto-shuts off after 30 s; firmware can only toggle it off/on. Blade must never move through material while not oscillating.

- **Free resets:** OFF at every LIFT, ON before every PLUNGE.
- **Budget:** track time since last ON; budget ≈ 25 s (margin for decel, spin-up, uncertainty).
- **Predict:** sum planned trapezoid durations along the queued knife-down stretch; recompute on every replan.
- **Choose a toggle point before the budget expires**, preferring: existing SWIVEL → low-speed junction (tight curve) → any block boundary.
- **Force a stop** there (junction max speed = 0), insert a CMD block: OFF → ON → DWELL(spin-up) → resume.
- **Safety net:** if the budget is about to expire with no stop reached (ring dry, bad plan) → immediate feed hold.
- Toggle commands travel **through the planner ring** as zero-length blocks so they execute in sync with motion.

**Measure first:**
1. Does the 30 s timer start at ON, or at some other command?
2. Minimum OFF time needed to reset the timer.
3. Spin-up time to full amplitude.
4. What timeout looks like (abrupt/ramp; any readable signal).

---

## 12. Core 1: executor and 1 kHz tick

### Claiming a block
Under the spinlock: if `tail + 1 != head`, advance `tail`, copy trapezoid to local snapshot. Precompute once:

```c
t_acc    = (v_cruise - v_entry) / a;
t_dec    = (v_cruise - v_exit)  / a;
s_acc    = (v_entry + v_cruise) / 2 * t_acc;
s_dec    = (v_cruise + v_exit)  / 2 * t_dec;
t_cruise = (length - s_acc - s_dec) / v_cruise;   // 0 for triangle
```

Empty ring → hold position (already stopped since last block ends at 0).

### Each tick
1. **Advance time** by 1 ms; closed-form `s`:
   ```c
   if (t < t_acc)               s = v_entry*t + 0.5f*a*t*t;
   else if (t < t_acc+t_cruise) s = s_acc + v_cruise*(t - t_acc);
   else { float u = t - t_acc - t_cruise;
                                s = length - s_dec + v_cruise*u - 0.5f*a*u*u; }
   ```
   Past the end → snap `s = length`, release slot, claim next block, spend leftover time there (a tick may span several short blocks).
2. **Evaluate pose** (order matters):
   ```c
   float t    = clamp01(poly(blk->ts, s));
   vec2  P    = bezier(blk, t);                 // blade tip, machine frame
   vec2  T    = normalize(bezier_d(blk, t));
   float a    = unwrap(atan2f(T.y, T.x));
   float z    = z_ref - depth + mesh(P);        // mesh at the TIP
   vec2  axis = P + blade_d * T + tool_offset;  // rotation-axis position
   ```
3. **Convert** to absolute Q32.32 step targets (for the end of the *next* tick):
   ```c
   target[X] = to_q32(axis.x * spm_x);
   target[Y] = to_q32(axis.y * spm_y);
   target[A] = to_q32((a + a_cal) * spr_a);
   target[Z] = to_q32(z * spm_z);
   ```
4. **Follower increments** (§13), stage them, set `staged_ready`.

Other block types use the same machinery: SWIVEL/LIFT/PLUNGE run a trapezoid over angle or Z with XY fixed; CMD executes at its tick (e.g. oscillator GPIO); DWELL lasts a fixed time.

Missed tick deadline → ISR keeps previous increments (motion continues), error absorbed next tick, fault recorded.

---

## 13. Position followers (phase accumulators)

Modelled on LinuxCNC stepgen (position mode): the follower tracks absolute targets and corrects error every tick → no drift.

- Accumulator per axis: **64-bit Q32.32** (integer part = step position, fraction = phase).
- Frequency: `f_step = f_tick × inc / 2³²`; resolution at 50 kHz ≈ 12 µHz (slow A creep is smooth).

```c
for (int i = 0; i < 4; i++) {
    int64_t err = target[i] - accum_at_boundary[i];     // includes residual
    int64_t inc = err / 50;                              // 50 ISR ticks per 1 ms
    inc = clamp(inc, prev_inc[i] - max_dinc[i], prev_inc[i] + max_dinc[i]); // accel
    inc = clamp(inc, -max_inc[i], max_inc[i]);                              // velocity
    staged_inc[i] = prev_inc[i] = inc;
}
```

- Division remainder / clamp shortfall → appears as error next tick → corrected.
- Velocity clamp keeps `|inc| < 0.5 step/ISR` → at most one step per ISR per axis; max rate `f_ISR / 2` = 25 kHz at 50 kHz (e.g. 312 mm/s at 80 steps/mm).

Why not Bresenham: minor axes are slaved to the major axis → timing error up to one major period (bad at low speed, especially for A). AMASS (GRBL) reduces this by oversampling 2ⁿ at low rates; the fixed-rate DDA removes it (jitter ≤ one ISR tick for every axis).

---

## 14. Step ISR (50 kHz) and PIO

```c
void step_isr(void) {
    if (sub == 0 && staged_ready) {         // swap exactly at tick boundary
        memcpy(inc, staged_inc, sizeof inc);
        staged_ready = false;
    }
    uint32_t mask = 0, dir = 0;
    for (int i = 0; i < 4; i++) {
        int64_t prev = accum[i];
        accum[i] += inc[i];
        if ((accum[i] >> 32) != (prev >> 32)) mask |= 1u << i;
        if (inc[i] < 0) dir |= 1u << i;
    }
    if (mask) pio_sm_put(pio, sm, (dir << 8) | mask);
    if (++sub == 50) { sub = 0; snapshot_accums(); tick_flag = true; }
}
```

**PIO program:** receives dir+step bits; sets DIR, waits dir-setup, raises STEP for min pulse width, lowers, waits hold. All driver timing in hardware.

Z only moves with XY/A during mesh following; otherwise Z moves alone (LIFT/PLUNGE).

---

## 15. Cross-core synchronisation

- Planner ring = single-producer (core 0) / single-consumer (core 1), `head` and `tail` indices.
- **Adding blocks:** write block → `__dmb()` → advance `head`. Core 1: read `head` → `__dmb()` → read block.
- **Replan vs claim race:** RP2350 hardware spinlock, held briefly on both sides.
  - Planner computes outside the lock; writes final speeds under the lock; reads `tail` under the lock to skip the executing block and pin the next block's entry.
  - Tick takes the lock only to advance `tail` + snapshot.
- The 50 kHz ISR never touches the ring or the lock.

---

## 16. Units and conversions

```
SVG user units ─(viewBox, transforms, Y flip)─► mm, job frame        [host]
mm ─(work offset, rotation, planner, sampler, mesh, blade/tool off.)─► mm, machine   [firmware]
mm ─(steps_per_mm per axis, once per tick, absolute)─► Q32.32 steps  [followers]
```

- Internal units: mm, mm/s, mm/s²; A in **radians** (atan2, ω = v·κ natural).
- **Belt:** `steps/mm = motor_steps × microsteps / (pitch × teeth)` — e.g. 200×16/(2×20) = 80.
- **Leadscrew (Z):** `motor_steps × microsteps / lead` — e.g. 3200/8 = 400.
- **A:** `steps/rad = motor_steps × microsteps × gear / 2π` — e.g. 3:1 → ≈1528 steps/rad (26.7 steps/°).
- Treat factors as **calibrated** values per axis (measure long moves).
- **Precision:** float32 (~7 digits) → ~0.0001 mm at 2000 mm. Avoid unbounded accumulating floats: `s` resets per block, time per trapezoid. Accumulators are integers.
- Keep everything single-precision; the DCP double coprocessor is available but unnecessary.

---

## 17. Offsets — where each is applied

| Offset | Where | Why |
|---|---|---|
| Work offset + rotation (job → machine) | Once per block, on control points | Affine → exact; tangent/A automatically include rotation |
| Blade offset `d` | Per tick, `P + d·T` | Depends on tangent; offset Bézier isn't a Bézier |
| Tool XY offset | Per tick (constant add) | Per-tool mounting position |
| Mesh | Per tick, evaluated at tip `P` | Depends on bed position |
| A calibration | At step conversion | Homing zero vs blade direction |
| Z reference / depth | At step conversion | Surface height, material depth |

Host coordinates are **absolute relative to the job origin** — not incremental deltas (deltas accumulate error, a lost block shifts everything, resume becomes hard). Relative encoding of control points is acceptable only as exact fixed-point compression with absolute anchors.

---

## 18. Mesh bed levelling

- `z_target = z_ref − depth + mesh(P)` each tick, bilinear interpolation in bed coordinates.
- Done in firmware because bilinear along a line is quadratic within a cell (host would have to subdivide).
- Z becomes continuous during cuts; the "XYA or Z, never both" rule holds only for travel.
- Planner ignores Z during cuts. At mesh load, verify `max_slope × max_feed < v_z,max` and `max_slope × max_xy_accel < a_z,max`; refuse mesh or cap feed otherwise.
- Check Z step size vs depth tolerance. Most useful for kiss-cutting/scoring.

---

## 19. Real-time budget and placement (RP2350)

| Task | Rate | Cost | Share |
|---|---|---|---|
| Step ISR | 50 kHz | 100–200 cycles | 3–7 % of core 1 |
| Tick | 1 kHz | 2–4 k cycles | 1–3 % of core 1 |
| Look-ahead (64 blocks) | per block | few k cycles | few % of core 0 |
| Parsing (binary) | continuous | small | low |

- Core 1: 150 k cycles/ms available; > 90 % idle.
- **Place ISR, tick and everything they call (Bézier eval, `atan2f`, mesh) in SRAM** (`__not_in_flash_func()` / `__time_critical_func()`) — flash XIP cache misses stall and both cores share flash.
- Verify with GPIO toggles + scope (cycle counts, jitter).

---

## 20. Safety and error handling

- Corrupt / missing / out-of-order messages → stop, report seq.
- Validation failures → reject block, stop.
- Travel-limit violation → reject before planning.
- Oscillator budget about to expire → feed hold.
- Ring dry → graceful stop (last block exits at 0).
- Tick deadline miss → continue on previous increments, log fault.
- Real-time commands bypass queues (feed hold, abort, override).
- Homing: A needs an index sensor; store blade-direction calibration.

---

## 21. Decisions made and alternatives rejected

| Topic | Decision | Rejected / deferred |
|---|---|---|
| Step generation | Fixed-rate DDA phase accumulators | Bresenham (+AMASS); per-step scheduling (Klipper-style) deferred |
| Stepgen model | Position follower (LinuxCNC-style) | Open-loop velocity |
| Primitives | Cubic Béziers only | Lines (tessellation), biarcs (approximate, G1 joints), native clothoids (Fresnel integration) |
| Speed limits | Piecewise-constant cap per split piece | Dense velocity-limit curve / TOPP (too many units for firmware ring) |
| Planning location | Firmware (real-time adaptability) | Host-side planner (can't react to overrides, pauses) |
| Curvature continuity | G1 + blade-lag junction limit; G2 only inside split curves | G2 Bézier blends at junctions — add later if junctions limit throughput |
| Corner handling | Firmware tool profiles; host sends BREAK | Host-side swivel/lift decisions |
| Offsets | Firmware | Host (couples jobs to machine) |

---

## 22. Open items / to measure

1. Oscillator controller behaviour (§11).
2. A-axis `α_max`, `ω_max`, blade misalignment tolerance → decides whether G1 is enough (`√(2·α·e_max)` vs `ω_max`).
3. Swivel threshold per material/blade.
4. Blade offset `d`, A calibration offset.
5. Calibrated steps/mm per axis.
6. Driver timing (dir setup, pulse width, hold) for the PIO program.
7. Whether A is mechanically coupled to XY (belt routing).
8. Ring size vs actual block lengths and feeds.
9. Whether S-curve profiles or the combined-acceleration guard are needed.

---

## 23. Symbols

| Symbol | Meaning |
|---|---|
| s | Arc length along path (per block) |
| t | Bézier parameter (0…1) |
| v, a | Linear speed, acceleration (a_t tangential) |
| θ, A | Tangent / blade angle |
| ω, α | Angular velocity, angular acceleration of A |
| κ | Signed curvature (1/R) |
| Δκ, Δω | Finite jumps at a junction |
| R | Radius |
| d | Blade offset |
| e | Misalignment / error |
| P, T | Tip position, unit tangent |
