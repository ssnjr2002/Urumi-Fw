// Paste into the page before pressing Connect, again after each reload:
// Connect then opens com_bridge.py instead of the browser's port picker.
navigator.serial.requestPort = async () => {
    let ws, rc;
    return {
        readable: new ReadableStream({ start(c) { rc = c; }, cancel() { ws.close(); } }),
        writable: new WritableStream({ write(b) { ws.send(b); } }),
        open: () => new Promise((ok, no) => {
            ws = new WebSocket('ws://127.0.0.1:8765');
            ws.binaryType = 'arraybuffer';
            ws.onopen = ok;
            ws.onerror = () => no(new Error('no bridge'));
            ws.onmessage = (e) => rc.enqueue(new Uint8Array(e.data));
            ws.onclose = () => { try { rc.close(); } catch { /* already closed */ } };
        }),
        close: async () => { ws.close(); },
    };
};
