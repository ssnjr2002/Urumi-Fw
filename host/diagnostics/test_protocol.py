"""
test_protocol.py — exercise the control-plane client against the Pico simulator.

No hardware: drives host.protocol.link.SimBackend through the wire_protocol.md
control commands and the PAUSE choreography, asserting the allowed-state matrix
and the getstate parsing hold. This is the offline proving ground for the host
side until the firmware track (steps 2-5) lands.

Run:  python -m host.diagnostics.test_protocol
"""

import sys

from host.protocol.link import Link
from host.protocol import commands as cmd
from host.protocol.state import MachineState, AlarmReason, RunningReason, axis_mask


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
    assert cmd.get_state(link).state == MachineState.RUNNING
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
