"""
gui.py — operator frontend (Tkinter) over the host.protocol library.

Built on the frozen wire contract: a Link (real serial OR the in-process Pico
simulator) speaks the control plane (getstate/enable/setorigin/pause/...) and the
data plane (jog bursts via link.stream). Status is polled with `getstate` +
`getpos`; polling pauses while a jog streams (the port is single-owner then).

Pick "Simulator" in the port list to drive the SimBackend with no hardware —
the whole operator workflow (status, enable, set-origin, pause/resume, stop) is
exercisable offline until the firmware track lands.

Run:
  python -m host.gui                 # opens with the Simulator preselected
  python -m host.gui --port COM8     # preselect a real port
  python -m host.gui --sim           # force the simulator
"""

import argparse, threading, queue, os

import tkinter as tk
from tkinter import ttk, filedialog, messagebox

import os
from pipeline.config import default as _config_default

def sim_machine():
    import host.config as _host_config
    sim_toml = os.path.join(os.path.dirname(__file__), "diagnostics", "sim_machine.toml")
    return _host_config.load(sim_toml).machine
from host.protocol.link import Link
from host.protocol.packets import make_jog
from host.protocol import commands as cmd
from host.protocol.state import MachineState
from host.execution.job_runner import send_plan, Operator
from host.production.plan_io import load_plan

try:
    from serial.tools import list_ports
except ImportError:
    list_ports = None

SIM_PORT = "Simulator"
STATUS_INTERVAL_MS = 400

_STATE_COLOR = {
    "IDLE": "green", "RUNNING": "blue", "PAUSED": "orange",
    "ESTOP": "red", "ALARM": "red", "HOMING": "purple",
}


class GuiOperator(Operator):
    """
    Bridges send_plan's Operator callbacks to the Tk main thread.

    mount() is called from the job worker thread. It sets _pending_mount and
    blocks on an Event; _poll_status (main thread) sees the flag, shows a
    modal askokcancel dialog (safe because it runs on the main thread), then
    sets the event to unblock the worker. note() appends to a plain list that
    _poll_status mirrors into the text area.
    """
    def __init__(self, ui):
        self._ui           = ui
        self._mount_event  = threading.Event()
        self._pending_mount = None   # tool name; main thread watches this
        self._mount_ok     = True    # False if operator cancelled

    def mount(self, tool_name):
        self._mount_ok = True
        self._mount_event.clear()
        self._pending_mount = tool_name
        self._mount_event.wait()     # blocks worker until main thread confirms
        self._pending_mount = None
        if not self._mount_ok:
            raise RuntimeError("job cancelled by operator")

    def note(self, text):
        self._ui._job_notes.append(text)


class OperatorUI:
    def __init__(self, root, default_port=None, machine=None):
        self.root = root
        self.machine = machine if machine is not None else _config_default().machine
        self.axes = self.machine.present_axes()   # [(letter, AxisConfig)] from config
        self.link = None
        self.busy = False          # a burst is streaming — pause status polling
        self.enabled = False       # UI view of energise state
        self.jog_q = queue.Queue() # (activity_label, packets) bursts for the worker
        self._worker_err = None     # last error from the worker (Tk-free thread)
        self._activity = "JOGGING"  # label shown while busy (worker-set, plain str)

        # job state
        self.plan = None             # loaded Plan, or None
        self._gui_op = None          # active GuiOperator while a job runs
        self._mount_dialog_active = False
        self._job_notes = []         # notes from GuiOperator.note(), reflected each poll

        root.title("RS485 Operator")
        root.resizable(False, False)
        self._build_connection(default_port)
        self._build_status()
        self._build_jog()
        self._build_controls()
        self._build_peripherals()
        self._build_job()

        self._worker = threading.Thread(target=self._jog_worker, daemon=True)
        self._worker.start()
        self._set_connected(False)
        self.root.after(STATUS_INTERVAL_MS, self._poll_status)

    # ── layout ────────────────────────────────────────────────────────────────

    def _build_connection(self, default_port):
        frm = ttk.LabelFrame(self.root, text="Connection")
        frm.grid(row=0, column=0, padx=8, pady=6, sticky="ew")
        ports = [p.device for p in list_ports.comports()] if list_ports else []
        ports = [SIM_PORT] + ports
        self.port_var = tk.StringVar(value=default_port or SIM_PORT)
        ttk.Label(frm, text="Port").grid(row=0, column=0, padx=4, pady=4)
        self.port_combo = ttk.Combobox(frm, textvariable=self.port_var,
                                       values=ports, width=14)
        self.port_combo.grid(row=0, column=1, padx=4)
        self.connect_btn = ttk.Button(frm, text="Connect", command=self._toggle_connect)
        self.connect_btn.grid(row=0, column=2, padx=4)

    def _build_status(self):
        frm = ttk.LabelFrame(self.root, text="Status")
        frm.grid(row=1, column=0, padx=8, pady=6, sticky="ew")
        self.state_var = tk.StringVar(value="—")
        self.enabled_var = tk.StringVar(value="—")
        self.homed_var = tk.StringVar(value="—")
        self.alarm_var = tk.StringVar(value="—")
        # position vars only for axes the config actually fits
        self.pos_vars = {ltr: tk.StringVar(value="—") for ltr, _ in self.axes}

        ttk.Label(frm, text="State:").grid(row=0, column=0, sticky="e", padx=4)
        self.state_lbl = ttk.Label(frm, textvariable=self.state_var, width=9)
        self.state_lbl.grid(row=0, column=1, sticky="w")
        ttk.Label(frm, text="En:").grid(row=0, column=2, sticky="e", padx=4)
        ttk.Label(frm, textvariable=self.enabled_var, width=5).grid(row=0, column=3, sticky="w")
        ttk.Label(frm, text="Homed:").grid(row=0, column=4, sticky="e", padx=4)
        ttk.Label(frm, textvariable=self.homed_var, width=5).grid(row=0, column=5, sticky="w")
        ttk.Label(frm, text="Alarm:").grid(row=0, column=6, sticky="e", padx=4)
        ttk.Label(frm, textvariable=self.alarm_var, width=10).grid(row=0, column=7, sticky="w")

        # one position readout per present axis, unit from config (rotary -> deg)
        for i, (ltr, ax) in enumerate(self.axes):
            unit = "deg" if ax.rotary else "mm"
            ttk.Label(frm, text=f"{ltr.upper()} ({unit}):").grid(
                row=1, column=i, sticky="e", padx=4, pady=(4, 2))
            ttk.Label(frm, textvariable=self.pos_vars[ltr], width=8).grid(row=2, column=i, padx=4)

    def _jog_defaults(self, ltr, ax):
        """(dist, feed) defaults for an axis, drawn from config where it has them."""
        if ax.rotary:
            return 90.0, (min(ax.max_rate, 60.0) if ax.max_rate else 60.0)
        if ltr == "z":
            return 2.0, self.machine.z_feed
        return 10.0, self.machine.jog_feed

    def _build_jog(self):
        frm = ttk.LabelFrame(self.root, text="Jog")
        frm.grid(row=2, column=0, padx=8, pady=6, sticky="ew")
        self.jog_widgets = []
        self.dist_vars, self.feed_vars = {}, {}
        # one jog row per present axis — generated from the config, not hardcoded
        for r, (ltr, ax) in enumerate(self.axes):
            unit = "deg" if ax.rotary else "mm"
            rate = "deg/s" if ax.rotary else "mm/s"
            dd, df = self._jog_defaults(ltr, ax)
            ttk.Label(frm, text=ltr.upper()).grid(row=r, column=0, padx=4, sticky="w")
            dv, fv = tk.DoubleVar(value=dd), tk.DoubleVar(value=round(df, 2))
            self.dist_vars[ltr], self.feed_vars[ltr] = dv, fv
            d = ttk.Entry(frm, textvariable=dv, width=7); d.grid(row=r, column=1)
            f = ttk.Entry(frm, textvariable=fv, width=7); f.grid(row=r, column=2)
            ttk.Label(frm, text=f"{unit} / {rate}").grid(row=r, column=3, padx=4, sticky="w")
            self.jog_widgets += [d, f]
            for col, (sign, sym) in enumerate(((1, f"{ltr.upper()}+"),
                                               (-1, f"{ltr.upper()}-")), start=4):
                b = ttk.Button(frm, text=sym, width=4,
                               command=lambda a=ltr, s=sign: self._jog(a, s))
                b.grid(row=r, column=col, padx=2, pady=2)
                self.jog_widgets.append(b)
        self.queue_var = tk.StringVar(value="Queue: 0")
        ttk.Label(frm, textvariable=self.queue_var).grid(
            row=len(self.axes), column=0, columnspan=4, padx=4, pady=(2, 4), sticky="w")

    def _build_controls(self):
        frm = ttk.LabelFrame(self.root, text="Control")
        frm.grid(row=3, column=0, padx=8, pady=6, sticky="ew")
        self.ctrl_widgets = []
        self.enable_btn = ttk.Button(frm, text="Enable", command=self._toggle_enable)
        self.enable_btn.grid(row=0, column=0, padx=4, pady=4)
        self.ctrl_widgets.append(self.enable_btn)
        # Machine-scope control only — job lifecycle (pause/resume/cancel) lives
        # in the Job panel.
        defs = [
            ("Set Origin", lambda: self._control(cmd.setorigin)),
            ("Unalarm",    lambda: self._control(cmd.unalarm)),
        ]
        for i, (label, fn) in enumerate(defs, start=1):
            b = ttk.Button(frm, text=label, command=fn)
            b.grid(row=0, column=i, padx=4, pady=4)
            self.ctrl_widgets.append(b)
        stop = tk.Button(frm, text="STOP", bg="#cc2222", fg="white",
                         font=("TkDefaultFont", 10, "bold"), command=self._stop)
        stop.grid(row=0, column=len(defs) + 1, padx=8, pady=4)

    def _build_peripherals(self):
        # Only appears if the config declares non-axis bus nodes. Presence-only:
        # the wire protocol has no peripheral actuation commands yet, so each row
        # shows role + node id + a Ping result. Controls land when the protocol
        # defines them (see deferred bus-overview work).
        periph = getattr(self.machine, "peripherals", ())
        self.periph_vars = {}
        self.periph_widgets = []
        if not periph:
            return
        frm = ttk.LabelFrame(self.root, text="Peripherals")
        frm.grid(row=4, column=0, padx=8, pady=6, sticky="ew")
        for r, node in enumerate(periph):
            ttk.Label(frm, text=f"{node.role}  (node {node.node_id})").grid(
                row=r, column=0, padx=4, pady=2, sticky="w")
            var = tk.StringVar(value="—")
            self.periph_vars[node.node_id] = var
            ttk.Label(frm, textvariable=var, width=10).grid(row=r, column=1, padx=4)
            b = ttk.Button(frm, text="Ping", width=6,
                           command=lambda n=node.node_id: self._ping_peripheral(n))
            b.grid(row=r, column=2, padx=4)
            self.periph_widgets.append(b)

    def _ping_peripheral(self, node_id):
        if not self.link or self.busy:
            return
        try:
            ok = cmd.ping_node(self.link, node_id)
        except Exception as e:
            self.periph_vars[node_id].set(f"err: {e}"); return
        self.periph_vars[node_id].set("present" if ok else "no-response")

    def _build_job(self):
        frm = ttk.LabelFrame(self.root, text="Job")
        frm.grid(row=5, column=0, padx=8, pady=6, sticky="ew")
        self.job_widgets = []

        # row 0: load .plan file
        self.job_file_var = tk.StringVar(value="(no plan loaded)")
        load = ttk.Button(frm, text="Load .plan…", command=self._load_plan_file)
        load.grid(row=0, column=0, padx=4, pady=4)
        self.job_widgets.append(load)
        ttk.Label(frm, textvariable=self.job_file_var, width=36).grid(
            row=0, column=1, columnspan=3, sticky="w")

        # row 1: status / notes — shows plan summary, pre-flight results, job notes
        self.pf_text = tk.Text(frm, width=46, height=6, state="disabled",
                               font=("TkFixedFont", 8))
        self.pf_text.grid(row=1, column=0, columnspan=4, padx=4, pady=4, sticky="ew")

        # row 2: Run + job lifecycle
        self.run_btn = ttk.Button(frm, text="Run Job", command=self._run_job,
                                  state="disabled")
        self.run_btn.grid(row=2, column=0, padx=4, pady=4)
        for i, (label, fn) in enumerate(
                [("Pause", cmd.pause), ("Resume", cmd.resume), ("Cancel", cmd.cancel)],
                start=1):
            b = ttk.Button(frm, text=label, command=lambda f=fn: self._control(f))
            b.grid(row=2, column=i, padx=4, pady=4)
            self.job_widgets.append(b)

    # ── connection ────────────────────────────────────────────────────────────

    def _set_connected(self, connected):
        state = "normal" if connected else "disabled"
        for w in self.jog_widgets + self.ctrl_widgets + self.job_widgets + self.periph_widgets:
            w.config(state=state)
        self._refresh_run_state()
        self.connect_btn.config(text="Disconnect" if connected else "Connect")

    def _toggle_connect(self):
        if self.link:
            try: self.link.close()
            except Exception: pass
            self.link = None
            self.enabled = False
            self.enable_btn.config(text="Enable")
            self._set_connected(False)
            self.state_var.set("—")
            return
        port = self.port_var.get()
        try:
            self.link = Link.open_sim() if port == SIM_PORT else Link.open_serial(port)
            self._set_connected(True)
        except Exception as e:
            self.link = None
            self.state_var.set(f"err: {e}")

    # ── control commands ──────────────────────────────────────────────────────

    def _control(self, fn):
        """Run a control-plane command (skip while a jog owns the port)."""
        if not self.link or self.busy:
            return
        try:
            ok, reason = fn(self.link)
            if not ok:
                self.alarm_var.set(f"rej: {reason}")
        except Exception as e:
            self.state_var.set(f"err: {e}")

    def _toggle_enable(self):
        if not self.link or self.busy:
            return
        fn = cmd.disable if self.enabled else cmd.enable
        try:
            ok, reason = fn(self.link)
        except Exception as e:
            self.state_var.set(f"err: {e}"); return
        if ok:
            self.enabled = not self.enabled
            self.enable_btn.config(text="Disable" if self.enabled else "Enable")
        else:
            self.alarm_var.set(f"rej: {reason}")

    def _stop(self):
        # Drop buffered jogs, then STOP. Bypasses the busy guard so STOP reaches
        # the Pico even mid-jog. Uses the proper request/response cmd.stop so the
        # reply is consumed (a raw write would orphan the 'ok' and desync the next
        # getstate). NOTE: a true mid-stream stop on real serial races the Sender's
        # ack reader for the port — hardening that is deferred until the firmware
        # streaming path is exercised on hardware.
        try:
            while True:
                self.jog_q.get_nowait(); self.jog_q.task_done()
        except queue.Empty:
            pass
        self._update_queue_label()
        self.enabled = False
        self.enable_btn.config(text="Enable")
        if self.link:
            try:
                cmd.stop(self.link)
            except Exception:
                pass

    # ── status polling ──────────────────────────────────────────────────────────

    def _poll_status(self):
        # Runs on the main thread (Tk only ever touched here + in button callbacks).
        # Reflects the worker's flags; never touches the port while a jog streams.
        self.enable_btn.config(text="Disable" if self.enabled else "Enable")
        if self.busy:
            self.state_var.set(self._activity)
            self.state_lbl.config(foreground="blue")
        elif self._worker_err:
            self.state_var.set(self._worker_err)
            self._worker_err = None
        elif self.link:
            try:
                st = cmd.get_state(self.link)
                self.state_var.set(st.state.name)
                self.state_lbl.config(foreground=_STATE_COLOR.get(st.state.name, "black"))
                self.enabled_var.set("".join(l for l, _ in self.axes if st.enabled(l)) or "-")
                self.homed_var.set("".join(l for l, _ in self.axes if st.homed(l)) or "-")
                self.alarm_var.set(st.alarm.name if st.alarm.value else "—")
                self.enabled = bool(st.axes_enabled)   # sync toggle from Pico truth
                pos = cmd.get_pos(self.link)            # (x, y, z, a) steps
                idx = {"x": 0, "y": 1, "z": 2, "a": 3}
                for ltr, ax in self.axes:
                    self.pos_vars[ltr].set(f"{pos[idx[ltr]] / ax.steps_per_unit:.2f}")
            except Exception as e:
                self.state_var.set(f"err: {e}")
        # Mount dialog: job worker sets _gui_op._pending_mount; we show it here
        # (main thread) so Tk is never touched from the worker thread.
        if (self._gui_op is not None
                and self._gui_op._pending_mount is not None
                and not self._mount_dialog_active):
            self._mount_dialog_active = True
            tool = self._gui_op._pending_mount
            ok = messagebox.askokcancel(
                "Mount Tool",
                f"Mount: {tool}\n\nPhysically swap the tool, then click OK.",
                parent=self.root)
            self._gui_op._mount_ok = ok
            self._gui_op._mount_event.set()
            self._mount_dialog_active = False

        # Reflect job notes accumulated by GuiOperator.note()
        if self._job_notes:
            self._set_pf_text("\n".join(self._job_notes))

        self._refresh_run_state()
        self._update_queue_label()
        self.root.after(STATUS_INTERVAL_MS, self._poll_status)

    # ── jog ─────────────────────────────────────────────────────────────────────

    def _jog(self, ltr, sign):
        if not self.link:
            return
        try:
            dist = self.dist_vars[ltr].get() * sign
            feed = self.feed_vars[ltr].get()
        except tk.TclError:
            return
        ax = getattr(self.machine, ltr)
        steps_n = int(round(dist * ax.steps_per_unit)) * (-1 if ax.invert else 1)
        if steps_n == 0:
            return
        vec = [0, 0, 0, 0]
        vec[("x", "y", "z", "a").index(ltr)] = steps_n
        feed_sps = feed * ax.steps_per_unit
        accel_sps2 = max(feed * 8.0, 50.0) * ax.steps_per_unit   # gentle ramp
        packets = make_jog(tuple(vec), feed_sps, accel_sps2, self.machine.f_cpu)
        if packets:
            self.jog_q.put(("JOGGING", packets))
            self._update_queue_label()

    def _update_queue_label(self):
        self.queue_var.set(f"Queue: {self.jog_q.qsize()}")

    def _jog_worker(self):
        # Tk-FREE: this runs off the main thread, so it must not touch any Tk
        # widget/var (Tkinter is single-threaded). It only flips plain flags;
        # _poll_status reflects them onto the UI on the main thread.
        while True:
            label, packets = self.jog_q.get()      # blocks until a burst arrives
            if self.link is None:
                self.jog_q.task_done(); continue
            self._activity = label
            self.busy = True
            try:
                if not self.enabled:               # energise once before motion
                    cmd.enable(self.link)
                    self.enabled = True
                self.link.stream(packets)
            except Exception as e:
                self._worker_err = f"err: {e}"
            finally:
                self.busy = False
                self.jog_q.task_done()

    # ── job: load / run ───────────────────────────────────────────────────────

    def _set_pf_text(self, text):
        self.pf_text.config(state="normal")
        self.pf_text.delete("1.0", "end")
        self.pf_text.insert("1.0", text)
        self.pf_text.config(state="disabled")

    def _load_plan_file(self):
        path = filedialog.askopenfilename(
            title="Load job plan",
            filetypes=[("Job plan", "*.plan"), ("All files", "*.*")])
        if not path:
            return
        try:
            self.plan = load_plan(path, self.machine)
        except Exception as e:
            self._set_pf_text(f"load failed: {e}")
            self.plan = None
            self._refresh_run_state()
            return
        summary = "\n".join(
            f"  {i+1}. {op.tool}  ({len(op.packets)} segments)"
            for i, op in enumerate(self.plan.operations))
        self.job_file_var.set(os.path.basename(path))
        self._set_pf_text(f"plan loaded — {len(self.plan.operations)} operations:\n{summary}")
        self._job_notes = []
        self._refresh_run_state()

    def _refresh_run_state(self):
        ready = bool(self.link) and bool(self.plan) and not self.busy
        if hasattr(self, "run_btn"):
            self.run_btn.config(state="normal" if ready else "disabled")

    def _run_job(self):
        if self.busy or not (self.link and self.plan):
            return
        self._gui_op = GuiOperator(self)
        self._job_notes = []
        self._set_pf_text("starting job…")
        self.busy = True
        self._activity = "RUNNING"
        self._refresh_run_state()

        plan, machine, link, gui_op = self.plan, self.machine, self.link, self._gui_op

        def _worker():
            try:
                ok, msg = send_plan(plan, machine, link, gui_op)
                gui_op.note("done: " + msg if ok else "failed: " + msg)
            except Exception as e:
                gui_op.note(f"error: {e}")
            finally:
                self.busy = False
                self._gui_op = None

        threading.Thread(target=_worker, daemon=True).start()


def main():
    ap = argparse.ArgumentParser(description="RS485 operator UI")
    ap.add_argument("--port", default=None, help="Preselect a port (or 'Simulator')")
    ap.add_argument("--sim", action="store_true",
                    help="Force the simulator + use the editable host/sim_config machine")
    args = ap.parse_args()
    # --sim runs against the sim machine (host/sim_config) so the config-driven UI
    # has peripherals etc. to show; real hardware uses the production config.
    machine = sim_machine() if args.sim else None
    root = tk.Tk()
    OperatorUI(root, default_port=SIM_PORT if args.sim else args.port, machine=machine)
    root.mainloop()


if __name__ == "__main__":
    main()
