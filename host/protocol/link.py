"""
link.py — owns the Pico USB link and the two-plane framing.

Two planes share one USB pipe (docs/wire_protocol.md "Two planes on one USB pipe"):
  - control plane: text lines, synchronous request/response (write line -> read
    reply line). This is what the GUI/orchestrator poll and command with.
  - data plane: binary packets (MSEG/JOG) streamed via host.protocol.stream.Sender,
    which borrows the underlying serial during a stream (control polling pauses,
    exactly as the old jog_ui did).

Backends:
  SerialBackend — real pyserial connection to the Pico.
  SimBackend    — an in-process fake Pico for the control plane, so the host
                  side (GUI, pre-flight, pause choreography) is testable before
                  the firmware track (steps 2-5) exists. It is a TEST DOUBLE: it
                  mirrors the wire_protocol.md control commands + allowed-state
                  matrix, not the real-time motion.

`Link.command(text)` is the one call the control plane needs. For streaming,
`link.serial` exposes the raw port to a Sender (real backend only).
"""

import threading, time
from collections import deque

from host.protocol.state import (
    MachineState, AlarmReason, RunningReason, AXIS_BITS, axis_mask,
)
from host.protocol.packets import unpack_microsegment

try:
    import serial as _pyserial
except ImportError:
    _pyserial = None


# ── backends ──────────────────────────────────────────────────────────────────

class SerialBackend:
    """Real pyserial connection. write() bytes, readline() one text line."""

    def __init__(self, port, baud=115200, timeout=0.2):
        if _pyserial is None:
            raise RuntimeError("pyserial not installed — pip install pyserial")
        self.serial = _pyserial.Serial(port, baud, timeout=timeout)

    def write(self, data: bytes):
        self.serial.write(data)
        self.serial.flush()

    def readline(self, timeout=1.0) -> bytes:
        self.serial.timeout = timeout
        return self.serial.readline()

    def close(self):
        self.serial.close()


class SimBackend:
    """
    In-process fake Pico — control plane + a paced motion executor.

    Answers text commands per the wire_protocol.md allowed-state matrix, AND
    "executes" streamed data-plane packets: their step deltas integrate into the
    tracked position over time while the state holds RUNNING, so the GUI shows a
    job/jog actually move and finish (returning to IDLE, or to PAUSED for a jog
    issued during a pause). pause holds the executor; resume continues it; stop /
    cancel flush the remaining motion. A test double — not real-time accurate.
    """

    serial = None   # no raw port to lend a Sender

    FRAME_S = 0.04          # executor wall-clock tick
    _CHUNK_DIV = 20         # packets applied per tick ≈ remaining // this (min 1)

    def __init__(self):
        self.state        = MachineState.IDLE
        self.alarm        = AlarmReason.NONE
        self.running      = RunningReason.JOB
        self.axes_homed   = 0
        self.axes_enabled = 0          # energised-axis bitmask (enable -> all present)
        self.pos          = [0, 0, 0, 0]
        self._replies   = []                 # queued reply lines (bytes)
        self._lock      = threading.RLock()  # guards state/pos/motion vs executor
        self._motion    = deque()            # pending (dx,dy,dz,da) step deltas
        self._executing = False              # a burst is in progress
        self._return_state = MachineState.IDLE
        self._exec = threading.Thread(target=self._executor, daemon=True)
        self._exec.start()

    def write(self, data: bytes):
        # Text line (control) vs binary packet (data plane): control commands are
        # lowercase ASCII ending in newline; everything else is a motion packet.
        if data[:1].isalpha() and data.rstrip().isascii():
            line = data.decode("ascii", "replace").strip()
            if line:
                with self._lock:
                    self._replies.append((self._handle(line) + "\n").encode())
            return
        try:
            ms = unpack_microsegment(bytes(data))
        except Exception:
            return                            # not a recognised packet — drop
        with self._lock:
            if self.state in (MachineState.ALARM, MachineState.HOMING):
                return                        # stream not accepted in these states
            if not self._executing:
                # start a burst. From IDLE → returns to IDLE (a job). From PAUSED
                # → a jog during pause, returns to PAUSED. From RUNNING → the
                # resumed continuation after a tool-change PAUSE; returns to IDLE.
                self._return_state = (MachineState.PAUSED
                                      if self.state == MachineState.PAUSED
                                      else MachineState.IDLE)
                self.running = (RunningReason.JOG if self.state == MachineState.PAUSED
                                else RunningReason.JOB)
                self.state = MachineState.RUNNING
                self._executing = True
            self._motion.append((ms["dx"], ms["dy"], ms["dz"], ms["da"], ms["flags"]))

    def readline(self, timeout=1.0) -> bytes:
        with self._lock:
            return self._replies.pop(0) if self._replies else b""

    def close(self):
        pass

    def _executor(self):
        """Drain queued motion into position while RUNNING; finish -> return_state."""
        while True:
            time.sleep(self.FRAME_S)
            with self._lock:
                if not self._executing or self.state != MachineState.RUNNING:
                    continue                  # idle, or paused/alarmed — hold
                if not self._motion:
                    self._executing = False   # burst complete
                    self.state = self._return_state
                    self.running = RunningReason.JOB
                    continue
                n = max(1, len(self._motion) // self._CHUNK_DIV)
                for _ in range(min(n, len(self._motion))):
                    dx, dy, dz, da, flags = self._motion.popleft()
                    self.pos[0] += dx; self.pos[1] += dy
                    self.pos[2] += dz; self.pos[3] += da
                    if flags & 0x04:          # MSEG_FLAG_PAUSE — predetermined stop
                        self.state = MachineState.PAUSED
                        self._executing = False   # resume + next stream starts anew
                        break

    # one place that mirrors the control-plane behaviour (called under _lock)
    def _handle(self, line: str) -> str:
        parts = line.split()
        cmd, args = parts[0], parts[1:]
        S, MS = self, MachineState
        idle_paused_alarm = (MS.IDLE, MS.PAUSED, MS.ALARM)

        if cmd == "ping":
            return "pong"
        if cmd == "seqreset":
            return "seq reset"    # sim doesn't track seq; just acknowledge
        if cmd == "pingnode":
            return f"node {args[0] if args else '?'} ok"
        if cmd == "getstate":
            return (f"state={int(S.state)} enabled=0x{S.axes_enabled:02x} "
                    f"homed=0x{S.axes_homed:02x} "
                    f"alarm={int(S.alarm)} running={int(S.running)}")
        if cmd == "getpos":
            return "pos " + " ".join(str(p) for p in S.pos)
        if cmd == "stop":                       # always available; de-energises
            S.state, S.alarm = MS.ALARM, AlarmReason.ESTOP
            S.axes_homed = S.axes_enabled = 0
            S._motion.clear(); S._executing = False
            return "ok"
        if cmd == "enable":
            if S.state in idle_paused_alarm:
                if not args or args[0] == "all":
                    S.axes_enabled = axis_mask("xyza")   # energise all present axes
                else:
                    node = int(args[0])
                    S.axes_enabled |= (1 << (node - 1))
                return "ok"
            return "err bad_state"
        if cmd == "disable":
            if S.state in idle_paused_alarm:
                if not args or args[0] == "all":
                    S.axes_homed = S.axes_enabled = 0    # de-energise -> position invalid
                else:
                    node = int(args[0])
                    S.axes_enabled &= ~(1 << (node - 1))
                    S.axes_homed   &= ~(1 << (node - 1))
                return "ok"
            return "err bad_state"
        if cmd == "setorigin":
            if S.state in idle_paused_alarm:
                axes = args[0] if args else "xyza"
                S.axes_homed |= axis_mask(axes)
                for i, a in enumerate("xyza"):
                    if a in axes:
                        S.pos[i] = 0
                if S.state == MS.ALARM:         # setorigin recovers from ALARM
                    S.state, S.alarm = MS.IDLE, AlarmReason.NONE
                return "ok"
            return "err bad_state"
        if cmd == "pause":
            if S.state == MS.RUNNING:
                S.state = MS.PAUSED             # executor holds; motion retained
                return "ok"
            return "err bad_state"
        if cmd == "resume":
            if S.state == MS.PAUSED:
                # Phase 1: host pre-positions before resuming; Pico returns to IDLE
                # and accepts a fresh stream for the next operation (mirrors firmware).
                S.state = MS.IDLE
                S._motion.clear(); S._executing = False
                return "ok"
            return "err bad_state"
        if cmd == "cancel":
            if S.state == MS.PAUSED:
                S.state = MS.IDLE
                S._motion.clear(); S._executing = False
                return "ok"
            return "err bad_state"
        if cmd == "unalarm":
            if S.state == MS.ALARM:
                S.state, S.alarm = MS.IDLE, AlarmReason.NONE
                return "ok"
            return "err bad_state"
        return "err unknown"

    # test-only hook: drive the sim into RUNNING so pause/resume can be exercised
    def _force_running(self):
        with self._lock:
            self.state = MachineState.RUNNING


# ── link ──────────────────────────────────────────────────────────────────────

class Link:
    """Owns a backend; offers control-plane request/response + raw packet send."""

    def __init__(self, backend):
        self.backend = backend

    @classmethod
    def open_serial(cls, port, baud=115200, timeout=0.2) -> "Link":
        return cls(SerialBackend(port, baud, timeout))

    @classmethod
    def open_sim(cls) -> "Link":
        return cls(SimBackend())

    @property
    def serial(self):
        """Raw pyserial port for a Sender during streaming (None for the sim)."""
        return self.backend.serial

    def command(self, text: str, timeout=1.0) -> str:
        """Send one control-plane line and return the reply line (stripped)."""
        self.backend.write((text + "\n").encode("ascii"))
        return self.backend.readline(timeout).decode("ascii", "replace").strip()

    def write_packet(self, data: bytes):
        """Send a raw data-plane packet (used by higher-level streaming)."""
        self.backend.write(data)

    def stream(self, packets, window=16, verbose=False) -> bool:
        """
        Stream a data-plane burst (a job, or a jog burst like return-to-pausePos)
        with Go-Back-N ACK/NACK. Borrows the raw port for the duration; the caller
        must pause control-plane polling while streaming (the port is single-owner
        during a stream — same discipline the old jog_ui used).

        On the simulator (no raw port) the packets are accepted without an ACK
        loop and True is returned, so the host side is exercisable offline.
        """
        if self.serial is None:
            for p in packets:
                self.write_packet(p)
            return True
        from host.protocol.stream import Sender
        sender = Sender(self.serial, window=window, verbose=verbose)
        try:
            return sender.send_stream(packets)
        finally:
            sender.stop()

    def close(self):
        self.backend.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
