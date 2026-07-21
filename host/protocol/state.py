"""
state.py — operational enums + getstate reply parsing.

Mirrors the machineState / alarmReason / runningReason values and the axes_homed
bitmask frozen in docs/wire_protocol.md. The host reads these via the `getstate`
control command (see host.protocol.commands.get_state).
"""

from dataclasses import dataclass
from enum import IntEnum


class MachineState(IntEnum):
    IDLE    = 0
    RUNNING = 1
    ESTOP   = 2   # transient inter-core flush signal; rarely seen by the host
    ALARM   = 3
    PAUSED  = 4
    HOMING  = 5


class AlarmReason(IntEnum):
    NONE        = 0
    ESTOP       = 1
    CONFIG      = 2   # Phase 2
    SOFT_LIMIT  = 3
    HOMING_FAIL = 4


class RunningReason(IntEnum):
    JOB = 0
    JOG = 1
    # Decelerating to rest after a pause/abort request (§4.5). A RunningReason
    # and not a MachineState: the machine IS running, so every IDLE/RUNNING/
    # PAUSED gate stays correct, and an older host reads it as plain RUNNING.
    ABORT_DECEL = 2


# axes_homed bitmask — bit0=X bit1=Y bit2=Z bit3=A
AXIS_BITS = {"x": 0x1, "y": 0x2, "z": 0x4, "a": 0x8}


def axis_mask(axes: str) -> int:
    """Mask for a string of axis letters, e.g. 'xy' -> 0b0011."""
    m = 0
    for a in axes:
        m |= AXIS_BITS.get(a, 0)
    return m


@dataclass(frozen=True)
class MachineStatus:
    """Parsed snapshot from a `getstate` reply."""
    state:        MachineState
    axes_homed:   int
    axes_enabled: int
    alarm:        AlarmReason
    running:      RunningReason
    # Everything below is only populated by the binary STATUS_RSP; a text
    # `getstate` reply cannot carry it and leaves the defaults in place.
    buf_count:    int = 0   # 0 = unknown (also a legitimate "ring is empty")
    # STATUS_RSP v2 (§4.2/§4.6). PARSED BUT NOT YET CONSUMED — position still
    # comes from the text `getpos` path and jog pacing still dead-reckons.
    # `None` distinguishes "this sample came from text" from a real zero.
    pos:          list | None = None   # [x, y, z, a] in steps
    expected_seq: int | None = None    # informational reconciliation only, not flow control
    queued_us:    int | None = None    # queued motion time, microseconds

    def homed(self, axis: str) -> bool:
        return bool(self.axes_homed & AXIS_BITS[axis])

    def enabled(self, axis: str) -> bool:
        return bool(self.axes_enabled & AXIS_BITS[axis])

    def all_homed(self, required_mask: int) -> bool:
        """True if every axis in required_mask is homed (the pre-flight/resume gate)."""
        return (self.axes_homed & required_mask) == required_mask

    def all_enabled(self, required_mask: int) -> bool:
        """True if every axis in required_mask is energised (a pre-flight gate)."""
        return (self.axes_enabled & required_mask) == required_mask

    def __str__(self) -> str:
        homed = "".join(a for a in "xyza" if self.axes_homed & AXIS_BITS[a]) or "-"
        en    = "".join(a for a in "xyza" if self.axes_enabled & AXIS_BITS[a]) or "-"
        return (f"{self.state.name} enabled={en} homed={homed} "
                f"alarm={self.alarm.name} running={self.running.name}")


def _to_int(tok: str) -> int:
    tok = tok.strip()
    return int(tok, 16) if tok.lower().startswith("0x") else int(tok)


def _enum_or(cls, fields, key, default):
    if key not in fields:
        return default
    try:
        return cls(_to_int(fields[key]))
    except ValueError:
        return default   # unknown enum value from a newer firmware — keep going


def _enum_or_int(cls, value, default):
    try:
        return cls(value)
    except ValueError:
        return default   # unknown enum value from a newer firmware — keep going


def parse_getstate(line: str) -> MachineStatus:
    """
    Parse a `getstate` reply line:
        state=<s> enabled=<hex> homed=<hex> alarm=<a> running=<r>

    Key=value tokens, space-separated. Tolerant of unknown trailing tokens
    (forward-compatible) and of out-of-range enum values. Requires at least
    `state` and `homed`; `enabled` defaults to 0 if absent. Raises ValueError if
    the line is not a status reply.
    """
    fields = {}
    for tok in line.strip().split():
        if "=" in tok:
            k, _, v = tok.partition("=")
            fields[k] = v
    if "state" not in fields or "homed" not in fields:
        raise ValueError(f"not a getstate reply: {line!r}")
    return MachineStatus(
        state=_enum_or(MachineState, fields, "state", MachineState.IDLE),
        axes_homed=_to_int(fields["homed"]),
        axes_enabled=_to_int(fields.get("enabled", "0")),
        alarm=_enum_or(AlarmReason, fields, "alarm", AlarmReason.NONE),
        running=_enum_or(RunningReason, fields, "running", RunningReason.JOB),
    )


def parse_status_rsp(data: bytes) -> MachineStatus:
    """
    Parse a binary STATUS_RSP packet (docs/wire_protocol.md) — a superset of
    `parse_getstate`, packed into 30 bytes instead of a text line. Used for the
    host UI's poll loop, including mid-stream, where the ASCII line would be a
    heavier and more awkward insertion between MSEG/jog packets.

    The frame also carries position, expectedSeq and queued motion time. Those
    are surfaced on the returned MachineStatus but nothing reads them yet — see
    docs/comms_architecture.md §5.
    """
    from host.protocol.packets import unpack_status_rsp   # avoid import cycle at module load
    fields = unpack_status_rsp(bytes(data))
    return MachineStatus(
        state=_enum_or_int(MachineState, fields["state"], MachineState.IDLE),
        axes_homed=fields["axes_homed"],
        axes_enabled=fields["axes_enabled"],
        alarm=_enum_or_int(AlarmReason, fields["alarm"], AlarmReason.NONE),
        running=_enum_or_int(RunningReason, fields["running"], RunningReason.JOB),
        buf_count=fields["buf_count"],
        pos=fields["pos"],
        expected_seq=fields["expected_seq"],
        queued_us=fields["queued_us"],
    )
