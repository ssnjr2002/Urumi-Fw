"""
Tests for the single-head multi-tool job runner (8b) against the simulator.

Drives send_plan() over a two-tool plan (knife, crease) and checks: the operator
is asked to mount each tool in order, the machine pauses at the tool-change
boundary and resumes, motion executes, it ends IDLE, and an infeasible machine is
rejected upfront without streaming.

Run: python -m host.diagnostics.test_job_runner
"""

import sys, os
from dataclasses import replace

from pipeline.stages.config import default, KNIFE
from host.protocol.link import Link
from host.protocol import commands as cmd
from host.protocol.state import MachineState
from host.production.planner import plan_job
from host.job_runner import send_plan, Operator

SVG = os.path.join(os.path.dirname(__file__), "..", "..",
                   "pipeline", "data", "test_layers.svg")


class RecordingOperator(Operator):
    def __init__(self):
        self.mounts = []
        self.notes = []
    def mount(self, tool_name):
        self.mounts.append(tool_name)
    def note(self, text):
        self.notes.append(text)


def _ready_sim():
    link = Link.open_sim()
    cmd.enable(link); cmd.setorigin(link)      # energise + home so pre-flight passes
    return link


def test_two_tool_job_runs_to_completion():
    link = _ready_sim()
    plan = plan_job(SVG, default().machine)     # [knife, crease]
    op = RecordingOperator()
    ok, msg = send_plan(plan, default().machine, link, op)
    assert ok, msg
    assert op.mounts == ["knife", "crease"]     # mounted, in order, once each
    assert cmd.get_state(link).state == MachineState.IDLE
    # both operations executed -> position moved off origin
    assert any(p != 0 for p in cmd.get_pos(link))


def test_pause_happens_at_the_boundary():
    # instrument the operator to snapshot machine state when asked to mount tool 2
    link = _ready_sim()
    plan = plan_job(SVG, default().machine)
    seen = {}
    class Probe(RecordingOperator):
        def mount(self, tool_name):
            super().mount(tool_name)
            seen[tool_name] = cmd.get_state(link).state
    send_plan(plan, default().machine, link, Probe())
    assert seen["knife"] == MachineState.IDLE      # first tool mounted while idle
    assert seen["crease"] == MachineState.PAUSED    # swap happens at the PAUSE boundary


def test_infeasible_machine_rejected_without_streaming():
    # drop the A axis -> tangential tools (knife/crease) are infeasible
    m = default().machine
    head = m.heads[0]
    a_off = replace(head.a, node=replace(head.a.node, present=False))
    m_noA = replace(m, heads=(replace(head, a=a_off),))
    link = Link.open_sim()                          # not even homed — must bail first
    plan = plan_job(SVG, m_noA)
    op = RecordingOperator()
    ok, msg = send_plan(plan, m_noA, link, op)
    assert not ok
    assert op.mounts == []                          # bailed before any mount/stream
    assert "cannot run" in msg


def test_single_tool_plan_no_pause():
    # a one-tool plan (knife only) should never pause — one op, straight to IDLE
    link = _ready_sim()
    m = default().machine
    # build a single-op plan by overriding both layers to knife
    plan = plan_job(SVG, m, overrides={"crease": KNIFE})
    op = RecordingOperator()
    ok, msg = send_plan(plan, m, link, op)
    assert ok, msg
    assert op.mounts == ["knife"]                   # crease layer also uses knife -> no swap
    assert cmd.get_state(link).state == MachineState.IDLE


if __name__ == "__main__":
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    passed = failed = 0
    for t in tests:
        try:
            t(); print(f"  PASS  {t.__name__}"); passed += 1
        except Exception as e:
            print(f"  FAIL  {t.__name__}: {e}"); failed += 1
    print(f"\n{passed} passed, {failed} failed")
    sys.exit(failed)
