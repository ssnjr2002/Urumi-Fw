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
from host.protocol.packets import (
    unpack_microsegment, pack_status_rsp, MAGIC_STATUS_REQ, STATUS_RSP_SIZE,
    MAGIC_ACK, MAGIC_NACK, NACK_FULL, NACK_BAD_STATE, MAGIC_SEQRESET,
    MAGIC_ABORT, NACK_ABORTING,
)
from host.protocol.state import parse_status_rsp
from host.protocol.reader import Demux, Reader, make_sinks
from host.protocol.writer import Writer
from host.protocol.session import Session, ListSource

try:
    import serial as _pyserial
except ImportError:
    _pyserial = None


# ── backends ──────────────────────────────────────────────────────────────────

class SerialBackend:
    """Real pyserial connection. Writes only — inbound bytes belong to the Reader
    thread, which owns the port for the connection's lifetime (D1)."""

    # Short, FIXED port timeout. Nothing may mutate it after construction: the
    # reader loop blocks on read(1) at this timeout, so raising it (as the old
    # readline() did, to serve a 1s control-plane wait) throttled the whole
    # inbound path and caused Go-Back-N stalls. With one reader there is no
    # per-call timeout to honour here at all — callers wait on their sink.
    BASE_TIMEOUT = 0.05

    def __init__(self, port, baud=115200, timeout=0.2):
        if _pyserial is None:
            raise RuntimeError("pyserial not installed — pip install pyserial")
        self.serial = _pyserial.Serial(port, baud, timeout=self.BASE_TIMEOUT)

    def attach(self, demux):
        self.reader = Reader(self.serial, demux).start()

    def write(self, data: bytes):
        self.serial.write(data)

    def flush(self):
        self.serial.flush()

    def close(self):
        reader = getattr(self, "reader", None)
        if reader:
            reader.stop()
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

    serial = None   # no raw port — replies are fed straight into the demux

    RING_SIZE = 64          # bounded like masterBuf, so backpressure is real
    ACK_COALESCE_MAX = 8    # mirrors ACK_COALESCE_MAX in shared.h
    FRAME_S = 0.04          # executor wall-clock tick
    F_CPU   = 150_000_000   # matches host/config/loader.py's machine default; used to
                            # convert a packet's `interval` (CPU cycles/step) to seconds

    def __init__(self):
        self.state        = MachineState.IDLE
        self.alarm        = AlarmReason.NONE
        self.running      = RunningReason.JOB
        self.axes_homed   = 0
        self.axes_enabled = 0          # energised-axis bitmask (enable -> all present)
        self.pos          = [0, 0, 0, 0]
        self._demux     = None               # set by attach(); replies go here
        self._expected_seq = 0               # mirrors the firmware's expectedSeq
        self._pending_acks = 0               # accepted but not yet confirmed
        self._aborting  = False              # abort barrier (see MAGIC_ABORT)
        self._lock      = threading.RLock()  # guards state/pos/motion vs executor
        self._motion    = deque()            # pending (dx,dy,dz,da,interval,flags)
        self._executing = False              # a burst is in progress
        self._time_credit = 0.0              # banked sim-seconds not yet spent (see _executor)
        self._return_state = MachineState.IDLE
        self._exec = threading.Thread(target=self._executor, daemon=True)
        self._exec.start()

    def attach(self, demux):
        self._demux = demux

    def flush(self):
        pass

    def _reply(self, data: bytes):
        if self._demux is not None:
            self._demux.feed(data)

    def write(self, data: bytes):
        # The Writer may hand us a BATCH of frames now, not one packet, so split
        # before dispatching. Text lines and STATUS_REQ are always written alone.
        if data[:1].isalpha() and data.rstrip().isascii():
            line = data.decode("ascii", "replace").strip()
            if line:
                with self._lock:
                    self._reply((self._handle(line) + "\n").encode())
            return
        if data == bytes([MAGIC_ABORT]):
            with self._lock:
                # The sim has no step loop to ramp, so it models the OUTCOME:
                # motion ends, the ring is discarded, position is kept. The ramp
                # distance the real machine would still travel is not simulated —
                # a test asserting exact post-abort position would be asserting
                # something hardware will not reproduce.
                self._motion.clear()
                self._executing = False
                self._time_credit = 0.0
                self._aborting = True
                if self.state in (MachineState.RUNNING, MachineState.PAUSED):
                    self.state = MachineState.IDLE
                self.running = RunningReason.JOB
                self._aborting = False
            return
        if data == bytes([MAGIC_SEQRESET]):
            with self._lock:
                self._expected_seq = 0
                self._pending_acks = 0
                self._reply(bytes([MAGIC_ACK, 0x00, 0x00]))
            return
        if data == bytes([MAGIC_STATUS_REQ]):
            with self._lock:
                self._reply(pack_status_rsp(
                    int(self.state), self.axes_enabled, self.axes_homed,
                    int(self.alarm), int(self.running), len(self._motion),
                    pos=self.pos, expected_seq=self._expected_seq,
                    queued_us=self._queued_us()))
            return
        for i in range(0, len(data) - 25, 26):
            self._write_packet(bytes(data[i:i + 26]))
        self._flush_ack()      # end of batch == the firmware's drain-empty flush

    # ── coalesced ACKs (mirrors data_plane.cpp §4.1) ──────────────────────────
    # The ACK is cumulative, so one frame confirms every packet accepted since
    # the last flush. Deferring them is what the firmware does; the sim does it
    # too so the host suites actually exercise the multi-packet advance rather
    # than only ever seeing +1 deltas.

    def _flush_ack(self):
        with self._lock:
            if self._pending_acks:
                self._pending_acks = 0
                self._reply(bytes([MAGIC_ACK, self._expected_seq, 0x00]))

    def _mark_ack(self):
        self._pending_acks += 1
        if self._pending_acks >= self.ACK_COALESCE_MAX:
            self._pending_acks = 0
            self._reply(bytes([MAGIC_ACK, self._expected_seq, 0x00]))

    def _write_packet(self, data: bytes):
        try:
            ms = unpack_microsegment(data)
        except Exception:
            return                            # not a recognised packet — drop

        # Mirror the firmware's seq duplicate guard (data_plane.cpp): a stale
        # retransmit after a go-back is ACKed but NOT executed. Without this the
        # sim would duplicate motion on every retry and silently disagree with
        # hardware about final position.
        with self._lock:
            if data[22] != self._expected_seq:
                self._pending_acks = 0        # immediate: the host's resync signal
                self._reply(bytes([MAGIC_ACK, self._expected_seq, 0x00]))
                return
            if self.state in (MachineState.ALARM, MachineState.HOMING):
                self._flush_ack()             # ACKs earned before a rewind land first
                self._reply(bytes([MAGIC_NACK, NACK_BAD_STATE, 0x00]))
                return                        # stream not accepted in these states
            if len(self._motion) >= self.RING_SIZE:
                self._flush_ack()
                self._reply(bytes([MAGIC_NACK, NACK_FULL, 0x00]))
                return                        # backpressure — sender retries
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
            self._motion.append((ms["dx"], ms["dy"], ms["dz"], ms["da"], ms["interval"], ms["flags"]))
            self._expected_seq = (self._expected_seq + 1) & 0xFF
            self._mark_ack()

    def _queued_us(self):
        """Queued motion time — the sim's mirror of the firmware's queuedUs (§4.6).

        The firmware maintains this incrementally as a single-writer counter
        pair because two cores touch it; here one lock covers the deque, so
        summing on demand is both simpler and exactly equivalent. Same
        whole-segment granularity: the executing segment counts in full.
        """
        total = 0.0
        for dx, dy, dz, da, interval, _flags in self._motion:
            steps = max(abs(dx), abs(dy), abs(dz), abs(da), 1)
            total += (interval * steps) / self.F_CPU
        return int(total * 1_000_000)

    def close(self):
        pass

    def _executor(self):
        """
        Drain queued motion into position while RUNNING; finish -> return_state.

        Paced by each packet's own `interval` (CPU cycles/step) converted to
        seconds via F_CPU, not by packet count — a packet's real duration is
        `interval * steps / F_CPU`, so a 10mm@10mm/s jog takes ~1s here just
        like it would on real hardware, regardless of how many packets the
        planner split it into. `_time_credit` banks unspent wall-clock time
        across ticks so a packet whose duration exceeds one FRAME_S tick still
        gets applied atomically (position updates can't be split mid-packet)
        once enough ticks have accrued to cover it.
        """
        while True:
            time.sleep(self.FRAME_S)
            with self._lock:
                if not self._executing or self.state != MachineState.RUNNING:
                    self._time_credit = 0.0
                    continue                  # idle, or paused/alarmed — hold
                if not self._motion:
                    self._executing = False   # burst complete
                    self.state = self._return_state
                    self.running = RunningReason.JOB
                    self._time_credit = 0.0
                    continue
                self._time_credit += self.FRAME_S
                while self._motion:
                    dx, dy, dz, da, interval, flags = self._motion[0]
                    steps = max(abs(dx), abs(dy), abs(dz), abs(da), 1)
                    duration = (interval * steps) / self.F_CPU
                    if duration > self._time_credit:
                        break                  # not enough banked time yet
                    self._motion.popleft()
                    self._time_credit -= duration
                    self.pos[0] += dx; self.pos[1] += dy
                    self.pos[2] += dz; self.pos[3] += da
                    if flags & 0x04:          # MSEG_FLAG_PAUSE — predetermined stop
                        self.state = MachineState.PAUSED
                        self._executing = False   # resume + next stream starts anew
                        self._time_credit = 0.0
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
            self._expected_seq = 0
            self._pending_acks = 0      # a deferred ACK names the old numbering
            return "seq reset"
        if cmd == "pingnode":
            # Mirrors the firmware: `all` (or no arg) answers on ONE line, not
            # one per node. The text plane is one line per command.
            if not args or args[0] == "all":
                return "nodes " + " ".join(f"{n}=ok" for n in range(1, 5))
            return f"node {args[0]} ok"
        if cmd == "getstate":
            return (f"state={int(S.state)} enabled=0x{S.axes_enabled:02x} "
                    f"homed=0x{S.axes_homed:02x} "
                    f"alarm={int(S.alarm)} running={int(S.running)}")
        if cmd == "getpos":
            # Trailing validity mask, as the firmware does — the counts are always
            # plain numbers, never a sentinel.
            return ("pos " + " ".join(str(p) for p in S.pos)
                    + f" homed=0x{S.axes_homed:02x}")
        if cmd == "stop":                       # always available; de-energises
            S.state, S.alarm = MS.ALARM, AlarmReason.ESTOP
            S.axes_homed = S.axes_enabled = 0
            S._motion.clear(); S._executing = False
            return "ok"
        if cmd == "axes_enable":                # bound axis slots only, never peripherals
            if S.state in idle_paused_alarm:
                if not args:
                    return "err usage"
                on = args[0] in ("1", "on", "On", "ON")
                if on:
                    S.axes_enabled = axis_mask("xyza")   # energise all present axes
                else:
                    S.axes_homed = S.axes_enabled = 0    # de-energise -> position invalid
                return "ok"
            return "err bad_state"
        if cmd == "bus_enable":                 # whole-bus broadcast, unacknowledged
            if S.state in idle_paused_alarm:
                if not args:
                    return "err usage"
                # `on` deliberately arms nothing: nobody ACKs a broadcast, so the
                # master may not conclude a node is energised. Mirrors firmware.
                if args[0] not in ("1", "on", "On", "ON"):
                    S.axes_homed = S.axes_enabled = 0
                return "ok"
            return "err bad_state"
        if cmd == "enable":
            if S.state in idle_paused_alarm:
                if not args:
                    return "err bad_node"
                node = int(args[0])
                S.axes_enabled |= (1 << (node - 1))
                return "ok"
            return "err bad_state"
        if cmd == "disable":
            if S.state in idle_paused_alarm:
                if not args:
                    return "err bad_node"
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
    """Owns a backend, one reader, one writer, and the sink set.

    Concerns SUBSCRIBE rather than seize (docs/comms_architecture.md D1/D3):
    command(), get_status() and stream() can all be in flight at once, because
    each awaits its own sink while the single reader keeps draining the port.
    """

    def __init__(self, backend):
        self.backend = backend
        self.sinks = make_sinks()
        self.demux = Demux(self.sinks["ack"], self.sinks["status"],
                           self.sinks["text"], self.sinks["cfg"])
        backend.attach(self.demux)
        self.writer = Writer(backend)
        # Text is strictly one-outstanding (D11). The writer guarantees frame
        # atomicity but not that two callers won't each be awaiting the text
        # sink at once — with concurrent pollers and UI commands, whoever gets
        # scheduled first takes the other's reply. This makes the rule real.
        self._text_lock = threading.Lock()
        # Orphaned text lines discarded by command(). Should stay 0 — anything
        # else means some command replied with more lines than it is allowed to,
        # which is a firmware contract bug and worth surfacing rather than
        # silently absorbing.
        self.text_desyncs = 0

    @classmethod
    def open_serial(cls, port, baud=115200, timeout=0.2) -> "Link":
        link = cls(SerialBackend(port, baud, timeout))
        # The Pico prints a banner on (re)entering its main loop, and bytes from
        # a previous process may still be in the OS buffer. Both are unsolicited
        # text, legitimately so. Drop them at connect, before text_desyncs starts
        # counting, so a non-zero count means a genuine contract breach rather
        # than "we just connected".
        time.sleep(0.2)
        link.sinks["text"].clear()
        return link

    @classmethod
    def open_sim(cls) -> "Link":
        return cls(SimBackend())

    @property
    def serial(self):
        """Raw pyserial port (None for the sim). NOT for reading — the Reader
        owns inbound bytes. Kept only for port-level operations."""
        return self.backend.serial

    def command(self, text: str, timeout=1.0) -> str:
        """Send one control-plane line and return the reply line (stripped).

        No flush of the PORT beforehand: routing on magic means a stale status
        reply or a stream ACK cannot land in the text sink (D10). Text stays
        strictly one-outstanding, so the next line in the sink is ours.

        The text sink itself is drained first, which is a different thing and
        not a violation of D10. Under this lock, one-outstanding means any line
        already sitting there is orphaned — its awaiter timed out, or the Pico
        emitted more lines than the command contract allows. Leaving orphans
        would make every later command read the previous one's tail, so a single
        contract breach desyncs the plane permanently rather than transiently.
        (`pingnode all` used to be exactly that: four lines for one command.)
        """
        with self._text_lock:
            stale = self.sinks["text"].clear()
            if stale:
                self.text_desyncs += stale
            self.writer.write_text(text)
            return self.sinks["text"].get(timeout=timeout) or ""

    def send(self, text: str):
        """Fire-and-forget control command — needs the writer, not a reply slot.
        `stop` is the case that matters: estop must never queue behind a pending
        text command, and it correlates nothing (confirmation arrives on the
        status sink as the state goes ESTOP→ALARM)."""
        self.writer.write_text(text)

    def get_status(self, timeout=1.0):
        """Binary mirror of `command("getstate")` — one byte out, one frame back.
        Cheap enough to poll during a stream, since it slots into a boundary
        between MSEG packets instead of needing a whole ASCII line."""
        before = self.sinks["status"]._stamp
        self.writer.write_frame(bytes([MAGIC_STATUS_REQ]))
        data, _ = self.sinks["status"].wait_update(timeout=timeout, since=before)
        if data is None:
            raise TimeoutError("no STATUS_RSP within timeout")
        return parse_status_rsp(data)

    @property
    def status(self):
        """Latest status sample without a round trip, or None. This is what an
        open (jog) session reads to decide when to blend or wind down."""
        raw = self.sinks["status"].value
        return parse_status_rsp(raw) if raw else None

    def abort(self):
        """Soft abort (§4.5): ramp to rest, flush the ring, land IDLE with
        position intact.

        Fire-and-forget, like `stop` — it correlates nothing, so it takes the
        writer lock but no reply slot and can never queue behind a pending text
        command. Confirmation arrives on the status sink as the state settles.

        Packets sent after this get NACK_ABORTING until the machine reaches
        IDLE. That is a barrier, not an error: wait and reopen.
        """
        self.writer.write_frame(bytes([MAGIC_ABORT]))

    def reset_seq(self, timeout=1.0):
        """Align the Pico's expectedSeq with a session's fresh seq counter. Every
        session stamps from 0, so this must precede one.

        Binary (§4.3): one byte out, ACK(0) back on the ack sink — so stream
        start no longer drags a pure data-plane session through the
        one-outstanding text plane. The reply is drained HERE rather than left
        for the session, which would otherwise open on a stale ACK.
        """
        self.sinks["ack"].clear()
        self.writer.write_frame(bytes([MAGIC_SEQRESET]))
        return self.sinks["ack"].get(timeout=timeout) is not None

    def stream(self, packets, window=16, verbose=False) -> bool:
        """Stream a closed sequence (a job, or a jog burst) with Go-Back-N."""
        self.reset_seq()
        return self.session(ListSource(list(packets)), window, verbose).run()

    def session(self, source, window=16, verbose=False) -> Session:
        """Build a Session over any PacketSource — use this directly for an OPEN
        session (manual jogging), where packets are produced in response to
        operator input and the session ends by truncation rather than exhaustion.

        Caller must reset_seq() first; stream() does it for you."""
        return Session(self.writer, self.sinks["ack"], source,
                       status_sink=self.sinks["status"],
                       window=window, verbose=verbose)

    def close(self):
        self.backend.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
