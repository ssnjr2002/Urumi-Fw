"""A serial port <-> one WebSocket client on localhost:8765; stays open across page reloads.

Usage: python -u com_bridge.py [port]   (default COM8)
"""
import base64, hashlib, socket, struct, sys, threading
import serial

port = serial.Serial(sys.argv[1] if len(sys.argv) > 1 else "COM8", 115200, timeout=0.01)
client = None
lock = threading.Lock()


def send(c, data):
    n = len(data)
    head = b"\x82" + (bytes([n]) if n < 126 else b"\x7e" + struct.pack(">H", n))
    with lock:
        c.sendall(head + data)


def from_serial():
    while True:
        data = port.read(4096)
        c = client
        if data and c:
            try:
                send(c, data)
            except OSError:
                pass


def recv_n(c, n):
    buf = b""
    while len(buf) < n:
        part = c.recv(n - len(buf))
        if not part:
            raise OSError("closed")
        buf += part
    return buf


def serve(c):
    global client
    req = b""
    while b"\r\n\r\n" not in req:
        req += c.recv(4096)
    key = next(l.split(b":", 1)[1].strip() for l in req.split(b"\r\n") if l.lower().startswith(b"sec-websocket-key"))
    accept = base64.b64encode(hashlib.sha1(key + b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11").digest())
    c.sendall(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept + b"\r\n\r\n")
    port.reset_input_buffer()
    client = c
    print("client connected", flush=True)
    try:
        while True:
            b0, b1 = recv_n(c, 2)
            n = b1 & 127
            if n == 126:
                n = struct.unpack(">H", recv_n(c, 2))[0]
            elif n == 127:
                n = struct.unpack(">Q", recv_n(c, 8))[0]
            mask = recv_n(c, 4)
            data = bytes(x ^ mask[i % 4] for i, x in enumerate(recv_n(c, n)))
            if b0 & 15 == 8:
                break
            if b0 & 15 == 2:
                port.write(data)
    except OSError:
        pass
    client = None
    c.close()
    print("client gone", flush=True)


threading.Thread(target=from_serial, daemon=True).start()
srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", 8765))
srv.listen(1)
print("bridge up", flush=True)
while True:
    conn, _ = srv.accept()
    conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    threading.Thread(target=serve, args=(conn,), daemon=True).start()
