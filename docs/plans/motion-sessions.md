# Motion sessions on the Pico

How the Pico runs jobs and jogs: their states, how a job starts, pauses,
resumes and ends, and how jogs and offsets work. Follows
docs/plans/state-handling.md (sessions, alarm exits) and
docs/plans/pico-planner.md (the ring, `BEZIER` records). Replaces
state-handling branch 5 ("motion gating, no jobs without homing") and planner
branch 3 (`feature/pico-jog`).

Draft: decisions settled so far. Preflight and the record layouts are still
open (below); no branches yet.

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
  mid-job (the planner has already braked to rest at the last block). Jogs
  have no buffering.
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
  `jogFeedUnhomed`, and a continuous jog runs at most `maxTravel`.
* Every jog needs its axes bound and enabled; none run in ALARM (leaving
  `LIMIT_LATCHED` is the homing session's reverse leg).
* **Soft limit:** a step jog or `jogto` that would cross it is refused,
  `err soft_limit <axis> <mm left>`, never clamped. A continuous jog runs to
  the limit and stops there.
* **Clicks append:** a step jog while one runs is queued behind it and the
  planner joins them, up to a small cap. A step jog during a continuous jog
  is refused.
* **Speed:** per-axis `jogFeed` and `jogFeedUnhomed` in the config; the step
  jog's `[feed]` and the continuous packet's speed fraction (u8, of
  `jogFeed`) scale it, clamped to `maxFeed`. Acceleration is the axis
  `maxAccel`.
* **Deadman:** the host repeats every 50 ms; the Pico holds after 150 ms
  without a packet (the ordinary hold, then discard).
* **Diagonal** continuous jogs (X and Y together) are allowed; speed is along
  the path.

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
