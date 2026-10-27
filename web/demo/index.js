import {
    Link, MachineState, Selection, NO_LAYER, stateName, faultNames, jobName,
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

/** The Link, once connected — null otherwise. */
let link = null;

// ── log ───────────────────────────────────────────────────────────────────────

/** Local wall-clock time, HH:MM:SS.mmm. */
function now() {
    const d = new Date();
    const p = (n, w = 2) => String(n).padStart(w, '0');
    return `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}.${p(d.getMilliseconds(), 3)}`;
}

function log(pre, line) {
    const t = now();
    const atBottom = pre.scrollTop + pre.clientHeight >= pre.scrollHeight - 2;
    pre.append(`${t} ${line}\n`);
    if (atBottom) pre.scrollTop = pre.scrollHeight;
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
    if (on) cmdIn.focus();
    else statusPollBox.checked = false;
}

// requestPort() must run inside a click handler.
connectBtn.addEventListener('click', async () => {
    connectBtn.disabled = true;
    try {
        link = new Link(await WebSerialTransport.requestAndOpen(BAUD_RATE));
        textOut(`connected @ ${BAUD_RATE}`);
        setConnected(true);
    } catch (e) {
        textOut(`connect failed: ${e.message}`);
        connectBtn.disabled = false;
    }
});

disconnectBtn.addEventListener('click', async () => {
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

async function submit(line) {
    if (!link || link.closed) {
        textOut('link closed');
        return;
    }
    textOut(`→ ${line}`);
    if (line === 'stop') {
        await link.send(line);
        textOut('← (no reply)');
        return;
    }
    const reply = await link.command(line);
    textOut(reply ? `← ${reply}` : '← (timeout)');
    desyncsOut.textContent = link.textDesyncs;
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

// ── status ────────────────────────────────────────────────────────────────────
// Get status logs to the binary log; the poll only updates the table.

const hex = (n, w) => `0x${n.toString(16).padStart(w, '0')}`;

function selectionName(s) {
    if (s === Selection.ANCHOR) return 'anchor';
    if (s === Selection.NONE) return '-';
    return `head ${s}`;
}

function statusRows(st) {
    return [
        ['state', stateName(st.state)],
        ['reason', st.state === MachineState.JOB ? jobName(st.reason) : st.reason],
        ['faults', `${hex(st.faults, 4)} ${faultNames(st.faults)}`],
        ['nodeEnabled', hex(st.nodeEnabled, 3)],
        ['nodeHomed', hex(st.nodeHomed, 3)],
        ['selection', selectionName(st.selection)],
        ['tool', st.tool],
        ['wpos', st.wpos.map((v) => (Number.isNaN(v) ? '-' : v.toFixed(3))).join(', ')],
        ['bufCount', st.bufCount],
        ['layer/record', st.layer === NO_LAYER ? '-' : `${st.layer}/${st.record}`],
    ];
}

function showStatus(st) {
    statusAtOut.textContent = now();
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
    while (statusPollBox.checked && link && !link.closed) {
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
