# /// script
# requires-python = ">=3.11"
# dependencies = [
#     "matplotlib",
#     "numpy",
# ]
# ///
import sys
import os
import numpy as np
import tkinter as tk
from tkinter import ttk
import matplotlib
matplotlib.use('TkAgg')
import matplotlib.pyplot as plt
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg, NavigationToolbar2Tk
import argparse

def calculate_trapezoidal_profile(dist, v_in, v_cr, v_out, a):
    v_in = max(0, v_in)
    v_cr = max(0.1, v_cr)
    v_out = max(0, v_out)
    a = max(0.1, a)
    dist = max(0, dist)

    if dist == 0:
        return 0, 0, 0, v_in, a

    d_accel = (v_cr**2 - v_in**2) / (2 * a) if v_cr > v_in else 0
    d_decel = (v_cr**2 - v_out**2) / (2 * a) if v_cr > v_out else 0

    if d_accel + d_decel > dist:
        v_peak_sq = (2 * a * dist + v_in**2 + v_out**2) / 2
        if v_peak_sq < 0:
            v_peak = max(v_in, v_out)
            d_accel = 0
            d_coast = dist
            d_decel = 0
        else:
            v_peak = np.sqrt(v_peak_sq)
            d_accel = (v_peak**2 - v_in**2) / (2 * a) if v_peak > v_in else 0
            d_decel = (v_peak**2 - v_out**2) / (2 * a) if v_peak > v_out else 0
            d_coast = 0
    else:
        v_peak = v_cr
        d_coast = dist - d_accel - d_decel

    return d_accel, d_coast, d_decel, v_peak, a

def generate_segment_kinematics(dx, dy, v_in, v_cr, v_out, a, curr_x, curr_y, curr_time):
    dist_major = max(abs(dx), abs(dy))
    if dist_major == 0:
        return None
        
    d_accel, d_coast, d_decel, v_peak, accel_rate = calculate_trapezoidal_profile(dist_major, v_in, v_cr, v_out, a)
    
    t_accel = (v_peak - v_in) / accel_rate if v_peak > v_in else 0
    t_coast = d_coast / v_peak if v_peak > 0 else 0
    t_decel = (v_peak - v_out) / accel_rate if v_peak > v_out else 0
    
    total_time = t_accel + t_coast + t_decel
    if total_time <= 0:
        return None

    num_points = 50
    t_vals = np.linspace(0, total_time, num_points)
    
    p_vals = np.zeros(num_points)
    v_vals = np.zeros(num_points)
    a_vals = np.zeros(num_points)
    
    for i, t in enumerate(t_vals):
        if t <= t_accel:
            v = v_in + accel_rate * t
            p = v_in * t + 0.5 * accel_rate * t**2
            a_curr = accel_rate
        elif t <= t_accel + t_coast:
            dt = t - t_accel
            v = v_peak
            p = d_accel + v_peak * dt
            a_curr = 0
        else:
            dt = t - t_accel - t_coast
            v = v_peak - accel_rate * dt
            p = d_accel + d_coast + (v_peak * dt - 0.5 * accel_rate * dt**2)
            a_curr = -accel_rate
            
        p_vals[i] = p / dist_major if dist_major > 0 else 0
        v_vals[i] = v
        a_vals[i] = a_curr

    path_x = curr_x + p_vals * dx
    path_y = curr_y + p_vals * dy
    
    sign_x = np.sign(dx) if dx != 0 else 0
    sign_y = np.sign(dy) if dy != 0 else 0
    
    ratio_x = abs(dx) / dist_major
    ratio_y = abs(dy) / dist_major
    
    vel_x = v_vals * ratio_x * sign_x
    vel_y = v_vals * ratio_y * sign_y
    acc_x = a_vals * ratio_x * sign_x
    acc_y = a_vals * ratio_y * sign_y
    
    times = curr_time + t_vals
    
    return {
        't': times, 'px': path_x, 'py': path_y,
        'vx': vel_x, 'vy': vel_y, 'v_comb': np.sqrt(vel_x**2 + vel_y**2),
        'ax': acc_x, 'ay': acc_y, 'a_comb': np.sqrt(acc_x**2 + acc_y**2),
        'total_time': total_time, 'end_x': curr_x + dx, 'end_y': curr_y + dy
    }

def main():
    parser = argparse.ArgumentParser(description="Move Command Plotter")
    parser.add_argument("filepath", nargs="?", default="output.nc", help="NC file with move commands")
    parser.add_argument("--output", type=str, help="Save plot to file")
    
    parser.add_argument("--window-start", type=float, help="Start time for plot window")
    parser.add_argument("--window-end", type=float, help="End time for plot window")
    parser.add_argument("--marker-time", type=float, help="Place marker at specific time")
    
    parser.add_argument("--comb", action="store_true", help="Show combined magnitude")
    parser.add_argument("--x", action="store_true", help="Show X-axis")
    parser.add_argument("--y", action="store_true", help="Show Y-axis")
    parser.add_argument("--abs", action="store_true", help="Show absolute values")
    parser.add_argument("--bounds", action="store_true", help="Show segment bounds")
    parser.add_argument("--smooth", type=int, default=0, help="Apply moving average filter of N points")

    args = parser.parse_args()
    
    filepath = args.filepath
        
    if not os.path.exists(filepath):
        print(f"Error: {filepath} not found")
        sys.exit(1)
        
    with open(filepath, 'r') as f:
        lines = f.readlines()
        
    current_x = 0.0
    current_y = 0.0
    current_time = 0.0
    
    time_pts = []
    path_x = []
    path_y = []
    vel_x_pts = []
    vel_y_pts = []
    vel_c_pts = []
    acc_x_pts = []
    acc_y_pts = []
    acc_c_pts = []
    
    segment_boundaries = []
    
    for idx, line in enumerate(lines):
        line = line.strip()
        if not line.startswith("move"):
            continue
            
        parts = line.split()
        if len(parts) < 10:
            continue
            
        dx = int(parts[4])
        dy = int(parts[5])
        v_in = float(parts[6])
        v_cr = float(parts[7])
        v_out = float(parts[8])
        a_max = float(parts[9])
        
        seg = generate_segment_kinematics(dx, dy, v_in, v_cr, v_out, a_max, current_x, current_y, current_time)
        if seg:
            segment_boundaries.append({'time': current_time, 'x': current_x, 'y': current_y, 'idx': idx})
            
            # Avoid duplicate time points between segments causing gradient divide-by-zero
            if time_pts and seg['t'][0] == time_pts[-1]:
                start_idx = 1
            else:
                start_idx = 0
                
            time_pts.extend(seg['t'][start_idx:])
            path_x.extend(seg['px'][start_idx:])
            path_y.extend(seg['py'][start_idx:])
            vel_x_pts.extend(seg['vx'][start_idx:])
            vel_y_pts.extend(seg['vy'][start_idx:])
            vel_c_pts.extend(seg['v_comb'][start_idx:])
            acc_x_pts.extend(seg['ax'][start_idx:])
            acc_y_pts.extend(seg['ay'][start_idx:])
            acc_c_pts.extend(seg['a_comb'][start_idx:])
            
            current_time += seg['total_time']
            current_x = seg['end_x']
            current_y = seg['end_y']
            
    if not time_pts:
        print("No valid move commands parsed.")
        sys.exit(1)
        
    if args.smooth > 1:
        # 1. Resample to uniform time steps (1ms resolution)
        dt = 0.001
        uniform_time = np.arange(time_pts[0], time_pts[-1], dt)
        
        vel_x_interp = np.interp(uniform_time, time_pts, vel_x_pts)
        vel_y_interp = np.interp(uniform_time, time_pts, vel_y_pts)
        path_x_interp = np.interp(uniform_time, time_pts, path_x)
        path_y_interp = np.interp(uniform_time, time_pts, path_y)
        
        # 2. Apply causal moving average in physical time
        kernel = np.ones(args.smooth) / args.smooth
        vel_x_smooth = np.convolve(vel_x_interp, kernel, mode='full')[:len(uniform_time)]
        vel_y_smooth = np.convolve(vel_y_interp, kernel, mode='full')[:len(uniform_time)]
        vel_c_smooth = np.sqrt(vel_x_smooth**2 + vel_y_smooth**2)
        
        # 3. Calculate derivatives on uniform grid
        acc_x_smooth = np.gradient(vel_x_smooth, dt)
        acc_y_smooth = np.gradient(vel_y_smooth, dt)
        acc_c_smooth = np.gradient(vel_c_smooth, dt)
        
        jerk_x_smooth = np.gradient(acc_x_smooth, dt)
        jerk_y_smooth = np.gradient(acc_y_smooth, dt)
        jerk_c_smooth = np.gradient(acc_c_smooth, dt)
        
        # 4. Overwrite original arrays for plotting
        time_pts = list(uniform_time)
        vel_x_pts = list(vel_x_smooth)
        vel_y_pts = list(vel_y_smooth)
        vel_c_pts = list(vel_c_smooth)
        acc_x_pts = list(acc_x_smooth)
        acc_y_pts = list(acc_y_smooth)
        acc_c_pts = list(acc_c_smooth)
        path_x = list(path_x_interp)
        path_y = list(path_y_interp)
        
        jerk_x_pts = list(jerk_x_smooth)
        jerk_y_pts = list(jerk_y_smooth)
        jerk_c_pts = list(jerk_c_smooth)
    else:
        jerk_c_pts = np.gradient(acc_c_pts, time_pts)
        jerk_x_pts = np.gradient(acc_x_pts, time_pts)
        jerk_y_pts = np.gradient(acc_y_pts, time_pts)
    
    if args.output:
        matplotlib.use('Agg')
    
    if not args.output:
        root = tk.Tk()
        root.title("Scrollable Kinematic Engine Visualizer")
        root.geometry("1000x800")
        
        main_frame = ttk.Frame(root)
        main_frame.pack(fill=tk.BOTH, expand=1)
        
        canvas = tk.Canvas(main_frame)
        canvas.pack(side=tk.LEFT, fill=tk.BOTH, expand=1)
        
        scrollbar = ttk.Scrollbar(main_frame, orient=tk.VERTICAL, command=canvas.yview)
        scrollbar.pack(side=tk.RIGHT, fill=tk.Y)
        
        canvas.configure(yscrollcommand=scrollbar.set)
        
        second_frame = ttk.Frame(canvas)
        window_id = canvas.create_window((0, 0), window=second_frame, anchor="nw")
        
        def on_canvas_configure(event):
            canvas.itemconfig(window_id, width=event.width)
            
        canvas.bind('<Configure>', on_canvas_configure)
        second_frame.bind('<Configure>', lambda e: canvas.configure(scrollregion=canvas.bbox("all")))
        
        def _on_mousewheel(event):
            canvas.yview_scroll(int(-1*(event.delta/120)), "units")
        canvas.bind_all("<MouseWheel>", _on_mousewheel)
    
    fig = plt.figure(figsize=(20, 16)) if args.output else plt.figure(figsize=(10, 16))
    
    ax1 = fig.add_subplot(411)
    line_path, = ax1.plot(path_x, path_y, 'b-', linewidth=2)
    ax1.set_aspect('equal')
    ax1.set_title("1. Spatial Path")
    ax1.set_xlabel("X Position (Steps)")
    ax1.set_ylabel("Y Position (Steps)")
    ax1.grid(True, linestyle="--", alpha=0.6)
    
    ax2 = fig.add_subplot(412)
    line_v_c, = ax2.plot(time_pts, vel_c_pts, color='darkviolet', linewidth=2.5, label='Combined')
    line_v_x, = ax2.plot(time_pts, vel_x_pts, 'r-', linewidth=1.5, label='X Axis')
    line_v_y, = ax2.plot(time_pts, vel_y_pts, 'b-', linewidth=1.5, label='Y Axis')
    ax2.set_title("2. Velocity Profile")
    ax2.set_xlabel("Time (s)")
    ax2.set_ylabel("Feedrate (steps/s)")
    ax2.grid(True, linestyle="--", alpha=0.6)
    ax2.legend(loc='upper right')
    
    ax3 = fig.add_subplot(413)
    line_a_c, = ax3.plot(time_pts, acc_c_pts, color='darkviolet', linewidth=2.5, label='Combined')
    line_a_x, = ax3.plot(time_pts, acc_x_pts, 'r-', linewidth=1.5, label='X Axis')
    line_a_y, = ax3.plot(time_pts, acc_y_pts, 'b-', linewidth=1.5, label='Y Axis')
    ax3.set_title("3. Acceleration Profile")
    ax3.set_xlabel("Time (s)")
    ax3.set_ylabel("Accel (steps/s²)")
    ax3.grid(True, linestyle="--", alpha=0.6)
    ax3.legend(loc='upper right')
    
    ax4 = fig.add_subplot(414)
    line_j_c, = ax4.plot(time_pts, jerk_c_pts, color='darkviolet', linewidth=2.5, label='Combined')
    line_j_x, = ax4.plot(time_pts, jerk_x_pts, 'r-', linewidth=1.5, label='X Axis')
    line_j_y, = ax4.plot(time_pts, jerk_y_pts, 'b-', linewidth=1.5, label='Y Axis')
    ax4.set_title("4. Jerk Profile")
    ax4.set_xlabel("Time (s)")
    ax4.set_ylabel("Jerk (steps/s³)")
    ax4.grid(True, linestyle="--", alpha=0.6)
    ax4.legend(loc='upper right')
    
    boundary_artists = []
    if segment_boundaries:
        times = [b['time'] for b in segment_boundaries]
        xs = [b['x'] for b in segment_boundaries]
        ys = [b['y'] for b in segment_boundaries]
        
        scat = ax1.scatter(xs, ys, color='k', s=10, zorder=4)
        boundary_artists.append(scat)
        
        for ax in [ax2, ax3, ax4]:
            vl = ax.vlines(times, 0, 1, transform=ax.get_xaxis_transform(), color='k', linestyle='--', alpha=0.2, linewidth=0.5)
            boundary_artists.append(vl)

    fig.tight_layout()
    
    marker_props = dict(marker='o', color='lime', markeredgecolor='black', zorder=5)
    marker1, = ax1.plot([], [], markersize=10, **marker_props)
    
    marker_v_c, = ax2.plot([], [], markersize=8, **marker_props)
    marker_v_x, = ax2.plot([], [], markersize=8, **marker_props)
    marker_v_y, = ax2.plot([], [], markersize=8, **marker_props)
    
    marker_a_c, = ax3.plot([], [], markersize=8, **marker_props)
    marker_a_x, = ax3.plot([], [], markersize=8, **marker_props)
    marker_a_y, = ax3.plot([], [], markersize=8, **marker_props)
    
    marker_j_c, = ax4.plot([], [], markersize=8, **marker_props)
    marker_j_x, = ax4.plot([], [], markersize=8, **marker_props)
    marker_j_y, = ax4.plot([], [], markersize=8, **marker_props)
    
    if not args.output:
        canvas_fig = FigureCanvasTkAgg(fig, second_frame)
        canvas_fig.draw()
        canvas_fig.get_tk_widget().pack(fill=tk.BOTH, expand=1)
        
        toolbar = NavigationToolbar2Tk(canvas_fig, second_frame)
        toolbar.update()
        
        control_frame = ttk.Frame(root)
        control_frame.pack(side=tk.BOTTOM, fill=tk.X, pady=10, padx=20)
    
    if not args.output:
        # If no specific flags were provided via command line, default to showing X, Y, and Bounds in the GUI
        show_default = not (args.comb or args.x or args.y)
        
        c_var = tk.BooleanVar(value=args.comb)
        x_var = tk.BooleanVar(value=True if show_default else args.x)
        y_var = tk.BooleanVar(value=True if show_default else args.y)
        abs_var = tk.BooleanVar(value=args.abs)
        bounds_var = tk.BooleanVar(value=args.bounds)
        
        def on_closing():
            root.quit()
            root.destroy()
            
        root.protocol("WM_DELETE_WINDOW", on_closing)
    
        def update_plots(*event_args):
            c_on = c_var.get()
            x_on = x_var.get()
            y_on = y_var.get()
            abs_on = abs_var.get()
            bounds_on = bounds_var.get()
            
            for art in boundary_artists:
                art.set_visible(bounds_on)
        
            px = path_x if x_on else [path_x[0]] * len(path_x)
            py = path_y if y_on else [path_y[0]] * len(path_y)
            if not x_on and not y_on:
                px = [path_x[0]] * len(path_x)
                py = [path_y[0]] * len(path_y)
            line_path.set_data(px, py)
        
            if c_on:
                if x_on and y_on:
                    v_data = vel_c_pts
                    a_data = acc_c_pts
                    j_data = jerk_c_pts
                elif x_on:
                    v_data = np.abs(vel_x_pts)
                    a_data = np.abs(acc_x_pts)
                    j_data = np.gradient(a_data, time_pts)
                elif y_on:
                    v_data = np.abs(vel_y_pts)
                    a_data = np.abs(acc_y_pts)
                    j_data = np.gradient(a_data, time_pts)
                else:
                    v_data = np.zeros_like(vel_c_pts)
                    a_data = np.zeros_like(acc_c_pts)
                    j_data = np.zeros_like(jerk_c_pts)
                
                line_v_c.set_ydata(v_data)
                line_a_c.set_ydata(a_data)
                line_j_c.set_ydata(j_data)
                line_v_c.set_visible(True)
                line_a_c.set_visible(True)
                line_j_c.set_visible(True)
                
                line_v_x.set_visible(False)
                line_a_x.set_visible(False)
                line_j_x.set_visible(False)
                line_v_y.set_visible(False)
                line_a_y.set_visible(False)
                line_j_y.set_visible(False)
            else:
                line_v_c.set_visible(False)
                line_a_c.set_visible(False)
                line_j_c.set_visible(False)
                
                line_v_x.set_visible(x_on)
                line_a_x.set_visible(x_on)
                line_j_x.set_visible(x_on)
                
                line_v_y.set_visible(y_on)
                line_a_y.set_visible(y_on)
                line_j_y.set_visible(y_on)
            
            if abs_on:
                line_v_x.set_ydata(np.abs(vel_x_pts))
                line_a_x.set_ydata(np.abs(acc_x_pts))
                line_j_x.set_ydata(np.abs(jerk_x_pts))
                line_v_y.set_ydata(np.abs(vel_y_pts))
                line_a_y.set_ydata(np.abs(acc_y_pts))
                line_j_y.set_ydata(np.abs(jerk_y_pts))
            else:
                line_v_x.set_ydata(vel_x_pts)
                line_a_x.set_ydata(acc_x_pts)
                line_j_x.set_ydata(jerk_x_pts)
                line_v_y.set_ydata(vel_y_pts)
                line_a_y.set_ydata(acc_y_pts)
                line_j_y.set_ydata(jerk_y_pts)
            
            ax2.relim()
            ax2.autoscale_view()
            ax3.relim()
            ax3.autoscale_view()
            ax4.relim()
            ax4.autoscale_view()
            update_markers()

        ttk.Checkbutton(control_frame, text="comb", variable=c_var, command=update_plots).pack(side=tk.LEFT, padx=5)
        ttk.Checkbutton(control_frame, text="x", variable=x_var, command=update_plots).pack(side=tk.LEFT, padx=5)
        ttk.Checkbutton(control_frame, text="y", variable=y_var, command=update_plots).pack(side=tk.LEFT, padx=5)
        ttk.Checkbutton(control_frame, text="abs", variable=abs_var, command=update_plots).pack(side=tk.LEFT, padx=15)
        ttk.Checkbutton(control_frame, text="bounds", variable=bounds_var, command=update_plots).pack(side=tk.LEFT, padx=5)
        
        ttk.Label(control_frame, text="Time (s):").pack(side=tk.LEFT, padx=(20, 5))
        
        max_t = time_pts[-1] if time_pts else 1.0
        time_var = tk.DoubleVar(value=0.0)
        
        def update_markers(*event_args):
            t_val = time_var.get()
            if not time_pts:
                return
            idx = np.argmin(np.abs(np.array(time_pts) - t_val))
            c_on = c_var.get()
            x_on = x_var.get()
            y_on = y_var.get()
            abs_on = abs_var.get()
            
            px_val = path_x[idx] if x_on else path_x[0]
            py_val = path_y[idx] if y_on else path_y[0]
            if not x_on and not y_on:
                px_val, py_val = path_x[0], path_y[0]
            marker1.set_data([px_val], [py_val])
            
            if c_on:
                marker_v_x.set_visible(False)
                marker_a_x.set_visible(False)
                marker_j_x.set_visible(False)
                marker_v_y.set_visible(False)
                marker_a_y.set_visible(False)
                marker_j_y.set_visible(False)
                
                marker_v_c.set_visible(True)
                marker_a_c.set_visible(True)
                marker_j_c.set_visible(True)
                
                v_val = line_v_c.get_ydata()[idx]
                a_val = line_a_c.get_ydata()[idx]
                j_val = line_j_c.get_ydata()[idx]
                
                marker_v_c.set_data([time_pts[idx]], [v_val])
                marker_a_c.set_data([time_pts[idx]], [a_val])
                marker_j_c.set_data([time_pts[idx]], [j_val])
            else:
                marker_v_c.set_visible(False)
                marker_a_c.set_visible(False)
                marker_j_c.set_visible(False)
                
                marker_v_x.set_visible(x_on)
                marker_a_x.set_visible(x_on)
                marker_j_x.set_visible(x_on)
                if abs_on:
                    vx_val, ax_val, jx_val = np.abs(vel_x_pts[idx]), np.abs(acc_x_pts[idx]), np.abs(jerk_x_pts[idx])
                    vy_val, ay_val, jy_val = np.abs(vel_y_pts[idx]), np.abs(acc_y_pts[idx]), np.abs(jerk_y_pts[idx])
                else:
                    vx_val, ax_val, jx_val = vel_x_pts[idx], acc_x_pts[idx], jerk_x_pts[idx]
                    vy_val, ay_val, jy_val = vel_y_pts[idx], acc_y_pts[idx], jerk_y_pts[idx]
                    
                if x_on:
                    marker_v_x.set_data([time_pts[idx]], [vx_val])
                    marker_a_x.set_data([time_pts[idx]], [ax_val])
                    marker_j_x.set_data([time_pts[idx]], [jx_val])
                    
                marker_v_y.set_visible(y_on)
                marker_a_y.set_visible(y_on)
                marker_j_y.set_visible(y_on)
                if y_on:
                    marker_v_y.set_data([time_pts[idx]], [vy_val])
                    marker_a_y.set_data([time_pts[idx]], [ay_val])
                    marker_j_y.set_data([time_pts[idx]], [jy_val])
                
            canvas_fig.draw_idle()
            
        slider = ttk.Scale(control_frame, from_=0, to=max_t, variable=time_var, orient=tk.HORIZONTAL, command=update_markers)
        slider.pack(side=tk.LEFT, fill=tk.X, expand=True, padx=10)
        
        time_entry = ttk.Entry(control_frame, textvariable=time_var, width=10)
        time_entry.pack(side=tk.LEFT)
        
        def on_entry_change(*event_args):
            try:
                val = float(time_var.get())
                val = max(0.0, min(max_t, val))
                time_var.set(val)
                update_markers()
            except ValueError:
                pass
                
        time_entry.bind("<Return>", on_entry_change)
        
        update_plots()
        root.mainloop()
    else:
        for art in boundary_artists:
            art.set_visible(args.bounds)
            
        if args.comb:
            line_v_x.set_visible(False)
            line_a_x.set_visible(False)
            line_j_x.set_visible(False)
            line_v_y.set_visible(False)
            line_a_y.set_visible(False)
            line_j_y.set_visible(False)
            
            line_v_c.set_visible(True)
            line_a_c.set_visible(True)
            line_j_c.set_visible(True)
        else:
            line_v_c.set_visible(False)
            line_a_c.set_visible(False)
            line_j_c.set_visible(False)
            
            line_v_x.set_visible(args.x)
            line_a_x.set_visible(args.x)
            line_j_x.set_visible(args.x)
            
            line_v_y.set_visible(args.y)
            line_a_y.set_visible(args.y)
            line_j_y.set_visible(args.y)
        
        if args.abs:
            line_v_x.set_ydata(np.abs(vel_x_pts))
            line_a_x.set_ydata(np.abs(acc_x_pts))
            line_j_x.set_ydata(np.abs(jerk_x_pts))
            line_v_y.set_ydata(np.abs(vel_y_pts))
            line_a_y.set_ydata(np.abs(acc_y_pts))
            line_j_y.set_ydata(np.abs(jerk_y_pts))
            
        if args.marker_time is not None:
            idx = (np.abs(np.array(time_pts) - args.marker_time)).argmin()
            ax1.plot([path_x[idx]], [path_y[idx]], **marker_props)
            if args.comb:
                ax2.plot([time_pts[idx]], [vel_c_pts[idx]], **marker_props)
                ax3.plot([time_pts[idx]], [acc_c_pts[idx]], **marker_props)
                ax4.plot([time_pts[idx]], [jerk_c_pts[idx]], **marker_props)
            else:
                if args.x:
                    v_val = np.abs(vel_x_pts[idx]) if args.abs else vel_x_pts[idx]
                    a_val = np.abs(acc_x_pts[idx]) if args.abs else acc_x_pts[idx]
                    j_val = np.abs(jerk_x_pts[idx]) if args.abs else jerk_x_pts[idx]
                    ax2.plot([time_pts[idx]], [v_val], **marker_props)
                    ax3.plot([time_pts[idx]], [a_val], **marker_props)
                    ax4.plot([time_pts[idx]], [j_val], **marker_props)
                if args.y:
                    v_val = np.abs(vel_y_pts[idx]) if args.abs else vel_y_pts[idx]
                    a_val = np.abs(acc_y_pts[idx]) if args.abs else acc_y_pts[idx]
                    j_val = np.abs(jerk_y_pts[idx]) if args.abs else jerk_y_pts[idx]
                    ax2.plot([time_pts[idx]], [v_val], **marker_props)
                    ax3.plot([time_pts[idx]], [a_val], **marker_props)
                    ax4.plot([time_pts[idx]], [j_val], **marker_props)

        if args.window_start is not None or args.window_end is not None:
            start = args.window_start if args.window_start is not None else time_pts[0]
            end = args.window_end if args.window_end is not None else time_pts[-1]
            for ax in [ax2, ax3, ax4]:
                ax.set_xlim(start, end)
                
        plt.savefig(args.output, dpi=150, bbox_inches='tight')
        print(f"Plot saved to {args.output}")

if __name__ == "__main__":
    main()
