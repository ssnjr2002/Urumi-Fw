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

from host.protocol.state import (
    MachineState, AlarmReason, RunningReason, AXIS_BITS, axis_mask,
)

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
    In-process fake Pico — control plane only.

    Maintains a minimal operational state (machineState, axes_homed, position)
    and answers text commands per the wire_protocol.md allowed-state matrix.
    Binary data-plane packets are accepted and dropped (no motion simulation).
    """

    serial = None   # no raw port to lend a Sender

    def __init__(self):
        self.state      = MachineState.IDLE
        self.alarm      = AlarmReason.NONE
        self.running    = RunningReason.JOB
        self.axes_homed = 0
        self.pos        = [0, 0, 0, 0]
        self._replies   = []          # queued reply lines (bytes)

    def write(self, data: bytes):
        # Text line (control) vs binary packet (data plane): control commands are
        # lowercase ASCII ending in newline; everything else is a data packet.
        if data[:1].isalpha() and data.rstrip().isascii():
            line = data.decode("ascii", "replace").strip()
            if line:
                self._replies.append((self._handle(line) + "\n").encode())
        # binary packets: accepted, not simulated

    def readline(self, timeout=1.0) -> bytes:
        return self._replies.pop(0) if self._replies else b""

    def close(self):
        pass

    # one place that mirrors the control-plane behaviour
    def _handle(self, line: str) -> str:
        parts = line.split()
        cmd, args = parts[0], parts[1:]
        S, MS = self, MachineState
        idle_paused_alarm = (MS.IDLE, MS.PAUSED, MS.ALARM)

        if cmd == "ping":
            return "pong"
        if cmd == "pingnode":
            return f"node {args[0] if args else '?'} ok"
        if cmd == "getstate":
            return (f"state={int(S.state)} homed=0x{S.axes_homed:02x} "
                    f"alarm={int(S.alarm)} running={int(S.running)}")
        if cmd == "getpos":
            return "pos " + " ".join(str(p) for p in S.pos)
        if cmd == "stop":                       # always available
            S.state, S.alarm, S.axes_homed = MS.ALARM, AlarmReason.ESTOP, 0
            return "ok"
        if cmd == "enable":
            return "ok" if S.state in idle_paused_alarm else "err bad_state"
        if cmd == "disable":
            if S.state in idle_paused_alarm:
                S.axes_homed = 0
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
                S.state = MS.PAUSED
                return "ok"
            return "err bad_state"
        if cmd == "resume":
            if S.state == MS.PAUSED:
                S.state, S.running = MS.RUNNING, RunningReason.JOB
                return "ok"
            return "err bad_state"
        if cmd == "cancel":
            if S.state == MS.PAUSED:
                S.state = MS.IDLE
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

    def close(self):
        self.backend.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
