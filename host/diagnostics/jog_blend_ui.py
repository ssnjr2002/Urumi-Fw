"""
jog_blend_ui.py — diagnostic Tkinter UI to test Jog Blending and Smooth Jog Cancel.

This proves that seamless jog blending and smooth jog cancel is possible by
keeping the decel packet off the wire until the last possible moment.
"""

import sys
import os
import argparse
import math
import time
import tkinter as tk
from tkinter import ttk
import threading
import queue
from collections import namedtuple

from host.protocol.link import Link
from host.protocol.session import ListSource
from host.protocol.packets import pack_jog, MSEG_FLAG_NONE, MSEG_FLAG_PATH_END
from pipeline.config import default as _config_default

_MS = namedtuple("MS", ["dx", "dy", "dz", "da", "interval", "flags"])

def _send_burst(link, packets, window=16):
    """Mechanical port to the new stack: one Session per burst, sharing the
    Link's reader instead of spawning its own.

    NOTE this is still burst-shaped — accel, cruise and decel are separate
    sessions, so a "blend" is really "did the next burst land before the buffer
    drained". Manual jogging is an OPEN session (docs/comms_architecture.md
    §2.3); the real port is one long-lived session fed from the intent queue,
    with reversal handled by Session.truncate(). Left as follow-on work."""
    if not packets:
        return
    link.reset_seq()
    link.session(ListSource(packets), window=window).run()

class JogBlendUI:
    def __init__(self, root, port, baud=115200):
        self.root = root
        self.root.title("Jog Blending & Cancel")
        self.root.geometry("300x180")

        self.port = port
        self.baud = baud
        self.link = None
        self.jog_q = queue.Queue()
        self.machine = _config_default().machine

        # Connect to machine
        try:
            self.link = Link.open_serial(self.port, baud=self.baud)
            time.sleep(0.3)
            self.link.backend.serial.reset_input_buffer()
            print("enable:", self.link.command(f"enable {self.machine.x.node.node_id}"))
        except Exception as e:
            print(f"Connection failed: {e}")

        # UI
        btn_frame = ttk.Frame(self.root)
        btn_frame.pack(expand=True, fill="both", padx=20, pady=(20, 10))

        self.btn_neg = ttk.Button(btn_frame, text="X -10mm", command=lambda: self.on_jog_click(-1))
        self.btn_neg.pack(side="left", expand=True, fill="both", padx=(0, 5))

        self.btn_pos = ttk.Button(btn_frame, text="X +10mm", command=lambda: self.on_jog_click(1))
        self.btn_pos.pack(side="left", expand=True, fill="both", padx=(5, 0))

        self.status_var = tk.StringVar(value="Idle. Queue: 0")
        self.status_lbl = ttk.Label(self.root, textvariable=self.status_var)
        self.status_lbl.pack(pady=(0, 10))

        if not self.link:
            self.btn_pos.config(state="disabled")
            self.btn_neg.config(state="disabled")
            self.status_var.set("Error: No Port")
            return

        # Keyboard Bindings
        self.root.bind("<Left>", lambda e: self.on_jog_click(-1))
        self.root.bind("<Right>", lambda e: self.on_jog_click(1))

        # Start the background trickle-feeder thread
        self.worker = threading.Thread(target=self._worker_loop, daemon=True)
        self.worker.start()

        self._update_status()

    def _update_status(self):
        if self.link:
            self.status_var.set(f"Queue: {self.jog_q.qsize()}")
        self.root.after(100, self._update_status)

    def on_jog_click(self, sign):
        # Smooth Jog Cancel: If direction reversed, flush all pending blocks and stop!
        with self.jog_q.mutex:
            # BUGFIX: queue.Queue.empty() acquires the mutex internally.
            # If we call it while holding the mutex, it deadlocks and freezes the UI!
            # Instead, we just check the underlying deque length directly.
            if len(self.jog_q.queue) > 0:
                if self.jog_q.queue[-1]['sign'] != sign:
                    print("--- JOG CANCEL! Clearing queued items and stopping due to direction change ---")
                    self.jog_q.queue.clear()
                    return # Discard the new direction block!

        # Calculate blocks for a 10mm move at 40mm/s
        ax = self.machine.x
        dist, rate = 30.0, 40.0
        accel = max(rate * 8.0, 50.0)
        v0 = 50.0

        feed_sps = rate * ax.steps_per_unit
        accel_sps2 = accel * ax.steps_per_unit
        total_steps = abs(int(dist * ax.steps_per_unit))
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
        interval_acc  = max(1, min(int(self.machine.f_cpu / v_acc_avg), self.machine.f_cpu))
        interval_crz  = max(1, min(int(self.machine.f_cpu / feed_sps), self.machine.f_cpu))
        interval_dec  = max(1, min(int(self.machine.f_cpu / v_dec_avg), self.machine.f_cpu))

        def seg(steps, interval, last):
            dx = invert * sign * steps
            flags = MSEG_FLAG_PATH_END if last else MSEG_FLAG_NONE
            return pack_jog(_MS(dx=dx, dy=0, dz=0, da=0, interval=interval, flags=flags))

        def seg_chunked(steps, interval, velocity, last):
            """Split a long constant-velocity run into ~10ms chunks instead of
            one opaque packet -- buf_count only changes at packet boundaries,
            so a single big packet makes 'buffer nearly drained' indistinguishable
            from 'buffer just filled'. Chunking gives the poll loop a fine-grained
            signal of how much cruise time is actually left."""
            chunk_steps = max(1, int(velocity * 0.01))   # ~10ms of steps at this speed
            out = []
            remaining = steps
            while remaining > 0:
                n = min(chunk_steps, remaining)
                remaining -= n
                out.append(seg(n, interval, last=(last and remaining == 0)))
            return out

        # Build Lego blocks
        blocks = {'sign': sign}
        if d_acc > 0:
            blocks['accel'] = seg(d_acc, interval_acc, last=False)
            blocks['decel'] = seg(d_dec, interval_dec, last=True)
        if d_cruise > 0:
            blocks['short_cruise'] = seg_chunked(d_cruise, interval_crz, feed_sps, last=False)

        # 10mm full cruise block
        blocks['blend_cruise'] = seg_chunked(total_steps, interval_crz, feed_sps, last=False)

        self.jog_q.put(blocks)

    # Poll get_status() at this interval while waiting for the buffer to run
    # low. Cheap (1 byte out, 9 bytes back) so this can run tight without
    # competing meaningfully with the stream for port bandwidth.
    POLL_INTERVAL_S = 0.02
    # Decide blend-vs-decel once at most this many queued segments remain
    # (including the one currently executing). Cruise is now chunked into
    # ~10ms packets (see seg_chunked), so a handful of chunks is a few tens
    # of ms of remaining runway -- enough to cover one status poll round-trip
    # plus the next packet's transmission, without waiting almost a full
    # phase's duration like the un-chunked version did.
    LOW_WATER_SEGMENTS = 3

    def _wait_for_buffer_low(self):
        """Block until the Pico reports <=LOW_WATER_SEGMENTS queued, polling
        real buffer occupancy instead of guessing from wall-clock timing."""
        while True:
            try:
                status = self.link.get_status()
            except Exception as e:
                print(f"(status poll failed: {e} -- falling back to short sleep)")
                time.sleep(self.POLL_INTERVAL_S)
                continue
            if status.buf_count <= self.LOW_WATER_SEGMENTS:
                return
            time.sleep(self.POLL_INTERVAL_S)

    def _worker_loop(self):
        while True:
            # Wait for a click
            blocks = self.jog_q.get()
            current_sign = blocks['sign']
            current_decel = blocks.get('decel')

            direction_str = "POSITIVE" if current_sign > 0 else "NEGATIVE"
            print(f"\n>>> Start of New Jog Sequence ({direction_str})")

            # Send Accel + Short Cruise
            to_send = []

            if 'accel' in blocks:
                to_send.append(blocks['accel'])
                print("Yielding Accel...")

            if 'short_cruise' in blocks:
                to_send.extend(blocks['short_cruise'])
                print(f"Yielding Short Cruise ({len(blocks['short_cruise'])} chunks)...")

            _send_burst(self.link, to_send)
            self.jog_q.task_done()

            # We are now cruising. Wait for the buffer to actually run low
            # (not a guessed duration) before deciding blend vs. decel --
            # this reacts to real firmware state instead of dead-reckoning.
            print("(Machine is accelerating/cruising... polling buffer occupancy)")
            self._wait_for_buffer_low()

            while True:
                # The buffer is nearly drained. Did they click again?
                if self.jog_q.empty():
                    print("Queue is empty! Yielding Decel...")
                    if current_decel:
                        _send_burst(self.link, [current_decel])
                    print("<<< Sequence Complete (Stopped)\n")
                    break
                else:
                    # Peek at the next queued item
                    next_blocks = self.jog_q.queue[0]
                    if next_blocks['sign'] == current_sign:
                        # BLEND!
                        blocks = self.jog_q.get()
                        print(f"BLENDING! Yielding Full Cruise ({len(blocks['blend_cruise'])} chunks)...")
                        _send_burst(self.link, blocks['blend_cruise'])
                        self.jog_q.task_done()

                        print("(Machine is cruising... polling buffer occupancy)")
                        self._wait_for_buffer_low()
                    else:
                        # DIRECTION CHANGE! (SMOOTH CANCEL)
                        print("Direction changed! Yielding Decel to bring machine to a stop...")
                        if current_decel:
                            _send_burst(self.link, [current_decel])

                        # Discard the reversed block so it doesn't move in the new direction
                        self.jog_q.get()
                        self.jog_q.task_done()

                        print("<<< Stopped gracefully. Discarded the reverse command.\n")
                        break # Exits the inner cruise loop. Outer loop waits for a fresh click.

    def on_closing(self):
        if self.link:
            self.link.close()
        self.root.destroy()

def main():
    ap = argparse.ArgumentParser(description="Jog Blending Trickle Feed")
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=115200)
    args = ap.parse_args()

    root = tk.Tk()
    app = JogBlendUI(root, port=args.port, baud=args.baud)
    root.protocol("WM_DELETE_WINDOW", app.on_closing)
    root.mainloop()

if __name__ == "__main__":
    main()
