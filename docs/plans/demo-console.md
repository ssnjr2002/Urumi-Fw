# Demo console: a plain host page over the wire library

A test page that calls the wire library (`web/src/wire/`) and nothing above
it. It is a harness for developing the library and a demo of the machine's
basic capabilities. No styling, no preview canvas, no planner UI.

The old demos (`web/demo/`: `barebones`, `bench`, `comms`, `orchestrate`,
`index.html` + `main.js`, `bench.json`, `comms.json`, `config.json`) are
archived at tag `archive/demos-2026-10` and deleted here. Read one with
`git show archive/demos-2026-10:web/demo/<file>`. `demo.svg` stays.

## Decisions

* **One page**, plain HTML. Each panel
  calls `Link` (`web/src/wire/link/link.ts`) or `commands.ts` directly.
* **Backend:** WebSerial only. The sim backend (`link/backends/sim.ts`) is out of
  scope for now; a backend picker can add it later.
* **Text plane is a serial console:** an input box, Enter to send, and ↑/↓ to
  step through history. One reply per command, the same as `Link.command`
  (`link.ts:145`). The `textDesyncs` count is shown, so a firmware reply with
  extra lines is visible. `stop` goes through `Link.send` (`link.ts:165`), so it
  never waits behind a pending command. No helper buttons yet.
* **Logs, one per plane:** on-page `<pre>`s with a timestamp and direction per
  line, plus Clear and Copy. The text log is the console's output (C1). The
  binary log sits under the status panel (C2); every binary call writes to it
  (magic name, ACK/NACK name), so C3–C5 add no log UI. A status poll only
  updates the table; it never writes to the log.
* **Library resolution:** the page imports `urumi-host` as a third party
  would. `pnpm demo` aliases it to `src/` through `web/demo/vite.config.js`,
  so wire edits show on reload; `pnpm demo:dist` uses the built package
  (`pnpm build` first).
* **Page:** `web/demo/index.html` + `index.js`.

## Branches

A dependent chain, Single Session: every branch after C1 edits
`index.html`/`index.js`.

### C1: console and log

* Type: `feature/demo-console`
* Purpose: delete the old demos; add the page with Connect/Disconnect and
  the text console with its log; both demo modes.
* Files: `web/demo/*` (delete), `web/demo/index.{html,js}`,
  `web/demo/vite.config.js`, `web/package.json` (scripts).
* Depends on: none.
* Status: done, merged at `63c5266`. Unblocks C2–C5.
* Outcome:
  * Bench-checked against the Pico: connect, commands, `stop`, history.
  * Out of scope: `reset` replies `ok`, then the soft reset prints the boot
    banner and `ready` (`core0.cpp:156,162`). No command awaits them, so the
    next `Link.command` drops them into `textDesyncs` and the console never
    shows them. Any host sending `reset` hits this; a fix is a wire API
    (e.g. `Link.reset()` reading to `ready` under the text lock).
  * Out of scope: `Link.send` (`link.ts:165`) assumes `stop` has no reply,
    but `cmdStop` prints `ok` (`lifecycle.cpp:29`), so each `stop` leaves
    one orphan line for the next command to drop. The console's desync count
    therefore rises by 2 per `reset` and 1 per `stop`. Either the firmware
    stops replying to `stop`, or the wire library expects the orphan.

### C2: status

* Type: `feature/demo-status`
* Purpose: `getStatus` (`link.ts:175`) on demand and as a poll at a set
  rate, decoded `MachineStatus` as a key/value table; the binary log below
  it.
* Files: `web/demo/index.{html,js}`.
* Depends on: C1.
* Status: done, merged at `409d74e`.
* Outcome:
  * Bench-checked against the Pico: Get status, poll, table.
  * The panel also shows the time of the last good reply; log times are
    local.

### C3: jog and frames

* Type: `feature/demo-jog`
* Purpose: key jog (`cjog`/`cjogStop`, `link.ts:207`) and the head/anchor
  select, carried over from the archived `barebones.js`.
* Files: `web/demo/index.{html,js}`.
* Depends on: C1.
* Status: done, merged at `45f50d6`.
* Outcome:
  * Bench-checked against the machine: Jog to.
  * Added in the branch: a Jog to row (`jogto <x> <y>`, `park`, `load`).
    `jogto` is XY only; Z and A have only the relative `jog`.
  * Select shows the current selection (`Selected:`, from `ok` and every
    status sample) above 0 | 1 | anchor buttons. The poll starts on connect.
  * Cjog ACK/NACKs share the ack sink with a stream `Session`; the page reads
    it while jogging. C5 must keep jogging off while a job streams, even where
    the firmware would NACK the jog.
  * For C4: the select buttons are fixed. They should follow the config's
    heads. The firmware accepts `select 1` because head 1 is configured,
    though its Z and A are `present: false`; whether `select` should refuse
    a head that cannot move is a firmware question.

### C4: config

* Type: `feature/demo-config`
* Purpose: push a config file and pull the stored one (`pushConfig`/`pullConfig`,
  `link.ts:227,247`); a failure shows the CFG_NACK name
  (`format/cfg.ts`). The select buttons follow the pulled config's heads.
* Files: `web/demo/index.{html,js}`, `web/src/index.ts` (one export).
* Depends on: C1.
* Status: done, merged at `78848e7`. Unblocks C5.
* Outcome:
  * Bench-checked: loading `config/controller.jsonc` and its warnings.
  * Pushes `.json` or `.jsonc`: `stripJsonComments` is now exported from
    `urumi-host`, and the page runs `loadConfig` (parse and validate) before
    `encodeConfigBlob`.
  * Pull runs on connect and after each push; the select buttons are one per
    `machine.heads` entry plus anchor, anchor alone with no stored config.
  * No download: a pulled blob decodes to the resolved model, not the file
    format. `loadConfig` (`load.ts:402`) nests `heads`, `defaultHead` and
    `peripherals` under `machine`, fills every default and derives
    `toolProfiles` from `tools`, so a downloaded model fails `loadConfig`
    (`heads: required`). msgpack itself round-trips exactly.
  * Follow-ups found here: see Open, config refactors.

### F1: cjog replies on their own magics

* Type: `fix/cjog-reply-routing`
* Purpose: a cjog's ACK and NACK are the stream's frames (`cjogReply`,
  `core0/data_plane.cpp:347`: `[0xAA, expectedSeq, 0]` and `sendNack`), so
  the demux (`link/demux.ts:249`) puts them in `sinks.ack` and a `Session`
  reads them as its own. A jog in TOOL_WAIT or PAUSED, which the firmware
  admits, then rewinds or ends the stream. Cjog replies get their own magic
  pair (picked from the free magics in the branch; the NACK's byte 2 stays
  reserved for preflight detail), routed to a new `sinks.jog` that
  `Link.cjog` reads. `sinks.ack` is the stream's only.
* Files: `core0/data_plane.cpp`, `core0/usb_protocol.h`;
  `web/src/wire/format/constants.ts`, `link/demux.ts`, `link/link.ts`,
  `link/backends/sim.ts`, tests; `docs/wire_protocol.md`;
  `web/demo/index.js` (`drainAcks` reads `sinks.jog`).
* Test that fails before: a cjog NACK from the demux must not reach
  `sinks.ack`.
* Depends on: C3.
* Status: done, merged at `e994a50`. Unblocks C5.
* Outcome:
  * Frames: `JOG_ACK` `[0xB6]`, `JOG_NACK` `[0xB7, reason]` with the stream's
    NACK codes; no seq. A cjog with a bad CRC is a `JOG_NACK` too.
  * Interfaces C5 uses: `sinks.jog` (`JogAck | Nack`), `JogAck`,
    `MAGIC_JOG_ACK`/`MAGIC_JOG_NACK`. The demo's `drainJog` reads it.
  * Bench (COM8, homed): a start gives one `b6`, a held jog of 8 packets one
    `b6`; refusals `b7 01`, `b7 06`, `b7 09`, `b7 0a`; `SEQRESET` still
    `aa 00 00`. Not tried: a jog during a live job (C5).
  * Out of scope: once, a repeat 100 ms after a start also got a `b6`; not
    reproduced. F1 changes the reply's bytes only, not when it is sent.
  * Found here: the Session ends on most NACKs (Open, Session NACK tolerance).

### H1: Session hold

* Type: `feature/session-hold`
* Purpose: a job `Session` that lives until the host ends the job and holds,
  sending nothing, while the machine is not taking records. Today it ends on
  `NACK_PAUSED`, ends at the last ACK, and resends a header every 250 ms on
  `NACK_TOOL_CHANGE` (`web/src/wire/link/session.ts:310-330`).
  * Life: `run()` returns only on `cancel()` (sends `cancel`; returns once
    the status leaves JOB), `truncate()` (with `link.abort()`), a job fault
    or a lost job. The last ACK does not end it: a pause can come after.
  * Status: the Session polls STATUS_REQ through the whole job; the status
    sink is a latest-value sink, so every reader sees its samples.
  * Holds, each sending nothing and polling status:

    | Hold | Entered by | Left by |
    |---|---|---|
    | `paused` | `NACK_PAUSED`, or status PAUSED | `resume()` |
    | `tool` | `NACK_TOOL_CHANGE` | itself while TOOL_CHANGE; `continue()` in TOOL_WAIT |
    | `refused` | `NACK_JOB` on a resume header | `resume()` again, or `cancel()` |
    | `silent` | no reply past the stall timeout | the next status, if the job is still there |

    Leaving resends: a resume or `continue()` sends SEQRESET (its ACK read on
    the ack sink) and the header at the status's `layer`/`record`, then the
    records from there; `silent` goes back to base. The stall clock restarts
    on leaving any hold.
  * Lost job: in any hold, a status that is not JOB on the Session's layer
    ends `run()` with `FATAL_JOB_LOST`, and nothing is resent. A rebooted
    Pico would ACK the resends as stale duplicates without running them.
  * Still fatal: `SOFT_LIMIT`, `BAD_CURVE`, `TOOL_UNKNOWN`, `BAD_MAGIC`,
    `BAD_STATE`, `FATAL_CRC_LIMIT`.
  * Source: `ReplayableSource.from(layer, record)` gives the packets from a
    point, header first; the Session never indexes records itself, so a
    streamed job file can be one later. `JobSource` (`LayerHdr` fields and
    the layer's BEZIER packets, record n = packet n) is the in-memory one.
* Files: `web/src/wire/link/session.ts`, `link/link.ts` (builds a job
  Session), `link/backends/sim.ts` (a tool-change hold; it has none), tests
  in `web/test/wire/link/`; `web/src/index.ts` exports.
* Depends on: F1; JT (motion-sessions.md, done).
* Status: done, merged at `a2b8077`. Unblocks C5.
* Outcome:
  * Interfaces C5 uses: `link.session(new JobSource(hdr, packets))` makes a
    job session (a plain `PacketSource` keeps the old behaviour, so jog
    sessions and `link.stream()` are unchanged); `pause()`, `cancel()` (both
    send the text command and resolve to the reply), `resume()`,
    `continue()`, `held`, `stats().held`, `FATAL_JOB_LOST`. Exported from
    `web/src/index.ts` with `Hold` and `ReplayableSource`.
  * Added in Write: `pause()`, so the demo's pause goes through the session;
    a pause sent elsewhere is still found by its NACK or the status.
  * Deviations: leaving a `tool` hold resends from the window's base (the
    unaccepted header), with no SEQRESET. A status PAUSED holds only with
    nothing in flight and from a sample newer than the last ACK or hold, so
    a reply written before a resume header cannot hold it again.
  * Sim: `tools` (mounts and zeros) turns on the JT header check, TOOL_CHANGE
    for `toolChangeMs` then TOOL_WAIT, `mount`, `wzero z`; `cancel` is also
    admitted in TOOL_WAIT. Off by default.
  * Tests: `web/test/wire/link/job-session.test.ts`, 10. Not covered:
    `refused` and `silent` (the sim has no wrong resume point or silence).
  * Checks: `pnpm typecheck` clean; `pnpm test` 578 passed, 1 skipped, 1
    failed: `blob.test.ts` (controller.msgpack vs `config/controller.jsonc`),
    failing on `main` too since JT added `positions.mount` without
    regenerating the fixture. Follow-up: regenerate it.
  * Not tried on the machine: pause, resume and the tool-change wait on COM8
    (with C5).

### S1: SVG layers named by tool

* Type: `feature/svg-tool-layers`
* Purpose: an SVG's layers are its tool-named groups, whatever app drew it.
  Today every `<g>` with a label or id adds a path segment
  (`web/src/svg/load.ts:436-439`), so an app's own groups (`g123`,
  `Group_3`) split a layer and C5 can't find its tool.
  * `loadSvgPaths(svg, { tools })` takes the pulled config's tool names. A
    `<g>` whose name is a tool, or `<tool>.<n>` (chamber n, 1-based, A at
    `chamberOffsets[n-1]`), starts that layer; plain `<tool>` is chamber 0.
    The whole name is matched to a tool before a `.<n>` is split off.
  * Everything inside it belongs to it: the outermost tool-named group wins.
    A tool-named group inside one is merged, with a warning.
  * Same-named groups join, in document order. Paths outside any tool group
    land under `""`, not cut.
  * A group's name: `inkscape:label`, else `serif:id` (Affinity keeps the
    original there), else `id`. Illustrator's `_xHH_` escapes are decoded
    (`pen_x2E_2` is `pen.2`), and a dedup suffix (`_1_` Illustrator, `_2`
    Figma) is stripped.
  * `LoadedSvg` gains `warnings: string[]`.
  * Without `tools`, today's rule, so other callers and the `load: layers`
    test (`web/test/svg/load.test.ts:193-202`) are unchanged.
  * `prepareBezierJob` (`web/src/toolpath/job.ts:58`) takes `tools` and
    passes it on.
  * Read: export one file each from Illustrator and Affinity as fixtures;
    the escapes and suffixes above are from forum reports.
* Files: `web/src/svg/load.ts`, `web/src/toolpath/job.ts`,
  `web/test/svg/load.test.ts`, fixtures in `web/test/fixtures/`.
* Depends on: none.
* Status: done, merged at `ed0c9ef`. Unblocks C5.
* Outcome:
  * Interface for C5: `toolLayer(name, tools)` gives `{ tool, chamber }` or
    null, and `toolLayerKey()` the `layers` key (`pen`, `pen.2`); both
    exported, with `ToolLayer`. C5 calls `toolLayer(key, tools)` on each
    key for its tool and chamber columns. `LoadedSvg.warnings` holds the
    merge messages; `BezierJobOptions.tools` passes the names on.
  * Deviation: no real exports. One handwritten fixture,
    `web/test/fixtures/svg_tool_layers.svg`, holds every app's case, from
    forum reports; a real Illustrator or Affinity export that misreads goes
    in beside it.
  * `toolLayer` refuses `.0`; the chamber's range against `chamberOffsets`
    is C5's.
  * Known failing check: `blob.test.ts` (controller.msgpack stale against
    `config/controller.jsonc`), failing on main too; follow-up as in H1.

### C5: job

* Type: `feature/demo-job`
* Purpose: run one SVG layer as a job: a `LAYER_HDR` and its BEZIERs through
  a `Session`, with live `Session.stats()` and the `StreamResult` and
  `fatalReasonName` at the end. Inputs are split by packet type, so a later
  `JOB_HDR` or `TOOL_HDR` adds its own row.
  * SVG file, loaded once a config is pulled and again after each pull;
    its layers (S1) listed in SVG order, with their tool and chamber, the
    tool's `id` going in the header. A chamber the tool doesn't have is
    listed but not runnable; loader warnings are shown.
  * A path preview of the selected layer, from the curves `loadSvgPaths`
    returned.
  * feed (0 = the tool's), k, material thickness t.
  * head: the operator picks it, one per pulled config head. The firmware
    ACKs if the tool is mounted there and the head selected, else starts a
    tool change (`NACK_TOOL_CHANGE`).
  * Execute layer: `prepareBezierJob` runs here, not on selection.
  * Pause (`pause`), Resume (`session.resume()`), Continue in TOOL_WAIT
    (`session.continue()`), Cancel (`cancel`), Abort (`link.abort()` and
    `session.truncate()`).
  * A finished layer stays in JOB, dry, until the operator presses Cancel.
    The job line shows records executed / total and records left, and says
    done once the ring is dry with every packet acked.
  * The status poll runs only outside a job; in one, the panel redraws on
    the session's own samples (`waitUpdate`).
  * Stop (`stop`) sits beside Connect, outside the job panel.
  * `wzero`, `wzero xy`, `wzero z` buttons in the Jog panel.
  * Nothing is gated on the host: jog, select and the console stay live in a
    job; the firmware refuses what it doesn't admit.
* Files: `web/demo/index.{html,js}`.
* Depends on: C4 for the pulled config; F1, so jogs in TOOL_WAIT reach the
  jog sink; H1, for
  the hold, resume and continue; S1, so a layer's name is its tool.

## Open

Found while doing this plan, out of its scope. Each needs its own plan.

### Boot counter and uptime in status

STATUS_RSP has nothing that tells one boot from the next. H1 takes "JOB on
the Session's layer" as the job still being there, which a reboot followed by
a new job on the same layer passes. A boot counter (or a session id the Pico
picks at boot) and an uptime in the status would make that certain, and show
resets on the bench. A firmware and frame change.

### Tool name rule

S1 matches SVG group names to tool names. A rule in `loadConfig`
(`web/src/machine/json/load.ts`) would keep them unambiguous: lowercase
`[a-z][a-z0-9]*`, so no `.` (the chamber separator) and no trailing
`_<digits>` (a dedup suffix), and never an app's own names (`layer\d*`,
`group\d*`, `g\d+`, `path\d+`).

### Config refactors

1. **Remove the web-only config fields.** `invert`, and in `homing`
   `hardTravel`, `atOrigin`, `pullInFeed`, `backoffMm`, `parkMm` and
   `datumDeg` (marked `// web only` in `config/controller.jsonc`) have no
   consumer: the web homing path is gone and the Pico's decoder
   (`core0/config/config_decode.cpp`) never reads them. They are still
   parsed and defaulted (`machine/json/load.ts`), checked
   (`machine/json/validate.ts`), typed (`machine/schema.ts`) and carried in
   every config blob. Removing them also clears two of
   `controller.jsonc`'s four load warnings: the stale `hardTravel` on Y
   (500) and head 0's Z (40) against the measured `maxTravel` (735, 48),
   with the header note on Z's `maxTravel` that cites it. The Pico reads
   none of them, so whether `CONFIG_BLOB_VERSION` must bump is for that plan.
2. **Make the file format match the model.** `loadConfig` builds a different
   shape than it reads (`load.ts:402`): `heads`, `defaultHead` and
   `peripherals` are top-level in the file and under `machine` in the model;
   `tools` becomes `toolProfiles` with derived fields (`name`, `toolType`,
   `periodCount`, `plunge`, `requiredPeripheralTypes`); every default is
   filled. A pulled config therefore decodes to a model `loadConfig` rejects
   (`heads: required`), which is why C4 has no download. Moving the three
   keys under `machine` in the file is host-only (`load.ts`,
   `controller.jsonc`, fixtures; name the old keys in the errors) and leaves
   `tools` → `toolProfiles` as the one reshape, so a `modelToFile()` (and the
   download) becomes nearly trivial.

The other two warnings (head 1 accepts nothing, so nothing takes `pen`) are
deliberate until the pen head can move.

