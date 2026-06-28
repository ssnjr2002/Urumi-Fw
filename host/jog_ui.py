"""
jog_ui.py — small Tkinter UI for manual jogging + live machine status.

Reuses jog.make_jog (ramped trapezoidal moves) and the Go-Back-N Sender, and
polls the Pico's `status` command for the state machine + dead-reckoned
position. One persistent serial connection is shared: status polling is
suspended while a jog streams (the Sender owns the port during a jog).

Run:
  python host/jog_ui.py
  python host/jog_ui.py --port COM8
"""

import sys, os, argparse, time, threading, queue

sys.path.insert(0, os.path.dirname(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "pipeline", "stages"))

import tkinter as tk
from tkinter import ttk

from jog import make_jog
from sender import Sender
from config import default as _config_default

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    serial = None
    list_ports = None

STATUS_INTERVAL_MS = 400


class JogUI:
    def __init__(self, root, default_port=None):
        self.root = root
        self.cfg = _config_default()
        self.machine = self.cfg.machine
        self.ser = None
        self.busy = False          # a jog is streaming — pause status polling
        self._rx = ""              # text accumulator for status replies
        self.enabled = False       # UI view of Enable/Disable toggle
        self._enabled_nodes = set()  # nodes already enabled — jog enables lazily
        self.jog_q = queue.Queue() # buffered jog presses, drained in order

        root.title("RS485 Jog + Status")
        root.resizable(False, False)

        self._build_connection(default_port)
        self._build_status()
        self._build_jog()
        self._build_controls()

        # single worker drains the jog queue so presses can be spammed
        self._worker = threading.Thread(target=self._jog_worker, daemon=True)
        self._worker.start()

        self._set_connected(False)
        self.root.after(STATUS_INTERVAL_MS, self._poll_status)

    # ── layout ────────────────────────────────────────────────────────────────

    def _build_connection(self, default_port):
        frm = ttk.LabelFrame(self.root, text="Connection")
        frm.grid(row=0, column=0, padx=8, pady=6, sticky="ew")
        ports = [p.device for p in list_ports.comports()] if list_ports else []
        self.port_var = tk.StringVar(value=default_port or (ports[0] if ports else ""))
        ttk.Label(frm, text="Port").grid(row=0, column=0, padx=4, pady=4)
        self.port_combo = ttk.Combobox(frm, textvariable=self.port_var, values=ports, width=14)
        self.port_combo.grid(row=0, column=1, padx=4)
        self.connect_btn = ttk.Button(frm, text="Connect", command=self._toggle_connect)
        self.connect_btn.grid(row=0, column=2, padx=4)

    def _build_status(self):
        frm = ttk.LabelFrame(self.root, text="Status")
        frm.grid(row=1, column=0, padx=8, pady=6, sticky="ew")
        self.state_var = tk.StringVar(value="—")
        self.buf_var   = tk.StringVar(value="—")
        self.pos_vars  = {ax: tk.StringVar(value="—") for ax in ("x", "y", "z", "a")}

        ttk.Label(frm, text="State:").grid(row=0, column=0, sticky="e", padx=4)
        self.state_lbl = ttk.Label(frm, textvariable=self.state_var, width=10)
        self.state_lbl.grid(row=0, column=1, sticky="w")
        ttk.Label(frm, text="Buffer:").grid(row=0, column=2, sticky="e", padx=4)
        ttk.Label(frm, textvariable=self.buf_var, width=10).grid(row=0, column=3, sticky="w")

        units = {"x": "mm", "y": "mm", "z": "mm", "a": "deg"}
        for i, ax in enumerate(("x", "y", "z", "a")):
            ttk.Label(frm, text=f"{ax.upper()} ({units[ax]}):").grid(row=1, column=i, sticky="e", padx=4, pady=(4,2))
            ttk.Label(frm, textvariable=self.pos_vars[ax], width=8).grid(row=2, column=i, padx=4)

    def _build_jog(self):
        frm = ttk.LabelFrame(self.root, text="Jog")
        frm.grid(row=2, column=0, padx=8, pady=6, sticky="ew")
        self.jog_widgets = []

        # group: (label, axes list, default dist, default feed, units)
        groups = [
            ("XY", ["x", "y"], 10.0, 20.0, "mm  /  mm/s"),
            ("Z",  ["z"],       2.0,  3.0, "mm  /  mm/s"),
            ("A",  ["a"],      90.0, 60.0, "deg / deg/s"),
        ]
        self.dist_vars = {}
        self.feed_vars = {}
        r = 0
        for name, axes, dd, df, units in groups:
            ttk.Label(frm, text=name).grid(row=r, column=0, padx=4, sticky="w")
            dv = tk.DoubleVar(value=dd); fv = tk.DoubleVar(value=df)
            self.dist_vars[name] = dv; self.feed_vars[name] = fv
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
                    self.jog_widgets.append(b)
                    col += 1
            r += 1
        self.queue_var = tk.StringVar(value="Queue: 0")
        ttk.Label(frm, textvariable=self.queue_var).grid(
            row=r, column=0, columnspan=4, padx=4, pady=(2, 4), sticky="w")

    def _build_controls(self):
        frm = ttk.LabelFrame(self.root, text="Control")
        frm.grid(row=3, column=0, padx=8, pady=6, sticky="ew")
        self.ctrl_widgets = []
        self.enable_btn = ttk.Button(frm, text="Enable All", command=self._toggle_enable)
        self.enable_btn.grid(row=0, column=0, padx=4, pady=4)
        self.ctrl_widgets.append(self.enable_btn)
        defs = [
            ("Set Origin", lambda: self._send_text("setorigin")),
            ("Unalarm",    lambda: self._send_text("unalarm")),
        ]
        for i, (label, cmd) in enumerate(defs, start=1):
            b = ttk.Button(frm, text=label, command=cmd)
            b.grid(row=0, column=i, padx=4, pady=4)
            self.ctrl_widgets.append(b)
        # STOP is always live, even mid-jog
        stop = tk.Button(frm, text="STOP", bg="#cc2222", fg="white",
                         font=("TkDefaultFont", 10, "bold"), command=self._stop)
        stop.grid(row=0, column=len(defs) + 1, padx=8, pady=4)

    # ── connection ────────────────────────────────────────────────────────────

    def _set_connected(self, connected):
        state = "normal" if connected else "disabled"
        for w in self.jog_widgets + self.ctrl_widgets:
            w.config(state=state)
        self.connect_btn.config(text="Disconnect" if connected else "Connect")

    def _toggle_connect(self):
        if self.ser:
            try:
                self.ser.close()
            except Exception:
                pass
            self.ser = None
            self.enabled = False
            self._enabled_nodes.clear()
            self.enable_btn.config(text="Enable All")
            self._set_connected(False)
            self.state_var.set("—")
            return
        if serial is None:
            self.state_var.set("no pyserial")
            return
        try:
            self.ser = serial.Serial(self.port_var.get(), 115200, timeout=0.05)
            time.sleep(0.3)
            self.ser.reset_input_buffer()
            self._set_connected(True)
        except Exception as e:
            self.ser = None
            self.state_var.set(f"err: {e}")

    # ── serial helpers ────────────────────────────────────────────────────────

    def _send_text(self, cmd):
        if not self.ser or self.busy:
            return
        try:
            self.ser.write((cmd + "\n").encode())
            self.ser.flush()
        except Exception as e:
            self.state_var.set(f"err: {e}")

    def _toggle_enable(self):
        if not self.ser or self.busy:
            return
        target = not self.enabled
        cmd = "enable" if target else "disable"
        for n in (1, 2, 3, 4):
            self._send_text(f"{cmd} {n}")
            time.sleep(0.05)
        self.enabled = target
        if target:
            self._enabled_nodes = {1, 2, 3, 4}
        else:
            self._enabled_nodes.clear()
        self.enable_btn.config(text="Disable All" if target else "Enable All")

    def _stop(self):
        # Drop any buffered jogs, then STOP — bypassing the busy guard so STOP
        # always reaches the Pico.
        try:
            while True:
                self.jog_q.get_nowait()
                self.jog_q.task_done()
        except queue.Empty:
            pass
        self._update_queue_label()
        self._enabled_nodes.clear()   # ESTOP -> re-enable before next jog
        if self.ser:
            try:
                self.ser.write(b"stop\n")
                self.ser.flush()
            except Exception:
                pass

    # ── status polling ────────────────────────────────────────────────────────

    def _poll_status(self):
        if self.ser and not self.busy:
            try:
                self.ser.write(b"status\n")
                self.ser.flush()
                time.sleep(0.03)
                self._rx += self.ser.read(self.ser.in_waiting or 1).decode(errors="replace")
                while "\n" in self._rx:
                    line, self._rx = self._rx.split("\n", 1)
                    if line.startswith("state="):
                        self._parse_status(line.strip())
            except Exception as e:
                self.state_var.set(f"err: {e}")
        self.root.after(STATUS_INTERVAL_MS, self._poll_status)

    def _parse_status(self, line):
        # state=IDLE pos=x,y,z,a valid=1 buf=0/512
        parts = dict(kv.split("=", 1) for kv in line.split() if "=" in kv)
        st = parts.get("state", "?")
        self.state_var.set(st)
        self.state_lbl.config(foreground={"IDLE": "green", "RUNNING": "blue",
                                          "ESTOP": "red", "ALARM": "red"}.get(st, "black"))
        self.buf_var.set(parts.get("buf", "—") + ("" if parts.get("valid") == "1" else "  (no origin)"))
        if "pos" in parts:
            try:
                steps = [int(v) for v in parts["pos"].split(",")]
                spu = [self.machine.x.steps_per_unit, self.machine.y.steps_per_unit,
                       self.machine.z.steps_per_unit, self.machine.a.steps_per_unit]
                for ax, s, u in zip(("x", "y", "z", "a"), steps, spu):
                    self.pos_vars[ax].set(f"{s / u:.2f}")
            except Exception:
                pass

    # ── jog ───────────────────────────────────────────────────────────────────

    def _jog(self, axis, sign, group):
        """Button press: snapshot dist/feed, build packets, enqueue. Presses
        can be spammed — they buffer in jog_q and run in order."""
        if not self.ser:
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
            self.jog_q.put((ax.node.node_id, packets))
            self._update_queue_label()

    def _update_queue_label(self):
        self.queue_var.set(f"Queue: {self.jog_q.qsize()}")

    def _jog_worker(self):
        while True:
            node, packets = self.jog_q.get()      # blocks until a press arrives
            if self.ser is None:
                self.jog_q.task_done()
                continue
            self.busy = True
            self.root.after(0, lambda: self.state_var.set("JOGGING"))
            try:
                # Enable each node once (avoids repeated "Node N: Enabled" text
                # interleaving with the binary stream on every queued jog)
                if node not in self._enabled_nodes:
                    self.ser.write(f"enable {node}\n".encode())
                    self.ser.flush()
                    time.sleep(0.12)
                    self._enabled_nodes.add(node)
                self.ser.reset_input_buffer()
                sender = Sender(self.ser, window=16)
                sender.send_stream(packets)
                sender.stop()
            except Exception as e:
                self.root.after(0, lambda e=e: self.state_var.set(f"err: {e}"))
            finally:
                self.busy = False
                self.jog_q.task_done()
                self.root.after(0, self._update_queue_label)


def main():
    ap = argparse.ArgumentParser(description="Tkinter jog + status UI")
    ap.add_argument("--port", default=None)
    args = ap.parse_args()
    root = tk.Tk()
    JogUI(root, default_port=args.port)
    root.mainloop()


if __name__ == "__main__":
    main()
