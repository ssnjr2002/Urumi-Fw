/**
 * bench.js — live compile & run benchmark demo.
 *
 * SVG + an editable config (bench.json, edited in memory) → bakePlan → schedule
 * /walk → stream straight to the machine via WebSerial. No .plan file round-trip
 * and no config file written: the textarea IS the config. Compile parses the
 * current editor text through the production loadConfig path — the same code as
 * the CLI — so every machine / head / tool / quality knob is tweakable and
 * re-runnable in one place.
 *
 * Migrated from demo/transport.js (the bespoke SerialTransport) to the wire/link
 * layer (Link + WebSerialTransport). The status poller now runs DURING a stream
 * because the demux routes by magic — a STATUS_RSP never lands in the text sink
 * — so polling stays live for the waitFor loop without colliding with the
 * stream's ACK sink (D1/D10). Stream failures surface via StreamResult.fatalReason
 * instead of a bare boolean, and a tool-swap pause now blocks on a confirm()
 * dialog before resume (the old code resumed the moment it hit the pause event,
 * mid-swap).
 */

import {
    loadConfig,
    bakePlan,
    walkSchedule,
    packMicrosegment,
    writeStream,
    MICRO_JOG,
    MICRO_LIFT,
    MICRO_PAUSE,
    Link,
    MachineState,
    getPos,
    fatalReasonName,
    settle,
    inState,
    stateName,
    walkSeconds,
    motionSegments,
} from '../src/index.js';
import { WebSerialTransport } from '../src/wire/link/backends/webserial.js';

// ── elements ────────────────────────────────────────────────────────────────

const svgInput     = document.getElementById('svg-input');
const defaultTool  = document.getElementById('default-tool');
const configEditor = document.getElementById('config-editor');
const revertBtn    = document.getElementById('revert-btn');
const compileBtn   = document.getElementById('compile-btn');
const metricsEl    = document.getElementById('metrics');
const connectBtn   = document.getElementById('connect-btn');
const stateLine    = document.getElementById('state-line').querySelector('b');
const pingBtn      = document.getElementById('ping-btn');
const enableBtn    = document.getElementById('enable-btn');
const originBtn    = document.getElementById('origin-btn');
const dumpBtn      = document.getElementById('dump-btn');
const runBtn       = document.getElementById('run-btn');
const stopBtn      = document.getElementById('stop-btn');
const progressFill = document.getElementById('progress-fill');
const statusEl     = document.getElementById('status');
const svgPreview   = document.getElementById('svg-preview');
const svgPane      = document.getElementById('svg-pane');
const configPane   = document.getElementById('config-pane');
const tabButtons   = document.querySelectorAll('.tab');

// ── state ───────────────────────────────────────────────────────────────────

let seedText   = '';     // the pristine bench.json text (for Revert)
let svgText    = null;
let svgName    = 'bench';
let lastEvents = null;
let lastBlocks    = null;

let link = null;               // Link over the open WebSerialTransport
let pollTimer    = null;
let jobRunning   = false;

function isConnected() { return link !== null && !link.closed; }

// ── helpers ─────────────────────────────────────────────────────────────────

function setStatus(msg, kind = 'idle') { statusEl.textContent = msg; statusEl.dataset.kind = kind; }

function readFile(file) {
    return new Promise((resolve, reject) => {
        const r = new FileReader();
        r.onload  = () => resolve(r.result);
        r.onerror = () => reject(new Error(`failed to read ${file.name}`));
        r.readAsText(file);
    });
}



// ── preview tabs (SVG | Config) ─────────────────────────────────────────────

function selectTab(name) {
    tabButtons.forEach(b => b.classList.toggle('active', b.dataset.tab === name));
    svgPane.hidden    = name !== 'svg';
    configPane.hidden = name !== 'config';
    revertBtn.hidden  = name !== 'config';
}
tabButtons.forEach(b => b.addEventListener('click', () => selectTab(b.dataset.tab)));

/**
 * Parse AND validate the editor text into a PipelineConfig. Throws on error.
 *
 * loadConfig, not parseConfig: the latter only proves the JSON is well-shaped,
 * so a config with duplicate node ids or an out-of-range defaultHead would sail
 * through and then misbehave on the machine. Warnings (e.g. a feed above its
 * axis ceiling, which gets clamped) never block — they go to the console so the
 * operator can see why the machine is slower than the number they typed.
 */
function buildConfig() {
    const loaded = loadConfig(configEditor.value);
    configEditor.classList.toggle('invalid', !loaded.ok);
    if (!loaded.ok) {
        selectTab('config'); // surface the red editor + let the operator fix it
        throw new Error('Config errors:\n' + loaded.errors.join('\n'));
    }
    for (const w of loaded.warnings) console.warn('[config]', w);
    return loaded.config;
}

// ── compile ─────────────────────────────────────────────────────────────────

/**
 * Compile SVG → compiled blocks + walk events using the current editor config.
 *
 * `initialState` seeds the walk's starting XY/A (TRUE steps). Omitted → the
 * walk starts from the origin (0,0), which is the canonical dry-run plan. The
 * Run path passes the machine's live position so the lead-in jog goes from
 * where the head actually is (see runBtn handler).
 */
function compile(initialState) {
    if (!svgText) throw new Error('Select an SVG file first.');
    const config = buildConfig();
    const tool = defaultTool.value.trim() || undefined;
    // bakePlan now schedules the mounts itself, before it compiles anything —
    // the head decides step counts, so mm cannot become steps until the
    // scheduler has said which socket each block's tool sits in. It hands back
    // the phases it used rather than letting us recompute them and disagree.
    const { blocks, phases } = bakePlan(config, svgText, { defaultTool: tool });
    const events = walkSchedule(phases, blocks, config.machine, initialState ? { initialState } : {});
    return { blocks, phases, events, config };
}

/**
 * Fetch the machine's live position and map it into the walk's TRUE-step
 * space. machinePos is emitted (post-invert) per axis, so un-invert each.
 */
async function liveInitialState(config) {
    const [x, y, , a] = await getPos(link);
    const m = config.machine;
    const head = m.heads[0];
    const aAxis = head.a;
    return {
        posX:  m.x.invert ? -x : x,
        posY:  m.y.invert ? -y : y,
        aPhys: aAxis.invert ? -a : a,
    };
}

const allMotionSegments = motionSegments;

function renderMetrics(blocks, events, config) {
    const segs = allMotionSegments(events);
    const jog  = segs.filter(s => s.flags & MICRO_JOG).length;
    const lift = segs.filter(s => s.flags & MICRO_LIFT).length;
    const cut  = segs.length - jog - lift;
    const secs = walkSeconds(events, config.machine.fCpu);
    const bytes = segs.length * 30;
    // The head is worth showing: it is what the block's Z steps were resolved
    // against, and on this bench machine the two heads differ 2:1.
    const lines = blocks.map(
        (b, i) => `  block ${i + 1}: ${b.profile.name} on head ${b.head}  ${b.segments.length} segs`,
    );
    metricsEl.textContent =
        `${blocks.length} block(s)  ${segs.length.toLocaleString()} segments\n` +
        `cut ${cut}  jog ${jog}  lift ${lift}\n` +
        `est run ${secs.toFixed(2)} s  (~${(bytes / 1024).toFixed(0)} KB stream)\n` +
        lines.join('\n');
}

compileBtn.addEventListener('click', () => {
    try {
        setStatus('Compiling…', 'working');
        const { blocks, events, config } = compile();
        lastBlocks = blocks; lastEvents = events;
        renderMetrics(blocks, events, config);
        dumpBtn.disabled = false;
        setStatus(`Compiled ${allMotionSegments(events).length.toLocaleString()} segments.`, 'ok');
    } catch (e) {
        metricsEl.textContent = e.message;
        setStatus('Compile failed.', 'error');
    }
});

dumpBtn.addEventListener('click', () => {
    if (!lastEvents) return;
    const segs = allMotionSegments(lastEvents);
    const bytes = writeStream(segs.map((s, i) => packMicrosegment(s, i & 0xFF)));
    const url = URL.createObjectURL(new Blob([bytes], { type: 'application/octet-stream' }));
    const a = Object.assign(document.createElement('a'), { href: url, download: `${svgName}.bin` });
    document.body.appendChild(a); a.click(); a.remove();
    URL.revokeObjectURL(url);
});

revertBtn.addEventListener('click', () => {
    configEditor.value = seedText;
    configEditor.classList.remove('invalid');
    setStatus('Config reverted to bench.json.', 'idle');
});

// ── SVG input + preview ─────────────────────────────────────────────────────

svgInput.addEventListener('change', async () => {
    const file = svgInput.files?.[0];
    if (!file) return;
    svgName = file.name.replace(/\.svg$/i, '');
    svgText = await readFile(file);
    svgPreview.innerHTML = svgText;
    const svg = svgPreview.querySelector('svg');
    if (svg) { svg.removeAttribute('width'); svg.removeAttribute('height'); }
});

// ── WebSerial connect + controls ────────────────────────────────────────────

if (!('serial' in navigator)) {
    connectBtn.textContent = 'WebSerial not supported';
    connectBtn.disabled = true;
    setStatus('WebSerial requires Chrome or Edge.', 'error');
}

connectBtn.addEventListener('click', async () => {
    if (isConnected()) {
        stopPoll();
        await link.close();
        link = null;
        connectBtn.textContent = 'Connect via WebSerial';
        [pingBtn, enableBtn, originBtn, runBtn, stopBtn].forEach(b => b.disabled = true);
        dumpBtn.disabled = !lastEvents; // keep dump enabled if a compile exists
        setStatus('Disconnected.', 'idle');
        stateLine.textContent = '—';
        return;
    }
    try {
        setStatus('Connecting…', 'working');
        const transport = await WebSerialTransport.requestAndOpen();
        link = new Link(transport);
        link.verbose = true; // per-event ACK/NACK/fatal trail on console.debug
        connectBtn.textContent = 'Disconnect';
        [pingBtn, enableBtn, originBtn, runBtn, stopBtn].forEach(b => b.disabled = false);
        setStatus('Connected at 115200 baud.', 'ok');
        startPoll();
    } catch (e) {
        setStatus(`Connect failed: ${e.message}`, 'error');
    }
});

function startPoll() {
    pollTimer = setInterval(async () => {
        if (!isConnected() || jobRunning) return;
        try {
            const s = await link.getStatus();
            stateLine.textContent = stateName(s.state);
        } catch { /* ignore */ }
    }, 500);
}
function stopPoll() { clearInterval(pollTimer); pollTimer = null; }

pingBtn.addEventListener('click', async () => {
    try { stateLine.textContent = (await link.command('ping')) === 'pong' ? 'alive ✓' : '?'; }
    catch (e) { stateLine.textContent = `ping: ${e.message}`; }
});
enableBtn.addEventListener('click', async () => {
    try { stateLine.textContent = `enable → ${await link.command('enable')}`; }
    catch (e) { stateLine.textContent = `enable: ${e.message}`; }
});
originBtn.addEventListener('click', async () => {
    try { stateLine.textContent = `origin → ${await link.command('setorigin')}`; }
    catch (e) { stateLine.textContent = `origin: ${e.message}`; }
});
stopBtn.addEventListener('click', async () => {
    try {
        jobRunning = false;
        // fire-and-forget — estop correlates nothing, never queues behind a
        // pending text command (confirmation arrives on the status sink).
        await link.send('stop');
        stateLine.textContent = 'ESTOP';
        setStatus('Stopped.', 'error');
    } catch (e) { stateLine.textContent = `stop: ${e.message}`; }
});

// ── run (compile fresh, then stream) ────────────────────────────────────────

const waitFor = target =>
    settle(link, inState(target), { onPoll: s => { stateLine.textContent = stateName(s.state); } });

runBtn.addEventListener('click', async () => {
    if (jobRunning || !isConnected()) return;
    let blocks, events, config;
    try {
        // Seed the walk with the machine's live position so the lead-in jog
        // moves from where the head actually is — not from an assumed origin.
        // This makes back-to-back runs repeat in place instead of drifting by
        // the origin→start vector each time.
        setStatus('Reading machine position…', 'working');
        const cfg = buildConfig();
        const initial = await liveInitialState(cfg);
        setStatus(`Head at X${initial.posX} Y${initial.posY} steps — compiling…`, 'working');
        ({ blocks, events, config } = compile(initial));
        lastBlocks = blocks; lastEvents = events;
        renderMetrics(blocks, events, config);
    } catch (e) {
        setStatus(e.message, 'error');
        return;
    }

    // Pre-flight: the machine must be IDLE (or PAUSED for a resumed job) before
    // we resetSeq + stream — a non-IDLE machine NACKs every packet with
    // NACK_BAD_STATE, which before D17 looked like a silent success.
    try {
        const pre = await link.getStatus();
        if (pre.state !== MachineState.IDLE && pre.state !== MachineState.PAUSED) {
            throw new Error(`Machine is ${stateName(pre.state)} — unalarm or set origin before running.`);
        }
    } catch (e) {
        setStatus(`Pre-flight failed: ${e.message}`, 'error');
        return;
    }

    jobRunning = true;
    runBtn.disabled = true;
    progressFill.style.width = '0';

    const total = allMotionSegments(events).length;

    try {
        let i = 0, totalSent = 0, machinePaused = false;

        while (i < events.length) {
            const ev = events[i];

            if (ev.kind === 'pause') {
                // Block on the operator: the old code sent `resume` immediately
                // on hitting the pause event, before the operator had touched
                // the tool. Now we wait for a confirm, then resume. The machine
                // is already PAUSED (the previous batch's last segment carried
                // MICRO_PAUSE), so confirm() never races motion.
                const ok = window.confirm('Tool swap — load/remove tool, then click OK to resume.');
                if (!ok) throw new Error('Job cancelled by operator at tool swap.');
                if (machinePaused) {
                    console.log('[bench] sending resume…');
                    const reply = await link.command('resume');
                    console.log('[bench] resume reply:', reply);
                    machinePaused = false;
                } else {
                    console.log('[bench] first swap — machine was IDLE, skipping resume');
                }
                i++;
                continue;
            }

            const batch = [];
            while (i < events.length && events[i].kind === 'motion') {
                batch.push(...events[i].segments);
                i++;
            }
            if (batch.length === 0) continue;

            const nextIsPause = i < events.length && events[i].kind === 'pause';
            if (nextIsPause) {
                const last = batch[batch.length - 1];
                batch[batch.length - 1] = { ...last, flags: last.flags | MICRO_PAUSE };
                machinePaused = true;
            }

            setStatus(`Streaming ${batch.length.toLocaleString()} segments…`, 'working');
            // Pre-pack with rolling seq; link.stream() calls resetSeq() once per
            // phase. No MCFG preamble — the firmware does not handle it yet.
            const packets = batch.map((seg, idx) => packMicrosegment(seg, idx & 0xff));
            const result = await link.stream(packets, 16);

            if (!result.ok) {
                const reason = result.fatalReason !== undefined
                    ? fatalReasonName(result.fatalReason)
                    : 'unknown';
                const dm = link.demux.stats();
                const msg = `Stream failed: ${reason} — emitted ${result.emitted}, acked ${result.acked}, nacks ${result.nacks}, demux.unknownBytes=${dm.unknownBytes}`;
                console.error('[bench] ' + msg, { result, demux: dm, writer: link.writer.stats() });
                throw new Error(msg);
            }

            totalSent += batch.length;
            progressFill.style.width = `${(totalSent / total * 100).toFixed(1)}%`;

            await waitFor(nextIsPause ? MachineState.PAUSED : MachineState.IDLE);
        }

        progressFill.style.width = '100%';
        setStatus(`Done — ${totalSent.toLocaleString()} segments streamed.`, 'ok');
    } catch (e) {
        setStatus(`Error: ${e.message}`, 'error');
    } finally {
        jobRunning = false;
        runBtn.disabled = false;
    }
});

// ── boot: load bench.json into the editor ───────────────────────────────────

(async () => {
    try {
        seedText = await (await fetch('./bench.json')).text();
        configEditor.value = seedText;
        setStatus('Ready. Load an SVG to begin.', 'idle');
    } catch (e) {
        setStatus(`Could not load bench.json: ${e.message}`, 'error');
    }
})();