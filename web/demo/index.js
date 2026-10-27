import { Link } from 'urumi-host';
import { WebSerialTransport } from 'urumi-host/wire/link/backends/webserial';

const BAUD_RATE = 115200;

const connectBtn    = document.getElementById('connect');
const disconnectBtn = document.getElementById('disconnect');
const cmdIn         = document.getElementById('cmd');
const desyncsOut    = document.getElementById('desyncs');
const textLog       = document.getElementById('text-log');

/** The Link, once connected — null otherwise. */
let link = null;

// ── log ───────────────────────────────────────────────────────────────────────

function log(pre, line) {
    const t = new Date().toISOString().slice(11, 23);
    const atBottom = pre.scrollTop + pre.clientHeight >= pre.scrollHeight - 2;
    pre.append(`${t} ${line}\n`);
    if (atBottom) pre.scrollTop = pre.scrollHeight;
}

const textOut = (line) => log(textLog, line);

document.getElementById('text-clear').addEventListener('click', () => { textLog.textContent = ''; });
document.getElementById('text-copy').addEventListener('click', () => navigator.clipboard.writeText(textLog.textContent));

// ── connection ────────────────────────────────────────────────────────────────

function setConnected(on) {
    connectBtn.disabled = on;
    disconnectBtn.disabled = !on;
    cmdIn.disabled = !on;
    if (on) cmdIn.focus();
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
