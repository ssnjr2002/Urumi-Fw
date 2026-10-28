# Session end and one resume

From demo-console.md, Open, "Session abort and one resume". The demo's job
buttons mirror the Session API, which splits what the operator sees as one
action (Resume / Continue, Cancel / Abort).

## Decisions

* `Session.end()`, not `abort()`: it reads apart from `cancel()` and from the
  raw `link.abort()`.
* `continue()` is dropped, not aliased; `resume()` leaves every hold the
  operator can leave.
* `cancel()` stays: the soft stop, at rest only, no Z lift.

## Branches

A dependent chain, Single Session: E2 needs E1's API.

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
* Status: planned.

### E2: demo job buttons

* Type: `feature/demo-job-buttons`
* Purpose: the demo's job buttons become Pause, Resume, Cancel, End job.
  Continue folds into Resume; Abort (`link.abort()` then `truncate()`,
  `web/demo/index.js:626`) becomes End job, `session.end()`.
* Files: `web/demo/index.js`, `web/demo/index.html`.
* Depends on: E1.
* Status: planned.
