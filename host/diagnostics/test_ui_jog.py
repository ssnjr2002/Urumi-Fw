"""
test_ui_jog.py — end-to-end proof of the new comms architecture through the UI's
OnlineSession, headless (no Tk).

A real OnlineSession, a real Link, the simulator standing in for the Pico, and
the UI's own background status poller running the whole time.

Jog model under test: ONE CLICK = ONE FIXED DISTANCE. Clicking again while the
machine is still moving extends the live move rather than queueing a separate
burst — that is the blend. Clicking the opposite direction cancels.

What it proves that seizure could not:
  - the status poller keeps sampling WHILE a jog streams
  - a text command still gets its reply mid-stream

Run:  python -m host.diagnostics.test_ui_jog
"""

import sys
import time

from host.ui.app_state import AppState
from host.ui.online.session import OnlineSession, SIM_PORT
from host.protocol.state import MachineState
from pipeline.config import default as config_default

_failures = []


def check(cond, label):
    print(f"  {'ok  ' if cond else 'FAIL'} {label}")
    if not cond:
        _failures.append(label)


def make_session():
    st = AppState()
    st.config = config_default()
    sess = OnlineSession(st)
    sess.connect(SIM_PORT)
    sess.enable_all()
    for _ in range(40):
        if sess.machine_state is not None:
            break
        time.sleep(0.05)
    return sess


def axis_pos(sess, idx=0):
    """Raw MOTOR steps, as the Pico counts them — not machine direction."""
    return sess.link.backend.pos[idx]


def axis_dir(sess, ltr="x"):
    """Motor-step sign for one unit of positive machine travel.

    axis.invert is a wiring correction, so on an inverted axis a +X jog counts
    DOWN in motor steps. Deriving the expected sign here (rather than assuming
    +1, or comparing magnitudes) keeps these direction assertions able to fail:
    they caught nothing while jog was skipping invert entirely.
    """
    ax = dict(sess.app_state.config.machine.present_axes())[ltr]
    return -1 if getattr(ax, "invert", False) else 1


def steps_per_mm(sess):
    return dict(sess.app_state.config.machine.present_axes())["x"].steps_per_unit


def wait_idle(sess, timeout=20.0):
    """Wait for the MACHINE to stop, not just the session to end.

    The session finishes on its own drain estimate (queued motion-time plus the
    last status sample), which can beat the machine by up to one poll interval.
    Position assertions have to wait for the real thing, so this also requires
    the simulator's motion queue to be empty and the state back to IDLE.
    """
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        backend = sess.link.backend if sess.link else None
        drained = backend is None or not getattr(backend, "_motion", None)
        if not sess.busy and drained:
            time.sleep(0.05)                     # let the last tick land
            backend = sess.link.backend if sess.link else None
            if not sess.busy and (backend is None or not getattr(backend, "_motion", None)):
                return True
        time.sleep(0.02)
    return False


# ── connection ────────────────────────────────────────────────────────────────

def test_connect_and_poll():
    print("\nconnect + background polling")
    sess = make_session()
    check(sess.is_connected, "connected to the simulator")
    check(sess.machine_state is not None, "poller populated machine_state")
    check(sess.polling_error is None, f"no polling error ({sess.polling_error})")
    check(sess.machine_state.enabled("x"), "X axis enabled after enable_all")
    sess.disconnect()


# ── the thing that was broken ─────────────────────────────────────────────────

def test_one_click_moves_exactly():
    """The button says 10mm. One click must move 10mm — no more, no less.
    The previous hold-to-jog build moved however far the mouse happened to be
    held down, which is what made it look erratic."""
    print("\none click = exactly the requested distance")
    sess = make_session()
    spm = steps_per_mm(sess)
    start = axis_pos(sess)
    want = int(round(10.0 * spm)) * axis_dir(sess)

    sess.jog_click("x", 1, dist=10.0, rate=20.0)
    check(wait_idle(sess), "jog finished")
    moved = axis_pos(sess) - start
    check(moved == want, f"moved exactly 10mm ({moved} steps, expected {want})")

    sess.jog_click("x", -1, dist=10.0, rate=20.0)
    check(wait_idle(sess), "reverse jog finished")
    check(axis_pos(sess) == start,
          f"returned exactly to start ({axis_pos(sess)} vs {start})")
    sess.disconnect()


def test_clicks_blend():
    """Three rapid clicks travel 3x the distance as ONE session — not three
    separate bursts with a stop between each."""
    print("\nrapid clicks blend into one move")
    sess = make_session()
    spm = steps_per_mm(sess)
    start = axis_pos(sess)
    want = int(round(30.0 * spm)) * axis_dir(sess)

    sess.jog_click("x", 1, dist=10.0, rate=20.0)
    time.sleep(0.05)
    sess.jog_click("x", 1, dist=10.0, rate=20.0)
    time.sleep(0.05)
    sess.jog_click("x", 1, dist=10.0, rate=20.0)

    src = sess._jog_source
    clicks = src.clicks if src else 0
    check(wait_idle(sess), "blended jog finished")

    moved = axis_pos(sess) - start
    check(clicks == 3, f"all three clicks landed on ONE session (clicks={clicks})")
    check(moved == want, f"moved exactly 30mm ({moved} steps, expected {want})")
    sess.disconnect()


def test_reversal_cancels():
    print("\nreverse click cancels the live move")
    sess = make_session()
    spm = steps_per_mm(sess)
    start = axis_pos(sess)

    sess.jog_click("x", 1, dist=50.0, rate=20.0)
    time.sleep(0.15)
    sess.jog_click("x", -1, dist=50.0, rate=20.0)
    check(wait_idle(sess), "cancelled jog finished")

    moved = (axis_pos(sess) - start) * axis_dir(sess)   # machine frame
    check(moved > 0, "did not reverse direction on the cancelling click")
    check(moved < int(round(50.0 * spm)),
          f"stopped short of the full 50mm ({moved} steps)")
    sess.disconnect()


# ── the architecture claims ───────────────────────────────────────────────────

def test_telemetry_live_during_jog():
    """Under seizure the poller skipped itself while busy, so this count would
    be zero for the whole duration of the jog."""
    print("\nlive telemetry DURING a jog")
    sess = make_session()

    samples = []
    sess.jog_click("x", 1, dist=40.0, rate=20.0)
    deadline = time.monotonic() + 0.6
    while time.monotonic() < deadline and sess.busy:
        if sess.machine_state is not None:
            samples.append(int(sess.machine_state.state))
        time.sleep(0.02)
    wait_idle(sess)

    check(len(samples) > 5, f"status sampled while streaming ({len(samples)} times)")
    check(int(MachineState.RUNNING) in samples,
          f"sampled state showed RUNNING mid-stream (saw {sorted(set(samples))})")
    sess.disconnect()


def test_text_command_during_jog():
    """The text plane stays usable mid-stream — it routes to its own sink."""
    print("\ncontrol-plane text command during a jog")
    sess = make_session()
    sess.jog_click("x", 1, dist=40.0, rate=20.0)
    time.sleep(0.2)

    reply = sess.link.command("ping", timeout=1.0)
    check(reply == "pong", f"ping answered during a stream (got {reply!r})")

    wait_idle(sess)
    sess.disconnect()


def main():
    print("=" * 62)
    print("OnlineSession — end-to-end over the simulator")
    print("=" * 62)
    for fn in (test_connect_and_poll, test_one_click_moves_exactly,
               test_clicks_blend, test_reversal_cancels,
               test_telemetry_live_during_jog, test_text_command_during_jog):
        fn()

    print("\n" + "=" * 62)
    if _failures:
        print(f"{len(_failures)} FAILED:")
        for f in _failures:
            print(f"  - {f}")
        return 1
    print("all passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
