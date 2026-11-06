# Architectural Plan: SVG Spline Tile Motion System

**Date:** 2026-06-06  
**Branch target:** `svg-tile-motion` (from `motion-plan`)  
**Status:** Plan only — no code written yet

---

## 1. System Overview

Replace the current PC-generates-steps model with a PC-sends-intent model:

```
SVG file
   │
   ▼
Host App (browser / Python / Node)
   │  Binary ToolConfig (21B) + SplineTile (37B) packets over USB serial
   ▼
RP2350 Core 0 — Tile ingest + motion planning
   │  MicroSegment queue (shared memory, lock-free)
   ▼
RP2350 Core 1 — Real-time step emission (unchanged role)
   │  RS485 9-bit UART (unchanged transport)
   ▼
ATtiny nodes — X / Y / Z / A stepper nodes (all identical, stream-driven)
```

The RP2350 owns all curve mathematics. The host sends geometry + intent; the Pico resolves to hardware steps.

---

## 2. New Data Structures

### 2.1 ToolConfig packet (PC → Pico, 22 bytes, sent once per path or on change)

```
Offset  Size  Field
0       1     magic = 0xAC
1       2     seq_num (uint16, wraps)
3       1     tool_type    (0=JOG, 1=CUT, 2=CREASE)
4       4     feed_max     (float, mm/s — XY cruise limit for this path)
8       4     lift_kappa   (float, 1/mm — curvature threshold for auto-lift)
12      4     lift_height  (float, mm — how far Z rises on lift)
16      4     z_feed       (float, mm/s — Z lift/lower speed)
20      1     CRC8
────────────────
21 bytes total
```

Pico caches one `current_tool_config`. All subsequent SplineTiles use these values until the next ToolConfig packet arrives.

### 2.2 SplineTile packet (PC → Pico, 37 bytes, sent per tile)

```
Offset  Size  Field
0       1     magic = 0xAB
1       2     seq_num (uint16, wraps)
3       1     flags   (bit0=MERGE_WITH_PREV, bit1=PATH_START, bit2=PATH_END)
4       32    control_points[4][2] as float32 pairs (P0x,P0y … P3x,P3y)
              coordinates in mm, machine origin
36      1     CRC8
────────────────
37 bytes total
```

Endianness: little-endian throughout.

Entry and exit velocities are not sent — the planner derives them entirely from lookahead. Boundary conditions are encoded by flags:
- `PATH_START` → planner sets v_entry = 0 (starting from rest after Z-lower)
- `PATH_END`   → planner sets v_exit = 0 (decelerate to stop before Z-lift)
- All other tiles → v_entry/v_exit computed by the forward + backward velocity pass

### 2.3 MicroSegment (Pico internal, Core 0 → Core 1, 14 bytes)

```cpp
struct MicroSegment {
    int16_t  dx;        // X axis steps (signed)
    int16_t  dy;        // Y axis steps (signed)
    int16_t  dz;        // Z axis steps (signed, positive = lift, negative = lower)
    int16_t  da;        // A axis steps (signed, tangential rotation)
    uint32_t interval;  // step interval in RP2350 clock cycles (for major axis)
    uint8_t  flags;     // MICRO_PATH_END | MICRO_STALL_RAMP
    uint8_t  pad;
};
```

`interval` drives the timing of the major axis step. All minor axes (X, Y, Z, A) use Bresenham error accumulators against the major axis at the same `interval` clock.

Z and A are no different from X and Y — they are stepper motor nodes receiving the same RS485 stream bytes. The planner simply schedules their steps alongside XY.

### 2.4 TilePlan (Pico internal, planner working struct)

```cpp
struct TilePlan {
    SplineTile  tile;
    float       path_length;    // computed arc length (mm)
    float       v_entry;        // resolved by planner forward+backward pass
    float       v_exit;         // resolved by planner forward+backward pass
    float       lift_at_t;      // parameter t where lift occurs (0 if none)
    float       lower_at_t;     // parameter t where Z lowers again (0 if none)
    bool        lift_required;
    bool        merged;         // absorbed into previous tile's velocity profile
};
```

### 2.5 Status telemetry packet (Pico → Host, 20 bytes, periodic)

Emitted by Core 0 every `STATUS_INTERVAL_MS` (default 20 ms) and on state change. This is what the validation app (Section 11) consumes to *measure* performance rather than assume it.

```
Offset  Size  Field
0       1     magic = 0xCC
1       4     t_micros        (uint32, Pico timestamp — for RTT / drift)
5       2     micro_seg_depth (uint16, occupancy of micro_seg_queue 0..512)
7       1     tile_q_depth    (uint8,  occupancy of tile_queue 0..32)
8       4     steps_emitted   (uint32, cumulative major-axis steps since reset)
12      2     underrun_count  (uint16, times Core 1 starved the queue)
14      4     current_feed    (float, mm/s — XY feed Core 1 is executing)
18      1     state           (0=IDLE 1=STREAMING 2=STALL 3=ESTOP 4=DRYRUN)
19      1     CRC8
────────────────
20 bytes total
```

`state = DRYRUN` indicates the Pico is ingesting + planning but **not** driving RS485 (see Section 11.2, dry-run validation mode).

---

## 3. RP2350 Firmware Architecture

### 3.1 Memory Layout (new buffers added)

```
current_tool_config — 1 × ToolConfig  = 21 B
tile_queue          — 32 × SplineTile = 32 × 37B  = 1,184 B
tile_plan_window    — 16 × TilePlan   = 16 × ~48B =   768 B
micro_seg_queue     — 512 × MicroSegment = 512 × 14B = 7,168 B
existing ring buf   — 128 × Segment   = replaced by micro_seg_queue
──────────────────────────────────────────────────────────────────
new RAM cost ≈ 9.1 KB  (well within 520 KB)
```

### 3.2 Core 0 — Three-Stage Pipeline

Core 0 now has three responsibilities running in a cooperative loop:

**Stage A: USB Ingest**
- Detect packet type by magic byte: 0xAC = ToolConfig, 0xAB = SplineTile
- Validate CRC8 on all packets; send NACK on failure
- ToolConfig (21B): update `current_tool_config`, send ACK — no queueing needed
- SplineTile (37B): on tile_queue full (>28/32) send NACK with BACKPRESSURE, host retries; otherwise push to `tile_queue`, send ACK with seq_num
- Also handles existing text commands: `stop`, `unalarm`, `ping`, `enable`, `disable`, `getpos`

**Stage B: Planner**
- Runs when `tile_queue` has ≥ 2 tiles and `micro_seg_queue` has < 256 entries (headroom)
- Pulls tiles into `tile_plan_window` (lookahead window, up to 16 tiles)
- Runs velocity resolution pass across the window (forward + backward scan)
- Evaluates each tile into MicroSegments
- Pushes MicroSegments to `micro_seg_queue`
- Details in section 3.4

**Stage C: FIFO / command relay**
- Same as current: polls multicore FIFO for responses from Core 1, prints to USB
- Handles `stop` / `unalarm` volatile flags immediately (unchanged)

Cooperative scheduling: A → B → C → A in a tight loop. No RTOS needed. B only runs one tile per loop iteration to keep ingest latency low.

### 3.3 Core 1 — Real-Time Engine (minimal changes)

Core 1 changes are limited:

- **Input source changes**: reads from `micro_seg_queue` instead of building its own kinematics. All velocity math moves to Core 0 planner.
- **Four-axis stream packing**: each MicroSegment's dx/dy/dz/da are resolved via Bresenham against the major axis. The stream byte packs all four axes (2 bits each = 8 bits, exactly as today). Z and A are just motors 3 and 4 — no protocol change.
- **Z-axis lift/lower**: the planner schedules Z-up steps before a sharp corner and Z-down steps after, interleaved in the stream. Core 1 has no awareness of "lift" as a concept — it just emits Z steps like any other axis.
- **A-axis rotation during lift**: while Z is moving up, the planner also schedules A steps to rotate to the new tangent. XY steps are zero during this phase. All four axes are driven simultaneously from the same MicroSegment stream.
- Emergency stop: unchanged — `emergencyStop` flag flushes `micro_seg_queue`.

### 3.4 Planner Detail

#### Step 1 — Path Length Computation
For each tile, approximate arc length via adaptive Gaussian quadrature on `|B'(t)|`:

```
L = ∫₀¹ sqrt(Bx'(t)² + By'(t)²) dt
```

Use 5-point Gauss-Legendre — accurate to <0.1% for typical SVG curves. Store in `TilePlan.path_length`.

#### Step 2 — Short Tile Merging
After computing path lengths, scan the window:

```
MERGE_THRESHOLD = 3.0 mm  (configurable)

For tile[i] where path_length < MERGE_THRESHOLD:
    if tile[i].flags & PATH_START → do not merge (new path, v_entry = 0)
    else → set tile[i].merged = true
           extend tile[i-1]'s velocity profile to cover tile[i]
```

Merged tiles share one trapezoidal velocity profile. Their geometry is still evaluated separately — only the velocity envelope is joint.

Chain merging: if tiles [3,4,5] are all short and consecutive, they form one merged group with one profile. The group profile's length = sum of constituent lengths.

#### Step 3 — Curvature Scan and Lift Detection
For each unmerged tile (or merged group), evaluate curvature at N=20 evenly-spaced t values:

```
κ(t) = |Bx'By'' - By'Bx''| / (Bx'² + By'²)^(3/2)
```

If `max(κ) > lift_kappa` from the tile's metadata:
- Find `t_lift` = parameter where κ first exceeds threshold
- Find `t_lower` = parameter where κ drops back below threshold
- Set `TilePlan.lift_required = true`, record `t_lift`, `t_lower`
- Force `v = 0` at `t_lift` (deceleration constraint)
- Force `v = 0` at `t_lower` (re-acceleration start)

#### Step 4 — Velocity Resolution (Forward + Backward Pass)

This is a standard trapezoidal velocity planner operating on the tile plan window:

**Forward pass** (i = 0 → N):
```
v_entry[0] = 0  if tile[0].flags & PATH_START, else v_exit of previous window
v_max[i]   = min(current_tool_config.feed_max, v_from_curvature[i])
             where v_from_curvature = sqrt(a_max / κ_max)  (centripetal limit)
v_reachable[i] = sqrt(v_entry[i]² + 2 × a_max × path_length[i])
v_exit[i]  = min(v_max[i], v_reachable[i])
v_entry[i+1] = v_exit[i]
```

**Backward pass** (i = N → 0):
```
v_exit[N]  = 0  if tile[N].flags & PATH_END, else leave forward-pass value
v_limited  = sqrt(v_exit[i]² + 2 × a_max × path_length[i])
v_exit[i]  = min(v_exit[i], v_limited)   ← tighten if decel can't reach target
v_entry[i] = min(v_entry[i], v_exit[i] + 2×a_max×length) ← propagate back
```

Lift points inject `v=0` constraints into the pass as hard breaks.

#### Step 5 — Bezier Evaluation → MicroSegments

For each tile (or merged group), evaluate the Bezier with adaptive step size:

```
tolerance = 0.01 mm  (chord deviation limit)
dt = min(dt_max, sqrt(8 × tolerance / |B''(t)|))
dt_max = 0.1  (never skip more than 10% of parameter range per step)
```

At each sample point t:
- Compute position B(t) → convert to steps (steps_per_mm × position)
- Δx = steps_x[t] - steps_x[t-dt]  (integer)
- Δy = steps_y[t] - steps_y[t-dt]
- Δz = 0  (tool is down, cutting)
- Compute tangent angle θ(t) = atan2(By'(t), Bx'(t))
- Δa = angle_to_steps(θ(t) - θ_current)  (shortest rotation path, wrap ±180°)
- Compute instantaneous feed rate from velocity profile at arc-length-equivalent t
- interval = (F_CPU / steps_per_mm) / v(t)
- Emit MicroSegment(Δx, Δy, Δz=0, Δa, interval, flags)

At `t_lift` (sharp corner detected):
1. Emit deceleration MicroSegments bringing XY to v=0 (Δz=0 throughout)
2. Emit Z-up MicroSegments: Δx=0, Δy=0, Δz=+lift_steps, Δa=0, interval=z_lift_interval
3. Emit A-rotation MicroSegments: Δx=0, Δy=0, Δz=0, Δa=Δangle_to_new_tangent, interval=a_rotate_interval
4. Emit Z-down MicroSegments: Δx=0, Δy=0, Δz=-lift_steps, Δa=0, interval=z_lift_interval
5. Resume normal XY+A MicroSegments from v=0

`z_lift_interval` and `a_rotate_interval` are derived from configurable Z and A feed rates (mm/s and deg/s respectively). All emitted as plain MicroSegments — Core 1 treats them identically.

---

## 4. ATtiny Firmware Changes

### 4.1 No Changes Required

All four ATtiny nodes are identical stepper driver nodes. The Z-axis (tool lift) and A-axis (tangential rotation) are stepper motors like X and Y — they receive the same step/direction stream bytes. The Pico planner is entirely responsible for generating appropriate Z and A steps.

The existing stream byte format already supports all four axes:

```
Bits 1-0: motor 1 = X  (dir | step)
Bits 3-2: motor 2 = Y
Bits 5-4: motor 3 = Z  ← lift/lower is just Z steps up or down
Bits 7-6: motor 4 = A  ← tangential rotation is just A steps CW/CCW
```

No new RS485 commands. No new pin assignments. No ISR changes. The ATtiny firmware is complete as-is for this feature.

---

## 5. Host Application Architecture

The host app has three deployment targets (all share the same SVG processing core):

| Target | Runtime | Serial access |
|---|---|---|
| Browser web app | Vanilla JS / TypeScript | WebSerial API |
| Python CLI | Python 3.10+ | pyserial |
| Node.js daemon | Node 20+ | serialport npm |

### 5.1 SVG Processing Pipeline (shared core, Python or JS)

```
SVG file
   │
   ├─ Parse SVG DOM
   │   Extract <path> elements, preserve layer/group/color
   │
   ├─ Decompose to cubic Beziers
   │   Convert all path commands to C (cubic) form:
   │   - L → degenerate cubic (control points on the line)
   │   - Q → degree-elevated cubic
   │   - A → approximate as 1-4 cubics (standard arc→bezier formula)
   │   - Z → explicit closing line segment
   │
   ├─ Enforce C1 continuity at joins
   │   For each join point between segment[i] and segment[i+1]:
   │   Check: P3-P2 of [i] parallel to P1-P0 of [i+1]
   │   If not: insert a short blending cubic (G1 repair)
   │   Log warning for joins that required repair
   │
   ├─ Tool assignment
   │   Map SVG layer / stroke color → tool type + feed rate:
   │   user-configurable table, e.g.:
   │   red (#FF0000) → CUT, 80 mm/s, lift_kappa=1.5
   │   blue (#0000FF) → CREASE, 120 mm/s, lift_kappa=3.0
   │   green (#00FF00) → JOG (no tool), 300 mm/s
   │
   ├─ Path ordering
   │   Nearest-neighbour greedy sort to minimise total jog distance.
   │   Optional: 2-opt improvement pass for longer jobs.
   │   Insert JOG segments between non-contiguous paths.
   │
   ├─ Short segment detection + flagging
   │   Compute path length of each tile.
   │   If length < MERGE_THRESHOLD (3mm): set MERGE_WITH_PREV flag.
   │   If first segment of a path < 3mm: flag PATH_START, do not merge.
   │
   └─ Tile stream output
       Send ToolConfig packet whenever tool_type / feed_max / lift_kappa changes
       (i.e. at the start of each new path color group, or once per job if uniform).
       Assign seq_num (uint16, wrapping) to each SplineTile.
       Set PATH_START flag on first tile of each path.
       Set PATH_END flag on last tile of each path.
       Serialise each tile to 37-byte binary SplineTile packet.
```

### 5.2 Serial Protocol (host ↔ Pico)

Baud: 115,200 (existing USB CDC)

**Host → Pico:**
```
ToolConfig packet  (21 bytes binary, magic 0xAC) — sent on tool/feed change
SplineTile packet  (37 bytes binary, magic 0xAB) — sent per tile
Text commands unchanged: stop, unalarm, ping, enable N, disable N, getpos N
```

**Pico → Host:**
```
ACK packet:   0xAA [seq_num 2B] [CRC8]              (4 bytes)
NACK packet:  0xBB [seq_num 2B] [reason 1B] [CRC8]  (5 bytes)
  reason: 0x01=CRC_ERROR, 0x02=BACKPRESSURE, 0x03=BAD_MAGIC
Text responses unchanged: ready, nope, pong, pos=..., ok, alarm
```

Host streaming logic:
```
window = 16  (max in-flight unACKed tiles)
send tile → add to in-flight set
on ACK → remove from in-flight, send next tile
on NACK/BACKPRESSURE → wait 10ms, resend
on timeout (100ms no ACK) → resend tile
```

### 5.3 Web App UI (browser)

Pages / views:

**Setup view:**
- Serial port connect (WebSerial)
- Machine config: steps/mm per axis, a_max, tool node assignments
- Tool table: color → tool type, feed, lift threshold

**Job view:**
- SVG file drop / open
- Rendered preview showing:
  - Toolpath with color per tool type
  - Jog moves shown dashed
  - Short-merge zones highlighted
  - Auto-lift points marked
- Path order visualised (numbered arrows)
- Estimated job time
- Send button → streams tiles with live progress bar

**Monitor view:**
- Tiles sent / ACKed counter
- Current feed rate (reported back from Pico, new status packet)
- Emergency stop button (sends `stop\n` text command)
- Position readout per axis

### 5.4 Python CLI

```
python svg_cut.py --port COM3 --svg design.svg --config tool_config.yaml
```

Flags:
- `--dry-run` — plan and preview path without sending
- `--preview` — open matplotlib plot of toolpath (reuse existing plot_moves.py style)
- `--order nearest|manual` — path ordering strategy
- `--merge-threshold 3.0` — short tile merge threshold in mm
- `--no-lift` — disable auto-lift (override per-color setting)

---

## 6. Motion Planning Issue Mitigations

### 6.1 Short Tile Chains (< 3 mm consecutive segments)

**Problem:** Fine detail (text, logos) produces many sub-3mm Beziers. Individual trapezoidal profiles would mean constant acceleration/deceleration with no cruise, degrading cut quality.

**Mitigation — Merge Groups:**
- Planner accumulates consecutive short tiles into a merge group
- One trapezoidal profile spans the entire group
- Entry/exit of the group follow normal lookahead rules
- Within the group, velocity varies only by centripetal limit, not by tile boundaries
- Group size cap: 20mm total — beyond that, split into a new group

**Mitigation — Minimum Detail Feed:**
- If a merge group's total length is still < 5mm, cap v_max at `DETAIL_FEED_MAX` (e.g. 20 mm/s)
- This ensures the machine can always complete the group without stopping
- Configurable per tool type

### 6.2 Sharp Corners / High Curvature

**Problem:** Curvature spike → centripetal limit → near-zero velocity → machine chatter, potential overcut.

**Mitigation — Auto-Lift Protocol (all via stream steps):**
1. Detect κ > threshold during planning scan
2. Backward pass forces v=0 at `t_lift`
3. Planner emits Z-up MicroSegments (Δz = +lift_height_steps, XY = 0)
4. Planner emits A-rotation MicroSegments (Δa = angle to new tangent, XY/Z = 0)
5. Planner emits Z-down MicroSegments (Δz = -lift_height_steps, XY = 0)
6. Resume XY from v=0

All steps go through the normal RS485 stream (9th bit = 0). No command channel used.
Core 1 has no concept of "lift" — it just emits whatever the MicroSegment says.

**Lift point detection refinement:** Only lift if:
```
κ > lift_kappa  AND  v_centripetal < v_min_usable (e.g. 5 mm/s)
```
This avoids spurious lifts on gentle curves where slowing down is sufficient.

### 6.3 Velocity Discontinuity at Tile Seams

**Problem:** If C1 continuity breaks at a tile join, the tangent jumps → A-axis must make a sudden rotation → velocity must drop to 0.

**Mitigation — Host-side C1 enforcement:**
- SVG processor checks each join and inserts repair cubics where needed
- Logs all repaired joins so user can inspect the SVG

**Mitigation — Pico-side fallback:**
- At each tile seam, compute angle delta between exit tangent of tile[i] and entry tangent of tile[i+1]
- If |Δθ| > C1_ANGLE_TOLERANCE (e.g. 5°): schedule a Z-up / A-rotate / Z-down sequence via stream steps (same as sharp corner handling)
- This catches any discontinuities the host missed

### 6.4 A-Axis Wrap-Around

**Problem:** Knife angle near ±180° → ambiguous shortest rotation path → potential 360° spin.

**Mitigation:**
- Track cumulative A-axis angle (unwrapped float, not modulo)
- Compute rotation delta as the shorter of the two directions: `delta = atan2(sin(target-current), cos(target-current))`
- Accumulate into `a_position_unwrapped`, convert to steps
- If |Δθ| > 90° in a single MicroSegment: schedule a lift-rotate sequence instead of stepping through the large angle in-cut

### 6.5 Feed Rate Lookahead Underrun

**Problem:** If the tile_queue drains before Core 1 finishes (USB stall, slow host), Core 1 must not emit random steps.

**Mitigation:**
- `micro_seg_queue` acts as elastic buffer. At 512 entries × 14 bytes it holds several seconds of steps at typical feeds
- If `micro_seg_queue` falls below LOW_WATER (64 entries) and no more tiles are being planned, Core 1 decelerates to v=0 gracefully (ramp-down flag set by Core 0)
- Core 0 sends `STALL` status packet to host

### 6.6 Acceleration Headroom on Very Short Merged Groups

**Problem:** Even a merged group of say 8mm total can't reach cruise speed at high a_max with tight entry/exit constraints.

**Mitigation — Triangular profile detection:**
```
d_required = (v_cruise² - v_entry²) / (2×a_max)  +  (v_cruise² - v_exit²) / (2×a_max)
if d_required > group_length:
    reduce v_cruise to achievable peak:
    v_peak = sqrt(v_entry²/2 + v_exit²/2 + a_max × group_length)
    profile becomes triangular (no flat top)
```
This is standard trapezoidal planner degeneration and should be a baseline requirement of the velocity resolver, not a special case.

---

## 7. Protocol Changes Summary

**RS485 protocol: no changes.**  
All four axes (X, Y, Z, A) are driven by the existing stream byte (2 bits per motor × 4 motors). No new RS485 commands are added. The ATtiny firmware is unchanged.

**USB protocol: new binary tile format added alongside existing text commands.**

| Direction | Format | Notes |
|---|---|---|
| Host → Pico | ToolConfig (21 bytes binary, on change) | new |
| Host → Pico | SplineTile (37 bytes binary, per tile) | new |
| Pico → Host | ACK (4 bytes binary) | new |
| Pico → Host | NACK (5 bytes binary) | new |
| Pico → Host | STATUS (20 bytes binary, periodic telemetry) | new |
| Host → Pico | text commands (stop, ping, etc.) | unchanged |
| Pico → Host | text responses (pong, pos=, etc.) | unchanged |

---

## 8. Node Assignment Convention (recommended)

| Node | Axis | Role |
|---|---|---|
| 1 | X | Carriage / gantry horizontal |
| 2 | Y | Bed / gantry vertical |
| 3 | Z | Tool lift (knife/wheel up-down, lead screw or rack) |
| 4 | A | Tangential rotation (knife/wheel angle tracking) |

All four nodes run identical ATtiny firmware. Node role is purely a matter of which stepper is physically wired to it. The Pico planner knows the axis assignment via a config struct (steps_per_mm for X/Y/Z, steps_per_degree for A).

If the machine has no Z or A axis, the planner skips those fields (dz=0, da=0 always). No firmware change needed — unused nodes simply never receive steps.

---

## 9. Build Order (suggested sequence when implementation begins)

1. `include/common.h` — add SplineTile struct, MachineConfig struct (steps_per_mm per axis)
2. Pico `shared.h` — add MicroSegment (dx/dy/dz/da), TilePlan, tile_queue and micro_seg_queue ring buffers; retire old Segment ring buffer
3. Pico `core0.cpp` — USB binary packet ingest (Stage A) + planner (Stage B, sections 3.4 steps 1-5)
4. Pico `core1.cpp` — switch to micro_seg_queue consumption; pack all four axes into stream byte per cycle
5. Python CLI — SVG parser, tile serialiser, basic serial streaming (no UI); dry-run mode outputs tile stream to file
6. Browser web app — UI wrapper around JS port of the same processing pipeline

ATtiny firmware: **no changes required at any step.**

Each step is independently testable:
- Step 2: compile-only check, verify struct sizes and buffer arithmetic
- Step 3: unit test planner with synthetic SplineTile inputs, inspect MicroSegment output offline
- Step 4: run with synthetic straight-line MicroSegments, verify XY motion matches current behaviour; then test Z-up/down and A-rotation sequences
- Step 5: dry-run a known SVG, compare tile stream against plot_moves.py visualisation
- Step 6: end-to-end job on machine with simple rectangle before attempting curves

**Revised ordering (after premortem):** do **Phase 0 first** — build the validation web app (Section 11) against the *current* firmware with a minimal STATUS packet and a dry-run flag, before rewriting the motion path. Keep the existing `move`/Segment path alive behind a compile flag until the tile path passes the underrun and lookahead-sufficiency tests (Section 11.3). Do not execute build-order step 2's "retire old Segment ring buffer" until then.

---

## 10. Premortem — Assume It Failed, Find Out Why

Imagine it is three months out and the machine cuts garbage, or the rewrite stalled. The autopsy below ranks failure modes by *severity × likelihood*. Each one folds a mitigation back into the plan. The validation app (Section 11) is designed to surface the **🟥 Critical** ones empirically before they reach material.

### 🟥 Critical — will break the machine if unaddressed

**P1. Lookahead window too shallow for the feed rate → can't stop in time for a corner.**
- *Cause:* Window = 16 tiles was asserted, not derived. Required lookahead is a function of feed and accel, not a constant.
- *The math:* stopping distance `d_stop = v_max² / (2·a_max)`. Required tiles `N = d_stop / min_planned_length`. At v_max = 375 mm/s (30k steps/s ÷ 80 steps/mm) and a_max = 1000 mm/s², `d_stop = 70 mm`. With merge-group floor ≈ 5 mm, **N ≈ 14 tiles just to stop from full speed** — and that leaves zero margin for a chain of decelerating corners. With a_max = 500 mm/s² it's **28 tiles**. Window=16 is under-sized for the lower-accel case.
- *Mitigation:* Size the window from `WINDOW ≥ ceil(v_max²/(2·a_max·min_planned_length)) + margin`. Make it a derived constant in `shared.h`, asserted at compile time against the configured v_max/a_max. Validate empirically with the underrun probe (11.3).

**P2. Commit-too-early: a tile's MicroSegments are emitted before a later tile can still lower its exit velocity.**
- *Cause:* Stage B as written ("evaluate each tile, push MicroSegments") commits on sight. Classic lookahead-planner trap.
- *Mitigation:* Split planning from committing. Maintain a *plan pointer* and a *commit pointer*. Only commit tile `i` (emit its MicroSegments) once either (a) the window holds ≥ N tiles past `i`, or (b) a guaranteed stop (`PATH_END`) is within the window. The backward pass must be re-run over the uncommitted span every time a tile is added. Until committed, a tile's velocities are provisional.

**P3. Acceleration quantization — constant velocity within a MicroSegment makes ramps steppy.**
- *Cause:* Step 5 sizes `dt` from geometry only (`sqrt(8·tol/|B''|)`). On a long, straight, *accelerating* segment, curvature ≈ 0 → `dt = dt_max = 0.1` → only ~10 MicroSegments for the whole move → velocity changes in ~10 audible plateaus. A 100 mm line becomes 10 constant-velocity chunks of 800 steps each.
- *Mitigation:* Add a **velocity-based subdivision limit** alongside the geometric one: `dt ≤ Δv_max / (a · |ds/dt|)` so velocity never changes by more than `Δv_max` (e.g. 2–5 mm/s) within one MicroSegment. Cruise stays coarse; accel/decel zones subdivide finely. This is the single most important correctness fix to Step 5.

**P4. `interval` overflow / divide-by-zero at v → 0.**
- *Cause:* `interval = F_CPU / v_steps`. At the v = 0 corner points the plan literally calls for, interval = ∞ and overflows uint32 below ~0.035 steps/s.
- *Mitigation:* Clamp velocity to `v_min` (e.g. 0.5 mm/s) everywhere; "stop" means v_min, not 0. Cap `interval` at a sane max (e.g. 0.1 s worth of cycles). Dwell at corners is achieved by Z-lift, not by infinite interval.

### 🟧 High — will produce wrong cuts, not crashes

**P5. SVG coordinate frame mishandled (the classic 90% of SVG bugs).**
- *Cause:* "Parse DOM, extract `<path>`" ignores: group `transform` matrices, `viewBox`/unit scaling (Inkscape documents are px with a viewBox), Y-axis sign (SVG +Y is *down*, machine +Y usually *up*), and non-path primitives (`<rect> <circle> <ellipse> <line> <polyline> <polygon>`).
- *Mitigation:* Flatten all ancestor transforms into absolute coordinates; convert to mm via document units + viewBox; flip Y to machine frame; convert all shape primitives to paths *before* Bezier decomposition. Use a vetted library (`svgpathtools`/`svgelements` in Python; `paper.js` or the browser `SVGGeometryElement.getPathData` + `DOMMatrix` in JS) rather than hand-rolling. This belongs in the shared core and must be unit-tested against a real Inkscape export.

**P6. Tangential knife blade offset ignored.**
- *Cause:* The plan assumes the A-axis points the blade along the tangent and that's sufficient — true only for a *zero-offset, centre-pivot* tangential knife. Drag/swivel knives have a caster offset δ (tip trails the pivot), so the cut point is not the commanded XY.
- *Mitigation:* State the assumption explicitly (centre-pivot tangential knife, δ = 0). If δ ≠ 0, add offset compensation: command `XY_pivot = XY_cut − δ · t̂` where `t̂` is the unit tangent. Make δ a `MachineConfig` field; δ = 0 disables compensation. Flag as a named scope decision, not a silent omission.

**P7. A-axis not pre-oriented at path start → knife drags sideways on the first millimetre.**
- *Cause:* `PATH_START` sets v_entry = 0 but nothing rotates A to the start tangent before XY moves.
- *Mitigation:* At every `PATH_START`, emit (with Z up): A-rotate-to-start-tangent → Z-down → begin XY. Same primitive as the corner lift sequence, run once at path entry.

**P8. No homing / undefined origin.**
- *Cause:* Tiles carry absolute mm coordinates but there is no homing routine, limit-switch handling, or origin-set command. Position is undefined at power-on.
- *Mitigation:* Add a `home` / `setorigin` text command (Core 0) and a `MachineConfig` work-area for soft limits. Reject (NACK) any tile whose evaluated extent leaves the work envelope. At minimum, require an explicit "set current position as origin" before a job can start.

### 🟨 Medium — will bite during bring-up

**P9. `int16` MicroSegment deltas overflow on long jogs.** A jog tile at dt_max = 0.1 can exceed 32767 steps (≈409 mm at 80 steps/mm). *Mitigation:* widen dx/dy to int32, or hard-cap MicroSegment length and split. Widening costs 4 bytes/segment (still <12 KB total) — do it.

**P10. USB framing desync between binary packets and legacy text commands.** Mixing 0xAB/0xAC/0xCC binary frames with newline-terminated text on one CDC stream invites a stray byte mid-binary-packet to be parsed as text (or vice-versa). *Mitigation:* require binary packets to validate magic+length+CRC atomically; on any mismatch, resync by scanning for a magic byte. Keep text commands confined to an idle/non-streaming state, or move them onto a distinct length-prefixed binary command type.

**P11. Replacing the known-good Segment path before the new path is trusted.** *Mitigation:* already folded into the revised build order — keep both behind a flag; the `move` command stays the regression baseline.

**P12. Estop state recovery undefined.** An estop mid-cut leaves Z and A at unknown positions. *Mitigation:* on estop, command Z to a known safe-up position before halting the stream; require re-home before resuming.

### 🟦 Low — watch, don't block

- **P13. atan2/sqrt cost** — actually fine: even at 375 tiles/s × ~30 samples, `atan2f`+`sqrtf` (FPU) cost <2% of one core. *Do not* use `double` (software-emulated on M33); enforce `float`/`f`-suffixed math. (Listed to pre-empt premature optimization.)
- **P14. Float determinism host vs Pico** — validation compares should use tolerances, not equality.
- **P15. seq_num uint16 wrap** mid-job — fine as long as the in-flight window (16) « 65536.

---

## 11. Performance Validation Web App (WebSerial)

A browser harness whose purpose is to **measure the assumptions in Section 10 against real hardware** — not just to be the production UI. It loads an Inkscape SVG, renders it, runs the shared planning pipeline, streams over WebSerial, and plots live telemetry from the STATUS packet (Section 2.5) so the 🟥 Critical risks are caught empirically.

### 11.1 Why WebSerial

- Zero install — runs in Chrome/Edge against the Pico's USB CDC.
- Same JS planning core as the production web app (Section 5.3), so what it validates is what ships.
- Direct access to send timestamps + STATUS timestamps → real RTT and queue-depth traces.

### 11.2 Modes

| Mode | Pico behaviour | What it isolates |
|---|---|---|
| **Dry-run** (`state=DRYRUN`) | Ingest + plan + emit STATUS, but **do not drive RS485** | Protocol throughput, planner keep-up, ACK latency — with no motor risk |
| **Loopback** | Pico echoes ACK only, no planning | Raw USB/protocol ceiling (baseline) |
| **Live** | Full motion | End-to-end timing vs prediction |

Dry-run is the workhorse: it lets you push real Inkscape jobs at real feeds and watch `micro_seg_depth` and `underrun_count` without touching steppers. A new `dryrun on/off` text command (Core 0) sets the flag; in dry-run Core 1 consumes MicroSegments on schedule but suppresses the RS485 write.

### 11.3 The Three Validation Tests (map directly to premortem)

**T1 — Throughput / ACK-latency benchmark** *(validates the USB assumptions, P10)*
- Blast tiles with the windowed protocol (window=16); measure sustained tiles/s and ACK RTT distribution (p50/p99).
- Pass: sustained ingest rate ≥ required rate = `peak_step_rate / avg_steps_per_tile`, with p99 RTT < one STATUS interval.

**T2 — Underrun probe** *(validates lookahead depth P1, commit-safety P2, accel quantization P3, planner keep-up)*
- Run a representative Inkscape job in **dry-run**, ramping `feed_max` upward across runs.
- Watch `underrun_count` and `micro_seg_depth`. The highest feed with zero underruns and depth never hitting LOW_WATER is the **certified max feed** for that artwork class.
- If underruns appear far below the spec'd 375 mm/s, the planner can't keep up or the window is too shallow → revisit P1/P2/P3 *before* cutting anything.

**T3 — Predicted-vs-actual profile** *(validates the velocity planner end-to-end)*
- Host independently computes expected job time from its own copy of the velocity profile.
- Compare against: (a) wall-clock from first tile to final STATUS `IDLE`, (b) Pico's `steps_emitted` vs host's expected step total.
- Pass: time within a few %, step counts exact. Divergence localizes planner bugs.

### 11.4 UI — "show the file along with sending"

Single-page, three panes:

**Left — SVG view (the file):**
- Drag-drop / open an Inkscape `.svg`; render the original as native `<svg>` for fidelity.
- Overlay the *planned* toolpath on a `<canvas>` aligned to the SVG: tool colors per ToolConfig (cut/crease/jog), jogs dashed, **auto-lift points marked**, short-merge groups tinted. This is the visual proof the parser handled transforms/units/Y-flip (P5) correctly — if the overlay doesn't sit on the artwork, the frame handling is wrong.
- A live **progress cursor** advances along the path as STATUS `steps_emitted` / ACKed tiles report back.

**Right — live telemetry dashboard (the sending):**
- Connect button (WebSerial port pick) + mode selector (Dry-run / Loopback / Live).
- Counters: tiles sent / ACKed / NACKed, underrun_count, state.
- Sparklines vs time: `micro_seg_depth` (the critical one — watch it never floor), `tile_q_depth`, `current_feed`.
- ACK RTT histogram (T1).
- Predicted vs actual job-time readout (T3), updated on completion.
- Big **STOP** button → sends `stop\n`.

**Bottom — log:** raw NACK reasons, repaired-C1-join warnings from the parser (P5/6.3), and any tile rejected for leaving the work envelope (P8).

### 11.5 Build cost

It reuses the Section 5.1 shared JS pipeline and the WebSerial code the production app needs anyway. The only firmware prerequisites are the **STATUS packet** (2.5) and the **dryrun flag** — both small, both useful in production too. This is why Phase 0 (Section 9, revised) builds this harness first: it de-risks the entire rewrite for a few hundred lines of glue.
