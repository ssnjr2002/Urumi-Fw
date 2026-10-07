import {
    Link, STATE_NAMES, loadConfig, encodeConfigBlob, decodeConfigBlob, ConfigTransferError,
    loadSvgPaths, prepareBezierJob, BezierFlag, ListSource, fatalReasonName, qualityConfig,
} from 'urumi-host';
import { WebSerialTransport } from 'urumi-host/wire/link/backends/webserial';

const TAG = '[barebones]';
const BAUD_RATE = 115200;

const connectBtn    = document.getElementById('connect');
const disconnectBtn = document.getElementById('disconnect');
const configFile    = document.getElementById('config-file');
const pushBtn       = document.getElementById('push-config');
const pullBtn       = document.getElementById('pull-config');
const statusCfgBtn  = document.getElementById('status-cfg');
const svgFile       = document.getElementById('svg-file');
const layerSel      = document.getElementById('layer');
const cutFeedIn     = document.getElementById('cut-feed');
const travelFeedIn  = document.getElementById('travel-feed');
const offsetXIn     = document.getElementById('offset-x');
const offsetYIn     = document.getElementById('offset-y');
const prepareBtn    = document.getElementById('prepare');
const runBtn        = document.getElementById('run');
const abortBtn      = document.getElementById('abort');

/** CFG_NACK reason byte → name (docs/config_storage.md §5). */
const CFG_NACK_NAMES = {
    0x01: 'crc', 0x02: 'too_big', 0x03: 'bad_state', 0x04: 'flash', 0x05: 'timeout', 0x06: 'schema',
};

/** The Link, once connected — null otherwise. No Controller: this is just
 *  proving the transport + text/binary planes work before anything else. */
let link = null;

/** The parsed PipelineConfig, once a config.json has been picked — null otherwise. */
let config = null;

/** The picked SVG's text — null until one is picked. */
let svgText = null;

/** The prepared job (prepareBezierJob) — null until Prepare, or after any input changes. */
let job = null;

/** The running stream's Session — null when no job is streaming. */
let session = null;

console.info(TAG, 'library imported ok — Link =', typeof Link);

function syncButtons() {
    pushBtn.disabled = !link || !config;
    pullBtn.disabled = !link;
    statusCfgBtn.disabled = !link;
    prepareBtn.disabled = !svgText || session !== null;
    runBtn.disabled = !link || !job || session !== null;
    abortBtn.disabled = !link;
}

configFile.addEventListener('change', async () => {
    const file = configFile.files[0];
    if (!file) return;
    console.debug(TAG, 'reading', file.name);
    const text = await file.text();
    const loaded = loadConfig(text);
    if (!loaded.ok) {
        config = null;
        console.error(TAG, 'config rejected:', loaded.errors);
        syncButtons();
        return;
    }
    config = loaded.config;
    cutFeedIn.value = config.machine.path.feed;
    travelFeedIn.value = config.machine.rapid.feed;
    job = null;
    console.info(TAG, 'config loaded —', config.machine.heads.length, 'head(s)');
    if (loaded.warnings.length) {
        for (const w of loaded.warnings) console.warn(TAG, 'config warning:', w);
    }
    syncButtons();
});

// requestPort() must run inside a click handler — the browser refuses to show
// the port picker from a script that fired on page load.
connectBtn.addEventListener('click', async () => {
    connectBtn.disabled = true;
    try {
        console.debug(TAG, 'requesting port…');
        const transport = await WebSerialTransport.requestAndOpen(BAUD_RATE);
        link = new Link(transport);
        console.info(TAG, 'connected');
        disconnectBtn.disabled = false;
        syncButtons();

        // One round trip, so this page proves something rather than just
        // opening a port and sitting there.
        console.debug(TAG, '> ping (text plane)');
        const reply = await link.command('ping');
        if (reply) console.debug(TAG, '<', reply);
        else console.warn(TAG, 'ping timeout — no reply');

        // The binary equivalent: STATUS_REQ/STATUS_RSP (0xA5/0xA7). There's no
        // dedicated binary "ping" — this is the closest thing, and it proves the
        // Demux's frame-magic routing independently of the text sink above.
        console.debug(TAG, '> getStatus (binary plane)');
        try {
            const st = await link.getStatus();
            console.debug(TAG, '<', STATE_NAMES[st.state] ?? st.state, st);
        } catch (e) {
            console.warn(TAG, 'getStatus failed:', e.message);
        }
    } catch (e) {
        console.error(TAG, 'connect failed:', e);
        connectBtn.disabled = false;
    }
});

async function statusCfg() {
    const reply = await link.command('cfg');
    console.info(TAG, '<', reply ?? '(no reply)');
}

pushBtn.addEventListener('click', async () => {
    const blob = encodeConfigBlob(config);
    console.debug(TAG, `> CFG_SET ${blob.length} bytes`);
    try {
        await link.pushConfig(blob);
        console.info(TAG, 'config stored (CFG_ACK)');
    } catch (e) {
        if (e instanceof ConfigTransferError && e.reason !== null) {
            console.error(TAG, 'push refused:', CFG_NACK_NAMES[e.reason] ?? e.reason, e.message);
        } else {
            console.error(TAG, 'push failed:', e);
        }
    }
    await statusCfg();
});

pullBtn.addEventListener('click', async () => {
    console.debug(TAG, '> CFG_GET');
    try {
        const blob = await link.pullConfig();
        if (!blob) console.info(TAG, 'no config stored');
        else console.info(TAG, `pulled ${blob.length} bytes:`, decodeConfigBlob(blob));
    } catch (e) {
        console.error(TAG, 'pull failed:', e);
    }
});

statusCfgBtn.addEventListener('click', statusCfg);

// ── job ──────────────────────────────────────────────────────────────────────

function invalidateJob() {
    job = null;
    syncButtons();
}

svgFile.addEventListener('change', async () => {
    const file = svgFile.files[0];
    svgText = null;
    layerSel.replaceChildren();
    layerSel.disabled = true;
    invalidateJob();
    if (!file) return;
    const text = await file.text();
    let layers;
    try {
        layers = loadSvgPaths(text).layers;
    } catch (e) {
        console.error(TAG, 'svg rejected:', e.message);
        return;
    }
    for (const [name, subpaths] of layers) {
        const opt = document.createElement('option');
        opt.value = name;
        opt.textContent = `${name || '(root)'} — ${subpaths.length} subpath(s)`;
        layerSel.append(opt);
    }
    svgText = text;
    layerSel.disabled = layers.size === 0;
    console.info(TAG, 'svg loaded —', file.name, layers.size, 'layer(s)');
    syncButtons();
});

for (const el of [layerSel, offsetXIn, offsetYIn]) el.addEventListener('change', invalidateJob);

prepareBtn.addEventListener('click', () => {
    const offset = { x: Number(offsetXIn.value), y: Number(offsetYIn.value) };
    let prepared;
    try {
        prepared = prepareBezierJob(svgText, layerSel.value, { offset, quality: config?.quality ?? qualityConfig() });
    } catch (e) {
        prepared = { error: e.message };
    }
    if ('error' in prepared) {
        job = null;
        console.error(TAG, 'prepare failed:', prepared.error);
        syncButtons();
        return;
    }
    job = prepared;
    const pieces = job.contours.flat();
    const breaks = pieces.filter((b) => b.flags & BezierFlag.BREAK).length;
    const { minX, minY, maxX, maxY } = job.bbox;
    const f = (v) => v.toFixed(2);
    console.info(TAG, `prepared — ${job.contours.length} contour(s), ${pieces.length} piece(s), ${breaks} BREAK(s)`);
    console.info(TAG, `bbox (machine mm): X ${f(minX)}..${f(maxX)}, Y ${f(minY)}..${f(maxY)} — check it fits the table`);
    syncButtons();
});

runBtn.addEventListener('click', async () => {
    const cut = Number(cutFeedIn.value), travel = Number(travelFeedIn.value);
    if (!(cut > 0 && travel > 0)) {
        console.error(TAG, 'feeds must be positive');
        return;
    }
    console.debug(TAG, `> feed ${cut} ${travel}`);
    const reply = await link.command(`feed ${cut} ${travel}`);
    console.debug(TAG, '<', reply ?? '(no reply)');
    if (reply?.trim() !== 'ok') {
        console.error(TAG, 'feed refused — not streaming');
        return;
    }

    // Built by hand rather than link.stream() so Abort can truncate it: on its
    // own the session waits out NACK_ABORTING and resends, restarting the job.
    await link.resetSeq();
    session = link.session(new ListSource(job.packets));
    syncButtons();
    console.info(TAG, `streaming ${job.packets.length} packet(s)…`);
    try {
        await session.run();
        const r = session.result();
        if (r.ok) console.info(TAG, r.truncated ? 'stream aborted' : 'stream done', r);
        else console.error(TAG, 'stream failed:', fatalReasonName(r.fatalReason), r);
    } finally {
        session = null;
        syncButtons();
    }
});

abortBtn.addEventListener('click', () => {
    session?.truncate();
    link.abort();
    console.warn(TAG, 'abort sent');
});

disconnectBtn.addEventListener('click', async () => {
    if (!link) return;
    disconnectBtn.disabled = true;
    await link.close();
    session?.truncate();
    link = null;
    console.info(TAG, 'disconnected');
    connectBtn.disabled = false;
    syncButtons();
});

// ── WASD jog ──────────────────────────────────────────────────────────────────
// While the checkbox is ticked, held keys send a continuous-jog packet every
// JOG_PERIOD_MS (inside the Pico's 150 ms deadman). Releasing every key, or
// unticking, losing focus or disconnecting, sends the stop byte.

const jogKeysBox = document.getElementById('jog-keys');
const jogSpeedIn = document.getElementById('jog-speed');
const jogDirOut  = document.getElementById('jog-dir');

const JOG_PERIOD_MS = 50;
const JOG_KEYS = { w: [0, 1], s: [0, -1], a: [-1, 0], d: [1, 0] };
const held = new Set();
let jogTimer = null;

function jogDir() {
    let x = 0, y = 0;
    for (const k of held) { x += JOG_KEYS[k][0]; y += JOG_KEYS[k][1]; }
    return [Math.sign(x), Math.sign(y)];
}

function jogSend() {
    const [x, y] = jogDir();
    const speed = Number(jogSpeedIn.value) || 1;
    jogDirOut.textContent = x || y ? `jogging x ${x} y ${y}` : '';
    if (!link || (!x && !y)) return;
    try { link.cjog(x, y, speed); } catch (e) { console.error(TAG, 'jog', e); }
}

function jogStop() {
    held.clear();
    if (jogTimer) { clearInterval(jogTimer); jogTimer = null; }
    jogDirOut.textContent = '';
    link?.cjogStop();
}

document.addEventListener('keydown', (e) => {
    const k = e.key.toLowerCase();
    if (!jogKeysBox.checked || !(k in JOG_KEYS) || e.target instanceof HTMLInputElement && e.target.type !== 'checkbox') return;
    e.preventDefault();
    if (e.repeat || held.has(k)) return;
    held.add(k);
    jogSend();                                   // a new direction goes out at once
    jogTimer ??= setInterval(jogSend, JOG_PERIOD_MS);
});

document.addEventListener('keyup', (e) => {
    const k = e.key.toLowerCase();
    if (!held.delete(k)) return;
    if (held.size === 0) jogStop();
    else jogSend();
});

jogKeysBox.addEventListener('change', () => { if (!jogKeysBox.checked) jogStop(); });
window.addEventListener('blur', jogStop);
disconnectBtn.addEventListener('click', jogStop);
