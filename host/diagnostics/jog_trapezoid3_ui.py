"""
jog_trapezoid3_ui.py — simple Tkinter UI to step through the 3-packet trapezoid.

Usage:
  python -m host.diagnostics.jog_trapezoid3_ui --port COM8
"""

import argparse
import math
import time
import tkinter as tk
from tkinter import ttk
from collections import namedtuple

from host.protocol.link import Link
from host.protocol.stream import Sender
from host.protocol.packets import pack_jog, MSEG_FLAG_NONE, MSEG_FLAG_PATH_END
from pipeline.config import default as _config_default

_MS = namedtuple("MS", ["dx", "dy", "dz", "da", "interval", "flags"])

def _send_burst(link, packets, label, window=16, verbose=True):
    sender = Sender(link.serial, window=window, verbose=verbose)
    try:
        t0 = time.monotonic()
        ok = sender.send_stream(packets)
        t1 = time.monotonic()
    finally:
        sender.stop()
    print(f"[{label}] ok={ok} time={t1 - t0:.3f}s sent={sender.sent} "
          f"acked={sender.acked} nacks={sender.nacks} retries={sender.retries}")
    return ok

class JogDiagnosticUI:
    def __init__(self, root, port, baud=115200):
        self.root = root
        self.root.title("Jog Packet Stepper")
        self.root.geometry("250x120")
        
        self.port = port
        self.baud = baud
        self.link = None
        
        # Build the packets (distance set to 20mm to guarantee all 3 phases)
        self.packets, self.labels = self.build_packets()
        self.total_packets = len(self.packets)
        self.current_packet_index = 0
        
        # Connect to machine
        try:
            self.link = Link.open_serial(self.port, baud=self.baud)
            time.sleep(0.3)
            self.link.backend.serial.reset_input_buffer()
            machine = _config_default().machine
            print("enable:", self.link.command(f"enable {machine.x.node.node_id}"))
            print("getstate (before):", self.link.command("getstate"))
        except Exception as e:
            print(f"Connection failed: {e}")
            self.total_packets = 0 # Disable UI if no connection
        
        # UI
        self.btn_text = tk.StringVar(value=f"Send (0/{self.total_packets})")
        self.btn = ttk.Button(self.root, textvariable=self.btn_text, command=self.send_next_packet)
        self.btn.pack(expand=True, fill="both", padx=20, pady=20)
        
        if not self.link or self.total_packets == 0:
            self.btn.config(state="disabled")
            self.btn_text.set("Error: No port / No packets")

    def build_packets(self):
        machine = _config_default().machine
        ax = machine.x

        dist, rate, sign = 20.0, 20.0, 1  
        # accel = max(rate * 8.0, 50.0)
        accel = 50.0
        v0 = 50.0

        feed_sps = rate * ax.steps_per_unit
        accel_sps2 = accel * ax.steps_per_unit
        total_steps = abs(int(dist * sign * ax.steps_per_unit))
        invert = -1 if ax.invert else 1

        d_acc = (feed_sps**2 - v0**2) / (2.0 * accel_sps2)
        if 2 * d_acc > total_steps:
            peak = math.sqrt(v0**2 + accel_sps2 * total_steps)
            d_acc = (peak**2 - v0**2) / (2.0 * accel_sps2)
            feed_sps = peak
        d_acc = int(round(d_acc))
        d_dec = d_acc
        d_cruise = total_steps - d_acc - d_dec

        v_acc_avg  = (v0 + feed_sps) / 2.0
        v_dec_avg  = (feed_sps + v0) / 2.0
        interval_acc  = max(1, min(int(machine.f_cpu / v_acc_avg), machine.f_cpu))
        interval_crz  = max(1, min(int(machine.f_cpu / feed_sps), machine.f_cpu))
        interval_dec  = max(1, min(int(machine.f_cpu / v_dec_avg), machine.f_cpu))

        def seg(steps, interval, last):
            dx = invert * sign * steps
            flags = MSEG_FLAG_PATH_END if last else MSEG_FLAG_NONE
            return pack_jog(_MS(dx=dx, dy=0, dz=0, da=0, interval=interval, flags=flags))

        packets = []
        labels = []
        if d_acc > 0:
            packets.append(seg(d_acc, interval_acc, last=(d_cruise == 0 and d_dec == 0)))
            labels.append("accel")
        if d_cruise > 0:
            packets.append(seg(d_cruise, interval_crz, last=(d_dec == 0)))
            labels.append("cruise")
        if d_dec > 0:
            packets.append(seg(d_dec, interval_dec, last=True))
            labels.append("decel")
            
        print(f"total_steps={total_steps} d_acc={d_acc} d_cruise={d_cruise} d_dec={d_dec}")
        return packets, labels

    def send_next_packet(self):
        if self.current_packet_index < self.total_packets:
            packet = self.packets[self.current_packet_index]
            label = self.labels[self.current_packet_index]
            
            _send_burst(self.link, [packet], label)
            
            self.current_packet_index += 1
            self.btn_text.set(f"Send ({self.current_packet_index}/{self.total_packets})")
            
            if self.current_packet_index >= self.total_packets:
                self.btn.config(state="disabled")
                print("getstate (after):", self.link.command("getstate"))
                
    def on_closing(self):
        if self.link:
            self.link.close()
        self.root.destroy()


def main():
    ap = argparse.ArgumentParser(description="Step-by-step Jog Packets UI")
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=115200)
    args = ap.parse_args()

    root = tk.Tk()
    app = JogDiagnosticUI(root, port=args.port, baud=args.baud)
    root.protocol("WM_DELETE_WINDOW", app.on_closing)
    root.mainloop()

if __name__ == "__main__":
    main()