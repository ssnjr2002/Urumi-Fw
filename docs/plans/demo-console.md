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
* Status: ready to merge, worktree `../urumi-demo-jog`.
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
* Purpose: push a config file and pull to a download (`pushConfig`/`pullConfig`,
  `link.ts:227,247`); a failure shows the CFG_NACK name
  (`format/cfg.ts`).
* Files: `web/demo/index.{html,js}`.
* Depends on: C1.

### C5: job

* Type: `feature/demo-job`
* Purpose: SVG → layer → feeds/offset → Prepare/Run/Abort, streamed through a
  `Session`; live `Session.stats()` while running, and the `StreamResult`
  and `fatalReasonName` at the end.
* Files: `web/demo/index.{html,js}`.
* Depends on: C1; C4 for the loaded config.
