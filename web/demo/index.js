import * as host from 'urumi-host';
import {
    Link, JogAck, MachineState, MachineContext, Selection, NO_LAYER, stateName, faultNames, jobName,
    loadConfig, stripJsonComments, encodeConfigBlob, decodeConfigBlob, modelToFile, crc32,
    JobReason, JobSource, fatalReasonName, loadSvgPaths, toolLayer, prepareBezierJob,
} from 'urumi-host';
import { WebSerialTransport } from 'urumi-host/wire/link/backends/webserial';

const BAUD_RATE = 115200;

const connectBtn    = document.getElementById('connect');
const disconnectBtn = document.getElementById('disconnect');
const cmdIn         = document.getElementById('cmd');
const desyncsOut    = document.getElementById('desyncs');
const textLog       = document.getElementById('text-log');
const binLog        = document.getElementById('bin-log');
const statusGetBtn  = document.getElementById('status-get');
const statusPollBox = document.getElementById('status-poll');
const statusMsIn    = document.getElementById('status-ms');
const statusAtOut   = document.getElementById('status-at');
const statusErrOut  = document.getElementById('status-err');
const statusTable   = document.getElementById('status-table');
const jogKeysBox    = document.getElementById('jog-keys');
const jogSpeedIn    = document.getElementById('jog-speed');
const jogDirOut     = document.getElementById('jog-dir');
const selectBtnsDiv = document.getElementById('select-btns');
const selectedOut   = document.getElementById('selected');
const cfgFileIn     = document.getElementById('cfg-file');
const cfgPushBtn    = document.getElementById('cfg-push');
const cfgPullBtn    = document.getElementById('cfg-pull');
const cfgSaveBtn    = document.getElementById('cfg-save');
const cfgInfoOut    = document.getElementById('cfg-info');
const gotoBtns      = document.querySelectorAll('button.goto, button.goto-row');
const stopBtn       = document.getElementById('stop');
const wzeroBtns     = document.querySelectorAll('button.wzero');
const textCmdBtns   = document.querySelectorAll('button.text-cmd');
const homeHeadSel   = document.getElementById('home-head');
const mountHeadSel  = document.getElementById('mount-head');
const mountToolSel  = document.getElementById('mount-tool');
const svgFileIn     = document.getElementById('svg-file');
const layerSel      = document.getElementById('layer');
const svgWarnOut    = document.getElementById('svg-warnings');
const previewCv     = document.getElementById('preview');
const jobFeedIn     = document.getElementById('job-feed');
const jobPIn        = document.getElementById('job-p');
const jobTIn        = document.getElementById('job-t');
const jobHeadSel    = document.getElementById('job-head');
const jobRunBtn     = document.getElementById('job-run');
const machineCtlBtns = document.querySelectorAll('button.machine-ctl');
const sessionCtlBtns = document.querySelectorAll('button.session-ctl');
const jobSessionOut = document.getElementById('job-session');
const jobInfoOut    = document.getElementById('job-info');

/** The Link, once connected — null otherwise. */
let link = null;

// ── log ───────────────────────────────────────────────────────────────────────

/** Local wall-clock time, HH:MM:SS.mmm. */
function now() {
    const d = new Date();
    const p = (n, w = 2) => String(n).padStart(w, '0');
    return `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}.${p(d.getMilliseconds(), 3)}`;
}

/** Append a line; returns its text node, so a caller can rewrite it in place. */
function log(pre, line) {
    const node = document.createTextNode(`${now()} ${line}\n`);
    const atBottom = pre.scrollTop + pre.clientHeight >= pre.scrollHeight - 2;
    pre.append(node);
    if (atBottom) pre.scrollTop = pre.scrollHeight;
    return node;
}

const textOut = (line) => log(textLog, line);
const binOut  = (line) => log(binLog, line);

for (const [pre, name] of [[textLog, 'text'], [binLog, 'bin']]) {
    document.getElementById(`${name}-clear`).addEventListener('click', () => { pre.textContent = ''; });
    document.getElementById(`${name}-copy`).addEventListener('click', () => navigator.clipboard.writeText(pre.textContent));
}

// ── connection ────────────────────────────────────────────────────────────────

function setConnected(on) {
    connectBtn.disabled = on;
    disconnectBtn.disabled = !on;
    cmdIn.disabled = !on;
    statusGetBtn.disabled = !on;
    statusPollBox.disabled = !on;
    jogKeysBox.disabled = !on;
    for (const b of [...selectBtnsDiv.children, ...gotoBtns, ...wzeroBtns, ...textCmdBtns, stopBtn]) b.disabled = !on;
    cfgPullBtn.disabled = !on;
    cfgPushBtn.disabled = !on || !pushBlob;
    setJobButtons();
    if (on) cmdIn.focus();
    else statusPollBox.checked = jogKeysBox.checked = false;
}

// requestPort() must run inside a click handler.
connectBtn.addEventListener('click', async () => {
    connectBtn.disabled = true;
    try {
        link = new Link(await WebSerialTransport.requestAndOpen(BAUD_RATE));
        textOut(`connected @ ${BAUD_RATE}`);
        setConnected(true);
        statusPollBox.checked = true;
        poll();
        await pullConfig();
    } catch (e) {
        textOut(`connect failed: ${e.message}`);
        connectBtn.disabled = false;
    }
});

disconnectBtn.addEventListener('click', async () => {
    jogStop();
    session?.truncate();
    const l = link;
    link = null;
    setConnected(false);
    await l?.close();
    textOut('disconnected');
});

// ── console ───────────────────────────────────────────────────────────────────
// One reply per command, as Link.command. `stop` goes through Link.send so it
// never waits behind a pending command; it has no reply.

/** Sent lines, oldest first; kept for the page's lifetime only. */
const history = [];
/** Index into history while stepping with ↑/↓; history.length = the draft. */
let histPos = 0;
let draft = '';

/** Send one line; returns the reply, "" on a timeout or no reply. */
async function submit(line) {
    if (!link || link.closed) {
        textOut('link closed');
        return '';
    }
    textOut(`→ ${line}`);
    if (line === 'stop') {
        await link.send(line);
        textOut('← (no reply)');
        return '';
    }
    const reply = await link.command(line);
    textOut(reply ? `← ${reply}` : '← (timeout)');
    desyncsOut.textContent = link.textDesyncs;
    return reply;
}

cmdIn.addEventListener('keydown', (e) => {
    if (e.key === 'Enter') {
        const line = cmdIn.value.trim();
        if (!line) return;
        if (history.at(-1) !== line) history.push(line);
        histPos = history.length;
        cmdIn.value = '';
        submit(line);
    } else if (e.key === 'ArrowUp' || e.key === 'ArrowDown') {
        e.preventDefault();
        if (histPos === history.length) draft = cmdIn.value;
        histPos = Math.max(0, Math.min(history.length, histPos + (e.key === 'ArrowUp' ? -1 : 1)));
        cmdIn.value = histPos === history.length ? draft : history[histPos];
    }
});

stopBtn.addEventListener('click', () => submit('stop'));

// ── status ────────────────────────────────────────────────────────────────────
// Get status logs to the binary log; the poll only updates the table. A job
// session polls for itself, so the poll waits out a job and the table follows
// the session's samples instead.

const hex = (n, w) => `0x${n.toString(16).padStart(w, '0')}`;

function selectionName(s) {
    if (s === Selection.ANCHOR) return 'anchor';
    if (s === Selection.NONE) return 'none';
    return String(s);
}

const contextName = (c) => Object.keys(MachineContext).find((k) => MachineContext[k] === c) ?? '-';

function statusRows(st) {
    return [
        ['state', stateName(st.state)],
        ['context', contextName(st.context)],
        ['reason', st.state === MachineState.JOB ? jobName(st.reason) : st.reason],
        ['faults', `${hex(st.faults, 4)} ${faultNames(st.faults)}`],
        ['nodeEnabled', hex(st.nodeEnabled, 3)],
        ['nodeHomed', hex(st.nodeHomed, 3)],
        ['selection', selectionName(st.selection)],
        ['tools', st.tools?.join(', ') ?? '-'],
        ['wpos', st.wpos.map((v) => (Number.isNaN(v) ? '-' : v.toFixed(3))).join(', ')],
        ['bufCount', st.bufCount],
        ['layer/record', st.layer === NO_LAYER ? '-' : `${st.layer}/${st.record}`],
    ];
}

function showStatus(st) {
    statusAtOut.textContent = now();
    selectedOut.textContent = selectionName(st.selection);
    statusTable.replaceChildren(...statusRows(st).map(([k, v]) => {
        const tr = document.createElement('tr');
        for (const text of [k, v]) {
            const td = document.createElement('td');
            td.textContent = text;
            tr.append(td);
        }
        return tr;
    }));
}

statusGetBtn.addEventListener('click', async () => {
    binOut('→ STATUS_REQ');
    try {
        const st = await link.getStatus();
        showStatus(st);
        binOut(`← STATUS_RSP ${stateName(st.state)} faults=${faultNames(st.faults)}`);
    } catch (e) {
        binOut(`← ${e.message}`);
    }
});

/** True while a poll loop runs, so re-ticking within one interval starts no second loop. */
let polling = false;

async function poll() {
    if (polling) return;
    polling = true;
    while (statusPollBox.checked && link && !link.closed && !session) {
        try {
            showStatus(await link.getStatus());
            statusErrOut.textContent = '';
        } catch (e) {
            statusErrOut.textContent = e.message;
        }
        await new Promise((r) => setTimeout(r, Number(statusMsIn.value) || 200));
    }
    polling = false;
}

statusPollBox.addEventListener('change', () => { if (statusPollBox.checked) poll(); });

// ── jog ───────────────────────────────────────────────────────────────────────
// While the box is ticked, held keys send a continuous-jog packet every
// JOG_PERIOD_MS (inside the Pico's deadman). Releasing every key, unticking,
// losing focus or disconnecting sends the stop byte. A packet moves one axis
// set (XY, Z or A), so the set of the last key pressed wins.
//
// The Pico ACKs a start or a change of direction, NACKs every refused packet,
// and says nothing to a repeat. Its replies land in the jog sink, drained here
// while jogging.

const JOG_PERIOD_MS = 50;
/** How long the jog sink is read after a stop, for replies still in flight. */
const JOG_DRAIN_MS = 200;
// [x, y, z, a] direction per key.
const JOG_KEYS = {
    w: [0, 1, 0, 0], s: [0, -1, 0, 0], a: [-1, 0, 0, 0], d: [1, 0, 0, 0],
    r: [0, 0, 1, 0], f: [0, 0, -1, 0], q: [0, 0, 0, -1], e: [0, 0, 0, 1],
};
const NACK_NAMES = Object.fromEntries(
    Object.entries(host).filter(([k]) => k.startsWith('NACK_')).map(([k, v]) => [v, k.slice(5)]),
);
const nackName = (r) => NACK_NAMES[r] ?? hex(r, 2);

const setOf = (k) => JOG_KEYS[k][2] ? 'z' : JOG_KEYS[k][3] ? 'a' : 'xy';
const held = new Set();
let jogSet = 'xy';
let jogTimer = null;
/** The last direction logged, so a repeat packet is not. */
let jogLogged = '';
/** Until when the jog sink is read; 0 while no reader runs. */
let drainUntil = 0;

function jogDir() {
    const d = [0, 0, 0, 0];
    for (const k of held) {
        if (setOf(k) !== jogSet) continue;
        for (let i = 0; i < 4; i++) d[i] += JOG_KEYS[k][i];
    }
    return d.map(Math.sign);
}

/** Log jog-sink replies; identical NACKs in a row fold into one line. */
async function drainJog() {
    let last = null, count = 0, node = null;
    while (link && Date.now() < drainUntil) {
        const r = await link.sinks.jog.get(JOG_PERIOD_MS);
        if (!r) continue;
        const line = r instanceof JogAck ? '← JOG_ACK' : `← JOG_NACK ${nackName(r.reason)}`;
        if (line === last && node) {
            node.data = node.data.replace(/( ×\d+)?\n$/, ` ×${++count}\n`);
        } else {
            node = binOut(line);
            last = line;
            count = 1;
        }
    }
    drainUntil = 0;
}

function keepDraining(ms) {
    const running = drainUntil !== 0;
    drainUntil = Math.max(drainUntil, Date.now() + ms);
    if (!running) drainJog();
}

function jogSend() {
    const [x, y, z, a] = jogDir();
    const speed = Number(jogSpeedIn.value) || 1;
    const moving = x || y || z || a;
    jogDirOut.textContent = moving ? `jogging x ${x} y ${y} z ${z} a ${a}` : '';
    if (!link || !moving) return;
    const dir = `${x} ${y} ${z} ${a} ×${speed}`;
    if (dir !== jogLogged) {
        binOut(`→ CJOG ${dir}`);
        jogLogged = dir;
    }
    try {
        link.cjog(x, y, speed, z, a);
    } catch (e) {
        binOut(`cjog: ${e.message}`);
    }
    keepDraining(JOG_PERIOD_MS + JOG_DRAIN_MS);
}

function jogStop() {
    held.clear();
    if (jogTimer) { clearInterval(jogTimer); jogTimer = null; }
    jogDirOut.textContent = '';
    if (!link || !jogLogged) return;
    link.cjogStop();
    binOut('→ CJOG_STOP');
    jogLogged = '';
}

const typing = (t) => t instanceof HTMLInputElement && t.type !== 'checkbox' && t.type !== 'radio';

document.addEventListener('keydown', (e) => {
    const k = e.key.toLowerCase();
    if (!jogKeysBox.checked || !(k in JOG_KEYS) || typing(e.target)) return;
    e.preventDefault();
    if (e.repeat || held.has(k)) return;
    held.add(k);
    jogSet = setOf(k);
    jogSend();                                   // a new direction goes out at once
    jogTimer ??= setInterval(jogSend, JOG_PERIOD_MS);
});

document.addEventListener('keyup', (e) => {
    const k = e.key.toLowerCase();
    if (!held.delete(k)) return;
    if (held.size === 0) return jogStop();
    // Releasing the last key of the set falls back to a set still held.
    if (![...held].some((h) => setOf(h) === jogSet)) jogSet = setOf([...held].at(-1));
    jogSend();
});

jogKeysBox.addEventListener('change', () => { if (!jogKeysBox.checked) jogStop(); });
window.addEventListener('blur', jogStop);

// ── select ────────────────────────────────────────────────────────────────────
// `select` is a text command, so it goes through the console and its log. The
// shown selection follows an `ok` and every status sample.

/** One button per configured head, then anchor; anchor alone with no config. */
function buildSelectButtons(headCount) {
    const values = [...Array(headCount).keys()].map(String).concat('anchor');
    selectBtnsDiv.replaceChildren(...values.map((v) => {
        const b = document.createElement('button');
        b.className = 'select';
        b.value = b.textContent = v;
        b.disabled = !link;
        b.addEventListener('click', async () => {
            if (await submit(`select ${v}`) === 'ok') selectedOut.textContent = v;
        });
        return b;
    }));
    for (const sel of [jobHeadSel, homeHeadSel, mountHeadSel]) {
        sel.replaceChildren(...[...Array(headCount).keys()].map((h) => new Option(String(h), String(h))));
    }
}

buildSelectButtons(0);

// ── config ────────────────────────────────────────────────────────────────────
// A picked .json/.jsonc is loaded (parsed and validated) and encoded on pick;
// push sends that blob. A pull on connect and after each push sets the select
// buttons from the stored config's heads.

const CFG_NACK_NAMES = Object.fromEntries(
    Object.entries(host).filter(([k]) => k.startsWith('CFG_NACK_')).map(([k, v]) => [v, k.slice(9).toLowerCase()]),
);

/** The picked file's encoded blob — null until a file loads cleanly. */
let pushBlob = null;
/** The last pulled config, decoded — null when none is stored or pulled. */
let pulledConfig = null;

const kB = (n) => `${(n / 1024).toFixed(1)} kB`;

function cfgError(e) {
    const reason = e?.reason;
    return reason != null ? `CFG_NACK ${CFG_NACK_NAMES[reason] ?? hex(reason, 2)}` : e.message;
}

cfgFileIn.addEventListener('change', async () => {
    pushBlob = null;
    cfgPushBtn.disabled = true;
    const file = cfgFileIn.files[0];
    if (!file) return;
    const loaded = loadConfig(stripJsonComments(await file.text()));
    if (!loaded.ok) {
        cfgInfoOut.textContent = `${file.name}: rejected`;
        for (const err of loaded.errors) binOut(`config error: ${err}`);
        return;
    }
    for (const w of loaded.warnings) binOut(`config warning: ${w}`);
    pushBlob = encodeConfigBlob(loaded.config);
    cfgInfoOut.textContent = `${file.name}: ${kB(pushBlob.length)}, ${loaded.config.machine.heads.length} head(s)`;
    cfgPushBtn.disabled = !link;
});

cfgPushBtn.addEventListener('click', async () => {
    binOut(`→ CFG_SET ${kB(pushBlob.length)} crc=${hex(crc32(pushBlob) >>> 0, 8)}`);
    try {
        await link.pushConfig(pushBlob);
        binOut('← CFG_ACK');
    } catch (e) {
        binOut(`← ${cfgError(e)}`);
        return;
    }
    await pullConfig();
});

/** Pull the stored config and set the select buttons from it. */
async function pullConfig() {
    binOut('→ CFG_GET');
    try {
        const blob = await link.pullConfig();
        pulledConfig = blob && decodeConfigBlob(blob);
        binOut(blob ? `← CFG_DATA ${kB(blob.length)}, ${pulledConfig.machine.heads.length} head(s)` : '← CFG_DATA (none stored)');
    } catch (e) {
        pulledConfig = null;
        binOut(`← ${cfgError(e)}`);
    }
    cfgSaveBtn.disabled = !pulledConfig;
    buildSelectButtons(pulledConfig?.machine.heads.length ?? 0);
    mountToolSel.replaceChildren(...Object.entries(pulledConfig?.toolProfiles ?? {})
        .map(([name, t]) => new Option(name, String(t.id))));
    loadLayers();
}

cfgPullBtn.addEventListener('click', pullConfig);

cfgSaveBtn.addEventListener('click', () => {
    const url = URL.createObjectURL(new Blob([modelToFile(pulledConfig)], { type: 'application/json' }));
    const a = Object.assign(document.createElement('a'), { href: url, download: 'config.json' });
    a.click();
    URL.revokeObjectURL(url);
});

// ── goto ──────────────────────────────────────────────────────────────────────
// `goto m|w …` and the named targets, text commands; the jog speed is the scale.
// A row's go button reads the inputs beside it. Material top is work Z t, from
// the Job panel: work Z 0 is the bed.

function gotoTarget(b) {
    if (b.classList.contains('goto-row')) {
        const vals = [...b.parentElement.querySelectorAll('input')].map((i) => i.value);
        if (vals.includes('')) { textOut(`goto ${b.value}: value needed`); return null; }
        return `${b.value} ${vals.join(' ')}`;
    }
    if (b.value !== 'top') return b.value;
    const t = Number(jobTIn.value);
    if (!(t > 0)) { textOut('material top: set t first'); return null; }
    return `w z ${t}`;
}

for (const b of gotoBtns) {
    b.addEventListener('click', () => {
        const to = gotoTarget(b);
        if (!to) return;
        const speed = Number(jogSpeedIn.value) || 1;
        const scale = 'noscale' in b.dataset || speed === 1 ? '' : ` ${speed}`;
        submit(`goto ${to}${scale}`);
    });
}

for (const b of wzeroBtns) b.addEventListener('click', () => submit(`wzero ${b.value}`));

// ── home and tool ─────────────────────────────────────────────────────────────
// A button's data-head / data-tool name the pickers its arguments come from.

for (const b of textCmdBtns) {
    b.addEventListener('click', () => {
        const args = [b.dataset.head, b.dataset.tool].filter(Boolean).map((id) => document.getElementById(id).value);
        if (args.includes('')) return textOut(`${b.value}: pull a config first`);
        submit([b.value, ...args].join(' '));
    });
}

// ── job ───────────────────────────────────────────────────────────────────────
// The picked SVG is split into tool layers against the pulled config's tools,
// again after each pull. Execute runs one layer as a job session: a LAYER_HDR
// and its BEZIERs. The session lives until the job leaves JOB, so a finished
// layer stays in JOB, dry, until Cancel or End job closes it.
//
// Pause and Cancel act on whatever job the machine has: through the session
// while one runs, so it expects the hold or the end, else as text commands.
// Resume and End job need the session.

/** The picked SVG's text — null until one is picked. */
let svgText = null;
/** The picked SVG's layers, by key; empty without an SVG or a config. */
let svgLayers = new Map();
/** The running job session — null outside a job. */
let session = null;
/** The running layer's BEZIER count. */
let jobTotal = 0;

const toolNames = () => Object.keys(pulledConfig?.toolProfiles ?? {});

/** Why a layer can't run, or '' if it can. */
function layerProblem(key) {
    if (key === '') return 'no tool';
    const tl = toolLayer(key, toolNames());
    const profile = pulledConfig.toolProfiles[tl.tool];
    if (profile.id == null) return 'tool has no id';
    const chambers = profile.chamberOffsets?.length ?? 0;
    const ok = chambers ? tl.chamber >= 1 && tl.chamber <= chambers : tl.chamber === 0;
    return ok ? '' : chambers ? `chamber ${tl.chamber} not in 1..${chambers}` : 'tool has no chambers';
}

function loadLayers() {
    svgLayers = new Map();
    svgWarnOut.textContent = '';
    layerSel.replaceChildren();
    if (svgText && pulledConfig) {
        try {
            const loaded = loadSvgPaths(svgText, { tools: toolNames() });
            svgLayers = loaded.layers;
            svgWarnOut.textContent = loaded.warnings.join('\n');
        } catch (e) {
            svgWarnOut.textContent = `svg: ${e.message}`;
        }
    } else if (svgText) {
        svgWarnOut.textContent = 'pull a config to list the layers';
    }
    for (const key of svgLayers.keys()) {
        const problem = layerProblem(key);
        const o = new Option(`${key || '(no tool)'}${problem ? ` — ${problem}` : ''}`, key);
        o.disabled = problem !== '';
        layerSel.append(o);
    }
    const first = [...layerSel.options].find((o) => !o.disabled);
    if (first) layerSel.value = first.value;
    drawPreview();
    setJobButtons();
}

svgFileIn.addEventListener('change', async () => {
    const file = svgFileIn.files[0];
    svgText = file ? await file.text() : null;
    loadLayers();
});

layerSel.addEventListener('change', drawPreview);

/** Every layer in grey, the selected one in black, fitted to the canvas, Y up. */
function drawPreview() {
    const ctx = previewCv.getContext('2d');
    ctx.clearRect(0, 0, previewCv.width, previewCv.height);
    const all = [...svgLayers.values()].flat();
    const pts = all.flatMap((sp) => sp.curves.flatMap((c) => [c.p0, c.p1, c.p2, c.p3]));
    if (pts.length === 0) return;
    const minX = Math.min(...pts.map((p) => p.x)), maxX = Math.max(...pts.map((p) => p.x));
    const minY = Math.min(...pts.map((p) => p.y)), maxY = Math.max(...pts.map((p) => p.y));
    const pad = 8;
    const s = Math.min((previewCv.width - 2 * pad) / (maxX - minX || 1), (previewCv.height - 2 * pad) / (maxY - minY || 1));
    const tx = (p) => [pad + (p.x - minX) * s, previewCv.height - pad - (p.y - minY) * s];
    for (const [key, subpaths] of svgLayers) {
        ctx.strokeStyle = key === layerSel.value ? '#000' : '#ccc';
        ctx.beginPath();
        for (const sp of subpaths) {
            ctx.moveTo(...tx(sp.curves[0].p0));
            for (const c of sp.curves) ctx.bezierCurveTo(...tx(c.p1), ...tx(c.p2), ...tx(c.p3));
            if (sp.closed) ctx.closePath();
        }
        ctx.stroke();
    }
}

function setJobButtons() {
    const on = !!link && !link.closed;
    const layer = layerSel.selectedOptions[0];
    jobRunBtn.disabled = !on || !!session || !layer || layer.disabled;
    for (const b of machineCtlBtns) b.disabled = !on;
    for (const b of sessionCtlBtns) b.disabled = !on || !session;
}

/** Records executed of the total; done once the ring is dry with every packet taken. */
function showJob() {
    if (!session) return;
    const s = session.stats();
    const st = link?.status;
    const inJob = st?.context === MachineContext.JOB;
    const done = inJob && st.state === MachineState.JOB && st.reason === JobReason.BUFFERING && st.bufCount === 0
        && s.emitted > jobTotal && s.acked === s.emitted;
    const executed = done ? jobTotal : inJob ? Math.min(st.record, jobTotal) : 0;
    jobInfoOut.textContent = `records ${executed}/${jobTotal}, ${jobTotal - executed} left`
        + (done ? ' — done, Cancel or End job closes it' : '')
        + ` | held ${s.held ?? '-'} sent ${s.sent} acked ${s.acked} nacks ${s.nacks} retries ${s.retries}`;
}

const HOLD_HINTS = {
    tool: 'mount and zero, then Resume',
    paused: 'Resume to go on',
    refused: 'header refused, Resume to retry',
    silent: 'no replies, resending',
};

/** Redraw the status and the job line on each of the session's samples; log each change of hold. */
async function watchJob() {
    let stamp = link.sinks.status.sample[1];
    let held = null;
    while (session) {
        const [raw, at] = await link.sinks.status.waitUpdate(500, stamp);
        stamp = at;
        if (raw && link?.status) showStatus(link.status);
        showJob();
        if (session && session.held !== held) {
            held = session.held;
            binOut(held ? `held: ${held} — ${HOLD_HINTS[held]}` : 'running');
        }
    }
}

jobRunBtn.addEventListener('click', async () => {
    const key = layerSel.value;
    const tl = toolLayer(key, toolNames());
    const job = prepareBezierJob(svgText, key, { tools: toolNames(), quality: pulledConfig.quality });
    if ('error' in job) return binOut(`job: ${job.error}`);
    const hdr = {
        tool: pulledConfig.toolProfiles[tl.tool].id,
        head: Number(jobHeadSel.value),
        chamber: tl.chamber,
        feed: Number(jobFeedIn.value) || 0,
        ...(jobPIn.value !== '' && { p: Number(jobPIn.value) }),
        thickness: Number(jobTIn.value) || 0,
    };
    try {
        await link.resetSeq();
        session = link.session(new JobSource(hdr, job.packets));
    } catch (e) {
        return binOut(`job: ${e.message}`);
    }
    jobTotal = job.packets.length;
    const p = hdr.p ?? hdr.thickness;
    jobSessionOut.textContent = `${key} · tool ${hdr.tool} · head ${hdr.head} · chamber ${hdr.chamber} · t ${hdr.thickness} p ${p}`;
    binOut(`→ LAYER_HDR ${key} tool=${hdr.tool} head=${hdr.head} chamber=${hdr.chamber} t=${hdr.thickness} p=${p}, ${jobTotal} BEZIER`);
    setJobButtons();
    watchJob();
    const ok = await session.run();
    const r = session.result();
    showJob();
    binOut(`job ${ok ? (r.truncated ? 'truncated' : 'ended') : `failed: ${fatalReasonName(r.fatalReason)}`}`
        + ` — sent ${r.sent} acked ${r.acked} nacks ${r.nacks} retries ${r.retries}`);
    session = null;
    jobSessionOut.textContent = 'none';
    setJobButtons();
    if (statusPollBox.checked) poll();
});

for (const b of machineCtlBtns) {
    b.addEventListener('click', async () => {
        if (!session) return submit(b.value);
        textOut(`→ ${b.value}`);
        textOut(`← ${await session[b.value]()}`);
    });
}

const RESUME_WAIT_MS = 2000;

/** Rewrite a resume's log line with what came of it: the first ACK or NACK after it. */
async function resumeOutcome(node, line, held, before) {
    const until = Date.now() + RESUME_WAIT_MS;
    let outcome = 'no reply yet';
    while (session && Date.now() < until) {
        const s = session.stats();
        if (s.acked > before.acked) { outcome = 'accepted'; break; }
        if (s.nacks > before.nacks) { outcome = held === 'tool' ? 'refused, still tool change' : 'refused'; break; }
        await new Promise((r) => setTimeout(r, 50));
    }
    node.data = node.data.replace(/\n$/, ` — ${outcome}\n`);
}

for (const b of sessionCtlBtns) {
    b.addEventListener('click', () => {
        if (!session) return;
        if (b.value === 'end') {
            session.end();
            return binOut('→ ABORT (end job)');
        }
        // True only means the session resends; the firmware checks again, and
        // a refusal brings the hold back (logged by watchJob). A tool hold is
        // left only from TOOL_WAIT, not while the change moves.
        const held = session.held;
        const before = session.stats();
        if (!session.resume()) {
            const st = link?.status;
            const moving = held && st && st.state !== MachineState.JOB ? `, ${stateName(st.state)}` : '';
            return binOut(`resume: ${held ? `not yet (${held}${moving})` : 'not held'}`);
        }
        const line = held === 'tool' ? 'resume: header resent' : 'resume: restarting from the resume point';
        resumeOutcome(binOut(line), line, held, before);
    });
}
