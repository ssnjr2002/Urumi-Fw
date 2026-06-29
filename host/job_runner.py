"""
job_runner.py — stream a multi-tool Plan to the Pico (single head).

Walks the ordered tool-tagged operations and resolves single-head tool changes:
it marks the boundary packet with MSEG_FLAG_PAUSE so the Pico drains and enters
PAUSED, runs the swap (operator mounts the next tool + lazy per-activation
pre-flight), resumes, then streams the next operation. An upfront config
feasibility gate runs first; a session-level validated-and-mounted tool set keeps
the per-activation checks honest (single head: at most one tool mounted).

The planner never inserts a pause — this runner does, against the connected
machine. Dual-head head-switching (no pause) is future (PLAN_phase1_host_impl §11).
"""

import time

from host.protocol.packets import with_flag, MSEG_FLAG_PAUSE
from host.protocol.state import MachineState
from host.protocol import commands as cmd
from host.preflight import preflight


class Operator:
    """
    Interactive hooks. The defaults auto-confirm, which is exactly what the
    simulator/tests want; a GUI subclasses this to prompt the human and block
    until they've physically swapped the tool.
    """
    def mount(self, tool_name):
        """Block until the operator has physically mounted `tool_name`."""

    def note(self, text):
        """Surface a status line to the operator."""


def send_plan(plan, machine, link, operator=None):
    """
    Stream `plan` to the Pico behind `link`, single head. Returns (ok, message).
    """
    operator = operator or Operator()
    head = machine.active_head

    ok, problems = plan.feasible_on(machine)
    if not ok:
        return False, f"machine cannot run this job: {problems}"

    validated = set()                 # tools physically validated AND still mounted
    ops = plan.operations
    for i, op in enumerate(ops):
        if op.tool not in validated:
            # Mount + lazy per-activation pre-flight. On a swap (i>0) the machine
            # is PAUSED from the previous boundary; for the first tool it's IDLE.
            on_swap = (i > 0)
            operator.note(f"mount tool '{op.tool}'")
            operator.mount(op.tool)
            pf = preflight(link, machine, op.profile,
                           head_index=head, require_idle=not on_swap)
            if not pf.ok:
                return False, f"pre-flight failed for '{op.tool}':\n{pf}"
            validated = {op.tool}      # single head: the swap removed the previous tool
            if on_swap:
                cmd.resume(link)       # PAUSED -> RUNNING, ready to stream this op

        tool_change_ahead = (i + 1 < len(ops)) and (ops[i + 1].tool != op.tool)
        packets = list(op.packets)
        if tool_change_ahead and packets:
            packets[-1] = with_flag(packets[-1], MSEG_FLAG_PAUSE)

        operator.note(f"running '{op.tool}' ({len(packets)} segments)")
        link.stream(packets)
        target = MachineState.PAUSED if tool_change_ahead else MachineState.IDLE
        if not _wait_state(link, target):
            return False, f"timed out waiting for {target.name} after '{op.tool}'"

    operator.note("job complete")
    return True, "job complete"


def _wait_state(link, target, timeout=15.0, interval=0.02):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if cmd.get_state(link).state == target:
            return True
        time.sleep(interval)
    return False
