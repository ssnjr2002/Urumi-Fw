# Session

A job on the controller ([jobs](../../system/jobs.md)): a `JobSource` holds
its frames, a `Session` sends them and follows the job until it closes.
[`Link.session()`](link.md#linksession) makes the session.

```ts
import { Link, JobSource } from "urumi-host/link";
```

**Checked against `280b897`.** To see what may have changed since:

```bash
git diff 280b897 -- web/src/link/session.ts
```

| Name | Kind |
|---|---|
| [`JobSource`](#jobsource) | class |
| [`Session`](#session-1) | class |
| [`SessionResult`](#sessionresult), [`Fatal`](#fatal) | types |

A session sends only inside a job: the controller refuses a record
(`NACK_JOB`) unless a job is open and its layer header taken.

# `JobSource`

A job's frames, built from its layers: `JOB_HDR`, one `TOOL_HDR` per
tool, then each layer's `LAYER_HDR`, `TOOL_HDR` and records. The session
also takes from it the frames that resume a job mid-layer.

## Constructor

```ts
new JobSource(layers: readonly JobLayer[], options?: { manual?: boolean })
```

**Parameters**

* `layers`: the layers, from [`buildJob()`](toolpath.md#buildjob).
* `options.manual`: hold each layer until [`release()`](#jobsourcerelease).
  Default false.

**Exceptions**

* `RangeError` if a tool is on two heads, or there are not 1 to
  `JOB_MAX_TOOLS` (4) tools.

**Example**

```ts
const source = new JobSource(job.layers, { manual: true });
```

## Properties

| Property | Type | |
|---|---|---|
| `jobId` | `number` | the id the job header carries: a CRC32 over the job's frames, so the same layers give the same id |
| `manual` | `boolean` | hold each layer until `release()`; may change mid-job |

## Methods

| Method | |
|---|---|
| [`release()`](#jobsourcerelease) | let the next layer go |

### `JobSource.release()`

In manual mode, lets the next layer go. The session's `hold` is
`"release"` while a layer waits for it.

**Syntax**

```ts
release(): void
```

**Return value**

None. Does nothing if every layer has gone.

# `Session`

A job running on the link. Made by
[`Link.session()`](link.md#linksession); nothing is sent until
[`run()`](#sessionrun).

## Properties

| Property | Type | |
|---|---|---|
| `hold` | `Hold \| null` | why the session isn't sending; `null` while it sends or has sent everything |
| `drained` | `boolean` | every frame is acknowledged; the machine may still be cutting |

```ts
type Hold = "place" | "tool" | "paused" | "refused" | "silent" | "release"
```

| Hold | Starts when | Leave by |
|---|---|---|
| `place` | the job's headers are taken; the controller is in `PLACE` | `place set`, then `place ok` ([commands.md](../commands.md)); the session sees `BUFFERING` |
| `tool` | a layer needs another tool (`NACK_TOOL_CHANGE`) | mount and zero once the controller is in `TOOL_WAIT`, then [`resume()`](#sessionresume) |
| `paused` | `NACK_PAUSED`, a [`pause()`](#sessionpause), or the status shows `PAUSED` | `resume()` |
| `refused` | a resume's layer header was refused (`NACK_JOB`) | `resume()` to retry |
| `silent` | no replies for 3 s | nothing: the session resends by itself, and turns to `paused` if the status shows it |
| `release` | manual mode: the next layer waits | [`source.release()`](#jobsourcerelease) |

Whatever the session isn't holding for, the status says:
`link.status.reason` is the controller's own view (`RUNNING`, `DWELL`,
`TOOL_CHANGE` …). The job is cut when the session is `drained` and the
status is `BUFFERING` with an empty buffer; it stays open until
[`close()`](#sessionclose).

## Methods

| Method | |
|---|---|
| [`run()`](#sessionrun) | send the job, or take over a paused one; resolve when it closes |
| [`resume()`](#sessionresume) | leave a `paused`, `refused` or `tool` hold |
| [`pause()`](#sessionpause) | pause the job |
| [`close()`](#sessionclose) | close a job at rest |
| [`abort()`](#sessionabort) | abort the job |
| [`stats()`](#sessionstats) | counts so far |

### `Session.run()`

Asks the controller first, then:

| The controller is | `run()` |
|---|---|
| idle | resets the seq and sends the job |
| in a job with the source's `jobId`, paused or on its way to the pause | holds `paused`; `resume()` carries on from where the controller stopped |
| in a job with another id | fails with `job-mismatch`, having sent nothing |

It follows the job through its holds and resolves only when the job
leaves the controller. If the link closes first, `run()` rejects with
"the link closed" and the job stays open on the controller, which pauses
it once the host has been silent for `HOST_TIMEOUT_MS`.

**Syntax**

```ts
run(): Promise<SessionResult>
```

**Return value**

A [`SessionResult`](#sessionresult). `ok` is true when the job closed by
`close()` or `abort()`; false on a fatal error, named by `fatal`.

**Example**

```ts
const session = link.session(new JobSource(job.layers));
const r = await session.run();
if (!r.ok) show(r.fatal!.by === "host" ? r.fatal!.why : nackName(r.fatal!.nack));
```

Coming back to a paused job after a reload: build the same layers again
and run a new session. The ids match, so it takes the job over.

```ts
const session = link.session(new JobSource(job.layers));
const done = session.run();   // holds "paused"
session.resume();
await done;
```

### `Session.resume()`

Leaves a hold: `paused` or `refused` from the status's resume point,
`tool` by resending the refused layer.

**Syntax**

```ts
resume(): boolean
```

**Return value**

False if the session isn't held `paused`, `refused` or `tool`, if the
controller isn't in `JOB` (a jog or probe inside the job), or if a `tool`
hold isn't yet in `TOOL_WAIT`.

### `Session.pause()`

Sends `pause`. On `ok` the session holds `paused` once the replies in
flight are in. A pause sent elsewhere is found too, by its NACK or the
status.

**Syntax**

```ts
pause(): Promise<string>
```

**Return value**

The reply line.

### `Session.close()`

Sends `cancel`, which closes a job at rest: a finished one, or one held
paused, placing or at a tool change. `run()` then resolves `ok`.

**Syntax**

```ts
close(): Promise<string>
```

**Return value**

The reply line. Refused while the machine moves; the session carries on,
and `abort()` is the way to stop.

### `Session.abort()`

Sends ABORT, as [`Link.abort()`](link.md#linkabort): brakes if moving,
lifts Z once, closes the job. `run()` then resolves `ok`.

**Syntax**

```ts
abort(): void
```

**Return value**

None. ABORT has no reply.

### `Session.stats()`

**Syntax**

```ts
stats(): SessionStats
```

**Return value**

| Field | |
|---|---|
| `sent` | frames written, resends included |
| `acked` | frames acknowledged |
| `nacks` | NACKs received |
| `retries` | resends |

# `SessionResult`

What [`run()`](#sessionrun) resolves to.

```ts
type SessionResult = SessionStats & {
    ok: boolean;
    fatal?: Fatal;                 // set when ok is false
    lastLayer?: number;            // the job's last layer and record seen,
    lastRecord?: number;           //   for a job that was lost
}
```

# `Fatal`

Why a session failed: the controller refused the job, or the host gave up.

```ts
type Fatal =
    | { by: "controller"; nack: number; detail: number }
    | { by: "host"; why: "stall" | "crc-limit" | "job-lost" | "job-mismatch" }
```

| `by` | Field | Means |
|---|---|---|
| `controller` | `nack` | the `NACK_*` byte, named by [`nackName()`](link.md#names): the job or the machine's state is wrong (`BAD_CURVE`, `SOFT_LIMIT`, `TOOL_UNKNOWN`, `NOT_HOMED`, `BAD_STATE` …) |
| | `detail` | the NACK's detail byte (`NACK_DETAIL_*`), 0 for most |
| `host` | `why: "stall"` | no reply for too long, outside a job hold |
| | `why: "crc-limit"` | too many `NACK_CRC`: the line is bad |
| | `why: "job-lost"` | the job left the controller without `close()` or `abort()`: a reboot, or a `cancel` from elsewhere while sending |
| | `why: "job-mismatch"` | the controller is in a job with another id |

None of these cross the wire; the host's reasons are the session's own.
