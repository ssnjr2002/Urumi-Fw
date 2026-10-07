# Motion sessions on the Pico

How the Pico runs jobs and jogs: their states, how a job starts, pauses,
resumes and ends, and how jogs and offsets work. Follows
docs/plans/state-handling.md (sessions, alarm exits) and
docs/plans/pico-planner.md (the ring, `BEZIER` records). Replaces
state-handling branch 5 ("motion gating, no jobs without homing") and planner
branch 3 (`feature/pico-jog`).

Jogging is settled and has branches (J1, J2). Jobs are still a draft:
preflight and the record layouts are open (below), and they have no branches yet.

## Decisions

### States

* **`STATE_JOB` with a reason byte**, like `STATE_HOMING` and `STATE_PROBING`
  (`ProbingReason`). Every session has the same shape: IDLE between sessions,
  the session owns the machine, ALARM ends it.
* **Job reasons:** `BUFFERING`, `RUNNING`, `HOLDING`, `PAUSED`, `ABORTING`.
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

### Job header and `TOOL` record

* **The job header is for preflight only:** job id, record count, the tools
  used, each with the bbox of its geometry. Sent at job start and at a
  restart.
* **No `config_crc32` and no `required_axes`.** The host no longer models the
  machine; the Pico infers axes and peripherals from each tool's profile.
* **The `TOOL` record starts every run of motion in a job:** tool id, the
  record index it applies from, an optional feed override. It selects the
  tool (as `select`, below), then records follow. Sent at each layer change
  and at every resume or restart:

  ```
  job start          header + TOOL(0) + records 0…
  resume (paused)             TOOL(N) + records N…
  restart (alarm,    header + TOOL(N) + records N…
   power loss)
  ```

* The first record after a `TOOL` is treated as `START`: the Pico travels to
  it (in air; lift and plunge once tool profiles exist).
* The Pico keeps nothing about tools that must survive a power loss; the host
  holds the job and can send the right `TOOL` for any index.

### Buffering (jobs only)

* `BUFFERING` is entered at job start, at resume, and when the ring runs dry
  mid-job (the planner has already braked to rest at the last block). A late
  block adoption (Core 1 needed a block before Core 0 committed it) is a dry
  ring too: hold to rest, `BUFFERING`, then on automatically. `PAUSED` is
  only ever the operator's pause. Jogs have no buffering.
* Leaves for `RUNNING` on the first of:
  * the ring holds enough path to reach full feed (queued distance ≥ braking
    distance at the job feed; a block count as a simpler stand-in);
  * the job's last record has arrived (a short job never fills the ring);
  * a timeout (Klipper: 250 ms), so a slow host still progresses.
* Threshold and timeout are config fields.

### Pause drains the ring

* `pause`: `HOLDING` (brake along the path, existing hold), then at rest
  `PAUSED`: the ring is dropped and the resume point `{index, s}` recorded
  (the block that was running and how far into it).
* With the ring empty, a jog can use it while paused. No second ring, no
  set-aside.
* **Contour framing resets on pause**, as on abort.
* **Resume is a `TOOL` record**, not a text command: in `PAUSED` only a
  `TOOL` whose index matches the resume point is accepted, and every other
  record is NACKed, so a stray record cannot restart motion. Then
  `BUFFERING`.
* The resumed block is re-cut from its start. Resuming at `s − margin` (trim
  by `s0`, plunge mid-block) comes with Z, A and tool profiles; the resume
  point already keeps `s`.
* `select` and `apply_offset` are allowed while paused (e.g. to jog another
  head); resume re-selects through its `TOOL`.
* Planner branch 2's in-place resume (`restartFrom`, `err moved`) is unused on
  this path; it stays in `lib/planner` for now.

### Record index and identity

* **Ring blocks carry an absolute record index**, counted from the job start
  (u32). The wire seq is one byte and wraps every 256 records; it stays for
  the ACK window only. `status` reports the executing index.
* **No CRC is kept per block.** The CRC guards transit; a resend is checked
  again on arrival.
* **A job id** identifies the job; a resume or restart must match it, so the
  wrong file cannot be resumed.

### Restart after an alarm

* Once the alarm is cleared and the machine re-homed, the job can restart
  from the recorded index: header with the same job id, `TOOL(N)`, records
  from N.

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
* The `TOOL` record's selection goes through `select`.
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

A dependent chain, one session: J1, then J1b and J2 in either order. The job branches come later
and get their own sections.

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

Done, ready to merge.

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
* Depends on: J1.
* What the code has today: stored positions are decoded
  (`core0/config/config_decode.h:78`, `:95`) but nothing resolves them.
  `homingParkBegin` (`core0/ops/homing.h:24`) arms a park leg in the homing
  session; `controller/seq/home.cpp` parks homed nodes that way (`JOB_PARK`).
* Scope:
  1. A resolver in `core0/ops/frames`: a name to the anchor's machine XY
     (`park` defaulting to where homing parks, `load`, `probe` as the
     selected head's `probeSwitch` minus its offset). Missing: `err
     no_position`.
  2. `controller/seq/jogto.cpp`: a homing session with one park leg per
     head's Z node (in parallel, `homingHold` keeping raw legs out), then the
     session closed and the XY line queued as a J1 jog. HOMING → IDLE →
     JOGGING → IDLE; the gap is the sequencer's (`err busy`). A failed park
     alarms as `ALARM_HOMING_FAIL` (`LEGFAIL_PARK`). `stop` mid-sequence
     aborts the legs.
  3. Docs: `docs/wire_protocol.md`.
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
  1. Packet: magic, direction per axis (X, Y: −1/0/+1), speed fraction (u8,
     of `jogFeed`), CRC; and a one-byte stop. Both are received beside the
     existing magics (`core0/data_plane.cpp:326`).
  2. The first packet queues a line to the soft-range end in that direction,
     or `maxTravel` from the start when unhomed or with `softLimits` off.
     Diagonals are allowed. Repeats renew a timer; 150 ms without one holds
     and discards (`abortRequested`), checked in `dataPlaneTick`. All-zero
     directions or the stop byte stop at once. A direction change stops,
     then queues a new line once at rest.
  3. A step jog during a continuous jog is refused. A continuous packet
     while step jogs run stops them first, the same as a direction change.
  4. Web: a `wire/` encoder for the packet, and a sim that answers it.
     The operator UI port stays deferred.
  5. Docs: `docs/wire_protocol.md`.
* Overlap: `web/src/wire/`.
* Checks: as J1. Human checks: hold and release on X, Y and a diagonal;
  pulling the cable mid-jog stops within about 150 ms plus braking; running
  into the soft limit stops there.

### Status

Not started.

## Open

* **Record layouts:** the header and `TOOL` byte layouts, magics; tool ids
  (config indices or names); per-tool bbox vs relying on the per-record
  soft-limit check alone.
* **Soft limits per record:** each `BEZIER`'s control points checked at
  ingest (a Bézier lies inside their hull), plus the Pico's own travels. The
  range and its check come from `feature/pico-frames`.
* **Preflight:** refuses and never prepares (proposed, not confirmed); its
  check list; `NACK_PREFLIGHT` plus a text `preflight` listing failures.
* `cancel` from `PAUSED`: ends the session to IDLE at once (proposed).
* A dry ring with no last record: wait indefinitely in `BUFFERING`, the
  operator cancels (proposed).
* Core 1 becomes a writer of the job reason (hold → `PAUSED`, dry →
  `BUFFERING`) and of the jog's return state, beside the `alarmReason`
  two-writer note.
* Order: state-handling branches 3 and 4 first; motion gating (planner pushes
  refuse unbound, fenced or unhomed axes) before the job branches.
