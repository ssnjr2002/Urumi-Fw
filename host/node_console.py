#!/usr/bin/env python3
"""node_console.py — talk to a node directly over the USART0 bench console.

Pairs with the -DNODE_DEBUG_CONSOLE firmware build (env vac_node5_dbg). It sends
command frames straight to the node over a plain serial link (USART0, PB2/PB3),
bypassing the Pico and the RS485 bus entirely — for bench-testing node logic.

Wire format (see src/node/debug_console.cpp): each command is one ASCII line of
space-separated hex bytes  [id] [cmd] [len] [payload...]  WITHOUT the CRC — the
node appends it. The node replies "OK <hex...>" / "NAK ..." / "ERR ...".

Usage:
    # one-shot:
    python node_console.py --port COM7 switch
    python node_console.py --port COM7 servo 1 90     # servo 1 -> 90 deg
    python node_console.py --port COM7 servo 0 180     # all servos -> 180 deg
    python node_console.py --port COM7 ssr 1
    python node_console.py --port COM7 raw 05 14 00      # arbitrary frame (no CRC)

    # interactive REPL (no command given):
    python node_console.py --port COM7 --id 7
    node5> ping
    node5> servo 3 45
    node5> switch

Requires: pyserial  (pip install pyserial)
"""
import argparse
import sys

try:
    import serial  # pyserial
except ImportError:
    sys.exit("pyserial not installed — run: pip install pyserial")

# Command opcodes — keep in sync with include/common.h.
CMD = {
    "ping":    0x01,
    "type":    0x06,
    "enable":  0x04,
    "disable": 0x05,
    # getpos / engage / nodestat all answer with the SAME status payload —
    # [type][flags][pos int32 BE][slot] for a stepper — from one serializer on the
    # node (buildNodeStatus). engage therefore reports position and enabled state
    # on the same transaction that does the bind.
    "getpos":  0x03,   # stepper only
    "engage":  0x20,   # stepper: slot(0..3), 255=disengage
    "laser":   0x21,   # stepper (laser node only): state(0/1)
    "nodestat":0x22,   # any type; tail varies by type
    "datum":   0x23,   # arm the continuity witness (flags bit1); reply = status
    "servo":   0x10,   # vacuum: idx(0=all,1..N) angle(0..180)
    "ssr":     0x11,   # vacuum: state(0/1)
    "switch":  0x14,   # vacuum: read NC switch level
    "knife":   0x12,   # knife: state(0/1)
    "blower":  0x13,   # knife: duty(0..100)
}


def build_frame(node_id, tokens):
    """Turn a mnemonic + args into frame bytes [id][cmd][len][payload...] (no CRC).

    'raw <hex...>' passes the given bytes through verbatim (already a full frame
    minus CRC), so you can hand-craft anything the firmware might grow.
    """
    verb = tokens[0].lower()

    if verb == "raw":
        return bytes(int(t, 16) for t in tokens[1:])

    if verb not in CMD:
        raise ValueError(f"unknown command '{verb}' "
                         f"(known: {', '.join(CMD)}, or 'raw')")

    payload = [int(t, 0) & 0xFF for t in tokens[1:]]
    return bytes([node_id, CMD[verb], len(payload), *payload])


def send(ser, frame, echo=True):
    line = " ".join(f"{b:02X}" for b in frame)
    if echo:
        print(f"  -> {line}")
    ser.reset_input_buffer()
    ser.write((line + "\n").encode("ascii"))
    ser.flush()
    reply = ser.readline().decode("ascii", "replace").strip()
    print(f"  <- {reply}" if reply else "  <- (timeout)")
    return reply


def main():
    ap = argparse.ArgumentParser(description="Direct USART0 bench console for a node.")
    ap.add_argument("--port", required=True, help="serial port, e.g. COM7 or /dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=115200, help="console baud (default 115200)")
    ap.add_argument("--id", type=int, default=5, help="node id byte (default 5)")
    ap.add_argument("--timeout", type=float, default=0.5, help="read timeout seconds")
    ap.add_argument("--no-dtr", action="store_true",
                    help="deassert DTR/RTS on open (only for adapters that "
                         "auto-reset the node; NOT the CH340 UPDI adapter)")
    ap.add_argument("cmd", nargs=argparse.REMAINDER,
                    help="one command to run then exit; omit for interactive mode")
    args = ap.parse_args()

    # NOTE on DTR/RTS: leave them at pyserial's default (asserted). The CH340
    # serial-UPDI adapter used here needs DTR/RTS asserted for the UART to work —
    # deasserting them garbles/kills the link. (If you use a *different* adapter
    # that auto-resets the node on connect, pass --no-dtr to deassert instead.)
    ser = serial.Serial()
    ser.port = args.port
    ser.baudrate = args.baud
    ser.timeout = args.timeout
    if args.no_dtr:
        ser.dtr = False
        ser.rts = False
    ser.open()
    with ser:
        if args.cmd:
            try:
                send(ser, build_frame(args.id, args.cmd))
            except ValueError as e:
                sys.exit(f"error: {e}")
            return

        print(f"connected {args.port} @ {args.baud}  (node id {args.id})")
        print("commands: " + ", ".join(CMD) + ", raw <hex...>   |  Ctrl-C to quit")
        while True:
            try:
                raw = input(f"node{args.id}> ").strip()
            except (EOFError, KeyboardInterrupt):
                print()
                break
            if not raw:
                continue
            if raw in ("quit", "exit"):
                break
            try:
                send(ser, build_frame(args.id, raw.split()))
            except ValueError as e:
                print(f"  error: {e}")


if __name__ == "__main__":
    main()
