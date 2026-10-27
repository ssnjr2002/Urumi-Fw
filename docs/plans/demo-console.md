# Demo console: a plain host page over the wire library

A test page that calls the wire library (`web/src/wire/`) and nothing above
it. It is a harness for developing the library and a demo of the machine's
basic capabilities. No styling, no preview canvas, no planner UI.

The old demos (`web/demo/`: `barebones`, `bench`, `comms`, `orchestrate`,
`index.html` + `main.js`, `bench.json`, `comms.json`, `config.json`) are
archived at tag `archive/demos-2026-10` and deleted here. Read one with
`git show archive/demos-2026-10:web/demo/<file>`. `demo.svg` stays.

## Decisions

* **One page:** `web/demo/console.html` + `console.js`, plain HTML. Each panel
  calls `Link` (`web/src/wire/link/link.ts`) or `commands.ts` directly.
* **Backend:** WebSerial only. The sim backend (`link/backends/sim.ts`) is out of
  scope for now; a backend picker can add it later.
* **Text plane is a serial console:** an input box, Enter to send, and ↑/↓ to
  step through history. One reply per command, the same as `Link.command`
  (`link.ts:145`). The `textDesyncs` count is shown, so a firmware reply with
  extra lines is visible. `stop` goes through `Link.send` (`link.ts:165`), so it
  never waits behind a pending command. No helper buttons yet.
* **Log:** one on-page `<pre>` with a timestamp and direction per line
  (`→`/`←`, text or binary magic name, ACK/NACK name), plus Clear and Copy.
  Every panel writes to it.
* **Library resolution** (open, settled in C1): the demos import
  `urumi-host`, which resolves through `package.json` exports to `dist/`, so
  they need `pnpm build` after every wire change. Proposal: `pnpm demo` aliases
  `urumi-host` to `src/` through `web/demo/vite.config.js`, and
  `pnpm demo:dist` uses the built package, the way a third party would.

## Branches

A dependent chain, Single Session: every branch after C1 edits
`console.html`/`console.js`.

### C1: console and log

* Type: `feature/demo-console`
* Purpose: delete the old demos; add the page with Connect/Disconnect, the
  text console and the log pane; settle library resolution.
* Files: `web/demo/*` (delete), `web/demo/console.{html,js}`,
  `web/demo/vite.config.js`, `web/package.json` (scripts).
* Depends on: none.
* Status: worktree `../urumi-demo-console` created from `760e824`.

### C2: status

* Type: `feature/demo-status`
* Purpose: `getStatus` (`link.ts:175`) on demand and as a poll at a set
  rate, decoded `MachineStatus` as a key/value table.
* Files: `web/demo/console.{html,js}`.
* Depends on: C1.

### C3: jog and frames

* Type: `feature/demo-jog`
* Purpose: key jog (`cjog`/`cjogStop`, `link.ts:207`) and the head/anchor
  select, carried over from the archived `barebones.js`.
* Files: `web/demo/console.{html,js}`.
* Depends on: C1.

### C4: config

* Type: `feature/demo-config`
* Purpose: push a config file and pull to a download (`pushConfig`/`pullConfig`,
  `link.ts:227,247`); a failure shows the CFG_NACK name
  (`format/cfg.ts`).
* Files: `web/demo/console.{html,js}`.
* Depends on: C1.

### C5: job

* Type: `feature/demo-job`
* Purpose: SVG → layer → feeds/offset → Prepare/Run/Abort, streamed through a
  `Session`; live `Session.stats()` while running, and the `StreamResult`
  and `fatalReasonName` at the end.
* Files: `web/demo/console.{html,js}`.
* Depends on: C1; C4 for the loaded config.
