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
  | `PREFLIGHT`, `RUNNING` | `TOOL_CHANGE` → `BUFFERING` | Core 0 / Core 1 | the next tool is not mounted and zeroed; a resent `TOOL_REC` |
  | `PREFLIGHT` | IDLE | Core 0 | any preflight NACK |
  | `BUFFERING` | `RUNNING` | Core 1 | buffered time, last record, or timeout |
  | `RUNNING` | `BUFFERING` | Core 1 | dry ring or late adoption, at rest |
  | `RUNNING` | `HOLDING` | Core 0 | `pause` |
  | `HOLDING` | `PAUSED` | Core 0 | at rest, after the lift (job-z.md) |
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
  TOOL_REC(L, N)         tool, layer L, record N, feed, k
  BEZIER …               record N of layer L onward
  TOOL_REC(M, 0) …       the next layer, in any order
  ```

* **Every start sends the headers;** the host picks where to start. A job
  start is `TOOL_REC(0)`; a restart after an alarm or power loss is a fresh
  `JOB_HDR` with the same record count, tool headers for only the tools
  still to run, and `TOOL_REC`s from where it left off. A resume from
  `PAUSED` is `TOOL_REC(N)` alone, guarded by the resume point and job id.
* **No `config_crc32` and no `required_axes`.** The host no longer models the
  machine; the Pico infers axes and peripherals from each tool's profile.
* **Only `BEZIER` records are numbered, per layer,** and the number is not on
  the wire: the seq window keeps order with no gaps, so a record's number is
  its `TOOL_REC`'s record plus the count since. A `TOOL_REC`'s record is that
  of the next `BEZIER`. How the job header bounds a job (a record count per
  layer, or each layer's end) is open.
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

TOOL_HDR 0xA1, 21 B
[0] magic  [1] tool id  [2] seq  [3] head
[4..19] xmin, xmax, ymin, ymax f32, work coordinates at the tip
[20] CRC8

TOOL_REC (LAYER_HDR) 0xA2, 22 B
[0] magic  [1] chamber (0 = none)  [2] seq
[3] tool id  [4] head  [5..6] layer u16  [7..10] record u32
[11..14] feed f32  [15..18] k f32  [19..20] t u16 µm  [21] CRC8
```

`LAYER_HDR` is the one sent today; `JOB_HDR` and `TOOL_HDR` are not built.
Both name the head (Tool change).

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
  lift, then `PAUSED`: the ring is dropped and the resume point
  `{layer, record, s}` recorded (the block that was running and how far into
  it).
* With the ring empty, a jog can use it while paused. No second ring, no
  set-aside.
* **Contour framing resets on pause**, as on abort.
* **Resume is a `TOOL_REC`**, not a text command: in `PAUSED` only a
  `TOOL_REC` whose layer and record match the resume point is accepted, and
  every other
  record is NACKed, so a stray record cannot restart motion. Then
  `BUFFERING`.
* The resumed block is trimmed to start at `s − margin` (a firmware
  constant; 0 if `s` is shorter), so the cut overlaps where it stopped. The
  plunge there comes with lifts.
* `select` and `apply_offset` are allowed while paused (e.g. to jog another
  head); resume re-selects through its `TOOL_REC`.
* Planner branch 2's in-place resume (`restartFrom`, `err moved`) is unused on
  this path; it stays in `lib/planner` for now.

### Lifts and heights

* Heights, what lifts, tool on and off, and duty: docs/plans/job-z.md.

### Record index and identity

* **Ring blocks carry `{layer, record}`** (u16, u32), counted per layer. The
  wire seq is one byte and wraps every 256 records; it stays for the ACK
  window only. `status` will report the executing record.
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
* **In a job** a record out of range at ingest (control points, a Bézier
  lying in their hull) never enters the ring: `NACK_SOFT_LIMIT`, which the
  host session treats as fatal. The Pico keeps the job open; it runs what it
  has, goes dry into `BUFFERING`, and waits for `cancel`. No alarm: nothing
  left the range. A START's travel needs no check of its own (it runs from
  the ring's end to a checked p0). The dry stop lifts (docs/plans/job-z.md).

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

* **The mount state** (which tool is on each head) is kept by the Pico
  across jobs, in RAM, set by `mount <head> <tool>` (0 none). There is no
  job mount plan: the Pico keeps nothing about which head a tool should be
  on (JT).
* **The host picks the head** and names it in each header: `TOOL_HDR`, so
  preflight checks the bbox with that head's offset and forgets it, and
  every `TOOL_REC`, checked against the mount state as it arrives. Keeping
  the two the same is the host's; the per-record soft-limit check still
  holds if they differ.
* **A header whose tool is not mounted on its head, or not zeroed,** is
  `NACK_TOOL_CHANGE` and holds the job; once the ring is dry the stop: park
  every Z, lock every A to 0, an XY line to `machine.positions.mount`, then
  `TOOL_WAIT`. There `mount` (any head), `jog`, `wzero` and `probe_park` are
  admitted.
* **Only a resent header leaves `TOOL_WAIT`:** with its tool mounted on its
  head and that head zeroed, the job goes back; otherwise
  `NACK_TOOL_CHANGE` again.
* A tool mounted and zeroed on its head with another head selected is a head
  switch alone: selected at rest, no stop.
* **Park legs run outside homing:** as steps of the tool-change sequence,
  ticked by the runner and dropped when the session loses the machine
  (docs/plans/machine-model.md). Records are held off while a
  leg is armed. A failed park leg alarms as `ALARM_HOMING_FAIL`
  (`LEGFAIL_PARK`) with the job kept. `pause` is refused in `TOOL_CHANGE`.
* The host orders layers by tool; returning to an earlier tool is another
  change, allowed but slow.

### Reporting

* **`STATUS_RSP` grows** (new magic, as `0xA6` → `0xA7`): `bufCount` goes
  with MicroSegment; the executing layer and record are added (S1; the host
  keeps the job id); the reason byte
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
  saves the executing `{layer, record}` from each status poll (S1).
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

> Superseded for the text commands by docs/plans/position-moves.md (Command
> shape): `jogto` is now `goto`, and `jog` gained `xy`. Kept as history.

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
are independent; JB3 needs both; JB3b and JB4 need JB3.

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
* Purpose: `jogto park|load|probe`, and every `jogto` parks the Zs before
  its XY move.
* Depends on: J1; M3 and M3c of docs/plans/machine-model.md.
* What the code has today: stored positions are decoded
  (`core0/config/config_decode.h`, `CfgPoint`) but nothing resolves them.
  `homingArmPark(node, last, …)` (`core0/ops/homing.h`) arms a park leg as
  a runner thread; `controller/seq/home.cpp` parks homed nodes that way.
  `jogto x y` is a `K_STEP` XY line that leaves Z where it is
  (`cmd/axis.cpp`). A RING step has no end: the driver advances on leg
  events only, and the observer drops `FACT_RING_DRAINED`.
* Scope (agreed in Read):
  1. A resolver in `core0/ops/frames`: a name to the anchor's machine XY
     (`park` defaulting to where homing parks, `load`, `probe` as the
     selected head's `probeSwitch` minus its offset). Missing: `err
     no_position`; `probe` with no head selected: `err no_head`.
  2. Step builders `parkZs()` and `lineTo()` (machine-model.md), with the
     leg numbers (`intervalUs`, `distSteps`, `parkSteps`) moved out of
     `controller/seq/home.cpp` into a shared file in `controller/seq/`.
  3. A RING step ends on `FACT_RING_DRAINED`: the observer hands it to the
     driver, which advances once the ring is empty and `!plannerActive`
     under `plannerLock`. A line of zero length ends its step at once.
  4. `controller/seq/jogto.cpp`: the `jogto` sequence, owner JOGGING, reason
     `JOGGING_GOTO` throughout: step 1 parks every head's Z node (legs, in
     parallel), step 2 is the XY line (ring move). A failed park alarms as
     `ALARM_HOMING_FAIL` (`LEGFAIL_PARK`). `stop` mid-sequence aborts the
     live step.
     * One label: `SEQ_JOGTO` publishes JOGGING in `stateOf`
       (`core0/ops/machine.cpp`) through both steps, not TRAVERSE then
       JOGGING.
     * `lineTo()` queues as a new ring owner `RING_SEQ`, so a cjog packet
       neither stops it (`acceptCjog` stops only jog owners) nor appends
       (`plannerJogFrom` refuses another owner's ring).
  5. `jogto x y` and `jogto park|load|probe` both run the sequence: the
     named form resolves the name, the XY form converts work to anchor
     machine XY, then one start. `jogto` moves to the controller table as
     `K_SEQUENCE` (`controller/cmd/`). X, Y and every present head's Z must
     be homed (`err not_homed`). Plain `jogto` no longer joins running jogs
     (`busy`) and is refused with faults other than `F_LATCHED`.
  6. `err busy` for commands that meet the live sequence (machine-model.md,
     Decisions); the web sim gets `JOGGING_GOTO`.
  7. Docs: `docs/wire_protocol.md`.
* Checks: as J1. Human checks: `jogto x y` and each name with each head
  selected, Z parked first; a missing position refused; `stop` during the
  park and during the line.

### Status

Done (merged). Nothing waits on J1b.

### Outcome

* Interfaces: `RING_SEQ = 3` (`jogging=3`, GOTO); `SEQ_JOGTO = 5`, published
  JOGGING. `framesNamed()` in `ops/frames`. `controller/seq/steps.*` holds the
  leg numbers and the step builders `parkZs()` and `lineTo()` for later
  recipes. A RING step ends on `FACT_RING_DRAINED` (`sequenceRing()` /
  `sequenceRingDrained()`).
* Deviations: none from the agreed plan. A `jogto` to where the head already
  is still parks the Zs, then ends.
* Known gap: a line refused after the park ran (a queue refusal, e.g. the
  mesh's range) ends the sequence with no fault, after `ok`. The soft range is
  checked before anything moves, so it should not happen in practice.
* Bench (COM8, head 0): plain `jogto` and `park` ran with Z parked first and
  `jogging=3` through park and line; `busy` to `jog` meanwhile; `stop` in the
  park and in the line latched as any `stop`. `load` and `probe` moved
  nothing: the bench config has no load position and head 0 no probe switch,
  so only `no_position` was seen.
* Also here: `wire_protocol.md`'s STATUS_RSP and `get` keys brought up to JB1
  (state 9 JOB, `job=` replacing `running=`, `bufCount`/`queuedUs`). Out of
  scope, still stale: the command state tables at the end of
  `wire_protocol.md` still use RUNNING/PAUSED columns.

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

Done (merged). Unblocks JB3 (with JB1).

### Outcome

* Interfaces: `CfgTool` / `MachineCfg.tools` (at most `CFG_MAX_TOOLS` 8, each
  at most `CFG_MAX_CHAMBERS` 8) with `configTool(cfg, id)`; decoded: id,
  `toolType`, `tangential`, `plunge`, `toolOffset`, `path.feed`, `z.feed`,
  `chamberOffsets`. `cfgerr=tools`. Blob version 6. `ops/tools`:
  `toolsMounted(head)` for JB3's not-mounted check. `mount`, `get tools`, a
  `tools=` field on `status`. Web: `ToolProfile.id`, `chamberOffsets`.
* Deviations:
  * Only the tools a config lists exist, each with a required `id`; names
    are still the four preset keys (free names with a `type` are later).
  * `mount` does not check the head's `accepts`.
  * The mount is reported in text only; STATUS_RSP waits for the job header.
  * `mount` does not post a sequence event yet (machine-model.md expected
    it here); that comes with JB4's WAIT.
  * A tool without its own feed decodes as 0, meaning the machine's.
* Known failing: `pnpm typecheck` has 2 errors beyond `main`'s 40:
  `orchestrate/walk.ts` and `production/bakePlan.ts` read `slotOffsets`.
  They are the dead host-driven pipeline (with `choreograph`), left broken;
  removing it is its own branch.
* Bench (COM8, 1-head config): the v6 blob loads; `mount` sets and clears,
  refuses `no_tool`, `no_head`, `usage`; `get tools` and `status` report it.
  Not tried: a config push clearing the mounts.

## Branch JB3: `feature/layer-header`

### Plan

**Type:** feature.

**Purpose:** the layer header (`LAYER_HDR`, `0xA2`, the `TOOL_REC` layout
above) opens and steers a job. `JOB_HDR` and `TOOL_HDR` are later.

* A layer header in IDLE opens `JOB.BUFFERING`; Béziers no longer open a
  job. A `BEZIER` before any layer header is `NACK_JOB`.
* It selects the head the tool is mounted on, sets the cut feed (0: the
  tool's `path.feed`, then the machine's) and checks the chamber against
  `chamberCount` (the angular offset waits for A in jobs). It stores `k` and
  the index, unused for now. The travel feed stays on `feed`.
* A tool not in the config: `NACK_TOOL_UNKNOWN`. A tool not mounted:
  `NACK_NOT_MOUNTED`; the job runs dry into BUFFERING. JB4 replaces this
  NACK with the tool-change sequence.
* A job still ends only by `cancel`; `STATUS_RSP` is unchanged.
* Web: the `LAYER_HDR` encoder, with tests.

**Depends on:** JB1, JB2.

**Overlap:** `web/src/wire/`, `usb_protocol.h`.

### Status

Done (merged). Unblocks JB3b and JB4.

### Outcome

* Interfaces: `LAYER_HDR` 0xA2 (17 B); `MSEG_NACK_JOB` 0x0C, `TOOL_UNKNOWN`
  0x0D, `NOT_MOUNTED` 0x0E. `layerResolve` (ops/layer),
  `plannerLayerCheck`/`plannerOpenLayer`, `axesSelectHead` (shared with
  `select`). Web: `packLayerHdr`, the constants; `stampSeq` takes a layer
  header; the sim opens a job on one.
* Deviations:
  * A head switch needs the bus free, so it happens from IDLE only; in a job
    it is always `NACK_BUSY`, including after the operator selects anchor
    while paused. A fix (switch in a dry BUFFERING job, or through PAUSED) is
    open; JB4's tool change will need it.
  * Chambers are numbered 1..`chamberCount`.
  * The session treats the three new NACKs as fatal.
* Left broken: anything streaming `prepareBezierJob` packets without a layer
  header gets `NACK_JOB`; no live caller does.
* Checks: `pio run -e pico` clean; `pio test -e native` 151/151; `test/wire`
  229/229; `pnpm test` and `pnpm typecheck` as `main`.
* Not benched.

## Branch JB3b: `feature/pause-drain`

### Plan

**Type:** feature.

**Purpose:** Pause drains the ring (Decisions, above); records are numbered
per layer.

* `LAYER_HDR` grows to 19 B: `[4..5]` layer u16, `[6..9]` record u32 (the
  first record's number within the layer), feed and k after. Layers may
  arrive in any order.
* Ring blocks carry `{layer, record}` (`lib/planner/`), counted per layer from
  the header's record; a START's travel takes the tag of the record it leads
  to.
* The first record after any layer header counts as START.
* `pause`: hold, then `PAUSED` with the ring dropped and `{layer, record, s}`
  kept. `get resume_layer` / `resume_record` report it (`-` otherwise).
* Resume is a layer header in `PAUSED` matching the layer and record; block
  `record` is trimmed to start at `max(0, s − 2 mm)` (a firmware constant).
  Any other record is NACKed. The head may switch there (the bus is free).
  The `resume` command, `resumePos`, `resumeRequested` and `err moved` go.
* Web: the encoder for the new layout; the sim drops the ring on pause and
  resumes on a matching header; the link loses `resume`.

**Depends on:** JB3.

**Overlap:** `lib/planner/`, `core1/emit/follower.cpp`, `usb_protocol.h`,
`web/src/wire/`.

### Status

Done (merged). Unblocks JB4.

### Outcome

* As planned. The margin is `kResumeMargin` (2 mm) in
  `core0/planner/queue.cpp`; `lib/planner` gained `setTag`,
  `pushBezierFrom` and `oldest()`, and the executor `current()` / `along()`.
* A pause with an empty ring resumes at the next record Core 0 expects (the
  last header's, counted on). A pause in a START's travel resumes that record
  whole.
* A trimmed curve keeps the whole curve's speed cap and mesh walk
  (conservative). A header inside a contour is still `NACK_JOB` in a running
  job; only a resume clears an open contour.
* Bench (head 0, pen, 10 mm/s): pause mid-curve, in a travel, in BUFFERING
  with an empty ring and right after a header all report the expected
  `{layer, record}`, hold still, refuse a wrong header with `NACK_JOB` and
  finish on the right one. Not seen: the 2 mm back-off (polling too coarse),
  a head switch on resume.
* Out of scope: `web/src/controller/runWalk.ts` still sends `resume` (left
  broken); the chamber change (lift, turn A, plunge) goes with the lifts
  work; how a job header bounds a job is open.

## Branch JB4: `feature/tool-change`

### Plan

**Type:** feature.

**Purpose:** a layer header names the head its tool goes on; a mismatch
holds the job for a tool change with a manual touch-off.

* **`LAYER_HDR` grows to 20 B:** a head byte (the head the layer's tool goes
  on) joins the header; feed and k move up one. The Pico never picks a head:
  `layerResolve`'s "lowest head with the tool" goes.
* **The header's reply is the host's flow control:**
  * ACK: the tool is mounted on the named head and that head is selected.
    The host keeps streaming; nothing drains.
  * `NACK_TOOL_CHANGE` (new, 0x0F): the header is not consumed. The host
    stops, waits for `BUFFERING`, then resends from the header (Go-Back-N
    already rewinds there). From IDLE the NACK still opens the job.
  * `NACK_NOT_MOUNTED` goes.
* **Draining:** the NACK raises `toolChangePending` with the wanted head and
  tool. The ring runs out (no record can join: the header was not consumed);
  when it runs dry Core 1 lands `TOOL_CHANGE` instead of `BUFFERING`. From
  IDLE or a dry `BUFFERING` Core 0 enters it at once.
* **A head switch alone** (the tool is mounted on the named head, another is
  selected): select it at rest, then `BUFFERING`. No park, no clear. Replaces
  JB3's `NACK_BUSY`.
* **A tool change** (the named head lacks the tool): the sequence on the M3
  driver (docs/plans/machine-model.md): park the Zs (`parkZs`, LEGS), select
  the named head, then a line to its probe switch (`lineTo`, RING). No WAIT
  step.
* **Two job reasons:**
  * `TOOL_CHANGE`: the Pico is moving (the sequence). Jogs and writes are
    `busy`. Published as JOB `TOOL_CHANGE` throughout, not JOGGING.
  * `TOOL_WAIT`: the sequence ended; the operator's turn. Entering it clears
    the named head's Z work offset (a per-head "Z set" flag; the old tool's
    zero holds until then). `mount`, `jog` (step and continuous) and `wzero`
    are admitted here only, within a job. `mount` must name the wanted head
    and tool. A matching mount plus that head's Z set gives `BUFFERING`, with
    an empty ring; the host resends the header, now ACKed.
* `pause` is refused in both reasons; `cancel` ends the job. A failed park
  leg alarms and loses the job (keeping it goes with M4).
* Probing in the job (`TOOL_PROBE`) is later, after M4.
* Web: the 20 B encoder, the new NACK and reasons; the sim follows.
* Docs: `docs/wire_protocol.md` (layout, NACK, reasons, admission).

**Depends on:** JB3 (done), JB3b (done), M3 of docs/plans/machine-model.md
(done).

**Overlap:** `usb_protocol.h`, `web/src/wire/`, `core1/emit/follower.cpp`,
`ops/machine.cpp`.

### Status

Done (merged). No planned branch waits on it; lifts and duty (Open) were
placed after it.

### Outcome

* Interfaces: `LAYER_HDR` 20 B (`[4]` head; layer, record, feed, k one byte
  on); `MSEG_NACK_TOOL_CHANGE` 0x0F (`NOT_MOUNTED` 0x0E gone); `JOB_TOOL_CHANGE`
  5, `JOB_TOOL_WAIT` 6; `SEQ_TOOL_CHANGE` 6; `K_IN_TOOL` (admits `mount`, `jog`,
  `wzero` and continuous jogs in TOOL_WAIT); `layerResolve` takes the head;
  `controller/seq/toolchange.*`; `plannerHoldForTool`, `plannerJobSwap`;
  `framesUnsetWorkZ`/`framesWorkZSet`; `zsReady()` in `steps.cpp`, shared with
  `jogto`. Web: `packLayerHdr({ head })`, the session retries
  `NACK_TOOL_CHANGE` every 250 ms.
* Deviations:
  * The head is selected before the park, not after (no motion depends on it).
  * A change made from PAUSED goes back to PAUSED, so the resent header keeps
    its resume trim; the host waits for BUFFERING or PAUSED.
  * With the tool mounted on its head and another head selected, IDLE and a
    resume select it at once (the bus is free), as before; only a running
    job's switch goes through the change.
  * `jogto` checks Z homing after its soft limits now (`zsReady`).
* Known gaps: a change that fails before its sequence starts (no probe
  switch, a Z not homed) reports `seq=-`, not 6. The sim models no mounts and
  never enters a change.
* Bench (COM8, head 0, pen, probe switch (100, 100) in a bench-only config):
  IDLE header for an unmounted tool → `NACK 0f`, park, line to the switch
  (13 s), TOOL_WAIT; `mount 0 1` `wrong_tool`, `pause`/`select`/`wclear`
  refused, the header `NACK 0f`, `jog z` allowed; `mount 0 2` holds until
  `wzero z`, then BUFFERING; the resent header ACKed and the record ran.
  Mid-job: header and next record `NACK 0f`, the ring finished, then the
  change. Without a probe switch: `seqfail=no_position`, job ended. `cancel`
  from TOOL_WAIT to IDLE. Not tried: from PAUSED, a head switch alone (head 1
  excluded), `stop` in TOOL_CHANGE, a failed park, a continuous jog in
  TOOL_WAIT, the session against the Pico.
* Checks: `pio run -e pico` clean; `pio test -e native` 152/152; `test/wire`
  231/231; `pnpm test` and `pnpm typecheck` as `main`.

## Branch JX: `feature/job-xy`

### Plan

**Type:** feature.

**Purpose:** finish X and Y in jobs before Z: feeds from the config, and the
mounted tool's offset in the tip.

* What the code has today:
  * Records already go work → machine (`framesToMachine`) and are checked
    against XY's soft range per control point (`data_plane.cpp:179-184`);
    a START's travel is an XY line at `travelFeed` (`queue.cpp:401-425`).
  * The travel feed and the machine's cut feed come only from the `feed`
    text command (`plannerSetFeed`). The config's machine-level `path` and
    `rapid` are not decoded by the Pico (`config_decode.cpp:146` reads only a
    tool's `path.feed`).
  * `framesTipOffset` (`ops/frames.cpp:114`) returns the head's offset only;
    the tool's `toolOffset` is decoded (`config_decode.h:93`) but unused.
    It feeds `framesToMachine`, jogs, `jogto`, `wzero`, the readout and the
    mesh's tip (`meshLatch`, `ops/mesh.cpp:95`).
* Scope:
  1. Config: machine-level `path` and `rapid` become
     `"feed": { "cutFallback": …, "travelDefault": … }`, decoded by the Pico
     at load and applied as the planner's feeds. The `feed` text command
     stays as a live override. Web schema, loader, defaults and validator
     follow; blob version bumped, fixtures regenerated. `machine.z` and
     `slew` are left as they are.
  2. Tip offset = head offset + the mounted tool's `toolOffset`; the head's
     alone when nothing is mounted (or the anchor is selected). One change in
     `framesTipOffset` reaches records, jogs, `jogto`, `wzero`, the readout
     and the mesh.
  3. The mesh latch picks the new offset up at the next ring reset. `mount`
     is `K_WRITE` (IDLE, probing, homing, a paused job) or `K_IN_TOOL`
     (TOOL_WAIT): never with a ring running, so no new gate.
  4. `feed <cut> <travel>` refuses either value below 1 mm/s (`err usage`).
     The override holds until reboot.
  5. `probe_park <tool>`: a head's probe switch is one machine position,
     around the anchor, shared by every tool. The command takes the selected
     head's switch, less the head's offset and `<tool>`'s `toolOffset`, and
     runs the `jogto` sequence there (Zs parked, then the line), so that
     tool's tip lands on the switch whether or not it is mounted. The tool
     change's line uses it with the wanted tool. `jogto probe` goes.
  6. Feeds are plain numbers, accels go (the planner uses the axes' only;
     a per-tool cap can come later if a tool needs it): the machine's `z`
     becomes `feed.plungeFallback`; a tool's `path` and `z` blocks become
     `cutFeed` and `plungeFeed`, falling back to `feed.cutFallback` and
     `feed.plungeFallback`. Old keys are refused by name. A machine-wide
     `liftFeed` comes with Z; `slew` stays until A.
  7. Docs: the config doc and `docs/wire_protocol.md` (`feed`, `probe_park`,
     the frame chain).
* Not in scope: the corner between a travel and its first cut (blended within
  `kDeviation`; the lift's stop removes it when Z comes), the soft-limit reply
  (stays a NACK; Out of range, above).
* Known effect: the position readout and `wzero`'s reference move on `mount`
  when the tool has an offset.
* Assumed: no tool's offset reaches past its head's footprint, so the load
  check of each probe switch against the soft range stays head-offset only;
  a tool that misses is still refused by the soft range when it moves.

**Depends on:** JB4 (done).

**Overlap:** `web/src/machine/schema.ts`, the config encoder, `ops/frames.cpp`.

### Status

Done (merged). No planned branch waits on it; Z in jobs (lifts and duty,
Open) comes next.

### Outcome

* Interfaces: `MachineCfg.cutFallback`, `travelDefault`, `plungeFallback`
  (`cfgerr=feed` below 1); `CfgTool.cutFeed`, `plungeFeed`;
  `plannerMachineFeed()` reads the config unless `feed` overrides it;
  `framesTipOffset` includes the mounted tool; `framesProbeFor(tool)` and
  `probe_park`. Blob and schema version 7. Web: `MachineConfig.feed`
  (`JobFeed`), `ToolProfile.cutFeed`/`plungeFeed`; `resolveTargets` returns
  `{cutFeed, plungeFeed, travelFeed, slew}`; `MachineTarget` is gone.
* Deviations: the plain tool feeds and `plungeFallback` were agreed mid-branch
  (item 6); the soft-limit gate already existed, so only the plan's wording
  changed (Out of range).
* For the Z branch: a machine-wide `liftFeed`; the travel still blends into
  its first cut within `kDeviation` until a lift's stop separates them.
* Bench (COM8, head 0, bench-only probe switch (100, 100) and pen
  `toolOffset` (5, 3)): `feed` below 1 refused; `jogto probe` gone; `mount`
  shifts the readout by the tool offset; `probe_park 2` (pen, unmounted) to
  anchor (165, 127), its tip then reading (100, 100); `probe_park 1` to
  (170, 130); `jogto 300 300` with the pen at anchor (365, 327); a tool
  change to the knife ended at the knife's probe position; after a reboot a
  job ran with no `feed` command, travelling at about 80 mm/s. Run on the
  config shape before the plain tool feeds (a key rename, covered by the
  decoder test). Not tried: `cutFallback` alone (both tools set `cutFeed`),
  a change from PAUSED, `probe_park` with no head selected.
* Checks: `pio run -e pico` clean; `pio test -e native` 152/152; `pnpm
  typecheck` clean; `pnpm test` 536 passed, 1 skipped.
* Out of scope: `docs/feed_accel_value_model.md` still describes the old
  targets; the web-only config fields (`invert`, the homing extras) are dead
  and proposed as their own refactor.

## Status and lift reporting

Three independent branches, run in parallel; S1 merges first (a protocol
change). The host is told nodes and the selection, never slots.

## Branch S1: `feature/status-v3`

### Plan

**Type:** feature.

**Purpose:** STATUS_RSP reports per node and in work coordinates; the text
`status` becomes a `get` group.

* What the code has today:
  * STATUS_RSP 0xA7, 31 B (`core0/usb_protocol.h:85-97`, written by
    `sendStatusRsp`, `core0/status.cpp:37`): `[2]` `axes_enabled` and `[3]`
    `axes_homed` are slot views, so homing a head that isn't engaged shows
    nothing; `pos[4]` is `machinePos` in steps, not the WPos the Reporting
    decision named.
  * The node truths exist: `nodeEnabled` (`ipc/shared_state.h`),
    `originMask()` (`get nodehomed`).
  * Text `status` (`cmd/query.cpp:48-85`, alias `?`) prints its own line;
    all of it is `get` keys except `buf`, `mute`, `excluded`, `touched`.
    The web polls with `get` over `STATE_KEYS`
    (`web/src/wire/link/commands.ts:60`), not `status`.
* Scope:
  1. STATUS_RSP v3, magic `0xA4`, 36 B (`0xA8` is SEQRESET):

     | byte | field | type |
     |---|---|---|
     | `[0]` | magic `0xA4` | |
     | `[1]` | machineState | u8 |
     | `[2]` | reason | u8 |
     | `[3..4]` | faults | u16 LE |
     | `[5..6]` | nodeEnabled | u16 LE |
     | `[7..8]` | nodeHomed (`originMask()`) | u16 LE |
     | `[9]` | selection: head index, `0xFE` anchor, `0xFF` none (`frames`' values) | u8 |
     | `[10]` | tool: the id mounted on the selected head, 0 none (as `mount`) | u8 |
     | `[11..26]` | WPos X/Y/Z/A, selected frame, mm and degrees | f32 LE ×4 |
     | `[27..28]` | bufCount | u16 LE |
     | `[29..30]` | layer: the executing block's | u16 LE |
     | `[31..34]` | record: the executing block's | u32 LE |
     | `[35]` | CRC8 over `[0..34]` | |

     Layer and record: Core 1 publishes the tag of the block it claims (an
     inserted travel, lift, plunge or swivel carries the record it leads
     into); Core 0 reads the pair under `plannerLock`. In `PAUSED` the
     resume point (`jobResume`); no job, layer `0xFFFF`, record 0.
     The host knows its job id; it cannot know how far the ring has run.

     A position with no value (unbound Z/A, X/Y with nothing selected) is
     NaN. The tool says what WPos is the tip of; other heads' tools stay on
     `get tools`. MPos stays on `get`. `queuedUs` and `expectedSeq` leave
     the frame (no host reads them: pacing went with MicroSegment, the ACK
     carries the seq) for `get queued` and `get expectedseq`. The Reporting
     decision's "`0xFF` none" for the tool becomes 0.
  2. `get` groups (`core0/cmd/get.cpp`): a table maps a group name to an
     ordered key list, expanded in place before the snapshot; repeats are
     dropped (first position wins), so `get status homed` prints `homed`
     once. Group and key names share one namespace. `status` = `state homed
     enabled nodehomed nodeenabled head tools pos mpos wpos buf
     mute excluded touched mesh`.
  3. New keys: `buf`, `mute`, `excluded`, `touched`, `nodeenabled`,
     `queued` (µs), `expectedseq`. `buf` is the block count alone. No
     node latch mask: it needs the bus.
  4. The `status` command goes; `?` becomes `get status`.
  5. Web: the v3 decoder (`web/src/wire/format/`), the demux's frame length,
     the sim. `MachineStatus` carries every v3 field (`nodeEnabled`,
     `selection`, `tool`, `wpos`, `layer`, `record`) whether or not a caller
     reads it yet; `axesHomed`/`axesEnabled` become text-plane only; `pos`,
     `expectedSeq`, `queuedUs` go; `session.ts` loses `queuedUs` and
     `queuedSample`; `STATE_KEYS` unchanged.
  6. Docs: `docs/wire_protocol.md`, and the 0xA7 frame where other docs
     describe it (`comms_architecture.md` §4.6, `validation_checklist.md`,
     `engage_and_axis_map.md`).
* Files: `core0/usb_protocol.h`, `core0/status.cpp`,
  `core1/emit/follower.cpp`, `ipc/shared_state.{h,cpp}`, `core0/cmd/get.cpp`,
  `core0/cmd/query.cpp`, `core0/control_plane.cpp`, `core0/cmd/table.h`;
  `web/src/wire/`; `docs/wire_protocol.md`.
* Checks: `pio run -e pico`; `pnpm typecheck` and `pnpm test` in `web/`.
  Human checks: homing a head that isn't engaged sets its bits in
  `nodeHomed`; WPos matches `get wpos`; `?` and `get status late`.

**Depends on:** nothing.

**Overlap:** `usb_protocol.h`, `web/src/wire/`, `cmd/get.cpp` (S2, S3).

### Status

Done (merged). S2 and S3 can start; they build on `get.cpp`'s groups.

### Outcome

* Plan changes agreed in Read: magic `0xA4` (`0xA8` is SEQRESET), no node
  latch mask (it needs the bus), so 36 B; Core 1 publishes the claimed
  block's tag (`jobExec`, `ipc/shared_state.h`).
* `plannerExecPoint` (`planner/queue.h`) is the one read of the executing
  record, under one hold of the lock: the resume point while paused, the
  claimed block, else the oldest queued or the next expected record.
* `?` prints `state` as a number, as `get` does; the old `status` printed
  the name. Bare `get` lists keys only: the web's `keys` parser would read a
  group as a key.
* Web: `MachineStatus` keeps its positional constructor; `wpos`, `layer`,
  `record` take the slots of `pos`, `expectedSeq`, `queuedUs`, and
  `nodeEnabled`, `selection`, `tool` are appended. The sim does not model
  the executing record, and its WPos is its step position over steps/mm.
* For S2 and S3: `get.cpp` now has a `Request` (dedupe) and a group table;
  a new key added to `status` goes in `kStatusKeys`.
* Bench (idle, no nodes enabled): `?`, a group with a repeated key, the
  new keys, `status` → `err unknown`, and a 36 B frame whose fields match
  `get` and whose CRC checks. Human checks still open: layer and record
  through a running and a paused job; `nodeHomed` for a head that isn't
  selected; NaN for an unbound Z/A.

## Branch S2: `feature/head-positions`

### Plan

**Type:** feature.

**Purpose:** the machine position of every head's Z and A, not only the
selected head's.

* What the code has today: `get mpos` is slot order (`fmtUnits`,
  `cmd/get.cpp:108-118`), `-` for an unbound Z/A, so other heads never show.
  `nodepos <node>` (`cmd/query.cpp:127`) asks a node over the bus for its
  raw steps, at rest only.
* Scope:
  1. At unbind the Pico keeps the node's machine position (the slot's
     `machinePos`, steps) beside its origin (`ops/position.{h,cpp}`,
     `originRecord`); dropped when the node loses its origin or is
     disabled; replaced by the live value while bound.
  2. `get headpos`: each head's `z,a` in head order, `;` between heads,
     units, `-` where there is no value (`headpos=12.000,0.000;5.000,-`).
  3. `nodepos` stays: the node's raw counter, the ground truth.
     `nodepos` − origin = `headpos` at rest is the bench check.
  4. Docs: `docs/wire_protocol.md`.
* To check in Read: whether an unbound, enabled node can move without the
  Pico tracking it (a park leg, `step`, homing); if so, those paths update
  or drop the kept position.
* Files: `core0/ops/position.{h,cpp}`, the unbind path (`slotUnbind`),
  `core0/cmd/get.cpp`; `docs/wire_protocol.md`.
* Checks: `pio run -e pico`. Human checks: deselect a head, jog the other,
  `headpos` holds the first head's Z/A and matches `nodepos` − origin;
  disabling the node gives `-`.

**Depends on:** nothing.

**Overlap:** `cmd/get.cpp` (S1, S3).

### Status

Planned.

## Branch S3: `feature/lift-reason`

### Plan

**Type:** feature.

**Purpose:** say why the blade is up, and what the tool's duty is doing
(docs/plans/job-a.md, Open). Today every lift reads as `JOB.RUNNING`, or the
reason `plannerQueueLift` sets, with Z moving.

* What the code has today:
  * Ring blocks carry `layer`, `record`, `break_dist`, set by `setTag`
    (`lib/planner/planner/planner.h:61-63`, `:119`); no lift reason.
  * Lifts: `plannerQueueRecord` (`core0/planner/queue.cpp:643-668`, duty,
    corner and unwind sharing one Z pair), the END lift (`:709`), the
    deadline duty insert (above `:723`), `plannerQueueLift(why)` (`:723`,
    pause and dry ring).
  * Duty is Core 1's: `dutyOnMs`, `toolOnAtMs`
    (`core1/emit/follower.cpp:181-284`); `toolOn`, `jobPower`
    (`ipc/shared_state.h:261-277`). `get` has neither.
* Scope:
  1. A lift tag, a `u8` bitmask on each block of a lift (Z, the A or XY it
     carries, the plunge): `END`, `CORNER`, `DUTY` (planned at a BREAK),
     `DEADLINE`, `UNWIND`, `PAUSE`, `DRY`. One lift sets every reason due
     at its spot. `lib/planner` stores it beside `layer`/`record`.
  2. Core 1 publishes the executing block's mask (`liftReason`, 0 on a cut
     or travel) as it claims a block, and the duty on-time.
  3. `get` keys: `lift` (the bits by name, `-` when 0), `tool` (on/off),
     `onms` (on-time since the duty's last reset, the open span included).
     Not in STATUS_RSP.
  4. Docs: `docs/wire_protocol.md`.
* Not in scope: AF's duty findings (a reset during an unwind's lift; two
  lifts when a deadline falls just before an unwind join).
* Files: `lib/planner/planner/planner.h` and native tests;
  `core0/planner/queue.cpp`; `core1/emit/follower.cpp`;
  `ipc/shared_state.{h,cpp}`; `core0/cmd/get.cpp`; `docs/wire_protocol.md`.
* Checks: `pio run -e pico`, `pio test -e native`. Human checks: each lift
  shows its bit (END, corner, unwind, pause, dry; duty and deadline with
  the laser standing in); a BREAK with duty and corner shows both; `onms`
  climbs while cutting and resets with the duty.

**Depends on:** job-a AF (done).

**Overlap:** `lib/planner/`, `cmd/get.cpp` (S1, S2).

### Status

Done (merged). Unblocks nothing.

### Outcome

* Bits: `END`, `CORNER`, `DUTY`, `DEADLINE`, `UNWIND`, `PAUSE`, `ABORT`;
  `DRY` reserved (a ring run dry does not lift; a pause at a dry ring is a
  `PAUSE`). `ABORT` added: `plannerQueueLift(JOB_ABORTING)` lifts too.
* Keys, all `-` outside a job: `liftreason` (names, `-` before the first
  lift), `liftcount`, `toolon`, `onms` (the last two `-` for a passive tool).
  They reset as a job opens, not on resume. A START's travel is not a lift.
* Planner: `setLift()` beside `setTag()`, which clears it.
* A BREAK's duty stop carries the `CORNER`/`UNWIND` reasons it absorbs.
* Core 1 counts a lift where the claimed block's mask changes to a nonzero
  one, so an END lift then a pause lift count two (`JobLift`).
* A pause or abort that only switches the tool off (Z cannot lift) is not
  tagged.
* The duty counters are shared now (`dutyOnMs`, `toolOnAtMs`, `dutySeen`);
  `onms` reads 0 while a reset is pending on Core 1.
* Human checks still open: each bit on the machine, `duty,corner` at a
  BREAK, `onms` climbing and resetting.

## Branch JT: `feature/tool-wait-exit`

### Plan

**Type:** feature.

**Purpose:** leave a tool change only by the host's resent header, checked
against the state at that moment; the Pico tracks no mount plan (replaces
the pair table). The operator may mount and zero the next head in the same
stop.

* What the code has today:
  * TOOL_WAIT exits by itself once the wanted tool is mounted and its Z set
    (`toolChangeTick`, `controller/seq/toolchange.cpp:133-136`).
  * `mount` in TOOL_WAIT takes only the wanted head and tool
    (`toolChangeMountCheck`, `toolchange.cpp:115-118`).
  * Z set is cleared on entering TOOL_WAIT (`enterWait`, `toolchange.cpp:32-35`),
    not on `mount`.
  * A header for a mounted tool with no Z set is `NACK_JOB`, fatal
    (`data_plane.cpp:244`). A header in TOOL_WAIT reaches
    `plannerHoldForTool` (`planner/queue.cpp:411-438`), which has no
    TOOL_WAIT case: `NACK_BAD_STATE`, fatal.
  * The change parks the Zs (`parkZs`, `seq/steps.cpp:68-87`), then one line
    to `framesProbeFor(wantTool)` with the selected A riding to its lock
    (`lineTo`, `steps.cpp:103-116`; `toolchange.cpp:61-96`).
  * `probe_park <tool>` (`controller/cmd/jogto.cpp:44-55`) is `K_SEQUENCE`
    and uses the selected head.
* Scope:
  1. Config: `machine.mountPosition {x, y}`, machine coordinates; load checks
     it against the XY soft range. Web schema, loader, defaults, validator
     and blob follow; blob version bumped, fixtures regenerated.
  2. The stop: park every Z (legs), lock every head's A to 0 (legs, a new
     `lockAs()`; each A node homed and not excluded, as `zsReady`), a plain
     XY line to the mount position, TOOL_WAIT. A at 0 is the lock and zero
     unwind; the leg's target is absolute, so it unwinds the whole angle.
  3. Z set follows `mount`: `toolsMount` clears a head's Z set when its tool
     changes; `enterWait` no longer does. Only `wzero z` sets it.
  4. `mount` in TOOL_WAIT accepts any head.
  5. No automatic exit. At a header, the immediate tool (the header's):

     | Immediate tool | Result |
     |---|---|
     | Not mounted | `NACK 0x0F`; outside TOOL_WAIT the stop, in it stay |
     | Mounted, not zeroed | `NACK 0x0F`; outside TOOL_WAIT the stop, in it stay; ACKed after `wzero z` |
     | Mounted, zeroed | back to BUFFERING or PAUSED, then the normal path, ACKed |

     This replaces the fatal `NACK_JOB` (no Z set) and `NACK_BAD_STATE` (a
     header in TOOL_WAIT). A head switch alone stays as JB4 has it.
  6. `probe_park <tool>`: also admitted in TOOL_WAIT; the head is the one the
     tool is mounted on, else the selected one; selects it if needed.
  7. Docs: `docs/wire_protocol.md` (the table, `mount`, `probe_park`,
     `mountPosition`), the config doc, `toolchange.h`.
* Not in scope:
  * Auto-probe (`probe <tool>`, an implicit probe of an unzeroed tool):
    with M4 (machine-model.md).
  * The web Session's wait: with the Session branch (demo-console.md),
    resending by itself only while the Pico reports TOOL_CHANGE and on the
    operator's continue in TOOL_WAIT. Until then its 250 ms resend ACKs once
    the immediate tool is mounted and zeroed, as today.
  * The zero's surface (Open).

**Depends on:** JB4 (done), JX (done).

**Overlap:** `toolchange.*`, `data_plane.cpp`, `planner/queue.cpp`,
`ops/tools.cpp`, `cmd/jogto.cpp`, `control_plane.cpp`, `config_decode.*`,
`web/src/machine/schema.ts`, the config encoder.

### Status

Done (merged). Unblocks the web Session's tool-change wait (resend by
itself in TOOL_CHANGE, on the operator's word in TOOL_WAIT).

### Outcome

* Interfaces: `toolChangeWaiting()`, `toolChangeLeave(head, tool)` (replace
  `toolChangeMountCheck`); `headANodes`, `asReady`, `lockAs` in
  `seq/steps.*`; `framesNamed("mount")`, so `jogto mount` too. `toolsMount`
  unsets the head's work Z on a change of tool.
* Deviations: the mount position is `machine.positions.mount` beside `park`
  and `load` (an anchor position), not `mountPosition`; optional, so no blob
  version bump (absent: the change fails `seqfail=no_position`).
  `config/controller.jsonc` sets (200, 150). The lock leg runs at A's
  `homing.startFeed` up to its `jogFeed`.
* `probe_park` takes the selected head if it has the tool, else the first
  that has it, and checks that head's switch before selecting it (the bench
  found a tool on two heads switching away and failing).
* Bench (COM8, head 0): an unmounted header `NACK 0f`, Zs park, A −63.2° to
  0, the line to (200, 150), TOOL_WAIT; resent unmounted `NACK 0f`; `mount 0
  1` and `mount 1 2` there; resent before `wzero z` `NACK 0f`, after it ACK
  and BUFFERING. `mount 0 2` in IDLE then its header: the stop, `NACK 0f`
  until `wzero z`, then ACK. Mounted and zeroed: ACK, no stop. `probe_park 2`
  in TOOL_WAIT `no_position` (no switch), selection kept. Not tried: a change
  from a running or paused job, a real probe switch, a second moving head,
  `stop` in the stop, a refusal mid-stop.
* Checks: `pio run -e pico` clean; `pio test -e native` 176/176; `pnpm
  typecheck` clean; `pnpm test` 569 passed, 1 skipped.
  `pnpm test` ran before `positions.mount` went into
  `config/controller.jsonc`, so `controller.msgpack` was stale on merge;
  regenerated in `fix/controller-fixture`.
* Out of scope: no native or decoder test for the header logic or
  `positions.mount`; the web Session still resends every 250 ms.

## The head stays in the header

Decided after H1 was written and dropped (branch
`feature/pico-mount-head`, never merged, kept for the record). H1
had the mount state pick a layer's head and dropped the head byte from
`LAYER_HDR`.

* Against it: a tool's bbox (`TOOL_HDR`) can only be checked against a head
  at preflight if the header names that head. Without it the check needs the
  bbox kept until the mount, or a tool-to-head map kept from the headers;
  both are job state on the Pico, which JT removed.
* So the host picks the head, in `TOOL_HDR` and in every `LAYER_HDR`
  (Tool change, above). The operator mounts on the head the header names.

## Open

* Preflight refuses and never prepares (proposed, not confirmed).
* **The zero's surface:** `wzero z_bed`, `wzero z_switch` (needs a
  per-switch `probeSwitch.z` above the bed; the auto probe uses it) and
  `wzero z_material` (subtracts the layer's `t`). Which surface work Z 0
  means changes job-z.md's heights (bed favoured: a full cut ends at the bed
  whatever `t`'s error, and a zero made ahead needs no `t`).
  Resolved: the bed (coordinate-system.md, Work frame).
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
