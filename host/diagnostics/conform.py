"""
conform.py â€” Phase 1 firmware conformance harness.

Drives the control plane through the wire_protocol.md state machine and asserts
each step. Run against --sim for the offline baseline, then against --port COM8
to confirm the firmware matches. Mismatches reveal where the implementation
diverges from what was intended.

Known timing caveat (firmware only): `stop` sets STATE_ESTOP on Core 0, but
Core 1 transitions to STATE_ALARM asynchronously. A short sleep is needed after
`stop` before reading state. The sim is synchronous; the firmware is not.

Usage:
  python -m host.diagnostics.conform --sim
  python -m host.diagnostics.conform --port COM8
  python -m host.diagnostics.conform --port COM8 --baud 115200
"""

import sys, argparse, time

from host.protocol.link import Link
from host.protocol import commands as cmd
from host.protocol.state import MachineState, AlarmReason
from host.protocol.packets import make_jog

PASS = "PASS"
FAIL = "FAIL"

_results = []

def check(name, cond, detail=""):
    status = PASS if cond else FAIL
    _results.append((name, status, detail))
    tag = f"  [{status}]  {name}"
    print(tag + (f"  ({detail})" if detail else ""))
    return cond


def section(title):
    print(f"\n-- {title} " + "-" * max(0, 50 - len(title)))


def run(link, is_firmware: bool):
    # firmware needs a moment after Core 1 handles ESTOP â†’ ALARM
    estop_settle = 0.15 if is_firmware else 0.0

    # â”€â”€ 1. initial state â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    section("initial state")
    check("ping", cmd.ping(link))
    st = cmd.get_state(link)
    check("initial state=IDLE",    st.state == MachineState.IDLE,   f"got {st.state}")
    check("initial axes_homed=0",  st.axes_homed == 0,              f"got 0x{st.axes_homed:02x}")
    check("initial axes_enabled=0",st.axes_enabled == 0,            f"got 0x{st.axes_enabled:02x}")
    check("initial alarm=NONE",    st.alarm == AlarmReason.NONE,    f"got {st.alarm}")

    # â”€â”€ 2. state gate â€” commands rejected from IDLE â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    section("state gate from IDLE")
    ok, r = cmd.pause(link);   check("pause from IDLE â†’ err", not ok and r == "bad_state", r)
    ok, r = cmd.resume(link);  check("resume from IDLE â†’ err", not ok and r == "bad_state", r)
    ok, r = cmd.cancel(link);  check("cancel from IDLE â†’ err", not ok and r == "bad_state", r)
    ok, r = cmd.unalarm(link); check("unalarm from IDLE â†’ err", not ok and r == "bad_state", r)

    # â”€â”€ 3. enable / disable â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    section("enable / disable")
    ok, _ = cmd.enable(link); check("enable â†’ ok", ok)
    st = cmd.get_state(link)
    check("after enable: axes_enabled=0x0f", st.axes_enabled == 0x0F,
          f"got 0x{st.axes_enabled:02x}")
    check("after enable: axes_homed still 0", st.axes_homed == 0,
          f"got 0x{st.axes_homed:02x}")

    ok, _ = cmd.disable(link); check("disable â†’ ok", ok)
    st = cmd.get_state(link)
    check("after disable: axes_enabled=0",  st.axes_enabled == 0, f"got 0x{st.axes_enabled:02x}")
    check("after disable: axes_homed=0",    st.axes_homed == 0,   f"got 0x{st.axes_homed:02x}")

    # â”€â”€ 4. setorigin â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    section("setorigin")
    ok, _ = cmd.setorigin(link, "xy"); check("setorigin xy â†’ ok", ok)
    st = cmd.get_state(link)
    check("homed xy bits set",   st.axes_homed & 0x03 == 0x03, f"got 0x{st.axes_homed:02x}")
    check("homed z,a still 0",   st.axes_homed & 0x0C == 0,    f"got 0x{st.axes_homed:02x}")
    check("state still IDLE",    st.state == MachineState.IDLE, f"got {st.state}")

    ok, _ = cmd.setorigin(link); check("setorigin all â†’ ok", ok)
    st = cmd.get_state(link)
    check("all axes homed after bare setorigin", st.axes_homed == 0x0F,
          f"got 0x{st.axes_homed:02x}")

    pos = cmd.get_pos(link)
    check("getpos returns 4-tuple", len(pos) == 4, str(pos))
    check("getpos all zero after setorigin", pos == (0,0,0,0), str(pos))

    # â”€â”€ 5. stop â†’ ALARM â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    section("stop â†’ ALARM")
    cmd.enable(link); cmd.setorigin(link)
    ok, _ = cmd.stop(link); check("stop â†’ ok", ok)
    time.sleep(estop_settle)   # Core 1 must flush and set ALARM
    st = cmd.get_state(link)
    check("state=ALARM after stop",     st.state == MachineState.ALARM,  f"got {st.state}")
    check("alarm=ESTOP after stop",     st.alarm == AlarmReason.ESTOP,   f"got {st.alarm}")
    check("axes_homed cleared by stop", st.axes_homed == 0,              f"got 0x{st.axes_homed:02x}")
    check("axes_enabled cleared",       st.axes_enabled == 0,            f"got 0x{st.axes_enabled:02x}")

    # enable is allowed from ALARM
    ok, _ = cmd.enable(link); check("enable from ALARM â†’ ok", ok)

    # â”€â”€ 6. unalarm â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    section("unalarm")
    ok, _ = cmd.unalarm(link); check("unalarm â†’ ok", ok)
    st = cmd.get_state(link)
    check("state=IDLE after unalarm",   st.state == MachineState.IDLE, f"got {st.state}")
    check("alarm=NONE after unalarm",   st.alarm == AlarmReason.NONE,  f"got {st.alarm}")

    # â”€â”€ 7. setorigin recovers from ALARM â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    section("setorigin recovers ALARM")
    ok, _ = cmd.stop(link); check("stop (setup for setorigin-recovery test)", ok)
    time.sleep(estop_settle)
    ok, _ = cmd.setorigin(link); check("setorigin from ALARM â†’ ok", ok)
    st = cmd.get_state(link)
    check("state=IDLE after setorigin-from-ALARM", st.state == MachineState.IDLE,
          f"got {st.state}")
    check("alarm cleared", st.alarm == AlarmReason.NONE, f"got {st.alarm}")
    check("homed bits set", st.axes_homed == 0x0F, f"got 0x{st.axes_homed:02x}")

    # â”€â”€ 8. seqreset â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    section("seqreset")
    r = link.command("seqreset")
    check("seqreset reply", r == "seq reset", f"got {repr(r)}")

    # â”€â”€ 9. binary loopback (MSEG stream) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    section("binary loopback (no nodes needed)")
    cmd.enable(link); cmd.setorigin(link)   # must be enabled + homed for stream gate
    link.command("seqreset")               # align seq before streaming

    # 8 zero-step packets â€” exercises the CRC, seq-guard, ACK path without moving
    from host.protocol.packets import pack_microsegment, MSEG_FLAG_NONE, MSEG_FLAG_PATH_END
    from collections import namedtuple
    MS = namedtuple("MS", ["dx","dy","dz","da","interval","flags"])
    F_CPU = 150_000_000
    N = 8
    pkts = []
    for i in range(N):
        flags = MSEG_FLAG_PATH_END if i == N - 1 else MSEG_FLAG_NONE
        pkts.append(pack_microsegment(MS(0,0,0,0, F_CPU // 1000, flags)))

    if link.serial is None:
        # sim â€” packets ingested directly
        ok = link.stream(pkts)
        check("sim stream accepted", ok)
    else:
        from host.protocol.session import ListSource
        link.reset_seq()
        sess = link.session(ListSource(pkts), window=8)
        ok = sess.run()
        check("loopback: all ACKed", ok and sess.acked == N,
              f"ACKed {sess.acked}/{N}, NACKs {sess.nacks}")

    # machine should return to IDLE after the burst drains
    time.sleep(0.3)
    st = cmd.get_state(link)
    check("state=IDLE after burst drains", st.state == MachineState.IDLE,
          f"got {st.state}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sim",  action="store_true")
    ap.add_argument("--port", help="Serial port, e.g. COM8")
    ap.add_argument("--baud", type=int, default=115200)
    args = ap.parse_args()

    if args.sim:
        print("Backend: SimBackend (offline)")
        link = Link.open_sim()
        is_fw = False
    elif args.port:
        print(f"Backend: SerialBackend {args.port} @ {args.baud}")
        link = Link.open_serial(args.port, args.baud)
        time.sleep(0.6)   # let USB CDC enumerate and flush startup banner
        is_fw = True
    else:
        ap.error("pass --sim or --port COM<n>")

    with link:
        run(link, is_fw)

    print(f"\n{'='*55}")
    passed = sum(1 for _, s, _ in _results if s == PASS)
    failed = sum(1 for _, s, _ in _results if s == FAIL)
    print(f"  {passed} passed  {failed} failed")
    sys.exit(0 if failed == 0 else 1)


if __name__ == "__main__":
    main()

