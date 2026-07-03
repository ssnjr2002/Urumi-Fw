"""
test_protocol.py — exercise the control-plane client against the Pico simulator.

No hardware: drives host.protocol.link.SimBackend through the wire_protocol.md
control commands and the PAUSE choreography, asserting the allowed-state matrix
and the getstate parsing hold. This is the offline proving ground for the host
side until the firmware track (steps 2-5) lands.

Run:  python -m host.diagnostics.test_protocol
"""

import sys
from dataclasses import replace

from host.protocol.link import Link
from host.protocol import commands as cmd
from host.protocol.state import MachineState, AlarmReason, RunningReason, axis_mask
from host.execution.preflight import preflight
from host.protocol.packets import make_jog
from pipeline.config import default, KNIFE, PEN, BusNode


def test_ping_and_initial_state():
    link = Link.open_sim()
    assert cmd.ping(link)
    st = cmd.get_state(link)
    assert st.state == MachineState.IDLE
    assert st.axes_homed == 0
    assert st.alarm == AlarmReason.NONE


def test_pingnode():
    link = Link.open_sim()
    assert cmd.ping_node(link, 3)


def test_setorigin_homes_axes():
    link = Link.open_sim()
    ok, _ = cmd.enable(link); assert ok
    ok, _ = cmd.setorigin(link, "xy"); assert ok
    st = cmd.get_state(link)
    assert st.all_homed(axis_mask("xy"))
    assert not st.homed("z")
    # full setorigin homes everything
    ok, _ = cmd.setorigin(link); assert ok
    assert cmd.get_state(link).all_homed(axis_mask("xyza"))


def test_getpos():
    link = Link.open_sim()
    assert cmd.get_pos(link) == (0, 0, 0, 0)


def test_pause_only_from_running():
    link = Link.open_sim()
    ok, reason = cmd.pause(link)             # IDLE -> reject
    assert not ok and reason == "bad_state"
    link.backend._force_running()
    ok, _ = cmd.pause(link); assert ok
    assert cmd.get_state(link).state == MachineState.PAUSED


def test_resume_and_cancel():
    link = Link.open_sim()
    link.backend._force_running()
    cmd.pause(link)
    ok, _ = cmd.resume(link); assert ok
    # Phase 1: resume -> IDLE (host pre-positions, then streams fresh next op)
    assert cmd.get_state(link).state == MachineState.IDLE
    # cancel path
    link2 = Link.open_sim()
    link2.backend._force_running()
    cmd.pause(link2)
    ok, _ = cmd.cancel(link2); assert ok
    assert cmd.get_state(link2).state == MachineState.IDLE


def test_disable_clears_homing():
    link = Link.open_sim()
    cmd.enable(link); cmd.setorigin(link)
    assert cmd.get_state(link).axes_homed != 0
    ok, _ = cmd.disable(link); assert ok
    assert cmd.get_state(link).axes_homed == 0       # de-energise invalidates position


def test_estop_then_recover():
    link = Link.open_sim()
    cmd.enable(link); cmd.setorigin(link)
    ok, _ = cmd.stop(link); assert ok
    st = cmd.get_state(link)
    assert st.state == MachineState.ALARM
    assert st.alarm == AlarmReason.ESTOP
    assert st.axes_homed == 0
    # setorigin recovers ALARM -> IDLE (per state_redesign.md)
    ok, _ = cmd.setorigin(link); assert ok
    assert cmd.get_state(link).state == MachineState.IDLE


def test_unalarm_only_from_alarm():
    link = Link.open_sim()
    ok, reason = cmd.unalarm(link)            # IDLE -> reject
    assert not ok and reason == "bad_state"
    cmd.stop(link)
    ok, _ = cmd.unalarm(link); assert ok
    assert cmd.get_state(link).state == MachineState.IDLE


# ── required_axes derivation ──────────────────────────────────────────────────

def test_required_axes_masks():
    assert PEN.required_axes == 0b0011                 # X,Y only (no lift, no A)
    assert KNIFE.required_axes == 0b1011               # X,Y,A (tangential, no lift)
    pen_lift = replace(PEN, lift_height=2.0)
    assert pen_lift.required_axes == 0b0111            # X,Y,Z


# ── streaming through Link (sim accepts the burst) ────────────────────────────

def test_stream_sim_accepts_jog_burst():
    link = Link.open_sim()
    pk = make_jog((1600, 0, 0, 0), 80 * 160, 200 * 160, 150_000_000)
    assert pk and link.stream(pk) is True


# ── pre-flight ────────────────────────────────────────────────────────────────

def test_preflight_passes_when_homed():
    link = Link.open_sim()
    cmd.enable(link); cmd.setorigin(link)              # home all axes
    pf = preflight(link, default().machine, KNIFE)     # KNIFE head is mounted
    assert pf.ok, "\n" + str(pf)


def test_preflight_fails_unhomed():
    link = Link.open_sim()
    pf = preflight(link, default().machine, KNIFE)     # nothing homed
    assert not pf.ok
    assert any("homed" in c.name and not c.ok for c in pf.checks)


def test_preflight_fails_disabled():
    link = Link.open_sim()
    cmd.setorigin(link)                                # homed but NOT enabled
    pf = preflight(link, default().machine, KNIFE)
    assert not pf.ok
    assert any("enabled" in c.name and not c.ok for c in pf.checks)


def test_setorigin_does_not_enable():
    link = Link.open_sim()
    cmd.setorigin(link)
    assert cmd.get_state(link).axes_enabled == 0       # homing != energising
    cmd.enable(link)
    assert cmd.get_state(link).all_enabled(axis_mask("xyza"))


def test_disable_clears_enabled():
    link = Link.open_sim()
    cmd.enable(link)
    assert cmd.get_state(link).axes_enabled != 0
    cmd.disable(link)
    assert cmd.get_state(link).axes_enabled == 0


def test_preflight_fails_tool_not_mounted():
    link = Link.open_sim()
    cmd.enable(link); cmd.setorigin(link)
    pf = preflight(link, default().machine, PEN)       # head carries KNIFE, not PEN
    assert not pf.ok
    assert any("mounted" in c.name and not c.ok for c in pf.checks)


def test_preflight_peripheral_present_and_missing():
    # synthetic tool needing an "oscillator" peripheral; exercise both branches
    link = Link.open_sim()
    cmd.enable(link); cmd.setorigin(link)
    m = default().machine
    knife_osc = replace(KNIFE, required_peripheral_roles=("oscillator",))
    head0 = replace(m.heads[0], profile=knife_osc)

    # (a) peripheral declared on the bus -> present (sim pings ok)
    m_with = replace(m, heads=(head0,),
                     peripherals=(BusNode(7, role="oscillator"),))
    assert preflight(link, m_with, knife_osc).ok

    # (b) peripheral required but absent from machine.peripherals -> topology gap
    m_without = replace(m, heads=(head0,), peripherals=())
    pf = preflight(link, m_without, knife_osc)
    assert not pf.ok
    assert any("oscillator" in c.name and not c.ok for c in pf.checks)


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
