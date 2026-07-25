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
    SimTransport,
    ClickJogSource,
    jogToPoint,
    axisMap,
    readAxisMap,
    MachineState,
    AlarmReason,
    RunningReason,
    fatalReasonName,
    bakePlan,
    scheduleMounts,
    walkSchedule,
    packMicrosegment,
    getPos,
    MICRO_JOG,
    MICRO_LIFT,
    MICRO_PAUSE,
    TOOL_PROFILES_BY_TYPE,
    NodeType,
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

const gotoAxesEl   = $('goto-axes');
const gotoHead     = $('goto-head');
const gotoFeed     = $('goto-feed');
const gotoStack    = $('goto-stack');
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
const statusBody   = $('status-table').querySelector('tbody');
const statusBanner = $('status-banner');
const linkBody     = $('link-table').querySelector('tbody');

// ── state ───────────────────────────────────────────────────────────────────

let config  = null;   // PipelineConfig, or null if the config did not load
let axes    = [];     // gated axis descriptors built from the config
let link    = null;
let pollTimer = null;

/** The live click jog, if any: { src, key, label, done }. */
let jog = null;
/** The live go-to, if any: { handle, entry }. */
let goTo = null;

/** Which axis row owns the arrow keys (axis.key), or null. */
let keyAxis = null;
/** The map read back from the Pico, or null. Slot i → bus id (null = unbound). */
let committedMap = null;
/** The head whose Z/A this host last bound to slots 2/3. */
let activeHead = 0;

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
function buildAxes(cfg) {
    const m = cfg.machine;
    const out = [
        { key: 'x', letter: 'x', slot: 0, label: 'X', group: 'Gantry', ax: m.x },
        { key: 'y', letter: 'y', slot: 1, label: 'Y', group: 'Gantry', ax: m.y },
    ];
    m.heads.forEach((h, i) => {
        const tag = `head ${i}${h.profile ? ` (${h.profile.name})` : ''}`;
        out.push({ key: `h${i}z`, letter: 'z', slot: 2, label: `Z${i}`, group: tag, head: i, ax: h.z });
        out.push({ key: `h${i}a`, letter: 'a', slot: 3, label: `A${i}`, group: tag, head: i, ax: h.a });
    });
    return out.map(a => ({
        ...a,
        present: !!a.ax.node.present,
        cal: { stepsPerUnit: a.ax.stepsPerUnit, invert: !!a.ax.invert },
        unit: a.ax.rotary ? 'deg' : 'mm',
    }));
}

/** The config decides which panels exist at all — rebuild them, then re-gate. */
function rebuildFromConfig() {
    renderHeadSel();
    renderJogPanel();
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
        (loaded.warnings.length ? `\nwarnings:\n${loaded.warnings.join('\n')}` : '');
    for (const w of loaded.warnings) console.warn('[config]', w);
    rebuildFromConfig();
}

reloadCfgBtn.addEventListener('click', () => loadConfigFrom(configUrl.value.trim()));

// ── axis map ────────────────────────────────────────────────────────────────

/**
 * The four bus ids this config wants bound to slots X/Y/Z/A, for `head`.
 * Z and A come from the selected head — that is the whole point of the map:
 * both heads' nodes exist on the bus, but only one pair is engaged at a time.
 * An absent node binds as null (slot left disengaged).
 */
function desiredMap(head) {
    const m = config.machine;
    const h = m.heads[head];
    const id = ax => (ax && ax.node.present ? ax.node.id : null);
    return [id(m.x), id(m.y), id(h?.z), id(h?.a)];
}

/**
 * Commit the map for the selected head and report the result.
 *
 * This is the first thing to do after connecting. Until a map commits, the Pico
 * sits in ALARM/ALARM_CONFIG and NACKs every job, jog and debug-step — the map
 * is host-authored and never appears in STATUS_RSP, so the host must re-assert
 * it on every connect (docs/engage_and_axis_map.md §8). Re-issuing the same map
 * is close to a no-op on the wire but deliberately re-sends every engage, so a
 * node that lost its slot is re-bound.
 *
 * Valid in IDLE/PAUSED/ALARM only — a head switch belongs at the PAUSED
 * tool-change boundary, never mid-RUNNING.
 */
async function commitAxisMap(head) {
    if (!isConnected() || !config) return false;
    const [x, y, z, a] = desiredMap(head);
    log(`> axis_map ${[x, y, z, a].map(v => v ?? '-').join(' ')}   (head ${head})`, 'tx');
    try {
        await axisMap(link, x, y, z, a);
        activeHead = head;
        log(`  ok — slots bound for head ${head}`, 'ok');
    } catch (e) {
        log(`  ${e.message}`, 'err');
        return false;
    }
    await refreshAxisMap();
    renderAll();
    return true;
}

/** Read the committed map back — the only way to observe it (§8). */
async function refreshAxisMap() {
    if (!isConnected()) { committedMap = null; renderAxisMap(); return; }
    try {
        committedMap = await readAxisMap(link);
    } catch (e) {
        committedMap = null;
        log(`axis_map read failed: ${e.message}`, 'err');
    }
    renderAxisMap();
}

/** Which head, if any, the committed map matches. null = neither/unbound. */
function committedHead() {
    if (!committedMap || !config) return null;
    for (let i = 0; i < config.machine.heads.length; i++) {
        const want = desiredMap(i);
        if (want.every((v, k) => v === committedMap[k])) return i;
    }
    return null;
}

function renderHeadSel() {
    headSel.innerHTML = '';
    if (!config) return;
    config.machine.heads.forEach((h, i) => {
        const o = document.createElement('option');
        o.value = String(i);
        o.textContent = `head ${i}${h.profile ? ` — ${h.profile.name}` : ''}`;
        headSel.appendChild(o);
    });
    headSel.value = String(Math.min(activeHead, config.machine.heads.length - 1));
}

mapCommit.addEventListener('click', () => void commitAxisMap(parseInt(headSel.value, 10)));
headSel.addEventListener('change', renderAll);

function renderAxisMap() {
    const shown = committedMap
        ? committedMap.map(v => (v === null ? '—' : v)).join('  ')
        : '— — — —';
    const h = committedHead();
    mapLine.textContent = `slots X Y Z A = ${shown}` +
        (h !== null ? `  (head ${h})` : committedMap ? '  (no head matches)' : '');
    mapLine.dataset.kind = h !== null ? 'ok' : 'idle';
}

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
        log(`connected via ${backendSel.value}`, 'ok');
        startPolling();
        // §8: the map is host-authored and absent from STATUS_RSP, so a fresh
        // connection must re-assert it — the Pico may be holding a map from a
        // previous host, or (on a cold boot) none at all, in which case it is
        // sitting in ALARM_CONFIG refusing all motion.
        await refreshAxisMap();
        if (config) await commitAxisMap(parseInt(headSel.value, 10) || 0);
    } catch (e) {
        log(`connect failed: ${e.message}`, 'err');
    }
    renderAll();
});

disconnectBtn.addEventListener('click', async () => {
    stopPolling();
    cancelJog();
    if (link) {
        try { await link.close(); } catch (e) { log(`close: ${e.message}`, 'err'); }
    }
    link = null;
    committedMap = null;
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

const STATE_NAMES   = invert(MachineState);
const ALARM_NAMES   = invert(AlarmReason);
const RUNNING_NAMES = invert(RunningReason);

function invert(enumObj) {
    const out = {};
    for (const [k, v] of Object.entries(enumObj)) out[v] = k;
    return out;
}

const maskStr = m => ['x', 'y', 'z', 'a'].filter((_, i) => m & (1 << i)).join('') || '—';

function startPolling() {
    stopPolling();
    if (!pollAuto.checked) return;
    const ms = Math.max(100, parseInt(pollMs.value, 10) || 500);
    pollTimer = setInterval(pollStatus, ms);
}

function stopPolling() {
    if (pollTimer) { clearInterval(pollTimer); pollTimer = null; }
}

pollAuto.addEventListener('change', () => (isConnected() ? startPolling() : stopPolling()));
pollMs.addEventListener('change', () => { if (isConnected()) startPolling(); });
pollOnceBtn.addEventListener('click', pollStatus);

/**
 * One STATUS_REQ → STATUS_RSP round trip. Safe to run during a stream: the
 * demux routes the reply to the status sink by magic, so it never lands in the
 * ack sink the session is awaiting, and the open jog source reads the same
 * samples for pacing.
 */
async function pollStatus() {
    if (!isConnected()) return;
    try {
        const st = await link.getStatus(500);
        renderStatus(st);
        statusBanner.dataset.kind = 'ok';
        statusBanner.textContent = `last poll ${new Date().toLocaleTimeString()}`;
    } catch (e) {
        statusBanner.dataset.kind = 'error';
        statusBanner.textContent = e.message;
    }
    renderLinkStats();
}

function renderStatus(st) {
    const mmOf = slot => {
        const a = axes.find(x => x.slot === slot && x.present);
        if (!a || !st.pos) return '';
        const inv = a.cal.invert ? -1 : 1;
        return `  (${(inv * st.pos[slot] / a.cal.stepsPerUnit).toFixed(3)} ${a.unit})`;
    };
    const rows = [
        ['state',       `${STATE_NAMES[st.state] ?? st.state}`],
        ['running',     `${RUNNING_NAMES[st.running] ?? st.running}`],
        ['alarm',       `${ALARM_NAMES[st.alarm] ?? st.alarm}`],
        ['axesHomed',   `0x${st.axesHomed.toString(16)} ${maskStr(st.axesHomed)}`],
        ['axesEnabled', `0x${st.axesEnabled.toString(16)} ${maskStr(st.axesEnabled)}`],
        ['bufCount',    fmt(st.bufCount)],
        ['expectedSeq', fmt(st.expectedSeq)],
        ['queuedUs',    st.queuedUs === undefined ? '—' : `${st.queuedUs} (${(st.queuedUs / 1000).toFixed(1)} ms)`],
        ['pos x',       st.pos ? `${st.pos[0]}${mmOf(0)}` : '—'],
        ['pos y',       st.pos ? `${st.pos[1]}${mmOf(1)}` : '—'],
        ['pos z',       st.pos ? `${st.pos[2]}${mmOf(2)}` : '—'],
        ['pos a',       st.pos ? `${st.pos[3]}${mmOf(3)}` : '—'],
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
        wrap.textContent = `${a.label} `;
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
        o.textContent = `head ${i}${h.profile ? ` — ${h.profile.name}` : ''}`;
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

/**
 * Poll until the machine is genuinely at rest — IDLE with an empty ring — or
 * give up. Needed before any command the firmware gates on state, since the
 * host's view of "done" runs ahead of the Pico's by the depth of its buffer.
 */
async function waitIdle(timeoutMs = 5000) {
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
        const st = await link.getStatus();
        if (st.state !== MachineState.RUNNING && !st.bufCount) return true;
        await new Promise(r => setTimeout(r, 50));
    }
    return false;
}

/** Resolve an entry's stacked targets against the live axis model. */
function entryTargets(entry) {
    return entry.targets
        .map(t => ({ target: t.target, axis: axes.find(a => a.key === t.axisKey) }))
        .filter(p => p.axis && p.axis.present);
}

function describeEntry(entry) {
    const body = entry.targets.map(t => {
        const a = axes.find(x => x.key === t.axisKey);
        return `${a ? a.label : t.axisKey}${t.target}`;
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
// They are also the one control surface deliberately live DURING a job. The
// firmware's IDLE/PAUSED/ALARM gate on all five verbs is commented out
// (control_plane.cpp) precisely so the operator can work the oscillator, blower
// and vacuum while a cut is running, so these buttons stay enabled while
// `running` is true — every other motion control here is disabled then.

function renderPeriphPanel() {
    periphPanel.innerHTML = '';
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
    return card;
}

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

/** Which head socket holds each tool type, read off the config's head profiles. */
function headAssignment() {
    const m = new Map();
    config.machine.heads.forEach((h, i) => { if (h.profile) m.set(h.profile.toolType, i); });
    return m;
}

/** MountSets carry ToolType numbers; operators read tool names. */
const toolName = t => TOOL_PROFILES_BY_TYPE[t]?.name ?? `tool ${t}`;

const motionSegments = events => events.filter(e => e.kind === 'motion').flatMap(e => e.segments);

/**
 * Sum segment durations → seconds at fCpu, mirroring core1.cpp's
 * emitMicroSegment: a segment runs `major` steps waiting `interval` cycles
 * each, so summing interval alone would treat every segment as one step.
 */
function estimateSeconds(events, fCpu) {
    let cycles = 0;
    for (const e of events) {
        if (e.kind !== 'motion') continue;
        for (const s of e.segments) {
            cycles += s.interval * Math.max(Math.abs(s.dx), Math.abs(s.dy), Math.abs(s.dz), Math.abs(s.da));
        }
    }
    return cycles / fCpu;
}

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
    const { plan } = bakePlan(config, svgText, { defaultTool: jobTool.value.trim() || undefined });

    // ONE tool per phase, even on a dual-head machine — deliberately not
    // `heads.length`.
    //
    // With both heads mounted, the walk switches heads at a BLOCK boundary
    // inside a single phase, and a WalkEvent of kind "motion" does not say
    // which head its segments belong to. The axis map therefore cannot follow
    // the switch, and the second block's Z/A would drive the first head's
    // motors. Scheduling one tool at a time forces every head change to become
    // a pause, which is a boundary this demo CAN rebind at (see handleSwap).
    //
    // The cost is real: a dual-head machine gives up its whole advantage and
    // swaps as often as a single-head one. Lifting it means teaching WalkEvent
    // to carry the head index, at which point this becomes heads.length.
    const schedule = scheduleMounts(plan, 1);
    const events = walkSchedule(schedule, plan, config.machine, {
        headAssignment: headAssignment(),
        ...(initialState ? { initialState } : {}),
    });
    return { plan, events };
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

function renderJobMetrics(plan, events) {
    const segs = motionSegments(events);
    const jogs = segs.filter(s => s.flags & MICRO_JOG).length;
    const lifts = segs.filter(s => s.flags & MICRO_LIFT).length;
    const secs = estimateSeconds(events, config.machine.fCpu);
    jobMetrics.textContent =
        `${plan.blocks.length} block(s)  ${segs.length.toLocaleString()} segments\n` +
        `cut ${segs.length - jogs - lifts}  jog ${jogs}  lift ${lifts}  ` +
        `pauses ${events.filter(e => e.kind === 'pause').length}\n` +
        `est run ${secs.toFixed(2)} s\n` +
        plan.blocks.map((b, i) => `  block ${i + 1}: ${b.profile.name}  ${b.segments.length} segs`).join('\n');
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
        const { plan, events } = compileJob();
        renderJobMetrics(plan, events);
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
    try { await link.send('stop'); log('job: STOP sent', 'err'); } catch { /* closing */ }
});

/** Poll until the machine reaches `target`, or throw if it alarms on the way. */
async function waitForState(target) {
    for (;;) {
        const st = await link.getStatus();
        if (st.state === target) return;
        if (st.state === MachineState.ESTOP || st.state === MachineState.ALARM) {
            throw new Error(`machine went ${STATE_NAMES[st.state] ?? st.state}`);
        }
        await new Promise(r => setTimeout(r, 150));
    }
}

jobRun.addEventListener('click', async () => {
    if (running || jog || goTo || !isConnected() || !config) return;

    let plan, events;
    try {
        const initial = await liveInitialState();
        log(`job: head at x${initial.posX} y${initial.posY} steps — compiling`, 'note');
        ({ plan, events } = compileJob(initial));
        renderJobMetrics(plan, events);
    } catch (e) {
        log(`job compile failed: ${e.message}`, 'err');
        return;
    }

    // Pre-flight. A non-IDLE machine NACKs every packet with BAD_STATE, which
    // is a far more confusing failure than refusing up front.
    const pre = await link.getStatus();
    if (pre.state !== MachineState.IDLE && pre.state !== MachineState.PAUSED) {
        log(`job: machine is ${STATE_NAMES[pre.state] ?? pre.state} — unalarm or commit an axis_map first`, 'err');
        return;
    }

    running = true;
    jobProgress.style.width = '0';
    renderAll();

    const total = motionSegments(events).length;
    let sent = 0;

    try {
        let i = 0, machinePaused = false;
        while (i < events.length) {
            if (events[i].kind === 'pause') {
                const ev = events[i];
                if (!await handleSwap(ev)) throw new Error('cancelled by operator at the tool swap');
                if (machinePaused) {
                    const reply = await link.command('resume');
                    log(`job: resume → ${reply}`, reply === 'ok' ? 'ok' : 'err');
                    machinePaused = false;
                }
                i++;
                continue;
            }

            const batch = [];
            while (i < events.length && events[i].kind === 'motion') { batch.push(...events[i].segments); i++; }
            if (!batch.length) continue;

            // The last segment before a swap carries MICRO_PAUSE, so the machine
            // parks itself in PAUSED rather than running on into the swap.
            const nextIsPause = i < events.length && events[i].kind === 'pause';
            if (nextIsPause) {
                const last = batch[batch.length - 1];
                batch[batch.length - 1] = { ...last, flags: last.flags | MICRO_PAUSE };
                machinePaused = true;
            }

            log(`job: streaming ${batch.length} segments`, 'tx');
            const result = await link.stream(batch.map((s, n) => packMicrosegment(s, n & 0xff)), 16);
            if (!result.ok) {
                const why = result.fatalReason !== undefined ? fatalReasonName(result.fatalReason) : 'unknown';
                throw new Error(`stream failed: ${why} — emitted ${result.emitted} acked ${result.acked} nacks ${result.nacks}`);
            }

            sent += batch.length;
            jobProgress.style.width = `${(sent / total * 100).toFixed(1)}%`;
            await waitForState(nextIsPause ? MachineState.PAUSED : MachineState.IDLE);
        }
        jobProgress.style.width = '100%';
        log(`job: done — ${sent} segments streamed`, 'ok');
    } catch (e) {
        log(`job: ${e.message}`, 'err');
    } finally {
        running = false;
        renderAll();
    }
});

/**
 * A tool swap. On a dual-head machine this is also a slot rebind: the walk has
 * switched to the head holding the incoming tool, so the map must follow before
 * any more motion streams — otherwise the new head's Z/A land on the old head's
 * motors. Returns false if the operator cancels.
 */
async function handleSwap(ev) {
    const swapIn = ev.swapIn.map(toolName).join(', ') || 'nothing';
    const swapOut = ev.swapOut.map(toolName).join(', ') || 'nothing';
    log(`job: tool swap — mount ${swapIn}, remove ${swapOut}`, 'note');

    const assign = headAssignment();
    const wantHead = ev.swapIn.map(t => assign.get(t)).find(h => h !== undefined);
    if (wantHead !== undefined && committedHead() !== wantHead) {
        if (!await waitIdle()) { log('  job: machine did not come to rest for the rebind', 'err'); return false; }
        log(`  job: rebinding slots to head ${wantHead}`, 'note');
        if (!await commitAxisMap(wantHead)) return false;
    }

    return window.confirm(`Tool swap\n\nmount: ${swapIn}\nremove: ${swapOut}\n\nOK when the head is ready.`);
}

// ── render gating ───────────────────────────────────────────────────────────

function renderAll() {
    const on  = isConnected();
    const cfg = config !== null;
    const busy = jog !== null || goTo !== null;

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

    gotoStack.disabled = !cfg;
    const hasPending = queue.some(e => e.state === 'pending');
    gotoNext.disabled  = !on || !cfg || !hasPending || busy || running;
    gotoAll.disabled   = !on || !cfg || !hasPending || busy || running;
    gotoAbort.disabled = goTo === null;
    gotoClear.disabled = queue.length === 0;

    // Compiling needs only a config and an SVG — it is a dry run and works
    // offline. Running needs the machine, and needs it to itself.
    // Peripherals are the ONE control surface that stays live during a job —
    // that is the whole point of ungating them in the firmware.
    for (const b of periphPanel.querySelectorAll('button[data-periph]')) b.disabled = !on;

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
