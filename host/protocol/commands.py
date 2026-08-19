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
    """Relay an RS485 ping to a bus node (or 'all'); True if it answered.

    Single node replies `node <n> ok|timeout`; `all` replies with one line,
    `nodes 1=ok 2=timeout …` — one line per command either way, which is the
    text plane's contract. For 'all' this is True only if EVERY node answered.
    """
    reply = link.command(f"pingnode {node_id}")
    if reply.startswith("nodes"):
        results = _parse_ping_all(reply)
        return bool(results) and all(results.values())
    return reply.endswith("ok")


def _parse_ping_all(reply: str) -> dict:
    """`nodes 1=ok 2=timeout 3=ok 4=timeout` → {1: True, 2: False, …}."""
    out = {}
    for tok in reply.split()[1:]:
        n, _, verdict = tok.partition("=")
        if verdict:
            try:
                out[int(n)] = (verdict == "ok")
            except ValueError:
                pass
    return out


def ping_all(link) -> dict:
    """Ping every node on the bus in ONE command; returns {node_id: answered}.

    The firmware walks nodes 1–4 regardless of what the host has configured, so
    the caller decides which of those it actually cares about — a node that is
    not in the config timing out is expected, not a failure.

    One round trip instead of N. It also reflects one instant on the bus rather
    than a sequence of them, which matters when a flaky node is what you are
    trying to catch.
    """
    return _parse_ping_all(link.command("pingnode all"))


def get_state(link) -> MachineStatus:
    """Operational status snapshot (the pre-flight / pause-choreography read)."""
    return parse_getstate(link.command("getstate"))


def get_status(link) -> MachineStatus:
    """Binary mirror of get_state (STATUS_REQ/STATUS_RSP) — cheaper poll, usable mid-stream."""
    return link.get_status()


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


def enable(link, node=None):   return _ok(link, f"enable {node}" if node is not None else "axes_enable on")
def disable(link, node=None):  return _ok(link, f"disable {node}" if node is not None else "axes_enable off")
def axes_enable(link, on):     return _ok(link, f"axes_enable {'on' if on else 'off'}")
# Whole-bus broadcast, peripherals included. Unacknowledged: `on` arms nothing in
# the Pico's bookkeeping (see bus_enable in control_plane.cpp). Use axes_enable
# to actually arm the axis map.
def bus_enable(link, on):      return _ok(link, f"bus_enable {'on' if on else 'off'}")
def setorigin(link, axes=""):  return _ok(link, f"setorigin {axes}".strip())
def pause(link):               return _ok(link, "pause")
def resume(link):              return _ok(link, "resume")
def cancel(link):              return _ok(link, "cancel")
def stop(link):                return _ok(link, "stop")
def unalarm(link):             return _ok(link, "unalarm")
