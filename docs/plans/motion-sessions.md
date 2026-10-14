# Motion sessions on the Pico

How the Pico runs jobs and jogs: their states, how a job starts, pauses,
resumes and ends, and how jogs and offsets work. Follows
docs/plans/state-handling.md (sessions, alarm exits) and
docs/plans/pico-planner.md (the ring, `BEZIER` records). Replaces
state-handling branch 5 ("motion gating, no jobs without homing") and planner
branch 3 (`feature/pico-jog`).

Jogging is settled and has branches (J1, J2). Jobs are still a draft:
preflight and the record layouts are open (below), and they have no branches yet.

## Glossary

```
job                      one file; header + records
└─ layer                 one TOOL_REC and what follows (tool, plunge scale k)
   └─ contour            the tool down: START … END; travel between contours
      └─ stretch         tangent continuous: between BREAKs
         └─ piece        one cubic the Pico queues without analysis

            host            wire                   Pico ring
            piece    ──►    record (BEZIER)  ──►   block
                            seq, index             + inserted blocks: lift,
                                                     travel, plunge, swivel
                                                     (index of the record
                                                     they lead into); jogs
```

* **Contour** is the host code's subpath. Touching subpaths that are one cut
  are joined by the host into one contour; the Pico never skips a travel.
* **Piece, record, block** are one unit of geometry where it is seen: a
  piece on the host (`AnnotatedBezier`), a record on the wire (`BEZIER`,
  with seq and index), a block in the Pico's ring. The ring also holds
  blocks no record sent: inserted lifts, travels, plunges, swivels, and
  jogs.
* **Flags** (`BEZIER` byte 1; `web/src/toolpath/annotate.ts:277`): `START`
  on a contour's first piece, `END` on its last, `BREAK` on the first piece
  after a corner (a join over `angleTol`) or a cusp. Other splits
  (inflection, curvature ratio, fit) are 0. A `BREAK` is inside a contour,
  never between two; a closed contour's closing corner has none (`END`
  lifts there).

```
layer (TOOL_REC)
├─ circle      START ─────────────────────────── END   1 stretch
├─ circle      START ─────────────────────────── END   1 stretch
└─ rectangle   START ── BREAK ── BREAK ── BREAK ── END   4 stretches
```

## Decisions

### States

* **`STATE_JOB` with a reason byte**, like `STATE_HOMING` and `STATE_PROBING`
  (`ProbingReason`). Every session has the same shape: IDLE between sessions,
  the session owns the machine, ALARM ends it.
* **Job reasons:** `PREFLIGHT`, `TOOL_CHANGE`, `BUFFERING`, `RUNNING`,
  `HOLDING`, `PAUSED`,
  `DWELL`, `ABORTING`.
* **Writers.** Core 0 writes the entries it is asked for; Core 1 writes every
  change made at rest or decided from the ring:

  | from | to | writer | trigger |
  |---|---|---|---|
  | IDLE | `PREFLIGHT` | Core 0 | `JOB_HDR` accepted |
  | `PREFLIGHT` | `BUFFERING` | Core 0 | first `TOOL_REC`, all tool headers ACKed, tools mounted |
  | `PREFLIGHT`, `RUNNING` | `TOOL_CHANGE` → `BUFFERING` | Core 0 / Core 1 | the next tools are not mounted; `loaded` |
  | `PREFLIGHT` | IDLE | Core 0 | any preflight NACK |
  | `BUFFERING` | `RUNNING` | Core 1 | buffered time, last record, or timeout |
  | `RUNNING` | `BUFFERING` | Core 1 | dry ring or late adoption, at rest |
  | `RUNNING` | `HOLDING` | Core 0 | `pause` |
  | `HOLDING` | `PAUSED` | Core 1 | at rest, after the lift |
  | `RUNNING` | `DWELL` → `RUNNING` | Core 1 | duty lift, then the delay |
  | `PAUSED` | `BUFFERING` | Core 0 | `TOOL_REC(N)`, N the resume point |
  | `PAUSED` | JOGGING → `PAUSED` | Core 0 / Core 1 | jog, with the return slot |
  | any | `ABORTING` → IDLE | Core 0 / Core 1 | `cancel`/`stop`; IDLE at rest |
  | `RUNNING` | IDLE | Core 1 | last record executed, after the end lift |

* **Records are admitted only in `JOB.BUFFERING` and `JOB.RUNNING`,** never
  from IDLE: a record needs a header first. `RUNNING_PLANNER` goes.
* **IDLE stays a state.** The job ends to IDLE on its last record executed,
  on cancel, and on abort; to ALARM on estop, soft limit or node fault.
* **`STATE_JOGGING`** is all jog motion (and the bench `line`/`bez`). Jogs are
  atomic: a jog runs, or is stopped (hold and discard); it never pauses.
* **A jog returns to the state it came from**, which is IDLE or `JOB.PAUSED`;
  jogging is refused anywhere else, so the memory is one slot, not a stack.
  The job session (job id, resume point) stays in memory under
  `STATE_JOGGING`; everything a job forbids, running motion forbids too.
  `status` shows the paused job beside `JOGGING`. Estop or a soft limit during
  the jog goes to ALARM with the resume point kept.
* **`STATE_RUNNING` and `STATE_PAUSED` are retired** with MicroSegment jobs,
  which keep them until then and are not ported.

### Job records

* **Three records before the geometry:** a job header, one tool header per
  tool, then tool records, each starting a run of `BEZIER`s:

  ```
  JOB_HDR                job id, record count, t, tool count n
  n × TOOL_HDR           any order; one per tool that will run
  TOOL_REC(N)            tool, index N, feed, k
  BEZIER …               from N
  TOOL_REC(M) …          the next layer, at any index
  ```

* **Every start sends the headers;** the host picks where to start. A job
  start is `TOOL_REC(0)`; a restart after an alarm or power loss is a fresh
  `JOB_HDR` with the same record count, tool headers for only the tools
  still to run, and `TOOL_REC`s from where it left off. A resume from
  `PAUSED` is `TOOL_REC(N)` alone, guarded by the resume point and job id.
* **No `config_crc32` and no `required_axes`.** The host no longer models the
  machine; the Pico infers axes and peripherals from each tool's profile.
* **Only `BEZIER` records are indexed,** and the index is not on the wire:
  the seq window keeps order with no gaps, so a record's index is its
  `TOOL_REC`'s index plus the count since. The job's last record is index =
  record count − 1. A `TOOL_REC`'s index is that of the next `BEZIER`.
* **Tools have an explicit `id`** (u8) in each config `tools` entry, unique,
  checked at load; it survives reordering the config. The host looks a
  layer's name up in the config it pulled and packs the `id`. `toolType`
  stays a separate field.
* **Chambers.** A revolver pen is one tool with chambers (the profile's
  `slotOffsets`, to be renamed `chamberOffsets`; "slot" is the stream
  slot). The host maps sub-layers to chamber numbers; `TOOL_REC` carries the
  number, 0 for a tool without chambers. One tool header covers all
  chambers.
* The first record after a `TOOL_REC` is treated as `START`: the Pico lifts,
  travels to it and plunges (Lifts, below). `TOOL_REC` selects the tool (as
  `select`, below).
* **Feed** in `TOOL_REC` is absolute (mm/s; 0 the profile's `path.feed`). A
  live feed override (a scale, re-planning the ring) is a later branch,
  replacing docs/feed_override.md.
* The Pico keeps nothing about tools that must survive a power loss; the host
  holds the job.

### Record layouts

Little-endian, seq at byte 2 in all three, CRC8 over the bytes before it.

```
JOB_HDR 0xA0, 17 B
[0] magic  [1] version (1)  [2] seq
[3..6] job id u32  [7..10] record count u32  [11..14] t f32 mm  [15] tool count 1..4
[16] CRC8

TOOL_HDR 0xA1, 20 B
[0] magic  [1] tool id  [2] seq
[3..18] xmin, xmax, ymin, ymax f32, work coordinates at the tip
[19] CRC8

TOOL_REC 0xA2, 17 B
[0] magic  [1] chamber (0 = none)  [2] seq
[3] tool id  [4..7] index u32  [8..11] feed f32  [12..15] k f32
[16] CRC8
```

The reserved `TOOL_MAGIC` (`0xAC`) goes with MicroSegment.

### Preflight

* **Each frame gets its own verdict,** ACK or NACK; the NACK's third byte
  (unused today) carries the detail.

  | frame | checks | NACK |
  |---|---|---|
  | `JOB_HDR` | state, homed, version, tool count 1–4 | `BAD_STATE`, `NOT_HOMED` |
  | `TOOL_HDR` | id in the config, no duplicate; head and peripheral present and bound; bbox with offsets, mesh and heights in range | `TOOL_UNKNOWN`, `TOOL_UNAVAILABLE`, `OUT_OF_RANGE` (detail: axis and side) |
  | `TOOL_REC` | its tool has a header; index valid (the resume point in `PAUSED`); chamber valid | `JOB` |

* **Any preflight NACK ends the session to IDLE;** the host fixes the job and
  sends the header again.
* **`JOB.PREFLIGHT`** lasts from `JOB_HDR` until the first `TOOL_REC` after all
  n tool headers are ACKed; a `TOOL_REC` or `BEZIER` before then is `NACK_JOB`.
* Codes for the new reasons are picked in the branch, after `0x0A`.

### Buffering (jobs only)

* `BUFFERING` is entered at job start, at resume, and when the ring runs dry
  mid-job (the planner has already braked to rest at the last block). A late
  block adoption (Core 1 needed a block before Core 0 committed it) is a dry
  ring too: hold to rest, `BUFFERING`, then on automatically. `PAUSED` is
  only ever the operator's pause. Jogs have no buffering.
* Core 1 leaves for `RUNNING` on the first of:
  * the ring holds `bufferTime` of motion (~1 s): Core 0 adds each block's
    length/feed to a shared counter on push, Core 1 subtracts it on
    release (a lower bound; accel ignored);
  * the job's last record is in the ring (index = header count − 1);
  * a timeout, counted in ticks since entering `BUFFERING`.
* `bufferTime` and the timeout are config fields.

### Pause drains the ring

* `pause`: `HOLDING` (brake along the path, existing hold), then at rest a
  lift, then `PAUSED`: the ring is dropped and the resume point `{index, s}`
  recorded (the block that was running and how far into it).
* With the ring empty, a jog can use it while paused. No second ring, no
  set-aside.
* **Contour framing resets on pause**, as on abort.
* **Resume is a `TOOL_REC`**, not a text command: in `PAUSED` only a
  `TOOL_REC` whose index matches the resume point is accepted, and every other
  record is NACKed, so a stray record cannot restart motion. Then
  `BUFFERING`.
* The resumed block is re-cut from its start. Resuming at `s − margin` (trim
  by `s0`, plunge mid-block) comes with Z, A and tool profiles; the resume
  point already keeps `s`.
* `select` and `apply_offset` are allowed while paused (e.g. to jog another
  head); resume re-selects through its `TOOL_REC`.
* Planner branch 2's in-place resume (`restartFrom`, `err moved`) is unused on
  this path; it stays in `lib/planner` for now.

### Lifts and heights

* **The Pico inserts every lift;** the wire stays 2D. Lifts and plunges are
  Z-only lines (a change of axis set stops), tagged with the index of the
  record they lead into, so `TOOL_REC(N)` replays its own travel.
* **Heights,** with work Z the bed top (the mesh applies by its rule):

  ```
  lift / travel   workZ + t + c
  plunge          workZ + t − k·t      k in [0, 2]; 1 a full cut, past 1 into the mat
  ```

  `t` from the header, `k` (plunge scale) from the `TOOL_REC`, `c` (clearance) from the
  tool profile. Preflight checks both against Z's soft range over the bbox,
  with the mesh.
* **What lifts:**

  | cause | stop | where |
  |---|---|---|
  | `START` | lift, travel, A to the start heading, plunge | every contour start |
  | corner turn > `cornerAngleDeg` | lift, swivel, plunge | that `BREAK` |
  | cable range | lift, unwind by whole periods, plunge | anywhere |
  | duty, past `maxOnS` | lift, tool off, `DWELL` for `dwellS`, tool on, plunge | anywhere |
  | hold or abort at rest | lift | where it stopped |
  | job end | lift | after the last record |

  Estop does not lift (Alarms in a job).
* **A `BREAK` is the host's tangent discontinuity** (a join turning more than
  `angleTol`, or a cusp; `web/src/toolpath/clean.ts:67`,
  `annotate.ts:281`). The Pico computes the turn from the two records'
  tangents; at or below `cornerAngleDeg` A swivels in the material.
* **A lift can happen anywhere on the path:** hold to rest, lift, plunge back
  at the same XY and the same physical heading (an unwind is whole periods),
  and continue the rest of the block from `s`. Today a hold resumes in place
  (branch 2's `restartFrom`); putting the lift and plunge between the held
  point and the rest of the block is new planner work. A `BREAK` is only a cheaper
  place: a cable or duty lift due shortly is moved to an earlier `BREAK`
  that lifts anyway. No stretch is refused for length.
* **Triggers are live counters on Core 1:** tool on-time, and A against the
  cable range; each requests a hold early enough to stop in time.
* **Tool on and off are event blocks** in the ring, executed by Core 1 at
  rest (where it owns the bus): motion stops before the plunge (a change of
  axis set), tool on, wait `settleS`, plunge; the lift runs with the tool on
  so the blade does not stick, and the tool goes off at clearance.
* **Duty** (the existing `dutyLimits`, docs/tool_duty_limits.md): the tool is
  on only while cutting, from the plunge's event to the lift's. On-time adds up across contours until a
  reset; an off of at least `dwellS` is a reset. At a contour's `START`:
  * the off time since the last lift is already ≥ `dwellS`: reset;
  * else, if on-time ≥ `minOnS`: wait until the off time reaches `dwellS`
    (`DWELL`), reset;
  * else: plunge without a reset.

  Mid-contour, past `maxOnS`: the duty lift, `DWELL` for `dwellS`.
* **Tool profiles are the existing `ToolProfile`** (`web/src/machine/schema.ts:508`,
  `tools` in the config), decoded by the Pico for the first time:
  `cornerAngleDeg`, `tangential`, `unwind`, `path.feed` (cut), `z.feed`
  (plunge), `offsetMm` (blade trail), `toolOffset`, `plunge` (false for a pen:
  `k` is 0), `dutyLimits`. New: `clearance`, the cable range, the node that
  switches the tool.

### Record index and identity

* **Ring blocks carry an absolute record index**, counted from the job start
  (u32). The wire seq is one byte and wraps every 256 records; it stays for
  the ACK window only. `status` reports the executing index.
* **No CRC is kept per block.** The CRC guards transit; a resend is checked
  again on arrival.
* **A job id** identifies the job; a resume or restart must match it, so the
  wrong file cannot be resumed.

### Vacuum

* **On for the whole session:** buffering, hold, `PAUSED`, `DWELL`, a jog
  while paused, and alarms other than estop; releasing it can shift the
  material. Off at job end, on `cancel`, or by an explicit command.
* **Zones** are the vacuum node's six valves. First iteration: the Pico
  opens all six; nothing about zones is on the wire.

### Out of range

* **Preflight** checks the bbox against the soft range, with what the Pico
  adds after the wire (offsets, mesh, lift heights, travels); a failure is
  `NACK_PREFLIGHT`, since the host waits on the header's reply.
* **In a job** the host is not waiting on a reply, so a record out of range
  at ingest (control points, a Bézier lying in their hull), or a Pico travel
  out of range, never enters the ring: the job holds before it, lifts, and
  goes to `ALARM_SOFT_LIMIT` with that record's index as the resume point.

### Alarms in a job

* **Estop ends the job:** tool and vacuum off, no lift, no resume point. The
  host's saved index lets the operator start a new run from there by hand.
  `pause` is the stop that resumes.
* **Every other alarm** lifts when Z can still move, turns the tool off and
  keeps the vacuum on; the job restarts from its index.

### Lost host

* A dry ring with no last record (the host stopped sending): at rest, lift,
  tool off, vacuum on, wait in `BUFFERING`. After `hostTimeoutMin` (config,
  minutes; 0 waits indefinitely) the job ends as `cancel`.

### Tool change

* **Two tables.** The machine's mount state (which tool is on each head) is
  kept by the Pico across jobs, in RAM, set by the operator's `loaded`
  command. The job's plan comes from the `TOOL_HDR` order: header p goes on
  head p mod `headCount`, so tools pair up (0, 1), (2, 3), ….
* **Preflight** checks each tool's head accepts it, then compares the mount
  state with the first pair; a mismatch goes to `TOOL_CHANGE` before
  `BUFFERING`.
* **Mid-job,** a `TOOL_REC` naming a tool not mounted means the next pair is
  due: hold, lift the selected head on the planner, park both heads' Z by
  node park legs (in parallel), then for each head of the pair: an XY line
  to its probe switch, wait for `loaded`. Then `BUFFERING`.
* **Park legs run outside homing:** as steps of the tool-change sequence,
  ticked by the runner and dropped when the session loses the machine
  (docs/plans/sequences.md). Records are held off while a
  leg is armed. A failed park leg alarms as `ALARM_HOMING_FAIL`
  (`LEGFAIL_PARK`) with the job kept. `pause` is refused in `TOOL_CHANGE`.
* The host orders layers by tool; returning to an earlier tool is another
  change, allowed but slow.

### Reporting

* **`STATUS_RSP` grows** (new magic, as `0xA6` → `0xA7`): `bufCount` goes
  with MicroSegment; job id (u32) and index (u32) are added; the reason byte
  is the current state's reason (job, jogging, homing, probing), replacing
  `runningReason`. Position is WPos (f32 mm; Core 0's `frames`); MPos stays
  on `get`. Two bytes say what the WPos is of: the frame (head 0, 1, or
  `0xFF` the anchor) and the tool mounted there (its `id`, `0xFF` none).

### Restart after an alarm

* Once the alarm is cleared and the machine re-homed, the job can restart
  from the recorded index: a fresh `JOB_HDR`, tool headers for the tools still
  to run, `TOOL_REC(N)`, records from N (Job records).

### Power loss

* **No flash writes during motion.** An erase or program stalls flash fetches
  on both cores (tens of ms for a sector erase); hold, abort, USB and Core 0
  run from flash. `CFG_SET` parks Core 1 for the same reason.
* **The host is the non-volatile record:** it keeps the job and its id, and
  saves `{job id, executed index}` from each status poll.
* Recovery: power up, home, restart from the saved index minus a margin (the
  poll gap); the operator confirms, since the material may have moved.
* Later, maybe: the Pico writes the resume point at rest, on entering
  `PAUSED`.

### Offsets and frames

* **The frames, `select`, the work offset, stored positions and the soft
  range are in docs/plans/coordinate-system.md** (branch `feature/pico-frames`,
  which lands before the jog branches). In short: move targets are work
  coordinates; the Pico adds the work offset and the selected head's offset,
  and checks the result against the signed soft range `[park, park ±
  maxTravel]`.
* Relative motion is frame-free; the frames matter for absolute targets, the
  position readout and preflight.
* The `TOOL_REC`'s selection goes through `select`.
* **A job leaves its last selection in place** when it ends.

### Jog command shape

Split by rate: text for operator commands and low-rate use, the data plane for
what is sent continuously.

| | plane |
|---|---|
| step jog `jog <axis> <dist> [feed]` | text |
| absolute `jogto <x> <y>`, work coordinates | text |
| stored position `jogto park\|load\|probe` | text |
| continuous jog | data plane, deadman packet |
| jog stop | data plane, one byte |

* **Continuous jog:** a small packet (magic, direction per axis, speed
  fraction, CRC) the host repeats every ~50 ms while held. The first queues a
  line toward the soft limit in that direction; repeats renew a timer;
  nothing for ~3 intervals holds and discards. All-zero directions or the
  stop byte stop at once. A direction change is a stop, then a new line.
* XY jogs replace the 26 B `JOG_MAGIC` packet.

### Jog behaviour

* **Z and A stay on `JOG_MAGIC`** until the planner gains them; only XY jogs
  move to the planner. An XY jog mixed with Z or A is refused.
* **Named `jogto`** (`park`, `load`, `probe`): the position comes from
  coordinate-system.md's stored positions; `probe` is the selected head's
  switch. Every head's Z goes to its park position first (a park leg until
  the planner moves Z), then the XY line. A numeric `jogto` moves XY only.
* **Homing gate:** `jogto` needs the axis homed. Relative jogs (step and
  continuous) need it unless the config field `jogUnhomed` is set (testing;
  off in production). Un-homed, soft limits are not enforced, the speed is
  `jogFeedUnhomed`, and a continuous jog runs at most `maxTravel` from where
  it started (also on a homed axis with `softLimits` off).
* **`maxTravel` is required on every linear axis** (positive, homing or not);
  rotary A keeps 0 until the A range pass.
* Every jog needs its axes bound and enabled; none run in ALARM (leaving
  `LIMIT_LATCHED` is the homing session's reverse leg).
* **Soft limit:** a step jog or `jogto` that would cross it is refused,
  `err soft_limit <axis> <mm left>`, never clamped. A continuous jog runs to
  the limit and stops there.
* **Clicks append:** a step jog while one runs is queued behind it and the
  planner joins them, up to 4 queued; past that `err busy`. A step jog during
  a continuous jog is refused.
* **Speed:** per-axis `jogFeed` and `jogFeedUnhomed` in the config; the step
  jog's `[feed]` and the continuous packet's speed fraction (u8, of
  `jogFeed`) scale it, clamped to `maxFeed`. Acceleration is the axis
  `maxAccel`.
* **Deadman:** the host repeats every 50 ms; the Pico holds after 150 ms
  without a packet (the ordinary hold, then discard).
* **Diagonal** continuous jogs (X and Y together) are allowed; speed is along
  the path.

## Branches

A dependent chain, one session: J1, then J1b and J2 in either order.

The job branches are built inside out: the JOB state around today's records
first, then mount state, then the layer header, then tool change. JB1 and JB2
are independent; JB3 needs both; JB4 needs JB3.

## Branch J1: `feature/pico-jog`

### Plan

* Type: feature.
* Purpose: XY jogs on the planner. Adds `STATE_JOGGING`, step `jog`,
  numeric `jogto`, the homing gate, refusal at the soft limit, and
  appending clicks. The jog config fields come in the same branch.
* Depends on: coordinate-system `feature/pico-frames` (done): `framesToMachine`,
  `framesCheckXY`, `framesTipOffset`
  (`core0/ops/frames.h`).
* What the code has today:
  * `admit()` (`core0/planner/queue.cpp:26`) accepts IDLE or
    `STATE_RUNNING`/`RUNNING_PLANNER`. The follower's `enter()`
    (`core1/emit/follower.cpp:83`) sets `RUNNING_PLANNER` for every ring.
  * `JOG_MAGIC` (`core0/data_plane.cpp:144`) is accepted in IDLE, PAUSED or
    a continuing jog, and moves any axis.
  * `CfgAxis` (`core0/config/config_decode.h:54`) has no jog fields. The
    decoder requires `maxTravel` only on a homeable linear axis
    (`config_decode.cpp:133`). `CFG_SCHEMA_VERSION` and
    `CONFIG_BLOB_VERSION` (`web/src/machine/json/blob.ts:15`) are 3.
  * `framesCheckXY` returns `soft_limit x|y` without the distance left.
  * Jogs and records share the ring and `admit()`, so a `line` could join a
    streamed job's ring.
  * A late block adoption sets `pauseRequested` (`follower.cpp:156`) and
    lands PAUSED with the ring kept.
* Scope:
  1. Config, Pico and web schema/loader: per-axis `jogFeed` and
     `jogFeedUnhomed`, machine `jogUnhomed`, and a positive `maxTravel`
     required on every linear axis. Schema and blob versions 3 → 4, fixtures
     and `controller.msgpack` regenerated.
  2. `STATE_JOGGING` (`ipc/shared_state.h`), set by the follower's `enter()`
     from a jog flag Core 0 sets when it queues the jog. A jog returns to
     IDLE. Refused while PAUSED until the job branches add the return to
     `JOB.PAUSED`. `admit()` accepts JOGGING in place of `RUNNING_PLANNER`;
     `status`, `get` and the web state names know it. The ring's owner (jog
     or job) is set when `resetIfIdle` restarts it; a push of the other kind
     is refused, and `enter()` picks JOGGING or RUNNING from it. Core 1's
     abort (`core1/core1.cpp:113`) returns JOGGING to IDLE too.
     A late adoption during a jog holds to rest and resumes the rest of the
     ring within JOGGING (no clicks lost); `get` reports the late-adoption
     count.
  3. Step `jog <axis> <dist> [feed]` (X or Y): homing gate (`jogUnhomed`),
     feed from `jogFeed`/`jogFeedUnhomed`, scaled by `[feed]`, clamped to
     `maxFeed`. Refused at the soft limit with `err soft_limit <axis> <mm
     left>` (a `framesCheckXY` variant that also returns the distance).
     Clicks append, up to 4 queued, else `err busy`.
  4. `jogto <x> <y>` in work coordinates, homed only, through
     `framesToMachine` and the soft-limit refusal.
  5. `line` and `bez` (`core0/cmd/axis.cpp:613`, `:625`) run as JOGGING.
  6. `JOG_MAGIC` narrowed to Z and A: a packet with nonzero X or Y is
     refused. The web host's XY jog stops working, as allowed by the web
     deferral in state-handling 1d.
  7. Docs: `docs/wire_protocol.md` (`jog`, `jogto`, `err soft_limit`, the
     JOGGING state, `JOG_MAGIC`'s narrowing), and the config doc.
* Out of scope: named `jogto` (J1b), the continuous jog (J2), jogging while paused (job
  branches), Z/A on the planner, tool offsets.
* Overlap: `src/rp2350/core0/cmd/table.h` (or `control_plane.cpp`),
  `web/src/machine/schema.ts`, `web/src/wire/`.
* Checks: `pio run -e pico`; `pio test -e native` where it builds;
  `pnpm typecheck` and `pnpm test` in `web/`.
  Human checks: a step jog on X and Y, homed and unhomed (with `jogUnhomed`);
  clicks join without stopping; a fifth click answers `err busy`; a jog past
  the soft range refused with mm left; `jogto` numeric with each head
  selected; `stop` mid-jog;
  `status` shows JOGGING, then IDLE.

### Status

Done (merged). Unblocks J1b and J2.

### Outcome

* Interfaces later branches use: `plannerJog` (ring owner, set in
  `resetIfIdle`), `plannerJogFrom()` (where the next jog starts),
  `framesCheckMove()` (soft range with mm left), and `queueJog()` in
  `cmd/axis.cpp` (cap, start point, soft-limit check, queueing).
* `jog`'s `[scale]` multiplies `jogFeed`; it is not a feed in mm/s.
* Added: an unhomed click (with `jogUnhomed`) longer than `maxTravel` is
  refused with `err too_far`.
* `jogto` runs at the slower of X's and Y's `jogFeed`.
* The jog's soft-range check follows `softLimits`, as jobs do. The pico build
  encodes `config/controller-1head.jsonc`; this branch turned `softLimits` on
  for X and Y there.
* Bench (human scope): every J1 human check passed with `nodestat` deltas
  matching the Pico, except: `jogto` with another head (one-head config) and
  the `JOG_MAGIC` refusal (left untested at the user's call).
* Untestable on the bench: the race on an idle ring between a jog and a
  record, and the hold-and-resume after a late adoption (`late=0` throughout).
* Checks: `pio run -e pico` clean; `pnpm typecheck` and `pnpm test` pass.
  `pio test -e native` skips `test_config` and `test_planner` on this Windows
  machine, so the new decoder test cases have not run.
* Out of scope:
  * `stateName` in `cmd/gate.h` has no PROBING.
  * The web host doesn't refuse `maxTravel` 0; only the Pico does.
  * `nodestat` answers `err bad_state` while JOGGING.
  * The web host's XY jog gets NACKs until J2.

## Branch J1b: `feature/jogto-named`

### Plan

* Type: feature.
* Purpose: `jogto park|load|probe`.
* Depends on: J1; M3 and M3b of docs/plans/machine-model.md.
* What the code has today: stored positions are decoded
  (`core0/config/config_decode.h:78`, `:95`) but nothing resolves them.
  `homingArmPark(node, last, …)` (`core0/ops/homing.h`) arms a park leg as
  a runner thread; `controller/seq/home.cpp` parks homed nodes that way.
* Scope:
  1. A resolver in `core0/ops/frames`: a name to the anchor's machine XY
     (`park` defaulting to where homing parks, `load`, `probe` as the
     selected head's `probeSwitch` minus its offset). Missing: `err
     no_position`.
  2. `controller/seq/jogto.cpp`: the first sequence (docs/plans/sequences.md),
     owner JOGGING, reason `JOGGING_GOTO` throughout: step 1 parks every
     head's Z node (legs, in parallel), step 2 is the XY line (ring move).
     A failed park alarms as `ALARM_HOMING_FAIL` (`LEGFAIL_PARK`). `stop`
     mid-sequence aborts the live step.
     * One label: `SEQ_JOGTO` publishes JOGGING in `stateOf`
       (`core0/ops/machine.cpp`) through both steps, not TRAVERSE then
       JOGGING.
     * `lineTo()` queues as a new ring owner `RING_SEQ`, so a cjog packet
       neither stops it (`acceptCjog` stops only jog owners) nor appends
       (`plannerJogFrom` refuses another owner's ring).
     * The named form is `K_SEQUENCE`; the raw XY `jogto` keeps `K_STEP`,
       applied by the handler.
  3. `err busy` for commands that meet the live sequence (sequences.md,
     Admission), mirrored in the web host.
  4. Docs: `docs/wire_protocol.md`.
* Checks: as J1. Human checks: each name with each head selected, Z parked
  first; a missing position refused; `stop` during the park.

### Status

Not started.

## Branch J2: `feature/jog-continuous`

### Plan

* Type: feature.
* Purpose: continuous XY jogs from a data-plane deadman packet, with a
  one-byte stop.
* Depends on: J1.
* Scope:
  1. Packet (7 bytes): `0xAF`, direction per axis X, Y, Z, A (i8: −1/0/+1;
     Z and A reserved and refused unless 0), speed (u8, multiplier of
     `jogFeed` in 1/64 steps, capped at `maxFeed`), CRC8. Stop byte `0xB3`,
     acting only on a continuous jog. Both are received beside the existing
     magics (`core0/data_plane.cpp:326`). Replies use the existing frames:
     ACK when a jog starts, NACK `bb <code> 00` when refused, nothing for
     repeats.
  2. The first packet queues a line to the soft-range end in that direction,
     or `maxTravel` from the start when unhomed or with `softLimits` off.
     Diagonals are allowed, at the slower axis's `jogFeed`, cut short where
     the first axis reaches its end. Repeats renew a timer; 150 ms without
     one holds and discards (`abortRequested`), checked in `dataPlaneTick`.
     All-zero directions or the stop byte stop at once. A direction change
     stops, then queues a new line once at rest.
  3. Jogging reasons: `JoggingReason` {NONE, STEP, CONT} and
     `joggingReason` (`ipc/shared_state.h`), replacing J1's `plannerJog`.
     Core 0 writes it when it restarts an empty, idle ring (`resetIfIdle`,
     under `plannerLock`); NONE means a job owns the ring. It decides ring
     ownership, Core 1's `enter()` picks JOGGING or RUNNING from it, and the
     deadman runs only on CONT. It changes only while the ring is empty and
     idle, so it is settled before Core 1 publishes JOGGING. Meaningful while
     JOGGING; `get` reports it as `jogging=`. Apart from `runningReason`,
     which the job branches rework; STATUS_RSP is unchanged.
  4. A step jog during a continuous jog is refused. A continuous packet
     while step jogs run stops them first, the same as a direction change.
  5. Web: a `wire/` encoder for the packet, and a sim that answers it.
     The operator UI port stays deferred.
  6. Docs: `docs/wire_protocol.md`.
* Overlap: `web/src/wire/`.
* Checks: as J1. Human checks: hold and release on X, Y and a diagonal;
  pulling the cable mid-jog stops within about 150 ms plus braking; running
  into the soft limit stops there.

### Status

Done (merged).

### Outcome

* Interfaces: `joggingReason` (NONE, STEP, CONT) replaced J1's `plannerJog`;
  `plannerQueueLine` and `plannerJogFrom` take the reason. New
  `plannerStopJog()` brakes the jogs Core 1 runs, or empties a ring it has
  not taken yet (an abort raised then could be consumed as nothing to stop
  and leave the line to run). Web: `packCjog`, `Link.cjog`, `Link.cjogStop`.
* Settled while writing: a speed change while holding does not restart the
  jog; repeats stay silent at the end of travel until released; the sim
  models no braking and takes `cjogStepsPerS`, `cjogTravel` and `jogUnhomed`
  in place of a config.
* Added at the user's request: WASD jog in `web/demo/barebones.html`, gated
  by a checkbox.
* Config: Y was mirrored. `controller-1head.jsonc` now has `invertDir: true`
  and `seekPositive: false` on Y, confirmed by jogging Y+; this answers the
  Y question in coordinate-system.md.
* Bench (human scope): hold and release on X, Y and a diagonal; a direction
  change (ACK about 215 ms later, once at rest); a stop at the soft limit,
  silent while held; the stop byte; packets 200 ms apart trip the deadman
  each time; a step jog during a continuous one refused; a continuous packet
  stopping step jogs. `nodestat` deltas matched the Pico throughout. Not
  tested: pulling the cable mid-jog; the demo on the machine was tried by the
  user.
* Checks: `pio run -e pico` clean; `pnpm typecheck` and `pnpm test` pass.
  `pio test -e native` skips `test_config` and `test_planner` on this
  Windows machine.
* Out of scope:
  * A direction change brakes to rest before the new line; blending would
    need the executor to cut a block short mid-flight.
  * The preview server starts in the main folder (`${workspaceFolder}`), so
    a worktree's demo can't be previewed; the demo also imports `dist/`,
    which a worktree has to build first.

## Branch JB1: `feature/job-state`

### Plan

**Type:** feature.

**Purpose:** `STATE_JOB` (new) with a `jobReason` byte replaces
`RUNNING_PLANNER` around today's records (`START`/`END` only, no headers yet),
with the reasons `BUFFERING`, `RUNNING`, `HOLDING` and `PAUSED`.

* MicroSegment goes first: its emitter, buffer, packets, `STATE_RUNNING`,
  `STATE_PAUSED` and `RunningReason`. Web: the MicroSegment wire format and
  its tests are deleted; callers are left broken and listed in the Outcome.
* The first record in IDLE opens `JOB.BUFFERING`. Buffering exits:
  `bufferTime` queued (a new shared counter) or a timeout. The last-record
  exit waits for JB3's `JOB_HDR`. Jogs are unaffected.
* A dry ring returns to `BUFFERING`; the job ends only by `cancel` (until
  JB3).
* `pause`: hold, then `JOB.PAUSED`, keeping the ring; `resume` continues it
  (`err moved` if XY moved, as today). Dropping the ring for `{index, s}`
  comes with JB3. No lift yet.
* A late adoption holds, then `BUFFERING`.
* `STATUS_RSP` carries the reason; job id and index wait for JB3's headers.

**Depends on:** nothing.

**Overlap:** `ipc/shared_state.h`, `web/src/wire/` (state and reason).

### Status

Done (merged). Unblocks JB3, together with JB2.

### Outcome

* Interfaces: `STATE_JOB = 9` and `JobReason` (BUFFERING 0, RUNNING 1,
  HOLDING 2, PAUSED 3, ABORTING 4). States 1 and 4 are retired, unused.
  `STATUS_RSP` byte 5 is the current state's reason (job, jogging, homing,
  probing; 0 otherwise); `bufCount` is planner blocks; `queuedUs` is the
  ring's unclaimed motion, read live under the lock. `get running` is gone;
  `get job` prints the reason while JOB, `-` otherwise. New
  `Planner::queuedTime()`. Web: `MachineState.JOB`, `JobReason`,
  `MachineStatus.reason`, `jobName`.
* Deviations:
  * The buffer exits (1 s queued, a full ring, or 500 ms with no new
    record) are constants in `core1/emit/follower.cpp`, not config fields.
  * No shared counter: Core 1 sums the ring with `queuedTime()`.
  * `cancel` also ends an open contour, so the next job's `START` is
    accepted.
  * The `MSEG_ACK`/`MSEG_NACK` names are kept for the data-plane replies.
* Web: `wire/link` was fixed to stay green: `atRest` counts a paused job or
  a dry buffering job as at rest, and the sim runs BEZIER records as a job.
  Left broken, to be removed: `choreograph`, `controller` (`runWalk`),
  `orchestrate` (`walk`, `estimate`), `production` (`compileBlock`,
  `dutyBreaks`; tests for `bakePlan`, `headResolve`, `snapshot`,
  `dutyInsert`, `dutyStream`), `toolpath/discretize`, `probe/probe.test`.
  `pnpm typecheck` and `pnpm test` are red only there (13 files).
* Bench (agent, on the machine, scripted over COM8): a short job runs, goes
  dry to BUFFERING and `cancel` ends it; pause holds then PAUSED with the
  ring kept, a record while paused gets NACK_PAUSED, resume continues with no
  jump, cancel from PAUSED; abort mid-cut ramps to IDLE and the next job runs;
  a trickled job alternates RUNNING and BUFFERING with `late=0`; `stop` in a
  job gives ALARM and drops the ring. Found and fixed there: `queuedUs` was
  only written on commit, so it never fell and survived cancel.
* Checks: `pio run -e pico` clean; `pio test -e native` 88/88;
  `test/wire` 223/223.
* Human scope still open: probing from a paused job; the late-adoption
  hold; `err moved` after a jog while paused.
* Out of scope: `docs/wire_protocol.md` and other docs still describe
  MicroSegment, `RUNNING` and `PAUSED`.

## Branch JB2: `feature/tool-mount`

### Plan

**Type:** feature.

**Purpose:** the Pico knows its tools and what is mounted.

* Config `tools` gain an explicit `id` (u8, unique, checked at load);
  `slotOffsets` becomes `chamberOffsets`. The Pico decodes the profiles.
* `mount <head> <tool id>` (0 = empty) sets the machine's mount state, in RAM,
  kept across jobs. A separate command from `select`: a head can be given a
  tool without being selected (tool change loads a pair one head at a time).
  This is the `loaded` of Tool change, above.
* `select` is unchanged; `get` and the status report each head's tool.

**Depends on:** nothing.

**Overlap:** `web/src/machine/schema.ts`, config encoder, `cmd/table.h`.

### Status

Planned.

## Branch JB3: `feature/layer-header`

### Plan

**Type:** feature.

**Purpose:** `JOB_HDR`, `TOOL_HDR` and the layer header (`TOOL_REC` above;
proposed name `LAYER_HDR`, `0xA2`), with preflight.

* A `BEZIER` before any layer header is `NACK_JOB`.
* The layer header selects the head and sets feed and `k`; resume from
  `PAUSED` becomes a layer header at the resume point.
* A layer whose tool is not mounted holds the job and reports that a change
  is due; the change itself is JB4.
* `STATUS_RSP` gains job id and index.

**Depends on:** JB1, JB2.

### Status

Planned.

## Branch JB4: `feature/tool-change`

### Plan

**Type:** feature.

**Purpose:** `JOB.TOOL_CHANGE` with a manual touch-off.

* Roughly: park both Zs (park legs), move XY to the load spot, wait for
  `mount`, then the operator touches off by hand (jog and `zero`, accepted in
  `TOOL_CHANGE` only), then `BUFFERING`.
* The second sequence (docs/plans/sequences.md), owner `JOB.TOOL_CHANGE`:
  park Zs (legs), then per head an XY line to load (ring move) and a wait for
  `mount`.
* Probing in the job (`TOOL_PROBE`) is later, once probe legs are a leg kind.

**Depends on:** JB3, and R3 of docs/plans/sequences.md.

### Status

Planned.

## Open

* Preflight refuses and never prepares (proposed, not confirmed).
* `cancel` from `PAUSED`: ends the session to IDLE at once, tool and vacuum
  off (proposed).
* **Vacuum zones from the bbox (later):** the servo ids drawn as seen from
  the front (first row at the back, X left to right), the servo covering
  machine (0, 0), and a cell size. The Pico finds the origin, which must be in
  a corner, and takes away from it as +X and +Y; load checks the corner and
  that every id appears once.

  ```jsonc
  "vacuum": {
    "cell": [392, 368],      // mm, X then Y
    "map": [[3, 4, 6],       // back row
            [5, 1, 2]],      // front row
    "origin": 5
  }
  ```

  Considered: rows written from the origin (first row the origin's, starting
  at the origin, columns +X, rows +Y), with no `origin` field and nothing to
  infer, but the operator flips the grid by hand whenever the origin is not
  top left of how they would draw it.

* Bench: how clean a re-plunge into the slit is; A backlash after an unwind.
* Not yet placed: state-handling branch 4 (alarm exits); motion gating
  (planner pushes refuse unbound, fenced or unhomed axes); lifts and duty
  after JB4.
