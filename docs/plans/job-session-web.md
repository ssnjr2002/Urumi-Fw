# Job session web

The job `Session` (`web/src/wire/link/session.ts`) and what the wire library
reads of the machine around it.

* E1–E2: from demo-console.md, Open, "Session abort and one resume". The
  demo's job buttons mirrored the Session API, which split what the operator
  sees as one action (Resume / Continue, Cancel / Abort).
* X1–X2: the wire library predates the firmware's contexts
  (`MachineContext`, `src/rp2350/ipc/shared_state.h:53`) and reads the job's
  life from the state machine. A jog in a job's hold reports JOGGING
  (`stateOf`, `core0/ops/machine.cpp:131`: a step outranks the job context),
  so the session takes the job as lost (E2 Outcome). The status gains the
  context and the library reads the job from it.

## Decisions (E)

* `Session.end()`, not `abort()`: it reads apart from `cancel()` and from the
  raw `link.abort()`.
* `continue()` is dropped, not aliased; `resume()` leaves every hold the
  operator can leave.
* `cancel()` stays: the soft stop, at rest only, no Z lift.

## Decisions (X)

* The status carries the context; the host does not infer it from the layer
  (`plannerExecPoint` sets the layer exactly while `CTX_JOB`, but that is a
  side effect, not a contract).
* The job session's rules, by context:
  * Alive: `context === JOB` on the session's own layer. A layer that moves
    to another while JOB is someone else's header: lost. The context opens
    with the first header and closes only on `cancel`, an abort at rest, an
    estop or a fault (`shared_state.h:55`); a new layer's header inside an
    open job keeps it open.
  * Held: while `state !== JOB`, or the job's reason is PAUSED, TOOL_CHANGE
    or TOOL_WAIT. A jog, `jogto` or traverse inside a job is a hold like any
    other.
  * `reason` is read only from a JOB sample (`stateReason`,
    `core0/status.cpp:19`, sends the jog's reason while JOGGING).
  * Leaving a hold needs a JOB sample; `resume()` returns false otherwise
    (refused, not queued, as in TOOL_CHANGE).
* Not in this plan: a boot id in the status (demo-console.md, Open, "Boot
  counter and uptime"). A rebooted Pico running a new job on the same layer
  still passes as the session's job.

## Survey (X)

What in `web/src/wire/` reads the job's life from the state:

* `link/session.ts`: `_ended` (`:584`), the status-PAUSED pause check
  (`:521`), the hold's layer check (`:618`), `_leave` (`:640-660`) and
  `resume()` (`:367`) read `state`/`reason` as if JOB were the context.
  The bug.
* `link/backends/sim.ts`: one `state` field stands for both. A cjog in JOB is
  `NACK_BUSY` (`:446-450`), so the sim cannot jog in a hold; `jobPaused()`
  (`:705`), `wzero`/`mount` (`:983-996`) and `cancel` (`:1001`) test
  `state === JOB`. The sim needs a context of its own, and a jog in a
  PAUSED or TOOL_WAIT job.
* `format/status.ts`: `MachineStatus` has no context; `parseStatusRsp`
  (`:514`) and `statusFromGet` (`:388`) to extend.
* `link/commands.ts`: `STATE_KEYS` (`:60`) has `job`, which the firmware
  sends only while STATE_JOB (`core0/cmd/get.cpp:189`).
* `link/settled.ts`: `atRest` (`:143`) is about motion, not the job: a jog in
  a job is moving and its ring is counted, so it reads right. `inState`,
  `inAnyState` and the fatal states are state questions. No change.
* `link/link.ts`, `link/demux.ts`, `link/writer.ts`: no state reads.

## Branches

E1–E2: a dependent chain, Single Session: E2 needs E1's API.
X1–X2: a dependent chain, Single Session: X2 needs X1's field and sim.

### E1: Session end and one resume

* Type: `feature/session-end`
* Purpose:
  * `end()`: sends ABORT (binary, `ABORT_MAGIC`), marks the end as asked for,
    and `run()` returns `true` once the status leaves JOB, not
    `FATAL_JOB_LOST`. ABORT brakes if moving and lifts Z once
    (`abortSettle`, `core0/ops/machine.cpp`). Modelled on `cancel()`
    (`web/src/wire/link/session.ts:326`).
  * `resume()` leaves `paused`, `refused` and `tool`; the run loop already
    picks the resend from `held` (`session.ts:349-363`, both methods set
    `_go`). `silent` still leaves by itself.
  * `continue()` removed.
* Files: `web/src/wire/link/session.ts`, `link/link.ts` (job session doc,
  `:324`; the binary path `end()` uses), tests in `web/test/wire/link/`
  (`job-session.test.ts:108,189,195` move to `resume()`; new `end()` tests).
* Depends on: H1 (demo-console.md, done).
* Status: done, merged at `d79f488`. Unblocks E2.
* Outcome:
  * Interfaces E2 uses: `session.end()` (void; ABORT has no reply),
    `session.resume()` for every hold the operator leaves. `continue()` is
    gone, so the demo's Continue button (`session[v]()`,
    `web/demo/index.js:632`) throws until E2.
  * Deviations: `link.ts` got a doc line only; `end()` writes ABORT through
    the session's own writer. Agreed in Read: `resume()` from a `tool` hold
    returns false until the status shows TOOL_WAIT.
  * `_cancelling` is `_ending`, set by `cancel()` and `end()`.
  * Tests: `job-session.test.ts`, 3 new (`end()` running, from a pause, from
    TOOL_CHANGE). Checks: `pnpm typecheck` clean; `pnpm test` 611 passed,
    1 skipped.
  * Not tried on the machine: `end()` mid-move (brake and Z lift) on COM8.

### E2: demo job buttons

* Type: `feature/demo-job-buttons`
* Purpose: the demo's job buttons become Pause, Resume, Cancel, End job.
  Continue folds into Resume; Abort (`link.abort()` then `truncate()`,
  `web/demo/index.js:626`) becomes End job, `session.end()`.
  * Two rows. "Machine job: [Pause] [Cancel]" acts on whatever job the
    machine has: through the session while one runs (`session.pause()`,
    `session.cancel()`), else the text `pause` / `cancel`, so a job this
    page didn't start can still be paused and cancelled.
  * "Session:" names the running layer (layer, tool, head, chamber) or
    "none", then the job line, then [Execute layer] [Resume] [End job],
    which need the session (Execute: none running).
  * Agreed in Read: the status table's `tool` row reads
    `MachineStatus.tools` (STATUS_RSP 0xA3), one id per head.
* Files: `web/demo/index.js`, `web/demo/index.html`.
* Depends on: E1.
* Status: done, merged at `4ebecec`. Last branch of the plan; unblocks
  none.
* Outcome:
  * As planned, plus from the bench: `resume()` returning true only means
    the session resends, so the Resume line ends with what came of it
    (accepted, refused, no reply yet: the first ACK or NACK within 2 s), and
    each change of hold is logged with a hint (`held: tool — mount and
    zero, then Resume`).
  * The done line reads "Cancel or End job closes it".
  * Checks: `pnpm typecheck` clean; `pnpm test` 614 passed, 1 skipped (the
    demo has no tests). Benched: Resume refused in TOOL_WAIT; not yet
    tried: End job, the Resume line's new wording, `pause`/`cancel` on a
    job the page didn't start.
  * Found on the bench, out of scope (a library `fix/`, not planned yet): a
    jog in a job's hold (TOOL_WAIT, PAUSED) ends the session with
    `FATAL_JOB_LOST`. The firmware reports a jog inside a job as JOGGING
    (`stateOf`, `core0/ops/machine.cpp:131`: a step outranks the job
    context), and `Session._ended` (`session.ts:584`) takes any state but
    JOB as the job gone. The status's layer is set exactly while the context
    is JOB (`plannerExecPoint`), so `_ended` can treat a layer other than
    `NO_LAYER` as the job still open.

### X1: context in the status

* Type: `feature/status-context`
* Purpose: the machine's context on both planes, and a sim that has one.
  * Pico: STATUS_RSP 0xA0, 38 bytes (0xA4 is a retired layout): a `context`
    byte (`MachineContext`), read under `plannerLock` with the layer and
    record (`plannerExecPoint` returns it) so one sample cannot disagree with
    itself. `get ctx` on the text plane. `get job` reports the
    job's reason whenever the context is JOB, not only in STATE_JOB.
  * Web: `MachineContext` enum, `MachineStatus.context` from
    `parseStatusRsp` and `statusFromGet` (`ctx` added to `STATE_KEYS`);
    `MAGIC_STATUS_RSP`, `STATUS_RSP_SIZE`; 0xA3 retired like the older
    layouts, with a reflash error.
  * Sim: a `context` beside `state`, opened by the first header and closed
    by `cancel`, ABORT and faults, as on the Pico. A cjog in a TOOL_WAIT
    job is admitted (`K_STEP | K_IN_TOOL`, `ops/cjog.cpp:35`): state
    JOGGING, context JOB, layer kept; the jog's end returns to JOB with the
    job's reason. In a PAUSED job it is refused BAD_STATE, as the Pico does.
* Files: `src/rp2350/core0/usb_protocol.h:85-113`, `core0/status.cpp`,
  `core0/cmd/get.cpp`; `web/src/wire/format/{constants,status}.ts`,
  `wire/link/commands.ts`, `wire/link/backends/sim.ts`, `web/src/index.ts`;
  tests in `web/test/wire/`; `docs/wire_protocol.md` (STATUS_RSP, `get`).
* Depends on: none.
* Checks: `pio run -e pico`; `pnpm typecheck`, `pnpm test`. Human scope:
  the 0xA0 frame and `get ctx` on COM8, in and out of a job.
* Status: done, merged at `186e60e`. Unblocks X2.
* Outcome:
  * Agreed in Read/Write: 0xA0, not 0xA4; 0xA3 retired as `STATUS_RSP_V4`.
    A cjog in a job: TOOL_WAIT only, from the bench (below).
  * Interfaces X2 uses: `MachineStatus.context` (`MachineContext`, both
    planes; `undefined` from a `get` without `ctx`); `get job` answers
    whenever `ctx=1`. `plannerExecPoint` takes a `ctx` out-parameter.
  * Sim: the status layer is set whenever the context is JOB (the resume
    point while paused, else the next record), as `plannerExecPoint`. A cjog
    in a paused job is BAD_STATE, not BUSY. No `jogto` in the sim.
  * Benched on COM8: the frame at rest (ctx 0, layer 0xFFFF) and in a job
    (header → ctx 1 layer 3 record 40; pause → reason 3, same layer; cancel
    → ctx 0, 0xFFFF), `get ctx job` alike; a cjog in PAUSED `b706`. Not
    benched: a jog in TOOL_WAIT on the machine.
  * Findings, out of scope:
    * `home` runs with ctx 0 throughout; only a manual `leg` opens
      CTX_HOMING (`ops/homing.cpp:93`).
    * On the text plane, `reason` is the job's while a jog runs inside it
      (state JOGGING); X2 reads `reason` only from JOB samples.
    * `wire_protocol.md`: `liftreason`, `liftcount`, `toolon` still say
      "only while state=JOB"; not checked against `get.cpp`.
  * Checks: `pio run -e pico` clean; `pnpm typecheck` clean; `pnpm test`
    619 passed, 1 skipped.

### X2: the job session reads the context

* Type: `fix/job-session-context`
* Purpose: the session's rules from Decisions (X): alive by context and own
  layer, held while not JOB or in a job hold reason, `reason` and leaving
  only from JOB samples, `resume()` refused otherwise.
  * A test that fails before the fix: a jog in a TOOL_WAIT hold leaves the
    session held and alive, and `resume()` after it resumes. (The Pico
    refuses a jog in PAUSED.)
  * Also: `resume()` during the jog returns false; `cancel` from elsewhere
    and a header for another layer are still `FATAL_JOB_LOST`.
* Files: `web/src/wire/link/session.ts`; `web/test/wire/link/job-session.test.ts`.
* Depends on: X1.
* Checks: `pnpm typecheck`, `pnpm test`. Human scope, on COM8 from the demo:
  a jog in TOOL_WAIT, then Resume; a jog in PAUSED is refused `b706` and the
  session stays paused.
* Status: done, merged at `3bcd836`. Last branch of the plan; unblocks
  none.
* Outcome:
  * As planned; no new hold kind. A jog or probe inside a hold keeps the
    hold it is in; the tool reads `state` if it wants to name the motion.
  * Found in Read: `_leave` from a `tool` hold resent the window on a jog's
    sample (its reason is neither TOOL_CHANGE nor TOOL_WAIT); the hold now
    skips non-JOB samples. `resume()` refuses any hold outside a JOB sample;
    the demo already logs the refusal (`resume: not yet (…)`).
  * The layer check stays off for `tool` holds: the refused header's layer
    is not yet the machine's, so the two differ legitimately.
  * `context === undefined` (a `get` without `ctx`) reads as not known: not
    lost, not alive.
  * Not tested: a header for another layer during a hold (the sim has no
    easy way to send one into a held job); the cancel-from-elsewhere test
    still passes as `FATAL_JOB_LOST`.
  * Out of scope: the demo reads the job from `state` too.
    `showJob()` (`web/demo/index.js:576`) shows "records 0/N" during a jog
    in a job; the status table's `reason` row (`:186`) shows the jog's raw
    reason.
  * Checks: `pnpm typecheck` clean; `pnpm test` 620 passed, 1 skipped. Not
    benched.

Not in this plan: the demo's status table showing the context (the demo UI
session's, after X1).
