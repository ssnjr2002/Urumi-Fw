"""
commands.py — control-plane command helpers over a Link.

One function per text command from docs/wire_protocol.md "Control Plane". Each
sends the command and parses the reply, so callers (GUI, pre-flight, pause
choreography) work with typed results instead of raw strings.

The `_ok` commands return (ok: bool, reason: str) — reason is '' on success or
the Pico's `err <reason>` text on rejection (e.g. 'bad_state').
"""

from host.protocol.state import parse_getstate, MachineStatus


def ping(link) -> bool:
    """USB liveness check."""
    return link.command("ping") == "pong"


from typing import Union

def ping_node(link, node_id: Union[int, str]) -> bool:
    """Relay an RS485 ping to a bus node (or 'all'); True if it answered."""
    return link.command(f"pingnode {node_id}").endswith("ok")


def get_state(link) -> MachineStatus:
    """Operational status snapshot (the pre-flight / pause-choreography read)."""
    return parse_getstate(link.command("getstate"))


def get_pos(link) -> tuple:
    """Absolute machinePos in steps: (x, y, z, a)."""
    parts = link.command("getpos").split()
    if not parts or parts[0] != "pos" or len(parts) < 5:
        raise ValueError(f"bad getpos reply: {parts}")
    return tuple(int(p) for p in parts[1:5])


def _ok(link, cmd: str):
    r = link.command(cmd)
    if r == "ok":
        return True, ""
    if r.startswith("err"):
        return False, r[3:].strip()
    return False, r


def enable(link):              return _ok(link, "enable")
def disable(link):             return _ok(link, "disable")
def setorigin(link, axes=""):  return _ok(link, f"setorigin {axes}".strip())
def pause(link):               return _ok(link, "pause")
def resume(link):              return _ok(link, "resume")
def cancel(link):              return _ok(link, "cancel")
def stop(link):                return _ok(link, "stop")
def unalarm(link):             return _ok(link, "unalarm")
