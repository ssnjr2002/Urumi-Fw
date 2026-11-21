# Host watch: pause a dry job when the host is gone

**Built on `feature/host-watch`.** Fixes the gap recorded in [system/jobs.md](../system/jobs.md) §6
and §14: when the host goes away mid-job, the queue runs dry and the job
waits in `BUFFERING` with the tool down in the material, still on.

## Today

* A dry queue drops the job to `BUFFERING` (`leaveDry()` in
  `core1/emit/follower.cpp`). The machine stops where the records ran out and
  doesn't lift, so a slow host leaves no mark in the cut.
* The controller can't tell a slow host from a lost one. Nothing on it notices
  the host: `STATUS_REQ` is answered in `processSerial()` (`core0.cpp`) and
  that's all, and DTR isn't read.
* While a job session runs, the host sends `STATUS_REQ` every 100 ms
  (`STATUS_POLL_MS`, `web/src/link/session.ts`). Outside one it doesn't poll.
* A pause already does what a lost host needs. It records a resume point
  (`landHeld()`), drops the queue, lifts, and switches the tool off
  (`held()` in `ops/observe.cpp`). The host resumes by resending the layer
  header at that point (`_restart()` in `session.ts`).

## Decisions

* **A lost host pauses the job; it doesn't end it.** Once the queue is dry and
  the host is lost, the job lands `PAUSED` exactly as an operator pause at
  rest does. The host can come back and resume.
* **It acts only on a dry queue.** A host lost with motion still queued cuts
  what it has, then pauses. A host that's merely slow is untouched, so the
  no-lift rule holds for it.
* **The host is lost** when, with a job open, either:
  * nothing has arrived from it for the **host timeout**, where any byte
    counts, not just `STATUS_REQ`; or
  * DTR drops (the port was closed, or the cable pulled). This is instant,
    and catches the common cases. Only the timeout catches a frozen tab with
    the port still open.
* **The timeout is a fixed time, not a ratio of the poll.** A tab in the
  background throttles its timers to about 1 s or worse, and a false timeout
  lifts the tool and marks the cut. Proposed default: 3 s against the 100 ms
  poll.
* **The timer is armed only while a job is open.** That's the only time the
  host polls steadily, and the only time a lost host matters.
* **The resume point is where the records ran out.** `landHeld()` today takes
  it from the block being cut, or the oldest one queued. On a dry queue there
  is neither, so `jobResume.known` would be false. Core 1 keeps the last
  block it finished, and a dry pause resumes at the end of that record. The
  resend restarts 2 mm before that point with a plunge, as any resume does,
  then carries on with the next record.
* **The host resumes after reconnecting.** That replaces "A lost job stops the
  session" (jobs.md §14) for this case only. A controller that is no longer
  in the job, for example after a reboot, is still a lost job.

* **The timeout is a firmware constant,** `HOST_TIMEOUT_MS`, until it needs
  tuning. It isn't a config field.
* **`STATUS_RSP` is unchanged.** Text `get` keys show the watch, for the
  console and for bench checks, in the style of `liftreason`:
  * `host`: 1 when the host is heard, 0 when lost (`-` with no job open);
  * `pausereason`: why the job last paused, 0 for the operator and 1 for a
    lost host.
* **For now a lost host only acts on a dry queue (`BUFFERING`).** The other
  job stages are left as they are; revisit them if one turns out to need it.

## Bench

* **DTR drops on close, on Windows.** `Serial.dtr()` under arduino-pico reads
  it. Checked with a `dtrdrops` counter: every close of the port counted one
  fall, from pyserial and from WebSerial in Chrome (Disconnect, then
  Connect), and from pulling the USB cable with the Pico powered from
  elsewhere. The count survives `reset`.

## Branches

1. **`feature/host-watch`** (firmware and docs):
   * a last-heard timestamp in `processSerial()`, plus the DTR check;
   * a host-lost flag that Core 1 reads in `enter()`, so `BUFFERING` plus
     host lost calls `landHeld()`, the same path as `pauseRequested`;
   * the last finished block kept for the dry resume point;
   * the `get` keys `host`, `pausereason` and `dtrdrops`;
   * the `JOB_HDR` job id kept and reported as `get jobid`;
   * jobs.md §5, §6 and §14 updated; a sim test if the sim models the dry
     queue.
2. **Branch 6, `feature/web-link`** (host): a session can attach to a job
   that is `PAUSED` on one of its layers, after the link reconnects, and
   resume it through the existing `_restart()`. The poll keeps running from
   a timer that survives a background tab.

## Outcome

Built and benched on the machine, with the pen on head 0 and no tool fitted:
a 400-record zigzag at 40 mm/s, with the host cut off about 5 s in.

* **Port closed (DTR):** the controller cut the ~45 s already queued, ran dry
  mid-contour, lifted Z by 2 mm, and landed `PAUSED` with `pausereason=1`
  and the resume point at record 63.
* **Host frozen with the port open (timeout):** the same, about 3 s after the
  queue ran dry.
* `get host` read the host as lost in the same pass as the `get` that asked,
  because the tick runs before the bytes. Hearing a byte now clears the loss
  unless DTR is down.

* `hostms` was dropped: asking for it is itself a byte from the host, so it
  always reads 0.
* The silence counts from the later of the last byte and the job's opening,
  so a job opened after a long idle isn't lost at once.
* The dry resume point: Core 1 keeps the end (`sEnd`) of the last curve it
  claimed in `jobExec`, and `landHeld()` uses it when the ring is empty.
* **Job id:** the controller keeps the `JOB_HDR` id and reports it as
  `get jobid`. The host is to set it to a CRC32 over the job's headers and
  records, without seq and CRC8 (branch 6), and to resume a paused job only
  when the ids match.
* **Power loss:** the job is lost and not recovered; see jobs.md §14.
* Seen on the bench for branch 6: a session that was frozen through the
  pause comes back sending into it (88 NACKs and 87 retries before the
  cancel). Branch 6 should treat a `PAUSED` it didn't ask for as a hold,
  then resume after reconnecting.
