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
from tkinter import ttk, filedialog

from config import default as _config_default, TOOL_PROFILES
from host.protocol.link import Link
from host.protocol.packets import make_jog
from host.protocol.stream import read_packets
from host.protocol import commands as cmd
from host.protocol.state import MachineState
from host.preflight import preflight

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


class OperatorUI:
    def __init__(self, root, default_port=None):
        self.root = root
        self.machine = _config_default().machine
        self.link = None
        self.busy = False          # a burst is streaming — pause status polling
        self.enabled = False       # UI view of energise state
        self.jog_q = queue.Queue() # (activity_label, packets) bursts for the worker
        self._worker_err = None     # last error from the worker (Tk-free thread)
        self._activity = "JOGGING"  # label shown while busy (worker-set, plain str)

        # job state
        self.job_packets = None     # loaded production packets, or None
        self.preflight_ok = False

        root.title("RS485 Operator")
        root.resizable(False, False)
        self._build_connection(default_port)
        self._build_status()
        self._build_jog()
        self._build_controls()
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
        self.homed_var = tk.StringVar(value="—")
        self.alarm_var = tk.StringVar(value="—")
        self.pos_vars  = {ax: tk.StringVar(value="—") for ax in ("x", "y", "z", "a")}

        ttk.Label(frm, text="State:").grid(row=0, column=0, sticky="e", padx=4)
        self.state_lbl = ttk.Label(frm, textvariable=self.state_var, width=10)
        self.state_lbl.grid(row=0, column=1, sticky="w")
        ttk.Label(frm, text="Homed:").grid(row=0, column=2, sticky="e", padx=4)
        ttk.Label(frm, textvariable=self.homed_var, width=6).grid(row=0, column=3, sticky="w")
        ttk.Label(frm, text="Alarm:").grid(row=0, column=4, sticky="e", padx=4)
        ttk.Label(frm, textvariable=self.alarm_var, width=10).grid(row=0, column=5, sticky="w")

        units = {"x": "mm", "y": "mm", "z": "mm", "a": "deg"}
        for i, ax in enumerate(("x", "y", "z", "a")):
            ttk.Label(frm, text=f"{ax.upper()} ({units[ax]}):").grid(
                row=1, column=i, sticky="e", padx=4, pady=(4, 2))
            ttk.Label(frm, textvariable=self.pos_vars[ax], width=8).grid(row=2, column=i, padx=4)

    def _build_jog(self):
        frm = ttk.LabelFrame(self.root, text="Jog")
        frm.grid(row=2, column=0, padx=8, pady=6, sticky="ew")
        self.jog_widgets = []
        groups = [
            ("XY", ["x", "y"], 10.0, 20.0, "mm  /  mm/s"),
            ("Z",  ["z"],       2.0,  3.0, "mm  /  mm/s"),
            ("A",  ["a"],      90.0, 60.0, "deg / deg/s"),
        ]
        self.dist_vars, self.feed_vars = {}, {}
        for r, (name, axes, dd, df, units) in enumerate(groups):
            ttk.Label(frm, text=name).grid(row=r, column=0, padx=4, sticky="w")
            dv, fv = tk.DoubleVar(value=dd), tk.DoubleVar(value=df)
            self.dist_vars[name], self.feed_vars[name] = dv, fv
            d = ttk.Entry(frm, textvariable=dv, width=7); d.grid(row=r, column=1)
            f = ttk.Entry(frm, textvariable=fv, width=7); f.grid(row=r, column=2)
            ttk.Label(frm, text=units).grid(row=r, column=3, padx=4, sticky="w")
            self.jog_widgets += [d, f]
            col = 4
            for ax in axes:
                for sign, sym in ((1, f"{ax.upper()}+"), (-1, f"{ax.upper()}-")):
                    b = ttk.Button(frm, text=sym, width=4,
                                   command=lambda a=ax, s=sign, g=name: self._jog(a, s, g))
                    b.grid(row=r, column=col, padx=2, pady=2)
                    self.jog_widgets.append(b); col += 1
        self.queue_var = tk.StringVar(value="Queue: 0")
        ttk.Label(frm, textvariable=self.queue_var).grid(
            row=len(groups), column=0, columnspan=4, padx=4, pady=(2, 4), sticky="w")

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

    def _build_job(self):
        frm = ttk.LabelFrame(self.root, text="Job")
        frm.grid(row=4, column=0, padx=8, pady=6, sticky="ew")
        self.job_widgets = []

        # row 0: file + tool
        self.job_file_var = tk.StringVar(value="(no job loaded)")
        load = ttk.Button(frm, text="Load .bin…", command=self._load_job)
        load.grid(row=0, column=0, padx=4, pady=4)
        self.job_widgets.append(load)
        ttk.Label(frm, textvariable=self.job_file_var, width=24).grid(
            row=0, column=1, columnspan=2, sticky="w")
        ttk.Label(frm, text="Tool:").grid(row=0, column=3, sticky="e", padx=4)
        self.tool_var = tk.StringVar(value="knife")
        tool = ttk.Combobox(frm, textvariable=self.tool_var, width=8,
                            values=list(TOOL_PROFILES), state="readonly")
        tool.grid(row=0, column=4, padx=4)
        self.job_widgets.append(tool)

        # row 1: pre-flight button + results
        pf = ttk.Button(frm, text="Pre-flight", command=self._run_preflight)
        pf.grid(row=1, column=0, padx=4, pady=4, sticky="n")
        self.job_widgets.append(pf)
        self.pf_text = tk.Text(frm, width=46, height=7, state="disabled",
                               font=("TkFixedFont", 8))
        self.pf_text.grid(row=1, column=1, columnspan=4, padx=4, pady=4, sticky="ew")

        # row 2: operator tool-mounted confirmation + Run
        self.confirm_var = tk.BooleanVar(value=False)
        self.confirm_chk = ttk.Checkbutton(
            frm, text="Confirm tool mounted", variable=self.confirm_var,
            command=self._refresh_run_state)
        self.confirm_chk.grid(row=2, column=0, columnspan=3, padx=4, sticky="w")
        self.job_widgets.append(self.confirm_chk)
        self.run_btn = ttk.Button(frm, text="Run Job", command=self._run_job,
                                  state="disabled")
        self.run_btn.grid(row=2, column=4, padx=4, pady=4)

        # row 3: job lifecycle (acts on the running/paused job)
        life = ttk.Frame(frm)
        life.grid(row=3, column=0, columnspan=5, padx=4, pady=(0, 4), sticky="w")
        for i, (label, fn) in enumerate(
                [("Pause", cmd.pause), ("Resume", cmd.resume), ("Cancel", cmd.cancel)]):
            b = ttk.Button(life, text=label, command=lambda f=fn: self._control(f))
            b.grid(row=0, column=i, padx=(0, 6))
            self.job_widgets.append(b)

    # ── connection ────────────────────────────────────────────────────────────

    def _set_connected(self, connected):
        state = "normal" if connected else "disabled"
        for w in self.jog_widgets + self.ctrl_widgets + self.job_widgets:
            w.config(state=state)
        if not connected:
            self.preflight_ok = False
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
                self.homed_var.set("".join(a for a in "xyza" if st.homed(a)) or "-")
                self.alarm_var.set(st.alarm.name if st.alarm.value else "—")
                pos = cmd.get_pos(self.link)
                spu = [self.machine.x.steps_per_unit, self.machine.y.steps_per_unit,
                       self.machine.z.steps_per_unit, self.machine.a.steps_per_unit]
                for ax, s, u in zip(("x", "y", "z", "a"), pos, spu):
                    self.pos_vars[ax].set(f"{s / u:.2f}")
            except Exception as e:
                self.state_var.set(f"err: {e}")
        self._update_queue_label()
        self.root.after(STATUS_INTERVAL_MS, self._poll_status)

    # ── jog ─────────────────────────────────────────────────────────────────────

    def _jog(self, axis, sign, group):
        if not self.link:
            return
        try:
            dist = self.dist_vars[group].get() * sign
            feed = self.feed_vars[group].get()
        except tk.TclError:
            return
        ax = getattr(self.machine, axis)
        steps_n = int(round(dist * ax.steps_per_unit)) * (-1 if ax.invert else 1)
        if steps_n == 0:
            return
        vec = [0, 0, 0, 0]
        vec[("x", "y", "z", "a").index(axis)] = steps_n
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

    # ── job: load / pre-flight / run ──────────────────────────────────────────

    def _set_pf_text(self, text):
        self.pf_text.config(state="normal")
        self.pf_text.delete("1.0", "end")
        self.pf_text.insert("1.0", text)
        self.pf_text.config(state="disabled")

    def _load_job(self):
        path = filedialog.askopenfilename(
            title="Load production .bin",
            filetypes=[("Job binary", "*.bin"), ("All files", "*.*")])
        if not path:
            return
        try:
            with open(path, "rb") as f:
                packets = list(read_packets(f))
        except Exception as e:
            self._set_pf_text(f"load failed: {e}")
            return
        self.job_packets = packets
        self.job_file_var.set(f"{os.path.basename(path)}  ({len(packets)} pkts)")
        self.preflight_ok = False
        self.confirm_var.set(False)
        self._set_pf_text("loaded — run pre-flight")
        self._refresh_run_state()

    def _run_preflight(self):
        if not self.link or self.busy:
            return
        profile = TOOL_PROFILES[self.tool_var.get()]
        try:
            pf = preflight(self.link, self.machine, profile)
        except Exception as e:
            self.preflight_ok = False
            self._set_pf_text(f"pre-flight error: {e}")
            self._refresh_run_state()
            return
        self.preflight_ok = pf.ok
        self._set_pf_text(str(pf))
        self.confirm_chk.config(text=f"Confirm '{profile.name}' mounted")
        self._refresh_run_state()

    def _refresh_run_state(self):
        ready = bool(self.link) and self.preflight_ok \
            and bool(self.job_packets) and self.confirm_var.get()
        if hasattr(self, "run_btn"):
            self.run_btn.config(state="normal" if ready else "disabled")

    def _run_job(self):
        if self.busy or not (self.link and self.preflight_ok
                             and self.job_packets and self.confirm_var.get()):
            return
        # Stream the production burst through the worker (state label RUNNING).
        # NOTE: the MCFG preamble (required_axes) is not emitted by svg_to_packets
        # yet — Phase 1 streams MSEG packets as-is; the preamble lands with step 8.
        self.jog_q.put(("RUNNING", self.job_packets))
        self._update_queue_label()


def main():
    ap = argparse.ArgumentParser(description="RS485 operator UI")
    ap.add_argument("--port", default=None, help="Preselect a port (or 'Simulator')")
    ap.add_argument("--sim", action="store_true", help="Force the in-process simulator")
    args = ap.parse_args()
    root = tk.Tk()
    OperatorUI(root, default_port=SIM_PORT if args.sim else args.port)
    root.mainloop()


if __name__ == "__main__":
    main()
