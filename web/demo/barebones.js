import {
    Link, STATE_NAMES, loadConfig, encodeConfigBlob, decodeConfigBlob, ConfigTransferError,
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

/** CFG_NACK reason byte → name (docs/config_storage.md §5). */
const CFG_NACK_NAMES = {
    0x01: 'crc', 0x02: 'too_big', 0x03: 'bad_state', 0x04: 'flash', 0x05: 'timeout', 0x06: 'schema',
};

/** The Link, once connected — null otherwise. No Controller: this is just
 *  proving the transport + text/binary planes work before anything else. */
let link = null;

/** The parsed PipelineConfig, once a config.json has been picked — null otherwise. */
let config = null;

console.info(TAG, 'library imported ok — Link =', typeof Link);

function syncButtons() {
    pushBtn.disabled = !link || !config;
    pullBtn.disabled = !link;
    statusCfgBtn.disabled = !link;
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
    const reply = await link.command('status cfg');
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

disconnectBtn.addEventListener('click', async () => {
    if (!link) return;
    disconnectBtn.disabled = true;
    await link.close();
    link = null;
    console.info(TAG, 'disconnected');
    connectBtn.disabled = false;
    syncButtons();
});
