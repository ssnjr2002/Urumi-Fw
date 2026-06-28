"""
Tests for the multi-tool planner (8b): SVG layers -> ordered tool-tagged Plan,
and the config-only feasibility gate.

Run: python -m host.diagnostics.test_planner
"""

import sys, os
from dataclasses import replace

from config import default, KNIFE, PEN, CREASE, can_run_tool
from host.production.planner import plan_job, Plan, ToolOperation

SVG = os.path.join(os.path.dirname(__file__), "..", "..",
                   "pipeline", "data", "test_layers.svg")


def test_plan_one_op_per_layer_in_order():
    plan = plan_job(SVG, default().machine)
    assert [op.tool for op in plan.operations] == ["knife", "crease"]
    assert plan.tools == ["knife", "crease"]
    assert all(op.packets for op in plan.operations)        # each op emitted packets
    assert all(len(p) == 26 for op in plan.operations for p in op.packets)


def test_plan_no_pause_flags():
    # the plan must carry NO pause markers — the sender inserts them
    from host.protocol.packets import MSEG_FLAG_PATH_END
    PAUSE = 0x04
    for op in plan_job(SVG, default().machine).operations:
        for p in op.packets:
            assert not (p[21] & PAUSE)                       # flags byte, no PAUSE


def test_feasibility_pass_on_full_machine():
    ok, problems = plan_job(SVG, default().machine).feasible_on(default().machine)
    assert ok, problems


def test_feasibility_fails_without_A_axis():
    # knife is tangential -> needs A; drop A and the knife op is infeasible
    m = default().machine
    head = m.heads[0]
    a_off = replace(head.a, node=replace(head.a.node, present=False))
    m_noA = replace(m, heads=(replace(head, a=a_off),))
    ok, reason = can_run_tool(m_noA, KNIFE)
    assert not ok and "a" in reason
    plan = plan_job(SVG, m_noA)            # planning still works (geometry only)
    feasible, problems = plan.feasible_on(m_noA)
    assert not feasible
    assert any(t == "knife" for t, _ in problems)


def test_can_run_tool_peripheral_gap():
    m = default().machine
    knife_osc = replace(KNIFE, required_peripheral_roles=("oscillator",))
    ok, reason = can_run_tool(m, knife_osc)      # default machine has no peripherals
    assert not ok and "oscillator" in reason


def test_unresolved_layer_needs_default_or_raises():
    # test_layers.svg layers DO resolve; a bogus override map leaves them resolvable
    # so just check the error path via an SVG-less synthetic: an unknown tool name.
    from config import tool_for_layer
    assert tool_for_layer("widget") is None
    assert tool_for_layer("widget", overrides={"widget": CREASE}) is CREASE


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
