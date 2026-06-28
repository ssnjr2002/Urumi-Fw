"""
preflight.py — Phase 1 pre-flight checks (host orchestration seam).

Runs the ordered checks from docs/PLAN_phase1_host_impl.md §2 against a Link
before a job streams: Pico alive, ready, every required axis node present, every
required peripheral node present, required axes homed. All checks are issued by
the host through the Link (node pings relay through the Pico onto RS485). The one
non-automatable gate — operator confirmation that the right tool is physically
mounted — is left to the caller (the GUI); the Pico cannot sense it.

`preflight(link, machine, profile)` returns a Preflight result; `.ok` is the
go/no-go. Short-circuits early if the Pico is unreachable or not ready, so a dead
link doesn't produce a wall of node-timeout failures.
"""

from dataclasses import dataclass, field

from host.protocol import commands as cmd
from host.protocol.state import MachineState
from config import select_head


@dataclass
class Check:
    name:   str
    ok:     bool
    detail: str = ""


@dataclass
class Preflight:
    checks: list = field(default_factory=list)

    @property
    def ok(self) -> bool:
        return bool(self.checks) and all(c.ok for c in self.checks)

    def add(self, name, ok, detail="") -> bool:
        self.checks.append(Check(name, bool(ok), detail))
        return bool(ok)

    def __str__(self) -> str:
        lines = [f"  [{'OK ' if c.ok else 'FAIL'}] {c.name}"
                 + (f" — {c.detail}" if c.detail else "")
                 for c in self.checks]
        return ("PRE-FLIGHT " + ("PASS" if self.ok else "FAIL") + "\n"
                + "\n".join(lines))


def _required_axis_nodes(machine, profile, head):
    """{axis_letter: node_id} for the axes this tool requires homed."""
    mask = profile.required_axes
    nodes = {}
    if mask & 0b0001: nodes["x"] = machine.x.node.node_id
    if mask & 0b0010: nodes["y"] = machine.y.node.node_id
    if mask & 0b0100: nodes["z"] = head.z.node.node_id
    if mask & 0b1000: nodes["a"] = head.a.node.node_id
    return nodes


def preflight(link, machine, profile) -> Preflight:
    """Ordered pre-flight checks for a job using `profile` on `machine`."""
    pf = Preflight()

    # 1. Pico alive
    if not pf.add("pico alive", cmd.ping(link)):
        return pf

    # 2. Pico ready (IDLE)
    st = cmd.get_state(link)
    if not pf.add("pico ready", st.state == MachineState.IDLE,
                  f"state={st.state.name}"):
        return pf

    # Tool mounted? (which head carries this tool) — gates the node lookups
    try:
        head = machine.heads[select_head(machine, profile.name)]
    except ValueError as e:
        pf.add(f"tool '{profile.name}' mounted", False, str(e))
        return pf
    pf.add(f"tool '{profile.name}' mounted", True, f"x_offset={head.x_offset}")

    # 3. Required axis nodes present on the bus
    for ax, nid in _required_axis_nodes(machine, profile, head).items():
        pf.add(f"axis {ax} node {nid} present", cmd.ping_node(link, nid))

    # 4. Required peripheral nodes present (resolve role -> machine.peripherals)
    for role in profile.required_peripheral_roles:
        node = next((n for n in machine.peripherals if n.role == role), None)
        if node is None:
            pf.add(f"peripheral '{role}'", False,
                   "no node with this role in machine.peripherals (topology gap)")
        else:
            pf.add(f"peripheral '{role}' node {node.node_id}",
                   cmd.ping_node(link, node.node_id))

    mask = profile.required_axes
    # 5. Required axes energised (a present-but-disabled axis drops steps silently)
    pf.add("required axes enabled", st.all_enabled(mask),
           f"enabled=0x{st.axes_enabled:02x} need=0x{mask:02x}")

    # 6. Required axes homed
    pf.add("required axes homed", st.all_homed(mask),
           f"homed=0x{st.axes_homed:02x} need=0x{mask:02x}")

    return pf
