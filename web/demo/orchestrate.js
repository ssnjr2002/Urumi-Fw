/**
 * orchestrate.js — demo: .plan + config.json → schedule → stream via WebSerial
 *
 * Flow:
 *   1. Load config.json + .plan → parseConfig + loadPlan
 *   2. scheduleMounts + walkSchedule → WalkEvent[]
 *   3. Connect WebSerial → SerialTransport
 *   4. Pre-flight: ping → enable all → setorigin
 *   5. Walk events in order:
 *        pause  → show swap UI, wait for operator confirm
 *        motion → sendStream (MSEG Go-Back-N), then poll until IDLE
 */

import {
    parseConfig,
    loadPlan,
    scheduleMounts,
    walkSchedule,
    packMicrosegment,
    writeStream,
    MICRO_JOG,
    MICRO_LIFT,
    MICRO_PAUSE,
    TOOL_PROFILES_BY_TYPE,
} from '../index.js';
import { SerialTransport, STATE_IDLE, STATE_PAUSED, STATE_ESTOP, STATE_ALARM } from './transport.js';

// ── elements ──────────────────────────────────────────────────────────────────

const configInput  = document.getElementById('config-input');
const planInput    = document.getElementById('plan-input');
const loadBtn      = document.getElementById('load-btn');
const loadStatus   = document.getElementById('load-status');
const scheduleEl   = document.getElementById('schedule');

const connectBtn   = document.getElementById('connect-btn');
const connStatus   = document.getElementById('conn-status');

const pingBtn      = document.getElementById('ping-btn');
const enableBtn    = document.getElementById('enable-btn');
const originBtn    = document.getElementById('origin-btn');
const machineState = document.getElementById('machine-state');

const dumpBtn      = document.getElementById('dump-btn');
const runBtn       = document.getElementById('run-btn');
const stopBtn      = document.getElementById('stop-btn');
const runStatus    = document.getElementById('run-status');
const progressWrap = document.getElementById('progress-wrap');
const progressFill = document.getElementById('progress-fill');
const progressText = document.getElementById('progress-text');

const pausePanel   = document.getElementById('pause-panel');
const swapInfo     = document.getElementById('swap-info');
const resumeBtn    = document.getElementById('resume-btn');

// ── state ─────────────────────────────────────────────────────────────────────

let config    = null;
let plan      = null;
let events    = null;   // WalkEvent[]
let schedule  = null;

const transport = new SerialTransport();
let statusPollTimer = null;
let pauseResolve = null;
let jobRunning   = false;

// ── helpers ───────────────────────────────────────────────────────────────────

function setLoadStatus(msg, kind = 'idle') {
    loadStatus.textContent = msg;
    loadStatus.dataset.kind = kind;
}

function setRunStatus(msg, kind = 'idle') {
    runStatus.textContent = msg;
    runStatus.dataset.kind = kind;
}

function setConnStatus(msg, kind = 'idle') {
    connStatus.textContent = msg;
    connStatus.dataset.kind = kind;
}

function readFile(file) {
    return new Promise((resolve, reject) => {
        const r = new FileReader();
        r.onload  = () => resolve(r.result);
        r.onerror = () => reject(new Error(`failed to read ${file.name}`));
        r.readAsText(file);
    });
}

function readBinaryFile(file) {
    return new Promise((resolve, reject) => {
        const r = new FileReader();
        r.onload  = () => resolve(new Uint8Array(r.result));
        r.onerror = () => reject(new Error(`failed to read ${file.name}`));
        r.readAsArrayBuffer(file);
    });
}

/** Compute the MCFG required_axes bitmask from a Plan. */
function requiredAxesMask(plan) {
    let mask = 0x03; // X (bit0) + Y (bit1) always
    for (const block of plan.blocks) {
        const p = block.profile;
        if (p.liftHeight > 0)              mask |= 0x04; // Z (bit2)
        if (p.tangential || p.slotOffsets) mask |= 0x08; // A (bit3)
    }
    return mask;
}

const STATE_NAMES = ['IDLE', 'RUNNING', 'ESTOP', 'ALARM', 'PAUSED', 'HOMING'];

function stateName(s) {
    return STATE_NAMES[s] ?? `STATE(${s})`;
}

// ── schedule rendering ────────────────────────────────────────────────────────

function renderSchedule(schedule, plan) {
    if (!schedule || schedule.phases.length === 0) {
        scheduleEl.innerHTML = '<div class="no-plan">No phases.</div>';
        return;
    }

    const rows = schedule.phases.map((phase, i) => {
        const tools = phase.mount.map(tt => TOOL_PROFILES_BY_TYPE[tt]?.name ?? `unknown(${tt})`).join(', ');
        const blocks = phase.blockIndices.length;
        const segs   = phase.blockIndices.reduce((s, bi) => s + plan.blocks[bi].segments.length, 0);
        const swapIn  = phase.swapIn.length  ? `+${phase.swapIn.join(', ')}`  : '';
        const swapOut = phase.swapOut.length ? `−${phase.swapOut.join(', ')}` : '';
        const swap    = [swapIn, swapOut].filter(Boolean).join('  ');

        return `<div class="phase" id="phase-row-${i}">
            <div class="phase-num">Phase ${i + 1}</div>
            <div class="phase-tools">${tools}</div>
            <div class="phase-info">${blocks} block${blocks !== 1 ? 's' : ''} · ${segs} segs</div>
            ${swap ? `<div class="phase-swap">${swap}</div>` : ''}
        </div>`;
    });

    scheduleEl.innerHTML = rows.join('');
}

function highlightPhase(idx) {
    document.querySelectorAll('.phase').forEach((el, i) => {
        el.classList.toggle('active', i === idx);
        el.classList.toggle('done',   i < idx);
    });
}

// ── load & schedule ───────────────────────────────────────────────────────────

loadBtn.addEventListener('click', async () => {
    const cfgFile  = configInput.files?.[0];
    const planFile = planInput.files?.[0];
    if (!cfgFile)  { setLoadStatus('Select a config.json first.', 'error'); return; }
    if (!planFile) { setLoadStatus('Select a .plan file first.',  'error'); return; }

    setLoadStatus('Loading…', 'working');
    scheduleEl.innerHTML = '';
    config = plan = events = schedule = null;
    runBtn.disabled = true;

    try {
        const [cfgText, planBytes] = await Promise.all([
            readFile(cfgFile),
            readBinaryFile(planFile),
        ]);

        const parsed = parseConfig(cfgText);
        if (!parsed.ok) {
            setLoadStatus('Config errors:\n' + parsed.errors.join('\n'), 'error');
            return;
        }
        config = parsed.config;
        plan   = loadPlan(planBytes);

        const heads = config.machine.heads.length;
        schedule = scheduleMounts(plan, heads);
        events   = walkSchedule(schedule, plan, config.machine);

        renderSchedule(schedule, plan);

        const totalSegs = events
            .filter(e => e.kind === 'motion')
            .reduce((s, e) => s + e.segments.length, 0);
        const pauses = events.filter(e => e.kind === 'pause').length;

        setLoadStatus(
            `${plan.blocks.length} blocks · ${schedule.phases.length} phase${schedule.phases.length !== 1 ? 's' : ''} · ${pauses} swap${pauses !== 1 ? 's' : ''} · ${totalSegs.toLocaleString()} segments`,
            'ok'
        );

        dumpBtn.disabled = false;
        if (transport.connected) runBtn.disabled = false;

    } catch (e) {
        setLoadStatus(`Error: ${e.message}`, 'error');
    }
});

// ── dump stream ───────────────────────────────────────────────────────────────

dumpBtn.addEventListener('click', () => {
    if (!events) return;

    // Collect all segments from motion events in walk order
    // (inter-block jogs + block segments, exactly as they'd be streamed).
    const allSegs = events
        .filter(e => e.kind === 'motion')
        .flatMap(e => e.segments);

    // Count segment types for the summary line in the status.
    const jogCount  = allSegs.filter(s => s.flags & MICRO_JOG).length;
    const liftCount = allSegs.filter(s => s.flags & MICRO_LIFT).length;
    const dzCount   = allSegs.filter(s => s.dz !== 0).length;
    const cutCount  = allSegs.length - jogCount - liftCount;

    // Pack with rolling seq (same as sendStream) and write the framed .bin.
    const packets = allSegs.map((seg, i) => packMicrosegment(seg, i & 0xFF));
    const bytes   = writeStream(packets);

    const blob = new Blob([bytes], { type: 'application/octet-stream' });
    const url  = URL.createObjectURL(blob);
    const a    = Object.assign(document.createElement('a'), {
        href: url,
        download: 'orchestrate_stream.bin',
    });
    document.body.appendChild(a);
    a.click();
    a.remove();
    URL.revokeObjectURL(url);

    setRunStatus(
        `Stream: ${allSegs.length} segs — ${cutCount} cut, ${jogCount} jog, ${liftCount} lift, ${dzCount} dz≠0`,
        'ok',
    );
});

// ── WebSerial connect ─────────────────────────────────────────────────────────

if (!('serial' in navigator)) {
    connectBtn.textContent = 'WebSerial not supported';
    connectBtn.disabled = true;
    setConnStatus('WebSerial requires Chrome or Edge.', 'error');
} else {
    connectBtn.disabled = false;
}

connectBtn.addEventListener('click', async () => {
    if (transport.connected) {
        stopStatusPoll();
        await transport.disconnect();
        setConnStatus('Disconnected.', 'idle');
        connectBtn.textContent = 'Connect via WebSerial';
        [pingBtn, enableBtn, originBtn, stopBtn].forEach(b => b.disabled = true);
        runBtn.disabled = true;
        return;
    }

    try {
        setConnStatus('Connecting…', 'working');
        await transport.connect();
        setConnStatus('Connected at 115200 baud.', 'ok');
        connectBtn.textContent = 'Disconnect';
        [pingBtn, enableBtn, originBtn, stopBtn].forEach(b => b.disabled = false);
        if (plan) runBtn.disabled = false;
        startStatusPoll();
    } catch (e) {
        setConnStatus(`Connect failed: ${e.message}`, 'error');
    }
});

// ── status poll ───────────────────────────────────────────────────────────────

function startStatusPoll() {
    statusPollTimer = setInterval(async () => {
        if (!transport.connected || jobRunning) return;
        try {
            const s = await transport.pollStatus();
            machineState.textContent = stateName(s.machineState);
            machineState.dataset.state = s.machineState;
        } catch { /* ignore */ }
    }, 500);
}

function stopStatusPoll() {
    clearInterval(statusPollTimer);
    statusPollTimer = null;
}

// ── pre-flight controls ───────────────────────────────────────────────────────

pingBtn.addEventListener('click', async () => {
    try {
        const reply = await transport.sendText('ping');
        machineState.textContent = reply === 'pong' ? 'Pico alive ✓' : `unexpected: ${reply}`;
    } catch (e) {
        machineState.textContent = `ping failed: ${e.message}`;
    }
});

enableBtn.addEventListener('click', async () => {
    try {
        const reply = await transport.sendText('enable');
        machineState.textContent = `enable → ${reply}`;
    } catch (e) {
        machineState.textContent = `enable failed: ${e.message}`;
    }
});

originBtn.addEventListener('click', async () => {
    try {
        const reply = await transport.sendText('setorigin');
        machineState.textContent = `setorigin → ${reply}`;
    } catch (e) {
        machineState.textContent = `setorigin failed: ${e.message}`;
    }
});

stopBtn.addEventListener('click', async () => {
    try {
        jobRunning = false;
        await transport.sendText('stop');
        machineState.textContent = 'ESTOP sent.';
        setRunStatus('Stopped.', 'error');
        pausePanel.hidden = true;
        if (pauseResolve) { pauseResolve('stop'); pauseResolve = null; }
    } catch (e) {
        machineState.textContent = `stop failed: ${e.message}`;
    }
});

// ── run ───────────────────────────────────────────────────────────────────────

runBtn.addEventListener('click', () => {
    if (jobRunning) return;
    if (!events || !transport.connected) return;
    runJob();
});

resumeBtn.addEventListener('click', () => {
    if (pauseResolve) { pauseResolve('ok'); pauseResolve = null; }
});

async function waitForIdle() {
    for (;;) {
        const s = await transport.pollStatus();
        machineState.textContent = stateName(s.machineState);
        machineState.dataset.state = s.machineState;
        if (s.machineState === STATE_IDLE) return;
        if (s.machineState === STATE_ESTOP || s.machineState === STATE_ALARM) {
            throw new Error(`Machine in ${stateName(s.machineState)}`);
        }
        await new Promise(r => setTimeout(r, 150));
    }
}

async function waitForPaused() {
    for (;;) {
        const s = await transport.pollStatus();
        machineState.textContent = stateName(s.machineState);
        machineState.dataset.state = s.machineState;
        if (s.machineState === STATE_PAUSED) return;
        if (s.machineState === STATE_ESTOP || s.machineState === STATE_ALARM) {
            throw new Error(`Machine in ${stateName(s.machineState)}`);
        }
        await new Promise(r => setTimeout(r, 150));
    }
}

function showSwapAndWait(swapIn, swapOut) {
    const lines = [];
    if (swapOut.length) lines.push(`Remove: ${swapOut.map(tt => TOOL_PROFILES_BY_TYPE[tt]?.name ?? `unknown(${tt})`).join(', ')}`);
    if (swapIn.length)  lines.push(`Load:   ${swapIn.map(tt => TOOL_PROFILES_BY_TYPE[tt]?.name ?? `unknown(${tt})`).join(', ')}`);
    swapInfo.innerHTML = lines.map(l => `<div>${l}</div>`).join('');
    pausePanel.hidden = false;
    resumeBtn.disabled = false;

    return new Promise(resolve => { pauseResolve = resolve; });
}

async function runJob() {
    jobRunning = true;
    runBtn.disabled = true;
    pausePanel.hidden = true;
    progressWrap.hidden = false;

    const axesMask = requiredAxesMask(plan);

    // Walk events: alternating pause / motion(s) groups.
    // Collect consecutive motion events into one stream batch so we only issue
    // seqreset + MCFG once per phase.
    let i = 0;
    let phaseIdx = 0;
    let totalSent = 0;
    let machinePaused = false; // true only after a batch that ended with MICRO_PAUSE

    const motionEventCount = events.filter(e => e.kind === 'motion').length;

    try {
        while (i < events.length) {
            const ev = events[i];

            if (ev.kind === 'pause') {
                setRunStatus('Waiting for tool swap…', 'working');
                const result = await showSwapAndWait(ev.swapIn, ev.swapOut);
                pausePanel.hidden = true;
                if (result === 'stop') throw new Error('Job cancelled by stop.');
                // Only send resume if the machine is actually in PAUSED state.
                // The first swap happens before any streaming, so the machine is
                // still IDLE — no resume needed. Subsequent swaps follow a batch
                // that ended with MICRO_PAUSE, so the machine is PAUSED.
                if (machinePaused) {
                    console.log('[orchestrate] sending resume…');
                    const resumeReply = await transport.sendText('resume');
                    console.log('[orchestrate] resume reply:', resumeReply);
                    machinePaused = false;
                } else {
                    console.log('[orchestrate] first swap — machine was IDLE, skipping resume');
                }
                i++;
                phaseIdx++;
                highlightPhase(phaseIdx - 1);
                continue;
            }

            // Collect all consecutive motion events
            const batch = [];
            while (i < events.length && events[i].kind === 'motion') {
                batch.push(...events[i].segments);
                i++;
            }

            if (batch.length === 0) continue;

            // If a pause event follows this batch, mark the last segment with
            // MICRO_PAUSE so the firmware transitions to PAUSED (not IDLE) after
            // draining — the machine holds position during the tool swap.
            const nextIsPause = i < events.length && events[i].kind === 'pause';
            if (nextIsPause && batch.length > 0) {
                const last = batch[batch.length - 1];
                batch[batch.length - 1] = { ...last, flags: last.flags | MICRO_PAUSE };
                console.log(`[orchestrate] MICRO_PAUSE set on last segment (index ${batch.length - 1}), flags=0x${batch[batch.length - 1].flags.toString(16)}`);
                machinePaused = true;
            }

            const batchStart = totalSent;
            setRunStatus(`Streaming ${batch.length.toLocaleString()} segments…`, 'working');

            await transport.sendStream(batch, axesMask, (sent, total) => {
                const overall = batchStart + sent;
                progressFill.style.width = `${(sent / total * 100).toFixed(1)}%`;
                progressText.textContent = `${sent.toLocaleString()} / ${total.toLocaleString()}`;
            });

            totalSent += batch.length;

            if (nextIsPause) {
                setRunStatus('Waiting for machine to pause…', 'working');
                console.log('[orchestrate] waiting for STATE_PAUSED…');
                await waitForPaused();
                console.log('[orchestrate] machine is PAUSED — showing swap UI');
            } else {
                setRunStatus('Waiting for machine to drain…', 'working');
                await waitForIdle();
            }
        }

        setRunStatus(`Done — ${totalSent.toLocaleString()} segments streamed.`, 'ok');
        progressFill.style.width = '100%';

    } catch (e) {
        setRunStatus(`Error: ${e.message}`, 'error');
    } finally {
        jobRunning = false;
        runBtn.disabled = false;
        pausePanel.hidden = true;
    }
}
