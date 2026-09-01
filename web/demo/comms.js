/**
 * comms.js — a demo for the COMMS layer, not the planner.
 *
 * The other demos (main.js, bench.js, orchestrate.js) all start from an SVG and
 * exercise src/plan + src/production. This one starts from a Link and exercises
 * src/wire/link and src/operatorJog: the text plane, the binary STATUS_RSP
 * poll, and the two jog shapes.
 *
 *   text console  — link.command() one line out, one line back (D11: strictly
 *                   one outstanding). `!line` uses link.send(), the
 *                   fire-and-forget path estop needs.
 *   status panel  — link.getStatus() every N ms. It keeps polling DURING a jog
 *                   because the demux routes by magic (D1/D10), which is also
 *                   what makes the open jog session pace itself.
 *   click jog     — ClickJogSource held directly (not jogClick) so a second
 *                   click on the same axis+sign can call src.add() and BLEND
 *                   into the live move. jogClick would open a second session.
 *   go-to queue   — jogToPoint per entry: closed session, read live pos, one
 *                   coordinated trapezoid across every axis the entry names
 *                   to the target. Stacked line by line and fired one at a
 *                   time, since only one session may own the ack sink.
 *   job runner    — the bench.js path (SVG → bakePlan → schedule/walk →
 *                   link.stream) on THIS page's config, so a job and the jog
 *                   panel cannot disagree about the machine. A tool swap also
 *                   re-commits the incoming head's axis_map.
 *
 * The whole jog UI is gated by the config: axis rows exist only where
 * `node.present` is true, and each head gets its own Z/A group. A config that
 * fails loadConfig() leaves everything disabled.
 *
 * Sessions stamp seq from 0, so every jog here is preceded by link.resetSeq()
 * — jogClick/jogToPoint do not do it for you (link.stream() does).
 */

import {
    loadConfig,
    Link,
    Controller,
    runWalk,
    SimTransport,
    ClickJogSource,
    jogToPoint,
    MachineState,
    fatalReasonName,
    bakePlan,
    walkSchedule,
    getPos,
    MICRO_JOG,
    MICRO_LIFT,
    MICRO_DUTY_RELEASE,
    TOOL_PROFILES_BY_TYPE,
    mountedTypes,
    NodeType,
    ToolType,
    machineAnchor,
    headOffset,
    toolFrameOffset,
    homeToTool,
    axisSlots,
    motionSegments,
    walkSeconds,
    STATE_NAMES,
    ALARM_NAMES,
    RUNNING_NAMES,
    maskStr,
    derivePlan,
    runHoming,
} from '../src/index.js';
import { WebSerialTransport } from '../src/wire/link/backends/webserial.js';

// ── elements ────────────────────────────────────────────────────────────────

const $ = id => document.getElementById(id);

const backendSel   = $('backend');
const baudInput    = $('baud');
const connectBtn   = $('connect-btn');
const disconnectBtn= $('disconnect-btn');
const verboseChk   = $('verbose');

const configUrl    = $('config-url');
const reloadCfgBtn = $('reload-config');
const configBanner = $('config-banner');

const consoleEl    = $('console');
const cmdInput     = $('cmd-input');
const sendBtn      = $('send-btn');
const quickCmds    = $('quick-cmds');
const clearConsole = $('clear-console');

const headSel      = $('head-sel');
const mapCommit    = $('map-commit');
const mapLine      = $('map-line');

const keyHint      = $('key-hint');
const jogStep      = $('jog-step');
const jogFeed      = $('jog-feed');
const jogCancelBtn = $('jog-cancel');
const jogStateEl   = $('jog-state');
const jogPanel     = $('jog-panel');

const homeAxesEl   = $('home-axes');
const homeDryBtn   = $('home-dry');
const homeRunBtn   = $('home-run');
const homeStopBtn  = $('home-stop');
const homeLine     = $('home-line');

const gotoAxesEl   = $('goto-axes');
const gotoHead     = $('goto-head');
const gotoFeed     = $('goto-feed');
const gotoStack    = $('goto-stack');
const gotoText     = $('goto-text');
const gotoBuild    = $('goto-build');
const gotoNext     = $('goto-next');
const gotoAll      = $('goto-all');
const gotoAbort    = $('goto-abort');
const gotoClear    = $('goto-clear');
const queueEl      = $('queue');

const periphPanel  = $('periph-panel');

const jobSvg       = $('job-svg');
const jobTool      = $('job-tool');
const jobCompile   = $('job-compile');
const jobRun       = $('job-run');
const jobStop      = $('job-stop');
const jobProgress  = $('job-progress');
const jobMetrics   = $('job-metrics');

const pollOnceBtn  = $('poll-once');
const pollAuto     = $('poll-auto');
const pollMs       = $('poll-ms');
const frameSel     = $('frame-sel');
const statusBody   = $('status-table').querySelector('tbody');
const statusBanner = $('status-banner');
const linkBody     = $('link-table').querySelector('tbody');

// ── state ───────────────────────────────────────────────────────────────────

let config  = null;   // PipelineConfig, or null if the config did not load
let axes    = [];     // gated axis descriptors built from the config

/**
 * The Controller — the one object here holding a MachineConfig and a Link at
 * once. It owns the Setup (which head is engaged, what is fitted), the axis-map
 * reconciliation, the status poll and the frame conversions; this file owns the
 * DOM and nothing else.
 *
 * `link` is kept beside it purely so the text console, the jog panel and the
 * peripheral relays can keep talking to the transport directly. Those are all
 * genuinely link-level — a text command and a jog burst do not need to know
 * what a tool is.
 */
let ctl  = null;
let link = null;

/** The live click jog, if any: { src, key, label, done }. */
let jog = null;
/** The live go-to, if any: { handle, entry }. */
let goTo = null;

/** Which axis row owns the arrow keys (axis.key), or null. */
let keyAxis = null;
/** Which tip the readout is expressed in — a frameOptions() key, or null. */
let frameView = null;
/** The last status seen, so changing the frame can re-render without a poll. */
let lastStatus = null;

let queue   = [];     // [{ id, head, targets: [{axisKey, target}], feedMmS, state }]
let queueSeq = 0;
let running = false;  // a "send all" walk or a job stream is in flight

/** The SVG the job runner compiles, if one has been chosen. */
let svgText = null;
let svgName = 'job';

const history = [];
let historyIdx = -1;

const isConnected = () => link !== null && !link.closed;

// ── console ─────────────────────────────────────────────────────────────────

function log(text, kind = 'note') {
    const line = document.createElement('div');
    line.className = kind;
    line.textContent = text;
    consoleEl.appendChild(line);
    consoleEl.scrollTop = consoleEl.scrollHeight;
}

clearConsole.addEventListener('click', () => { consoleEl.textContent = ''; });

// ── config → axis model ─────────────────────────────────────────────────────

/**
 * Flatten the config into the axis rows the jog UI can offer.
 *
 * Slot, not node id, is what the wire cares about: a MicroSegment carries four
 * step deltas and the Pico maps slot→node through the committed axis map. X and
 * Y take slots 0 and 1; both heads' Z/A compete for slots 2 and 3, so only the
 * ENGAGED head can move — which is why the head selector below is not a UI
 * preference but the thing that decides what `axis_map` binds.
 *
 * Rows for absent nodes are rendered disabled rather than dropped, so the
 * operator can see the config said "not present".
 */

/**
 * What is fitted to head `i`, or null.
 *
 * `heads[].profile` is gone (docs/head_binding.md stage 1): config no longer
 * claims to know what is in a socket, only which tools the socket ACCEPTS.
 * Live truth is the Controller's mount table; without one, the best available
 * answer is the head's first-choice tool, which is what `setupFor` would fit.
 */
function headTool(i, cfg = config) {
    if (ctl) return ctl.setup.mounts[i] ?? null;
    const accepts = cfg?.machine.heads[i]?.accepts ?? [];
    return accepts.length ? TOOL_PROFILES_BY_TYPE[accepts[0]] ?? null : null;
}

/** `head 2 (knife)` / `head 2` — the shared label for a socket. */
function headLabel(i, sep = ' ') {
    const p = headTool(i);
    return `head ${i}${p ? `${sep}(${p.name})` : ''}`;
}

function buildAxes(cfg) {
    // The slot model is the library's (machine/slots.ts); this adds only the
    // optgroup label, which is presentation and stays here.
    const groupOf = r => {
        if (r.head === undefined) return 'Gantry';
        const p = headTool(r.head, cfg);
        return `head ${r.head}${p ? ` (${p.name})` : ''}`;
    };
    return axisSlots(cfg.machine).map(r => ({
        key: r.key, letter: r.letter, slot: r.slot, label: r.label, head: r.head,
        ax: r.axis, present: r.present, unit: r.unit, group: groupOf(r),
        cal: { stepsPerUnit: r.stepsPerUnit, invert: r.invert },
    }));
}

/** The config decides which panels exist at all — rebuild them, then re-gate. */
function rebuildFromConfig() {
    renderHeadSel();
    renderFrameSel();
    renderJogPanel();
    renderHomePanel();
    renderPeriphPanel();
    renderGotoHead();
    renderAll();
}

async function loadConfigFrom(url) {
    configBanner.dataset.kind = 'idle';
    configBanner.textContent = `Loading ${url}…`;
    let text;
    try {
        const r = await fetch(url);
        if (!r.ok) throw new Error(`${r.status} ${r.statusText}`);
        text = await r.text();
    } catch (e) {
        config = null; axes = [];
        configBanner.dataset.kind = 'error';
        configBanner.textContent = `Fetch failed: ${e.message}`;
        rebuildFromConfig();
        return;
    }

    const loaded = loadConfig(text);
    if (!loaded.ok) {
        config = null; axes = [];
        configBanner.dataset.kind = 'error';
        configBanner.textContent = 'Config rejected:\n' + loaded.errors.join('\n');
        rebuildFromConfig();
        return;
    }
    config = loaded.config;
    axes = buildAxes(config);
    const heads = config.machine.heads.length;
    const present = axes.filter(a => a.present).map(a => a.label).join(' ');
    configBanner.dataset.kind = 'ok';
    configBanner.textContent =
        `OK — ${heads} head(s), axes present: ${present || 'none'}` +
        `\nanchor: ${anchorLabel()} — every offset is measured from it` +
        (loaded.warnings.length ? `\nwarnings:\n${loaded.warnings.join('\n')}` : '');
    for (const w of loaded.warnings) console.warn('[config]', w);
    rebuildFromConfig();
}

reloadCfgBtn.addEventListener('click', () => loadConfigFrom(configUrl.value.trim()));

/**
 * What sits at (0,0). Worth stating plainly on the page: every head offset in
 * the config is measured from this, so it is what "the machine position" means.
 * `none` is a legal config that puts no hardware at the origin.
 */
function anchorLabel() {
    const a = machineAnchor(config.machine);
    if (a.kind === 'laser') return 'laser';
    if (a.kind === 'head') {
        return headLabel(a.index);
    }
    return 'none — no head sits at (0,0)';
}

// ── axis map ────────────────────────────────────────────────────────────────

/**
 * Commit the map for the selected head and report the result.
 *
 * The Controller does the work — it computes the slot bindings from the Setup,
 * refuses the rebind while RUNNING, sends `axis_map` and reads the map back.
 * What is left here is the narration.
 *
 * Why it is the first thing after connecting: until a map commits, the Pico
 * sits in ALARM/ALARM_CONFIG and NACKs every job, jog and debug-step — the map
 * is host-authored and never appears in STATUS_RSP (docs/engage_and_axis_map.md
 * §8).
 */
async function commitAxisMap(head) {
    if (!ctl || !isConnected()) return false;
    log(`> axis_map for head ${head}`, 'tx');
    try {
        await ctl.commit(head);
        log(`  ok — slots bound for head ${head}`, 'ok');
    } catch (e) {
        log(`  ${e.message}`, 'err');
        renderAxisMap();
        return false;
    }
    renderAll();
    return true;
}

/** Which head, if any, the committed map matches. null = neither/unbound. */
const committedHead = () => (ctl && ctl.synced ? ctl.setup.engaged : null);

function renderHeadSel() {
    headSel.innerHTML = '';
    if (!config) return;
    config.machine.heads.forEach((h, i) => {
        const o = document.createElement('option');
        o.value = String(i);
        o.textContent = headLabel(i, ' — ').replace(/[()]/g, '');
        headSel.appendChild(o);
    });
    const engaged = ctl ? ctl.setup.engaged : config.machine.defaultHead;
    headSel.value = String(Math.min(engaged, config.machine.heads.length - 1));
}

mapCommit.addEventListener('click', () => void commitAxisMap(parseInt(headSel.value, 10)));
headSel.addEventListener('change', renderAll);

function renderAxisMap() {
    const committed = ctl?.committed ?? null;
    // Homed state per BOUND slot, which is the only place it means anything —
    // an empty slot has no datum by definition. Worth showing beside the map
    // rather than only as the `axesHomed` mask on the right: the firmware
    // re-derives the mask from each incoming node when a slot binds, so a head
    // swap can change what is homed without anything moving, and this is the
    // line an operator is already looking at when that happens.
    const homed = lastStatus?.axesHomed ?? 0;
    const shown = committed
        ? committed.map((v, s) =>
            v === null ? '—' : `${v}${homed & (1 << s) ? '✓' : '·'}`).join('  ')
        : '— — — —';
    const h = committedHead();
    mapLine.textContent = `slots X Y Z A = ${shown}` +
        (h !== null ? `  (head ${h})` : committed ? '  (no head matches)' : '');
    mapLine.dataset.kind = h !== null ? 'ok' : 'idle';
}

// ── homing ──────────────────────────────────────────────────────────────────
//
// The panel drives src/homing/: derivePlan() turns an AxisConfig into four legs
// and a datum, runHoming() arms each leg and waits. Nothing about the four-leg
// structure or the arithmetic lives here — a demo that re-derived any of it
// would be a second implementation to keep in step with the machine.

/** The live home, if any: { abort } — abort is a flag runHoming does not read;
 *  the Stop button estops, because mid-home there is nothing to unwind to. */
let homing = null;

/** Axis rows that CAN be homed: a homing block in the config and a node present. */
const homeableAxes = () => axes.filter(a => a.present && a.ax.homing !== undefined);

// Delegated, and attached once: renderHomePanel() replaces the checkboxes on
// every config load, so a listener per box would stack up.
homeAxesEl.addEventListener('change', renderAll);

function renderHomePanel() {
    homeAxesEl.innerHTML = '';
    const rows = config ? homeableAxes() : [];
    if (rows.length === 0) {
        const s = document.createElement('span');
        s.className = 'hint';
        s.textContent = 'No axis in this config has a homing block.';
        homeAxesEl.appendChild(s);
        return;
    }
    for (const a of rows) {
        const l = document.createElement('label');
        l.className = 'name';
        l.innerHTML = `<input type="checkbox" data-home="${a.key}" checked> ${a.label}` +
                      ` <span class="tick" data-hometick="${a.key}">·</span>`;
        homeAxesEl.appendChild(l);
    }
    renderHomeTicks();
}

/**
 * The homed flag per selectable axis. Same mask the map line reads, shown a
 * second time here because this is the panel you look at when deciding what to
 * home -- being told "X is already done" one line away from the checkbox is the
 * whole point.
 *
 * A Z/A on the head that is NOT engaged reads `—`, not `·`: its slot currently
 * holds the other head's node, so the mask has nothing to say about it. `·`
 * would be a claim ("not homed") the machine never made.
 */
function renderHomeTicks() {
    const homed = lastStatus?.axesHomed ?? 0;
    const engaged = committedHead();
    for (const el of homeAxesEl.querySelectorAll('[data-hometick]')) {
        const a = axes.find(r => r.key === el.dataset.hometick);
        if (!a) continue;
        const off = a.head !== undefined && a.head !== engaged;
        el.textContent = off ? '—' : (homed & (1 << a.slot) ? '✓' : '·');
        el.dataset.kind = off ? 'off' : (homed & (1 << a.slot) ? 'ok' : 'idle');
    }
}

/** The checked rows, minus any belonging to a head that is not engaged. */
function selectedHomeAxes() {
    const engaged = committedHead();
    return [...homeAxesEl.querySelectorAll('input[data-home]')]
        .filter(cb => cb.checked && !cb.disabled)
        .map(cb => axes.find(a => a.key === cb.dataset.home))
        .filter(a => a && (a.head === undefined || a.head === engaged));
}

function setHomeLine(text, kind = 'idle') {
    homeLine.textContent = text;
    homeLine.dataset.kind = kind;
}

homeDryBtn.addEventListener('click', () => {
    const rows = selectedHomeAxes();
    if (rows.length === 0) { setHomeLine('nothing selected', 'idle'); return; }
    for (const a of rows) {
        let plan;
        try {
            plan = derivePlan(a.letter, a.ax);
        } catch (e) {
            log(`${a.label}: ${e.message}`, 'err');
            continue;
        }
        const mm = (plan.datumSteps / a.cal.stepsPerUnit).toFixed(1);
        log(`${a.label} — toward=dir ${plan.legs[0].dir}   ` +
            `datum ${plan.datumSteps} steps (${mm} ${a.unit})`, 'note');
        plan.legs.forEach((g, i) => {
            const ramp = g.startUs === g.floorUs ? `${g.floorUs}us` : `${g.startUs}->${g.floorUs}us`;
            // `<=` for a seek, `=` for a retract: a seek stops at the switch and
            // its budget is only a runaway cap, while a retract ignores the
            // switch and travels EXACTLY this far. That difference is the whole
            // reason leg 4's distance is knowable and leg 1's is not.
            const budget = g.endsLatched ? `<=${g.maxSteps}` : `=${g.maxSteps}`;
            log(` ${i + 1} ${g.kind.padEnd(8)} dir ${g.dir}  ${ramp.padEnd(13)}` +
                `ramp ${String(g.rampSteps).padEnd(5)}${budget.padEnd(9)}` +
                `ends: switch ${g.endsLatched ? 'HELD' : 'CLEAR'}`, 'rx');
        });
    }
    setHomeLine(`dry run: ${rows.length} axis(es) — see console`, 'ok');
});

homeRunBtn.addEventListener('click', async () => {
    const rows = selectedHomeAxes();
    if (rows.length === 0 || !ctl) return;
    homing = {};
    renderAll();
    try {
        // Sequential, and not merely for tidiness: the firmware answers
        // `err busy` to a second `home` while one is in flight, because Core 0
        // supervises exactly one at a time.
        for (const a of rows) {
            const plan = derivePlan(a.letter, a.ax);
            await runHoming(link, plan, {
                onLeg: (leg, i, n) =>
                    setHomeLine(`${a.label} — leg ${i + 1}/${n} · ${leg.kind} · dir ${leg.dir} · ` +
                                `${leg.maxSteps} steps · ends switch ` +
                                `${leg.endsLatched ? 'HELD' : 'CLEAR'}`, 'idle'),
                onLegDone: leg => log(`  ${a.letter} ${leg.kind}: ok`, 'ok'),
            });
            log(`${a.label} homed — datum ${plan.datumSteps} steps`, 'ok');
        }
        setHomeLine(`homed: ${rows.map(a => a.letter).join(' ')}`, 'ok');
    } catch (e) {
        setHomeLine(e.message, 'error');
        log(e.message, 'err');
    } finally {
        homing = null;
        // The datum landed via setorigin, so the homed mask has changed and the
        // last poll predates it.
        if (ctl) await ctl.refresh().catch(() => {});
        renderAll();
    }
});

// Stop, not "cancel". A home is four legs deep in the node's own pulser and
// there is no partial state to unwind to — the axis is somewhere between two
// known points and only a fresh home can say where. estop() is the honest verb.
homeStopBtn.addEventListener('click', () => { if (ctl) void ctl.estop().catch(() => {}); });

// ── connect / disconnect ────────────────────────────────────────────────────

connectBtn.addEventListener('click', async () => {
    if (isConnected()) return;
    connectBtn.disabled = true;
    try {
        const transport = backendSel.value === 'sim'
            // The sim's bus is whatever the config says is present, so a
            // two-head config gets both heads' nodes answering ENGAGE. Ids the
            // config does not claim time out, exactly like a missing node.
            // Peripherals belong on it too — they are bus nodes that no axis
            // map ever binds, and leaving them off makes every knife/vacuum
            // relay answer `timeout` for a reason that is purely the demo's.
            ? new SimTransport({
                busNodes: [
                    ...axes.filter(a => a.present).map(a => a.ax.node.id),
                    ...(config?.machine.peripherals ?? []).filter(n => n.present).map(n => n.id),
                ],
            })
            : await WebSerialTransport.requestAndOpen(parseInt(baudInput.value, 10) || 115200);
        link = new Link(transport);
        link.verbose = verboseChk.checked;
        ctl = new Controller(config.machine, link);
        wireController(ctl);
        log(`connected via ${backendSel.value}`, 'ok');
        startPolling();
        // §8: the map is host-authored and absent from STATUS_RSP, so a fresh
        // connection must re-assert it. sync() reads back what the Pico is
        // actually bound to first — the map survives across a host reload, so
        // adopting it beats assuming defaultHead and then fighting the machine.
        if (await ctl.sync()) {
            log(`adopted the committed map — head ${ctl.setup.engaged}`, 'ok');
        } else {
            await commitAxisMap(parseInt(headSel.value, 10) || 0);
        }
    } catch (e) {
        log(`connect failed: ${e.message}`, 'err');
    }
    renderAll();
});

disconnectBtn.addEventListener('click', async () => {
    stopPolling();
    cancelJog();
    if (ctl) {
        try { await ctl.close(); } catch (e) { log(`close: ${e.message}`, 'err'); }
    }
    ctl = null;
    link = null;
    // Whatever the peripherals were doing, we can no longer command them and no
    // longer know. Forget, so the next job re-asserts from scratch.
    periphCommanded.clear();
    renderAxisMap();
    log('disconnected', 'note');
    statusBanner.dataset.kind = 'idle';
    statusBanner.textContent = 'Not connected.';
    renderAll();
});

verboseChk.addEventListener('change', () => { if (link) link.verbose = verboseChk.checked; });

// ── text plane ──────────────────────────────────────────────────────────────

const QUICK = [
    'ping', 'getstate', 'getpos', 'pingnode all', 'axis_map',
    'enable', 'disable', 'setorigin', 'unalarm',
    'pause', 'resume', 'cancel', '!stop', '!abort',
];

for (const cmd of QUICK) {
    const b = document.createElement('button');
    b.type = 'button';
    b.textContent = cmd;
    b.dataset.quick = '1';
    if (cmd === '!stop') b.className = 'danger';
    b.addEventListener('click', () => runCommand(cmd));
    quickCmds.appendChild(b);
}

/**
 * `!abort` is not a text line at all — it is the one-byte binary soft abort
 * (§4.5). Routing it through the same input keeps the operator's muscle memory
 * in one place; `!` means "no reply slot, do not wait".
 */
async function runCommand(text) {
    if (!text || !isConnected()) return;
    if (text.startsWith('!')) {
        const body = text.slice(1).trim();
        log(`> ${text}`, 'tx');
        if (body === 'abort') { link.abort(); log('  (ABORT frame sent)', 'note'); return; }
        await link.send(body);
        log('  (sent, no reply expected)', 'note');
        return;
    }
    log(`> ${text}`, 'tx');
    try {
        const reply = await link.command(text);
        if (reply === '') log('  (timeout — no reply)', 'err');
        else log(`  ${reply}`, reply.startsWith('err') ? 'err' : 'rx');
    } catch (e) {
        log(`  error: ${e.message}`, 'err');
    }
    if (link.textDesyncs) log(`  textDesyncs=${link.textDesyncs}`, 'err');
    renderLinkStats();
}

function submitCommand() {
    const text = cmdInput.value.trim();
    if (!text) return;
    history.push(text);
    historyIdx = history.length;
    cmdInput.value = '';
    runCommand(text);
}

sendBtn.addEventListener('click', submitCommand);
cmdInput.addEventListener('keydown', e => {
    if (e.key === 'Enter') { submitCommand(); return; }
    if (e.key === 'ArrowUp' && historyIdx > 0) {
        historyIdx--; cmdInput.value = history[historyIdx]; e.preventDefault();
    } else if (e.key === 'ArrowDown' && historyIdx < history.length - 1) {
        historyIdx++; cmdInput.value = history[historyIdx]; e.preventDefault();
    }
});

// ── status poll ─────────────────────────────────────────────────────────────

// STATE_NAMES / ALARM_NAMES / RUNNING_NAMES / maskStr come from
// wire/format/names.ts now — see the import block.

/**
 * Subscribe to the Controller. Every sample the Controller takes lands here —
 * from the background poll, from an explicit refresh, and (the part a hand-rolled
 * `setInterval` never gets) from the settle loops inside a job run, so the panel
 * keeps painting while a stream waits for the machine to come to rest.
 */
function wireController(c) {
    c.on('status', st => {
        notePeripheralPark(st.state);
        renderStatus(st);
        renderAxisMap();     // the map line carries the homed ticks
        renderHomeTicks();   // and so does each homing checkbox
        statusBanner.dataset.kind = 'ok';
        statusBanner.textContent = `last poll ${new Date().toLocaleTimeString()}`;
        renderLinkStats();
    });
    c.on('error', e => {
        statusBanner.dataset.kind = 'error';
        statusBanner.textContent = e.message;
    });
    // A commit re-derives the homed mask from the incoming nodes without
    // anything moving, so both views of it have to be repainted.
    c.on('committed', () => { renderAxisMap(); renderHomeTicks(); });
    c.on('setup', renderAll);
    c.on('busy', renderAll);
}

function startPolling() {
    if (!ctl) return;
    void ctl.stopPolling();
    if (!pollAuto.checked) return;
    ctl.startPolling(Math.max(100, parseInt(pollMs.value, 10) || 500));
}

function stopPolling() {
    if (ctl) void ctl.stopPolling();
}

pollAuto.addEventListener('change', () => (isConnected() ? startPolling() : stopPolling()));
pollMs.addEventListener('change', () => { if (isConnected()) startPolling(); });
pollOnceBtn.addEventListener('click', () => { if (ctl) void ctl.refresh().catch(() => {}); });

// ── readout frames (docs/coordinate_frames_and_limits.md §1) ────────────────
//
// The firmware knows ONE frame: its own step counters, which are the home frame
// once divided by stepsPerUnit. Tip frames are host-side — home plus a fixed
// offset — so switching between them changes the number and moves nothing.
//
// Head centre and tool tip are offered separately because they are genuinely
// different frames that differ by exactly the tool offset (§3.2): plan geometry
// is baked in head-centre space, while the operator is looking at the tip. A
// few millimetres apart, and this is where you can see it.

/**
 * The tip frames this config can report.
 *
 * A tip frame needs a TOOL, and which tool is in a socket is now live state,
 * not config — `heads[].accepts` says what fits, not what is fitted. So this
 * follows the Controller's mount table when there is one, and falls back to the
 * head's first-choice tool when there is not. Swapping a tool changes the tip
 * offsets, which is exactly what the operator is looking at here.
 */
function frameOptions() {
    if (!config) return [];
    const out = [];
    config.machine.heads.forEach((_, i) => {
        out.push({ key: `h${i}c`, label: `head ${i} · centre`, head: i, profile: null });
        const profile = headTool(i);
        if (profile) {
            out.push({ key: `h${i}t`, label: `head ${i} · ${profile.name} tip`, head: i, profile });
        }
    });
    return out;
}

/** The selected frame plus the offset that reaches it from home. */
function currentFrame() {
    if (!config) return null;
    const o = frameOptions().find(f => f.key === frameView);
    if (!o) return null;
    const offset = o.profile
        ? toolFrameOffset(config.machine, o.head, o.profile)
        : headOffset(config.machine, o.head);
    return { ...o, offset };
}

function renderFrameSel() {
    frameSel.innerHTML = '';
    const opts = frameOptions();
    for (const o of opts) {
        const el = document.createElement('option');
        el.value = o.key;
        el.textContent = o.label;
        frameSel.appendChild(el);
    }
    // A config reload can retire the selected frame; fall back to the first.
    if (!opts.some(o => o.key === frameView)) frameView = opts[0]?.key ?? null;
    if (frameView) frameSel.value = frameView;
}

frameSel.addEventListener('change', () => {
    frameView = frameSel.value;
    if (lastStatus) renderStatus(lastStatus);
});

function renderStatus(st) {
    lastStatus = st;
    // Z and A live on a head, so slot 2/3 must read the ENGAGED head's
    // calibration — head 0's Z is 1200 st/mm and head 1's is 600, and picking
    // the wrong one silently halves or doubles the reading. With no head bound
    // there is no right answer, so it shows steps only.
    const mmOf = slot => {
        // With no head bound there is no right answer for Z/A, so it shows
        // steps only rather than a plausible number from the wrong motor.
        if (!ctl || !st.pos || (slot >= 2 && !ctl.synced)) return '';
        const letter = ['x', 'y', 'z', 'a'][slot];
        const unit = ctl.axes[letter].rotary ? 'deg' : 'mm';
        return `  (${ctl.axisUnits(letter, st).toFixed(3)} ${unit})`;
    };

    const f = currentFrame();
    const home = ctl ? ctl.homeXY(st) : null;
    const tip = home && f ? homeToTool(home, f.offset) : null;
    const mm = v => `${v.toFixed(3)} mm`;
    const signed = v => `${v >= 0 ? '+' : ''}${v.toFixed(3)}`;

    const rows = [
        ['state',       `${STATE_NAMES[st.state] ?? st.state}`],
        ['running',     `${RUNNING_NAMES[st.running] ?? st.running}`],
        ['alarm',       `${ALARM_NAMES[st.alarm] ?? st.alarm}`],
        ['axesHomed',   `0x${st.axesHomed.toString(16)} ${maskStr(st.axesHomed)}`],
        ['axesEnabled', `0x${st.axesEnabled.toString(16)} ${maskStr(st.axesEnabled)}`],
        // '—' here is "the poll cannot see it", not "nothing is latched". The
        // panel is fed by the binary STATUS_RSP, which has no field for the
        // mask; only a text `getstate` carries it. Rendering 0x0 would claim
        // every switch is clear on the strength of a frame that never asked.
        ['axesLatched', st.axesLatched === undefined
            ? '—  (binary poll — run getstate)'
            : `0x${st.axesLatched.toString(16)} ${maskStr(st.axesLatched)}`],
        ['bufCount',    fmt(st.bufCount)],
        ['expectedSeq', fmt(st.expectedSeq)],
        ['queuedUs',    st.queuedUs === undefined ? '—' : `${st.queuedUs} (${(st.queuedUs / 1000).toFixed(1)} ms)`],
        ['pos x',       st.pos ? `${st.pos[0]}${mmOf(0)}` : '—'],
        ['pos y',       st.pos ? `${st.pos[1]}${mmOf(1)}` : '—'],
        ['pos z',       st.pos ? `${st.pos[2]}${mmOf(2)}` : '—'],
        ['pos a',       st.pos ? `${st.pos[3]}${mmOf(3)}` : '—'],
        ['home x',      home ? mm(home.x) : '—'],
        ['home y',      home ? mm(home.y) : '—'],
        ['frame',       f ? f.label : '—'],
        ['offset',      f ? `${signed(f.offset.x)}, ${signed(f.offset.y)} mm` : '—'],
        ['tip x',       tip ? mm(tip.x) : '—'],
        ['tip y',       tip ? mm(tip.y) : '—'],
    ];
    statusBody.innerHTML = '';
    for (const [k, v] of rows) statusBody.appendChild(tr(k, v));
}

const fmt = v => (v === undefined ? '—' : String(v));

function tr(k, v) {
    const row = document.createElement('tr');
    const a = document.createElement('td'); a.textContent = k;
    const b = document.createElement('td'); b.textContent = v;
    row.append(a, b);
    return row;
}

function renderLinkStats() {
    if (!link) { linkBody.innerHTML = ''; return; }
    const d = link.demux.stats();
    const w = link.writer.stats();
    const rows = [
        ['frames in',    d.frames],
        ['text lines',   d.textLines],
        ['unknownBytes', d.unknownBytes],
        ['overruns',     d.overruns],
        ['textDesyncs',  link.textDesyncs],
        ['frames out',   w.frames],
        ['batches out',  w.batches],
        ['bytes out',    w.bytes],
        ['aborts',       w.aborts],
    ];
    linkBody.innerHTML = '';
    for (const [k, v] of rows) linkBody.appendChild(tr(k, String(v)));
}

// ── click jog (open session, blending) ──────────────────────────────────────

/**
 * Clicks are serialised through one chain. Starting a jog is async (it awaits
 * the previous session and resetSeq), and `jog` is only assigned at the end of
 * that — so two fast clicks racing through would both find `jog === null` and
 * open two sessions on the same ack sink. Chained, the second click runs after
 * the first has published its source and blends into it instead.
 */
let jogChain = Promise.resolve();

function onJogClick(axis, sign) {
    jogChain = jogChain
        .then(() => startOrBlend(axis, sign))
        .catch(e => log(`jog error: ${e.message}`, 'err'));
}

/**
 * If the live source is the SAME axis+sign and has not finished, add() extends
 * the move in place — the blend. Anything else (other axis, reversal) cancels
 * the live source, which hands deceleration to the Pico via link.abort(), and
 * starts fresh once the old session settles.
 */
async function startOrBlend(axis, sign) {
    if (!isConnected() || !axis.present) return;
    const distMm = Math.abs(parseFloat(jogStep.value)) || 1;
    const feed   = Math.abs(parseFloat(jogFeed.value)) || 1;
    const steps  = Math.max(1, Math.round(distMm * axis.cal.stepsPerUnit));
    const key    = `${axis.key}${sign > 0 ? '+' : '-'}`;
    const label  = `${axis.label}${sign > 0 ? '+' : '-'}`;

    // `jog.cancelled` is tracked here, not read off the source: ClickJogSource
    // only sets its own `_finished` inside pull(), so add() still returns true
    // on a cancelled source that has not been polled yet. Blending into one
    // silently discards the distance — the next pull returns null — and leaves
    // the UI showing a live jog whose session already ended.
    if (jog && !jog.cancelled && jog.key === key && jog.src.add(steps)) {
        jog.clicks++;
        log(`jog ${jog.label}: blended (+${distMm} ${axis.unit}, click ${jog.clicks})`, 'note');
        jogStateEl.textContent = `${jog.label} blended ×${jog.clicks}`;
        return;
    }
    // A click that is NOT a blend spends itself entirely on stopping. It does
    // not also start the new move: the operator has to click again, on a
    // machine they can now see is stationary, to commit to the new direction.
    // Reversing under one click would mean the axis never stops between two
    // opposite moves, and the second one begins while the operator is still
    // reacting to the first. Cancel-then-go also cannot be made clean from
    // here — the soft abort (§4.5) ramps the Pico down asynchronously, so a
    // new session opened on its heels contends with a decelerating machine and
    // the motion comes out as a stutter rather than a stop.
    if (jog) {
        const prev = jog;
        prev.cancelled = true;
        prev.src.cancel();            // §4.5 soft abort — the Pico ramps down
        log(`jog ${prev.label}: cancelled by ${label} — click again to jog ${label}`, 'note');
        jogStateEl.textContent = `${prev.label} cancelling`;
        await prev.done;              // let the session settle; frees the ack sink
        return;
    }

    const src = new ClickJogSource(axis.cal, axis.letter, sign, feed, link);
    src.add(steps);
    // The demo counts operator clicks itself: ClickJogSource.clicks starts at 1
    // and the constructing add() above already bumps it, so it is not the
    // number of buttons pressed.
    const record = { src, key, label, clicks: 1, cancelled: false, done: null };

    await link.resetSeq();            // sessions stamp from 0; align the Pico
    const session = link.session(src);
    // Publish BEFORE run(): the .finally below can fire synchronously-ish on a
    // short move, and it clears `jog` — assigning afterwards would resurrect a
    // finished session as the live one.
    jog = record;
    jogStateEl.textContent = `${label} running`;
    log(`jog ${label}: ${distMm} ${axis.unit} @ ${feed} ${axis.unit}/s`, 'tx');
    renderAll();

    record.done = session.run().then(ok => {
        const r = session.result();
        const why = r.fatalReason !== undefined ? ` (${fatalReasonName(r.fatalReason)})` : '';
        log(`jog ${label}: ${ok ? 'done' : 'FAILED'}${why} — ` +
            `${record.clicks} click(s), ${src.stepsTotal} steps, ` +
            `emitted=${r.emitted} acked=${r.acked} nacks=${r.nacks} retries=${r.retries}`,
            ok ? 'ok' : 'err');
        return ok;
    }).finally(() => {
        if (jog && jog.src === src) { jog = null; jogStateEl.textContent = 'idle'; renderAll(); }
    });
}

// ── keyboard jogging ────────────────────────────────────────────────────────
//
// The radio on each row picks the axis the arrow keys drive: ← = −, → = +,
// Escape = cancel. A key press is exactly a button click — one press, one fixed
// increment.
//
// HOLDING A KEY DOES NOT REPEAT. The OS auto-repeat is discarded, so a leaned-on
// or stuck key commands one increment and no more. This mirrors the click-jog
// contract the library is built around (`clickJogSource.ts`: one click = one
// fixed distance) and keeps the operator's intent countable: every increment of
// travel corresponds to a deliberate press. Continuous motion is what the
// go-to-coordinate path is for, where the total distance is stated up front
// rather than accumulated by however long a key was down.

/** Is the operator typing? Then the arrows belong to the field, not the axis. */
function typingInField(el) {
    if (!el) return false;
    if (el.tagName === 'TEXTAREA' || el.tagName === 'SELECT') return true;
    // The row radios are inputs too, but focus lands on them the moment the
    // operator picks an axis — that focus must not disable the very keys the
    // radio just enabled.
    return el.tagName === 'INPUT' && el.type !== 'radio';
}

window.addEventListener('keydown', e => {
    if (e.key === 'Escape') { cancelJog(); return; }
    if (e.key !== 'ArrowLeft' && e.key !== 'ArrowRight') return;
    if (typingInField(document.activeElement)) return;

    const axis = axes.find(a => a.key === keyAxis);
    if (!axis) return;

    // Always preventDefault once an axis is selected: otherwise the browser's
    // own radio-group arrow navigation moves the selection out from under the
    // operator on the first key press. Done before the repeat check so a held
    // key is swallowed rather than walking the radio group.
    e.preventDefault();
    if (e.repeat) return;   // one press, one increment — see above
    if (!isConnected() || !axis.present || goTo !== null || running) return;
    if (axis.head !== undefined && axis.head !== committedHead()) return;

    onJogClick(axis, e.key === 'ArrowRight' ? +1 : -1);
});

function renderKeyHint() {
    const axis = axes.find(a => a.key === keyAxis);
    keyHint.textContent = axis
        ? `← / → jog ${axis.label} by one step · holding does not repeat · Esc cancels`
        : 'Pick an axis to drive it with ← / →.';
}

function cancelJog() {
    if (!jog) return;
    jog.cancelled = true;
    jog.src.cancel();
    jogStateEl.textContent = `${jog.label} cancelling`;
}

jogCancelBtn.addEventListener('click', cancelJog);

function renderJogPanel() {
    jogPanel.innerHTML = '';
    // A rebuild drops the radios; if the selected axis went away with a config
    // or head change, the arrow keys have nothing to drive.
    if (!axes.some(a => a.key === keyAxis)) keyAxis = null;
    renderKeyHint();
    if (!config) {
        const p = document.createElement('div');
        p.className = 'absent';
        p.textContent = 'No valid config — jog panel unavailable.';
        jogPanel.appendChild(p);
        return;
    }
    const groups = new Map();
    for (const a of axes) {
        if (!groups.has(a.group)) groups.set(a.group, []);
        groups.get(a.group).push(a);
    }
    const wrap = document.createElement('div');
    wrap.style.display = 'flex';
    wrap.style.flexDirection = 'column';
    wrap.style.gap = '8px';
    for (const [name, rows] of groups) {
        const g = document.createElement('div');
        g.className = 'head-group';
        const h = document.createElement('h3');
        h.textContent = name;
        g.appendChild(h);
        for (const a of rows) g.appendChild(jogRow(a));
        wrap.appendChild(g);
    }
    jogPanel.appendChild(wrap);
}

function jogRow(axis) {
    const row = document.createElement('div');
    row.className = 'jog-axis';

    const name = document.createElement('label');
    name.className = 'name';
    const pick = document.createElement('input');
    pick.type = 'radio';
    pick.name = 'jog-key';           // one axis owns the arrow keys at a time
    pick.dataset.pick = axis.key;
    pick.checked = keyAxis === axis.key;
    pick.addEventListener('change', () => {
        keyAxis = axis.key;
        renderKeyHint();
    });
    name.appendChild(pick);
    name.append(` ${axis.label} (slot ${axis.slot})`);
    row.appendChild(name);

    for (const sign of [-1, +1]) {
        const b = document.createElement('button');
        b.type = 'button';
        b.textContent = `${axis.label} ${sign > 0 ? '+' : '−'}`;
        b.dataset.jog = '1';
        if (!axis.present) b.dataset.absent = '1';
        if (axis.head !== undefined) b.dataset.head = String(axis.head);
        b.disabled = !axis.present || !isConnected() || goTo !== null;
        b.addEventListener('click', () => onJogClick(axis, sign));
        row.appendChild(b);
    }

    const cal = document.createElement('span');
    cal.className = 'cal';
    cal.textContent = axis.present
        ? `node ${axis.ax.node.id} · ${axis.cal.stepsPerUnit} st/${axis.unit}${axis.cal.invert ? ' · inv' : ''}`
        : 'node absent';
    row.appendChild(cal);
    return row;
}

// ── go-to queue (closed sessions, one at a time) ────────────────────────────

/**
 * One field per slot: X, Y, and the Z/A of the head chosen in this panel — NOT
 * the head that happens to be engaged. A queue entry is a plan, not an immediate
 * motion, so it may name a head that is not currently bound; `sendNext` commits
 * that head's axis_map before streaming it. That is what lets an operator stack
 * "point on head 0, point on head 1" and let the walk do the tool change.
 *
 * A blank field means "hold this axis", which is deliberately different from
 * typing its current coordinate: unspecified axes never enter the move at all.
 */
function renderGotoAxes() {
    const prev = {};
    for (const inp of gotoAxesEl.querySelectorAll('input[data-slot]')) prev[inp.dataset.axisKey] = inp.value;
    gotoAxesEl.innerHTML = '';
    if (!config) return;
    for (const a of gotoAxesFor(parseInt(gotoHead.value, 10) || 0)) {
        const wrap = document.createElement('label');
        // Same argument as describeEntry: the head dropdown sits in this row.
        wrap.textContent = `${a.letter.toUpperCase()} `;
        const inp = document.createElement('input');
        inp.type = 'number';
        inp.step = '0.1';
        inp.size = 6;
        inp.placeholder = a.present ? a.unit : 'absent';
        inp.disabled = !a.present;
        inp.dataset.slot = String(a.slot);
        inp.dataset.axisKey = a.key;
        inp.value = prev[a.key] ?? '';
        wrap.appendChild(inp);
        gotoAxesEl.appendChild(wrap);
    }
}

/** X, Y and the named head's Z/A — the four slots, in slot order. */
function gotoAxesFor(head) {
    return axes.filter(a => a.head === undefined || a.head === head)
        .sort((p, q) => p.slot - q.slot);
}

function renderGotoHead() {
    const prev = gotoHead.value;
    gotoHead.innerHTML = '';
    if (!config) return;
    config.machine.heads.forEach((h, i) => {
        const o = document.createElement('option');
        o.value = String(i);
        o.textContent = headLabel(i, ' — ').replace(/[()]/g, '');
        gotoHead.appendChild(o);
    });
    if (prev && [...gotoHead.options].some(o => o.value === prev)) gotoHead.value = prev;
    renderGotoAxes();
}

gotoHead.addEventListener('change', () => { renderGotoAxes(); renderAll(); });

gotoStack.addEventListener('click', () => {
    if (!config) return;
    const head = parseInt(gotoHead.value, 10) || 0;
    const targets = [];
    for (const inp of gotoAxesEl.querySelectorAll('input[data-slot]')) {
        if (inp.disabled || inp.value.trim() === '') continue;
        const v = parseFloat(inp.value);
        if (!Number.isFinite(v)) continue;
        targets.push({ axisKey: inp.dataset.axisKey, target: v });
    }
    if (!targets.length) { log('goto: nothing to stack — every axis field is blank', 'err'); return; }
    queue.push({
        id: ++queueSeq,
        head,
        targets,
        feedMmS: Math.abs(parseFloat(gotoFeed.value)) || 1,
        state: 'pending',
    });
    renderQueue();
});

/**
 * Scratch text form of the stack — a temporary convenience, not a language.
 * H and F are modal because that is what makes a column of bare `X.. Y..` lines
 * readable; a line carrying no axis letter is a pure state change and stacks
 * nothing. Parsing is all-or-nothing: a bad line leaves the existing queue
 * untouched rather than half-replacing it, so a typo can never send a partial
 * program.
 */
function parseGotoText(text) {
    const out = [];
    let head = parseInt(gotoHead.value, 10) || 0;
    let feed = Math.abs(parseFloat(gotoFeed.value)) || 1;
    const lines = text.split('\n');

    for (let n = 0; n < lines.length; n++) {
        const line = lines[n].replace(/;.*$/, '').trim();
        if (!line) continue;
        const targets = [];
        for (const word of line.split(/\s+/)) {
            const m = /^([HFXYZAhfxyza])(-?\d*\.?\d+)$/.exec(word);
            if (!m) throw new Error(`line ${n + 1}: cannot read "${word}"`);
            const letter = m[1].toLowerCase();
            const v = parseFloat(m[2]);
            if (letter === 'h') {
                if (!config.machine.heads[v]) throw new Error(`line ${n + 1}: no head ${v}`);
                head = v;
                continue;
            }
            if (letter === 'f') { feed = Math.abs(v) || 1; continue; }
            // Resolved left-to-right against the head in force at this word, so
            // an H later in the line does not retroactively rebind axes before it.
            const key = (letter === 'x' || letter === 'y') ? letter : `h${head}${letter}`;
            const ax = axes.find(a => a.key === key);
            if (!ax) throw new Error(`line ${n + 1}: no axis ${letter.toUpperCase()} on head ${head}`);
            if (!ax.present) throw new Error(`line ${n + 1}: axis ${ax.label} is absent`);
            targets.push({ axisKey: key, target: v });
        }
        if (targets.length) out.push({ id: 0, head, targets, feedMmS: feed, state: 'pending' });
    }
    return out;
}

gotoBuild.addEventListener('click', () => {
    if (!config) return;
    let parsed;
    try {
        parsed = parseGotoText(gotoText.value);
    } catch (e) {
        log(`goto: ${e.message} — stack left alone`, 'err');
        return;
    }
    queue = parsed.map(e => ({ ...e, id: ++queueSeq }));
    log(`goto: built ${queue.length} entr${queue.length === 1 ? 'y' : 'ies'} from text`, 'note');
    renderQueue();
});

gotoClear.addEventListener('click', () => { queue = []; renderQueue(); });
gotoNext.addEventListener('click', () => void sendNext());
gotoAll.addEventListener('click', () => void sendAll());
gotoAbort.addEventListener('click', () => { goTo?.handle.abort(); });

/** Stream the first pending entry. One at a time — a second session would
 *  correlate its ACKs against the first one's window. */
async function sendNext() {
    if (!isConnected() || goTo || jog) return false;
    const entry = queue.find(e => e.state === 'pending');
    if (!entry) return false;

    const parts = entryTargets(entry);
    if (!parts.length) { entry.state = 'failed'; log('  goto: no present axis in this entry', 'err'); renderQueue(); return false; }

    entry.state = 'running';
    renderQueue();

    // A head switch is a tool change: re-bind slots 2/3 before the move, never
    // during it — axis_map is refused while RUNNING (§6.2).
    //
    // "The session finished" is NOT "the machine stopped": run() resolves when
    // the last packet is ACKed, and the Pico is still draining its ring for a
    // while after that. So a walk that steps straight from one entry to the
    // next arrives with the machine still RUNNING and the map is rejected —
    // which is exactly why this worked when the operator clicked Send next by
    // hand (the pause between clicks was the wait) and failed under Send all.
    if (parts.some(p => p.axis.head !== undefined) && committedHead() !== entry.head) {
        log(`goto: engaging head ${entry.head} first`, 'note');
        if (!await waitIdle()) {
            log('  goto: machine did not come to rest — head switch abandoned', 'err');
            entry.state = 'failed';
            renderQueue(); renderAll();
            return false;
        }
        if (!await commitAxisMap(entry.head)) {
            entry.state = 'failed';
            renderQueue(); renderAll();
            return false;
        }
    }

    log(`goto ${describeEntry(entry)} @ ${entry.feedMmS}`, 'tx');

    let ok = false;
    try {
        await link.resetSeq();
        // One coordinated move: every named axis starts and stops together,
        // rather than a per-axis walk that would trace the move's bounding box.
        const handle = jogToPoint(
            link,
            parts.map(p => ({ axisIndex: p.axis.slot, axis: p.axis.cal, targetPos: p.target })),
            entry.feedMmS,
        );
        goTo = { handle, entry };
        renderAll();
        ok = await handle.done;
    } catch (e) {
        log(`  goto error: ${e.message}`, 'err');
    } finally {
        goTo = null;
    }
    entry.state = ok ? 'done' : 'failed';
    log(`  goto: ${ok ? 'reached' : 'failed/aborted'}`, ok ? 'ok' : 'err');
    renderQueue();
    renderAll();
    return ok;
}

/** Poll until the machine is genuinely at rest, or give up. See wire/link/settled.ts. */
const waitIdle = (timeoutMs = 5000) => ctl.waitAtRest(timeoutMs);

/** Resolve an entry's stacked targets against the live axis model. */
function entryTargets(entry) {
    return entry.targets
        .map(t => ({ target: t.target, axis: axes.find(a => a.key === t.axisKey) }))
        .filter(p => p.axis && p.axis.present);
}

function describeEntry(entry) {
    // Bare letter, not `a.label`: the head is named right there in the suffix,
    // so the Z0/A1 disambiguation the label carries is redundant here — and
    // actively misread, since `A190` looks like a value and `A1 90` is not what
    // the text form accepts either. Elsewhere (the jog panel) the label still
    // earns its index, because nothing near it says which head is meant.
    const body = entry.targets.map(t => {
        const a = axes.find(x => x.key === t.axisKey);
        return `${a ? a.letter.toUpperCase() : t.axisKey}${t.target}`;
    }).join(' ');
    const usesHead = entry.targets.some(t => axes.find(x => x.key === t.axisKey)?.head !== undefined);
    return usesHead ? `${body}  (head ${entry.head})` : body;
}

async function sendAll() {
    if (running) return;
    running = true;
    renderAll();
    try {
        while (queue.some(e => e.state === 'pending')) {
            const ok = await sendNext();
            if (!ok) break;   // stop the walk at the first failure/abort
        }
    } finally {
        running = false;
        renderAll();
    }
}

function renderQueue() {
    queueEl.innerHTML = '';
    for (const e of queue) {
        const li = document.createElement('li');
        li.dataset.state = e.state;

        const text = document.createElement('span');
        text.textContent = `${describeEntry(e)} @ ${e.feedMmS}`;
        li.appendChild(text);

        const st = document.createElement('span');
        st.className = 'st';
        st.textContent = e.state;
        li.appendChild(st);

        const del = document.createElement('button');
        del.type = 'button';
        del.textContent = '×';
        del.disabled = e.state === 'running';
        del.addEventListener('click', () => {
            queue = queue.filter(x => x.id !== e.id);
            renderQueue();
        });
        li.appendChild(del);

        queueEl.appendChild(li);
    }
    renderAll();
}

// ── peripherals (knife / vacuum) ────────────────────────────────────────────
//
// These are type-blind relays: they address a BUS ID, never a motion slot, so
// unlike everything in the jog panel they neither need nor care about the axis
// map — a knife node is reachable whichever head is engaged.
//
// They are NOT usable mid-stream. The firmware gates all five verbs on
// IDLE/PAUSED/ALARM (control_plane.cpp) because the relay blocks Core 0 on a
// Core 1 round trip that Core 1 services between microsegments — mid-cut it
// stretches a step interval and marks the material. So these buttons are for
// setup and teardown, and a running job gets its peripheral changes from the
// orchestrator at phase boundaries instead (see applyPeripherals).

/**
 * node id → a handle the job orchestrator can drive: the same relay the manual
 * buttons use, plus the card's live state readout so an orchestrated change
 * shows up in the panel rather than silently diverging from it.
 */
const periphCtl = new Map();

function renderPeriphPanel() {
    periphPanel.innerHTML = '';
    periphCtl.clear();
    // The memo describes hardware we are about to stop tracking. Keeping it
    // across a rebuild would let applyPeripherals diff away a command it still
    // owes — the knife would silently never start.
    periphCommanded.clear();
    const nodes = config ? (config.machine.peripherals ?? []).filter(n => n.present) : [];
    if (!nodes.length) {
        const p = document.createElement('div');
        p.className = 'absent';
        p.textContent = config
            ? 'No peripherals in this config — add a knife or vacuum node to peripherals[].'
            : 'Load a config to see peripheral controls.';
        periphPanel.appendChild(p);
        return;
    }
    for (const n of nodes) {
        if (n.type === NodeType.KNIFE_OSC) periphPanel.appendChild(knifeCard(n));
        else if (n.type === NodeType.VACUUM) periphPanel.appendChild(vacuumCard(n));
    }
}

/** Shared shell: a titled card with a live one-line state readout. */
function periphCard(title, node) {
    const card = document.createElement('div');
    card.className = 'periph';
    const h = document.createElement('h3');
    h.innerHTML = `${title} <span>node ${node.id}</span>`;
    const state = document.createElement('span');
    state.className = 'state';
    state.textContent = 'unknown';
    const head = document.createElement('div');
    head.className = 'row';
    head.append(h, state);
    card.appendChild(head);
    return { card, state };
}

/**
 * Send a peripheral relay and report BOTH the command and the machine's exact
 * reply line.
 *
 * The typed helpers (knifeOsc, vacPump, …) collapse the reply to a boolean,
 * which is right for a caller that just wants to know if it worked and wrong
 * for a bring-up console: `node 8 ok`, `node 8 timeout`, `err usage` and
 * `err bad_state` are four different problems and only one of them is the
 * node's fault. So this path sends the text itself and echoes it verbatim —
 * when a button "does not work", the console says which of the four it is.
 */
async function relay(state, label, cmd) {
    state.textContent = '…';
    log(`> ${cmd}`, 'tx');
    try {
        const reply = await link.command(cmd);
        const ok = /^node\s+\d+\s+ok$/.test(reply);
        log(`  ${reply}`, ok ? 'ok' : 'err');
        state.textContent = ok ? label : reply;
        return reply;
    } catch (e) {
        state.textContent = 'error';
        log(`  ${e.message}`, 'err');
        return null;
    }
}

function knifeCard(node) {
    const { card, state } = periphCard('Oscillating knife', node);

    const oscRow = document.createElement('div');
    oscRow.className = 'row';
    oscRow.appendChild(Object.assign(document.createElement('label'), { textContent: 'oscillator' }));
    for (const on of [true, false]) {
        const b = document.createElement('button');
        b.type = 'button';
        b.className = on ? 'go' : '';
        b.textContent = on ? 'On' : 'Off';
        b.dataset.periph = '1';
        b.addEventListener('click', () =>
            relay(state, `osc ${on ? 'on' : 'off'}`, `knife_osc ${node.id} ${on ? 'on' : 'off'}`));
        oscRow.appendChild(b);
    }
    card.appendChild(oscRow);

    const blowRow = document.createElement('div');
    blowRow.className = 'row';
    const duty = document.createElement('input');
    Object.assign(duty, { type: 'number', value: '50', min: '0', max: '100', step: '5', size: 4 });
    const label = document.createElement('label');
    label.textContent = 'blower % ';
    label.appendChild(duty);
    blowRow.appendChild(label);
    const set = document.createElement('button');
    set.type = 'button'; set.textContent = 'Set'; set.dataset.periph = '1';
    set.addEventListener('click', () => {
        const d = Math.max(0, Math.min(100, parseInt(duty.value, 10) || 0));
        return relay(state, `blower ${d}%`, `knife_blower ${node.id} ${d}`);
    });
    const off = document.createElement('button');
    off.type = 'button'; off.textContent = 'Blower off'; off.dataset.periph = '1';
    off.addEventListener('click', () => relay(state, 'blower 0%', `knife_blower ${node.id} 0`));
    blowRow.append(set, off);
    card.appendChild(blowRow);

    // The orchestrator drives oscillator and blower together: an oscillating
    // knife with no air packs swarf into the cut, and a blower with no
    // oscillator is just noise. The duty comes from the same field the operator
    // set by hand, so the job honours whatever they dialled in.
    periphCtl.set(node.id, {
        node,
        title: 'knife',
        wantsTool: t => t === ToolType.KNIFE,
        async set(on) {
            const d = on ? Math.max(0, Math.min(100, parseInt(duty.value, 10) || 0)) : 0;
            const a = await relay(state, `osc ${on ? 'on' : 'off'}`, `knife_osc ${node.id} ${on ? 'on' : 'off'}`);
            const b = await relay(state, `blower ${d}%`, `knife_blower ${node.id} ${d}`);
            return isNodeOk(a) && isNodeOk(b);
        },
    });
    return card;
}

const isNodeOk = reply => typeof reply === 'string' && /^node\s+\d+\s+ok$/.test(reply);

function vacuumCard(node) {
    const { card, state } = periphCard('Vacuum', node);

    const pumpRow = document.createElement('div');
    pumpRow.className = 'row';
    pumpRow.appendChild(Object.assign(document.createElement('label'), { textContent: 'pump' }));
    for (const on of [true, false]) {
        const b = document.createElement('button');
        b.type = 'button';
        b.className = on ? 'go' : '';
        b.textContent = on ? 'On' : 'Off';
        b.dataset.periph = '1';
        b.addEventListener('click', () =>
            relay(state, `pump ${on ? 'on' : 'off'}`, `vac_pump ${node.id} ${on ? 'on' : 'off'}`));
        pumpRow.appendChild(b);
    }
    const read = document.createElement('button');
    read.type = 'button'; read.textContent = 'Read switch'; read.dataset.periph = '1';
    read.addEventListener('click', async () => {
        // A read, not an ok: the reply carries the level, so relay()'s
        // `node <id> ok` test would call every successful read a failure.
        state.textContent = '…';
        const cmd = `vac_switch ${node.id}`;
        log(`> ${cmd}`, 'tx');
        try {
            const reply = await link.command(cmd);
            const m = /^node\s+\d+\s+switch\s+(open|closed)/.exec(reply);
            log(`  ${reply}`, m ? 'ok' : 'err');
            state.textContent = m ? `switch ${m[1]}` : reply;
        } catch (e) {
            state.textContent = 'error';
            log(`  ${e.message}`, 'err');
        }
    });
    pumpRow.appendChild(read);
    card.appendChild(pumpRow);

    // Servo channels: the node carries several; 0 and 1 are enough to drive a
    // zone valve pair without turning this into a config-driven table.
    for (const idx of [0, 1]) {
        const row = document.createElement('div');
        row.className = 'row';
        row.appendChild(Object.assign(document.createElement('label'), { textContent: `servo ${idx}` }));
        for (const on of [true, false]) {
            const b = document.createElement('button');
            b.type = 'button';
            b.textContent = on ? 'On' : 'Off';
            b.dataset.periph = '1';
            b.addEventListener('click', () =>
                relay(state, `servo ${idx} ${on ? 'on' : 'off'}`, `vac_servo ${node.id} ${idx} ${on ? 'on' : 'off'}`));
            row.appendChild(b);
        }
        card.appendChild(row);
    }

    // Hold-down is not tool-specific — anything that cuts or draws wants the
    // sheet held flat, so the pump follows the JOB, not the phase (wantsTool
    // is absent; applyPeripherals reads that as "on for the whole run").
    periphCtl.set(node.id, {
        node,
        title: 'vacuum pump',
        async set(on) {
            return isNodeOk(await relay(state, `pump ${on ? 'on' : 'off'}`, `vac_pump ${node.id} ${on ? 'on' : 'off'}`));
        },
    });
    return card;
}

// ── job runner (SVG → plan → schedule/walk → stream) ────────────────────────
//
// Ported from bench.js, which owned the only working job path. Two deliberate
// differences, both because this demo has a committed axis map and bench.js
// did not:
//
//   · The config is the one loaded at the top of the page, not an editable
//     textarea. There is already one config here deciding which axes exist and
//     what axis_map binds; a second, differently-edited copy driving the job
//     stream is how the jog panel and the job end up on different machines.
//   · A tool swap re-commits the incoming head's axis_map. On a dual-head
//     machine the swap IS a slot rebind — resuming without it streams the new
//     head's Z/A at the old head's motors.

/** Mounts carry ToolType numbers; operators read tool names. */
const toolName = t => TOOL_PROFILES_BY_TYPE[t]?.name ?? `tool ${t}`;



/**
 * Compile SVG → walk events against the loaded config.
 *
 * `initialState` seeds the walk's starting XY/A in TRUE (pre-invert) steps.
 * Omitted, the walk starts from the origin — the dry run. Run passes the live
 * position so the lead-in jog starts where the head actually is, which is what
 * makes back-to-back runs repeat in place instead of drifting.
 */
function compileJob(initialState) {
    if (!config) throw new Error('no config loaded');
    if (!svgText) throw new Error('select an SVG first');
    // The mounts we bake AGAINST are what is screwed in right now, so the
    // scheduler minimises operator swaps rather than reproducing the config's
    // preferred arrangement. bakePlan schedules before it compiles — the head
    // decides step counts, so mm cannot become steps until each block's socket
    // is known — and hands back the phases it used, so nothing downstream
    // recomputes them and disagrees.
    const { blocks, phases } = bakePlan(config, svgText, {
        defaultTool: jobTool.value.trim() || undefined,
        mounts: ctl ? mountedTypes(ctl.setup) : undefined,
    });

    // The scheduler never reorders blocks, so a job that alternates tools
    // produces the same number of phase boundaries either way — with one head
    // each is an operator swap, with two the walk emits a `rebind` event and
    // runWalk does it in under a second.
    const events = walkSchedule(phases, blocks, config.machine,
        initialState ? { initialState } : {});
    return { blocks, phases, events };
}

// ── peripheral orchestration ────────────────────────────────────────────────
//
// The bus is the stream. While a job is RUNNING, Core 0 cannot relay a
// peripheral command without stealing time from Core 1 between microsegments,
// so the firmware refuses one (`err bad_state`) — clicking "oscillator on"
// mid-cut simply does not work, and the fix is not to remove the gate but to
// stop needing it.
//
// A job already has boundaries where the bus is free: the machine is IDLE or
// PAUSED at every phase edge, because the last segment before a swap carries
// MICRO_PAUSE and the runner waits for the state before continuing. That is
// where peripheral state belongs, and the pause event now carries the phase's
// mount set (walk.ts) so the runner knows what is about to cut.
//
// The policy is derived, not operated: a knife phase runs the oscillator and
// blower; the vacuum runs for the whole job. Nobody has to remember to switch
// the knife on, and — more to the point — nobody can leave it on through a pen
// phase.

/** Commanded state per node, so a phase boundary only sends what changed. */
const periphCommanded = new Map();

/**
 * Firmware parks the WHOLE bus on estop and on soft reset (busDisableAll in
 * core1/core1.cpp) — steppers de-energise, the pump stops, the oscillator and
 * blower die. That silently invalidates the memo above: it still says "knife
 * on" while the hardware is off, so the next applyPeripherals would diff the
 * command away and the blade would drag through material cold.
 *
 * Same failure the panel rebuild guards against, reached by a different route.
 * Forget once on entry, not every poll, so a machine sitting in ALARM does not
 * clear a memo the operator has since rebuilt by hand from the panel.
 */
let parkedSeen = false;
function notePeripheralPark(state) {
    const parked = state === MachineState.ESTOP || state === MachineState.ALARM;
    if (parked && !parkedSeen) periphCommanded.clear();
    parkedSeen = parked;
}

/**
 * Reconcile every peripheral against `mount`, the tool set for the phase about
 * to run. `mount` of null means teardown — everything off.
 *
 * Throws on refusal. A knife that did not start is not a cosmetic failure: the
 * next thing that happens is a blade dragging through material it cannot cut.
 */
async function applyPeripherals(mount, why) {
    for (const c of periphCtl.values()) {
        // No wantsTool → not tool-specific (the vacuum): on for the whole job.
        const on = mount !== null && (c.wantsTool ? mount.some(c.wantsTool) : true);
        if (periphCommanded.get(c.node.id) === on) continue;
        log(`job: ${c.title} ${on ? 'on' : 'off'} — ${why}`, 'note');
        if (!await c.set(on)) throw new Error(`${c.title} (node ${c.node.id}) refused — see the reply above`);
        periphCommanded.set(c.node.id, on);
    }
}

/**
 * Teardown that must not mask the error that caused it. Used from the run
 * loop's finally, where throwing would replace a real stream failure with a
 * peripheral one.
 */
async function shutdownPeripherals() {
    try {
        await applyPeripherals(null, 'job over');
    } catch (e) {
        log(`job: could not stop peripherals — ${e.message}`, 'err');
        log('  the gate only passes IDLE/PAUSED/ALARM; after an e-stop, unalarm then use the panel', 'err');
    }
}

/** machinePos is post-invert per axis; the walk wants TRUE steps. */
async function liveInitialState() {
    const [x, y, , a] = await getPos(link);
    const m = config.machine;
    return {
        posX: m.x.invert ? -x : x,
        posY: m.y.invert ? -y : y,
        aPhys: m.heads[0].a.invert ? -a : a,
    };
}

function renderJobMetrics(blocks, events) {
    const segs = motionSegments(events);
    const jogs = segs.filter(s => s.flags & MICRO_JOG).length;
    const lifts = segs.filter(s => s.flags & MICRO_LIFT).length;
    const secs = walkSeconds(events, config.machine.fCpu);
    // Duty breaks are baked into the segments, so they are invisible in the
    // event list — an operator compiling a knife job wants to know how many
    // resets it will stop for before they start it.
    const duty = segs.filter(s => s.flags & MICRO_DUTY_RELEASE).length;
    jobMetrics.textContent =
        `${blocks.length} block(s)  ${segs.length.toLocaleString()} segments\n` +
        `cut ${segs.length - jogs - lifts}  jog ${jogs}  lift ${lifts}  ` +
        `pauses ${events.filter(e => e.kind === 'pause').length}` +
        (duty ? `  duty breaks ${duty}` : '') + '\n' +
        `est run ${secs.toFixed(2)} s\n` +
        blocks.map((b, i) => `  block ${i + 1}: ${b.profile.name} on head ${b.head}  ${b.segments.length} segs`).join('\n');
}

jobSvg.addEventListener('change', async () => {
    const file = jobSvg.files?.[0];
    if (!file) return;
    svgName = file.name.replace(/\.svg$/i, '');
    svgText = await file.text();
    jobMetrics.textContent = `${file.name} loaded — Compile to see the plan.`;
    renderAll();
});

jobCompile.addEventListener('click', () => {
    try {
        const { blocks, events } = compileJob();
        renderJobMetrics(blocks, events);
        log(`job: compiled ${motionSegments(events).length} segments from ${svgName}.svg`, 'ok');
    } catch (e) {
        jobMetrics.textContent = e.message;
        log(`job compile failed: ${e.message}`, 'err');
    }
    renderAll();
});

jobStop.addEventListener('click', async () => {
    // Fire-and-forget: estop correlates nothing and must never queue behind a
    // pending text command.
    try { await ctl.estop(); log('job: STOP sent', 'err'); } catch { /* closing */ }
});

/**
 * Run the compiled walk.
 *
 * Everything about *when* to send what — batching, the MICRO_PAUSE stamp before
 * a swap, cutting a batch at a baked duty break, waiting for the machine rather
 * than for the writer, and rebinding the axis map when a swap changes heads —
 * lives in `runWalk`. It is the same logic this file used to carry inline, and
 * none of it was demo-specific.
 *
 * What stays here is the part that genuinely is: which node runs the knife
 * oscillator, whether the vacuum belongs to the job, and what to put in front
 * of the operator at a tool change.
 */
jobRun.addEventListener('click', async () => {
    if (running || jog || goTo || !isConnected() || !config) return;

    let blocks, phases, events;
    try {
        const initial = await liveInitialState();
        log(`job: head at x${initial.posX} y${initial.posY} steps — compiling`, 'note');
        ({ blocks, phases, events } = compileJob(initial));
        renderJobMetrics(blocks, events);
    } catch (e) {
        log(`job compile failed: ${e.message}`, 'err');
        return;
    }

    running = true;
    jobProgress.style.width = '0';
    renderAll();

    try {
        await runWalk(ctl, events, {
            initialMount: phases[0]?.mounts ?? [],
            onPhase: (mount, why) => applyPeripherals(mount, why),
            onDutyBreak: handleDutyBreak,
            confirmSwap: confirmSwap,
            onProgress: (sent, total) => {
                jobProgress.style.width = `${(sent / total * 100).toFixed(1)}%`;
            },
            onLog: (m, kind) => log(`job: ${m}`, kind),
        });
        jobProgress.style.width = '100%';
    } catch (e) {
        log(`job: ${e.message}`, 'err');
    } finally {
        await shutdownPeripherals();
        running = false;
        renderAll();
    }
});

/**
 * A baked duty break (docs/tool_duty_limits.md §9). The machine is PAUSED at a
 * lift, the blade is clear of the material, and the bus is free — the one
 * window in a job where a peripheral relay is allowed.
 *
 * Release, wait out the dwell, re-assert. In this version both markers sit on
 * the same segment, so the settle before the plunge is a wait here rather than
 * something the pipeline guaranteed geometrically; splitting the markers across
 * the off-window is the follow-up that makes it free.
 *
 * A failed re-assert ABORTS the job. Resuming would drive an unpowered blade
 * back into the workpiece, which is the exact failure this whole mechanism
 * exists to prevent — a logged warning is not good enough here.
 */
async function handleDutyBreak(mount) {
    // The LOADED profiles, not TOOL_PROFILES_BY_TYPE. dutyLimits is config, not
    // preset — the catalogue entry for 'knife' carries none, because not every
    // knife is an ultrasonic one. Reading the presets here finds a knife with no
    // limits and throws on a stream that legitimately contains a break.
    const profile = mount
        .map(t => Object.values(config.toolProfiles).find(p => p.toolType === t))
        .find(p => p?.dutyLimits);
    if (!profile) {
        throw new Error('duty break in the stream, but no live tool declares dutyLimits');
    }
    const d = profile.dutyLimits;
    const ctl = [...periphCtl.values()].find(c => c.wantsTool?.(profile.toolType));
    if (!ctl) {
        throw new Error(`duty break for '${profile.name}', but no peripheral node claims that tool`);
    }

    log(`job: duty break — ${ctl.title} off for ${d.dwellS}s`, 'note');
    if (!await ctl.set(false)) throw new Error(`${ctl.title}: release failed`);
    periphCommanded.set(ctl.node.id, false);

    await new Promise(r => setTimeout(r, d.dwellS * 1000));

    if (!await ctl.set(true)) {
        throw new Error(`${ctl.title}: re-assert failed — aborting rather than plunging a dead tool`);
    }
    periphCommanded.set(ctl.node.id, true);
    if (d.settleS > 0) await new Promise(r => setTimeout(r, d.settleS * 1000));
}

/**
 * Put the swap in front of the operator.
 *
 * Returning true is a claim, not evidence: runWalk re-checks the mount table
 * against the phase the moment this resolves and throws if the tool went into
 * the wrong socket. The handler must actually UPDATE the mount table — saying
 * OK is no longer enough.
 */
function confirmSwap(req) {
    const swapIn = req.swapIn.map(toolName).join(', ') || 'nothing';
    const swapOut = req.swapOut.map(toolName).join(', ') || 'nothing';
    log(`job: tool swap — mount ${swapIn}, remove ${swapOut}`, 'note');
    log(`  phase wants ${req.mounts.map((t, h) => `head ${h}: ${t === null ? 'empty' : toolName(t)}`).join(', ')}`, 'note');
    const ok = window.confirm(
        `Tool swap\n\nmount: ${swapIn}\nremove: ${swapOut}\n\nOK when the head is ready.`,
    );
    if (!ok) return false;

    // Record what the operator just did. runWalk re-checks the mount table the
    // moment this returns, so a handler that only said "OK" gets refused — and
    // rightly: nothing else in the system would know a tool had moved.
    req.mounts.forEach((t, head) => {
        ctl.mount(head, t === null ? null : TOOL_PROFILES_BY_TYPE[t] ?? null);
    });
    renderAll();
    return true;
}

// ── render gating ───────────────────────────────────────────────────────────

function renderAll() {
    const on  = isConnected();
    const cfg = config !== null;
    const busy = jog !== null || goTo !== null || homing !== null;

    connectBtn.disabled    = on;
    disconnectBtn.disabled = !on;
    backendSel.disabled    = on;
    baudInput.disabled     = on;

    cmdInput.disabled = !on;
    sendBtn.disabled  = !on;
    for (const b of quickCmds.querySelectorAll('button')) b.disabled = !on;
    pollOnceBtn.disabled = !on;

    jogCancelBtn.disabled = jog === null;
    // Presence is decided once at build time (data-absent); connection and an
    // in-flight go-to are re-gated on every render.
    const engaged = committedHead();
    // The arrow-key radio follows the same rule as the buttons — an axis you
    // cannot click is not one the keyboard should reach either.
    for (const r of jogPanel.querySelectorAll('input[data-pick]')) {
        const a = axes.find(x => x.key === r.dataset.pick);
        const wrong = a && a.head !== undefined && a.head !== engaged;
        r.disabled = !a || !a.present || wrong;
        if (r.disabled && r.checked) { r.checked = false; keyAxis = null; renderKeyHint(); }
    }
    for (const b of jogPanel.querySelectorAll('button[data-jog]')) {
        // A row for a head that is not engaged is dead: slots 2/3 are bound to
        // the OTHER head's nodes, so the click would move that head instead.
        const rowHead = b.dataset.head === undefined ? null : parseInt(b.dataset.head, 10);
        const wrongHead = rowHead !== null && rowHead !== engaged;
        b.disabled = b.dataset.absent === '1' || wrongHead || !on || goTo !== null || running;
    }
    mapCommit.disabled = !on || !cfg;

    // A row for a head that is not engaged is dead for the same reason the jog
    // rows are: slots 2/3 are bound to the OTHER head's motors, so homing "Z"
    // would drive the wrong one. Commit that head's map first.
    for (const cb of homeAxesEl.querySelectorAll('input[data-home]')) {
        const a = axes.find(x => x.key === cb.dataset.home);
        cb.disabled = !a || (a.head !== undefined && a.head !== engaged);
    }
    const homeSel = cfg ? selectedHomeAxes().length : 0;
    // Dry run works offline — it is arithmetic on the config, and checking the
    // numbers before anything moves is most of its value.
    homeDryBtn.disabled  = !cfg || homeSel === 0;
    homeRunBtn.disabled  = !on || !cfg || homeSel === 0 || busy || running ||
                           committedHead() === null;
    homeStopBtn.disabled = homing === null;

    gotoStack.disabled = !cfg;
    gotoBuild.disabled = !cfg;
    const hasPending = queue.some(e => e.state === 'pending');
    gotoNext.disabled  = !on || !cfg || !hasPending || busy || running;
    gotoAll.disabled   = !on || !cfg || !hasPending || busy || running;
    gotoAbort.disabled = goTo === null;
    gotoClear.disabled = queue.length === 0;

    // Compiling needs only a config and an SVG — it is a dry run and works
    // offline. Running needs the machine, and needs it to itself.
    // Peripherals follow the firmware gate: refused while RUNNING, so offering
    // the buttons mid-job would only produce `err bad_state`. During a job the
    // orchestrator owns them and changes them at phase boundaries.
    for (const b of periphPanel.querySelectorAll('button[data-periph]')) b.disabled = !on || running;

    jobCompile.disabled = !cfg || !svgText;
    jobRun.disabled     = !on || !cfg || !svgText || busy || running;
    jobStop.disabled    = !on;
    mapCommit.disabled  = mapCommit.disabled || running;

    renderLinkStats();
}

// ── boot ────────────────────────────────────────────────────────────────────

renderQueue();
log('Pick a backend and connect. Sim needs no hardware.', 'note');
loadConfigFrom(configUrl.value.trim());
