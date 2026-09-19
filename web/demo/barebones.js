import { Link, STATE_NAMES, loadConfig } from 'urumi-host';
import { WebSerialTransport } from 'urumi-host/wire/link/backends/webserial';

const TAG = '[barebones]';
const BAUD_RATE = 115200;

const connectBtn    = document.getElementById('connect');
const disconnectBtn = document.getElementById('disconnect');
const configFile    = document.getElementById('config-file');

/** The Link, once connected — null otherwise. No Controller: this is just
 *  proving the transport + text/binary planes work before anything else. */
let link = null;

/** The parsed PipelineConfig, once a config.json has been picked — null otherwise. */
let config = null;

console.info(TAG, 'library imported ok — Link =', typeof Link);

configFile.addEventListener('change', async () => {
    const file = configFile.files[0];
    if (!file) return;
    console.debug(TAG, 'reading', file.name);
    const text = await file.text();
    const loaded = loadConfig(text);
    if (!loaded.ok) {
        config = null;
        console.error(TAG, 'config rejected:', loaded.errors);
        return;
    }
    config = loaded.config;
    console.info(TAG, 'config loaded —', config.machine.heads.length, 'head(s)');
    if (loaded.warnings.length) {
        for (const w of loaded.warnings) console.warn(TAG, 'config warning:', w);
    }
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

disconnectBtn.addEventListener('click', async () => {
    if (!link) return;
    disconnectBtn.disabled = true;
    await link.close();
    link = null;
    console.info(TAG, 'disconnected');
    connectBtn.disabled = false;
});
