"""
plan_io.py — the Plan data model, plus save/load as a self-describing binary
.plan file.

Plan/ToolOperation are the in-memory representation shared by every stage
that touches a job after planner.plan_job builds it: this file's own
save_plan/load_plan, host.execution.job_runner, and the GUI. They live here
— next to the format they describe — rather than in planner.py, since nothing
about them is specific to planning; they're just what a Plan *is*.

Format (all little-endian):
  magic:    4B  b'\xAB\xCD\x50\x01'
  version:  1B  0x01
  n_tools:  1B  unique ToolType values in manifest (for upfront feasibility check)
  n_ops:    2B  total operation count

  TOOL MANIFEST  (n_tools × 1B ToolType enum value)

  OPERATIONS  (n_ops × interleaved blocks):
    tool_type: 1B   ToolType enum value
    pkt_count: 4B   number of MSEG packets that follow
    packets:   pkt_count × PACKET_SIZE bytes

The PAUSE flag is NOT baked into packets here — the sender (job_runner.send_plan)
injects it at the last packet of each non-final operation, same as for in-memory
Plans. One plan file runs on any machine (single or dual head).
"""

import struct
from dataclasses import dataclass, field
from pipeline.config import ToolType, TOOL_PROFILES_BY_TYPE, can_run_tool
from host.protocol.packets import PACKET_SIZES

MAGIC   = b'\xAB\xCD\x50\x01'
VERSION = 0x01

_HDR     = struct.Struct("<4sBBH")   # magic, version, n_tools, n_ops
_OP_HDR  = struct.Struct("<BI")      # tool_type (1B), pkt_count (4B)


@dataclass
class ToolOperation:
    tool:    str           # ToolProfile.name
    profile: object        # the ToolProfile
    packets: list          # list[bytes] — MSEG step packets for this op


@dataclass
class Plan:
    operations: list = field(default_factory=list)   # in execution order

    @property
    def tools(self):
        """Unique tools the plan uses, in first-appearance order."""
        seen = []
        for op in self.operations:
            if op.tool not in seen:
                seen.append(op.tool)
        return seen

    def feasible_on(self, machine):
        """
        (ok, problems) — can `machine`'s topology run every tool this plan uses?
        The upfront, config-only gate (no hardware). problems is a list of
        (tool, reason) for the tools that don't fit.
        """
        problems = []
        for op in self.operations:
            ok, reason = can_run_tool(machine, op.profile)
            if not ok and (op.tool, reason) not in problems:
                problems.append((op.tool, reason))
        return (not problems), problems


def save_plan(plan: Plan, path: str):
    """Write a Plan to a .plan binary file."""
    unique_types = list(dict.fromkeys(op.profile.tool_type for op in plan.operations))
    with open(path, "wb") as f:
        f.write(_HDR.pack(MAGIC, VERSION, len(unique_types), len(plan.operations)))
        for tt in unique_types:
            f.write(bytes([int(tt)]))
        for op in plan.operations:
            pkts = list(op.packets)
            f.write(_OP_HDR.pack(int(op.profile.tool_type), len(pkts)))
            for pkt in pkts:
                f.write(pkt)


def load_plan(path: str, machine) -> Plan:
    """
    Load a .plan file back into a Plan, resolving ToolType → ToolProfile via
    TOOL_PROFILES_BY_TYPE. Raises ValueError on unknown magic/version or
    unrecognised ToolType.
    """
    with open(path, "rb") as f:
        raw_hdr = f.read(_HDR.size)
        if len(raw_hdr) < _HDR.size:
            raise ValueError("truncated .plan file")
        magic, version, n_tools, n_ops = _HDR.unpack(raw_hdr)
        if magic != MAGIC:
            raise ValueError(f"not a .plan file (bad magic {magic!r})")
        if version != VERSION:
            raise ValueError(f"unsupported .plan version {version}")

        # manifest — validate all tool types are known before touching packets
        for _ in range(n_tools):
            b = f.read(1)
            if not b:
                raise ValueError("truncated .plan manifest")
            _resolve_type(b[0])    # raises if unknown

        ops = []
        for i in range(n_ops):
            raw_op = f.read(_OP_HDR.size)
            if len(raw_op) < _OP_HDR.size:
                raise ValueError(f"truncated op header at op {i}")
            tt_byte, pkt_count = _OP_HDR.unpack(raw_op)
            profile = _resolve_type(tt_byte)
            pkts = []
            for j in range(pkt_count):
                magic_byte = f.read(1)
                if not magic_byte:
                    raise ValueError(f"truncated packet {j} in op {i}")
                pkt_size = PACKET_SIZES.get(magic_byte[0])
                if pkt_size is None:
                    raise ValueError(
                        f"unknown packet magic 0x{magic_byte[0]:02x} at packet {j} op {i}")
                rest = f.read(pkt_size - 1)
                if len(rest) < pkt_size - 1:
                    raise ValueError(f"truncated packet {j} in op {i}")
                pkts.append(magic_byte + rest)
            ops.append(ToolOperation(tool=profile.name, profile=profile, packets=pkts))

    return Plan(operations=ops)


def _resolve_type(byte_val: int):
    try:
        tt = ToolType(byte_val)
    except ValueError:
        raise ValueError(f"unknown ToolType byte 0x{byte_val:02x} in .plan file")
    if tt not in TOOL_PROFILES_BY_TYPE:
        raise ValueError(f"ToolType {tt.name} has no registered ToolProfile")
    return TOOL_PROFILES_BY_TYPE[tt]
