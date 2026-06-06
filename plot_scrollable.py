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
from plot_splines import parse_spline

import argparse

def main():
    parser = argparse.ArgumentParser(description="Kinematics Visualizer")
    parser.add_argument("filepath", nargs="?", default="test_tight_loops.nc", help="NC file to plot")
    parser.add_argument("--output", type=str, help="Save plot to file instead of showing GUI")
    args = parser.parse_args()
    
    filepath = args.filepath
        
    if not os.path.exists(filepath):
        print(f"Error: File '{filepath}' not found.")
        sys.exit(1)

    with open(filepath, 'r') as f:
        lines = f.readlines()

    current_x = 3000.0
    current_y = 0.0
    
    path_x = []
    path_y = []
    
    time_pts = []
    vel_pts = []
    vel_x_pts = []
    vel_y_pts = []
    acc_pts = []
    acc_x_pts = []
    acc_y_pts = []
    
    current_time = 0.0
    
    segment_boundaries = []
    
    for line in lines:
        if line.startswith(";; SEGMENT"):
            parts = line.strip().split()
            if len(parts) >= 4:
                b_time = float(parts[2])
                b_type = parts[3]
                segment_boundaries.append({'time': b_time, 'type': b_type})
            continue
            
        spline = parse_spline(line)
        if not spline:
            continue
            
        dt = spline["duration"]
        idx_x = spline["nodes"].index(1) if 1 in spline["nodes"] else -1
        idx_y = spline["nodes"].index(2) if 2 in spline["nodes"] else -1
        
        if idx_x == -1 or idx_y == -1:
            continue
            
        Ax, Bx, Cx = spline["A"][idx_x], spline["B"][idx_x], spline["C"][idx_x]
        Ay, By, Cy = spline["A"][idx_y], spline["B"][idx_y], spline["C"][idx_y]
        
        u_vals = np.linspace(0, 1, 50)
        for u in u_vals:
            dx = Ax*u**3 + Bx*u**2 + Cx*u
            dy = Ay*u**3 + By*u**2 + Cy*u
            
            path_x.append(current_x + dx)
            path_y.append(current_y + dy)
            
            vx_u = 3*Ax*u**2 + 2*Bx*u + Cx
            vy_u = 3*Ay*u**2 + 2*By*u + Cy
            vx_sec = vx_u / dt
            vy_sec = vy_u / dt
            vel_x_pts.append(vx_sec)
            vel_y_pts.append(vy_sec)
            vel_pts.append(np.sqrt(vx_sec**2 + vy_sec**2))
            
            ax_u = 6*Ax*u + 2*Bx
            ay_u = 6*Ay*u + 2*By
            ax_sec = ax_u / (dt * dt)
            ay_sec = ay_u / (dt * dt)
            acc_x_pts.append(ax_sec)
            acc_y_pts.append(ay_sec)
            acc_pts.append(np.sqrt(ax_sec**2 + ay_sec**2))
            
            time_pts.append(current_time + u * dt)
            
        current_x += Ax + Bx + Cx
        current_y += Ay + By + Cy
        current_time += dt

    # Interpolate segment boundary positions
    for b in segment_boundaries:
        b['x'] = np.interp(b['time'], time_pts, path_x)
        b['y'] = np.interp(b['time'], time_pts, path_y)

    jerk_pts = np.gradient(acc_pts, time_pts)
    jerk_x_pts = np.gradient(acc_x_pts, time_pts)
    jerk_y_pts = np.gradient(acc_y_pts, time_pts)
    
    if args.output:
        matplotlib.use('Agg')
    
    if not args.output:
        root = tk.Tk()
        root.title("Scrollable Kinematic Engine Visualizer")
        root.geometry("1000x800")
        
        # Main container
        main_frame = ttk.Frame(root)
        main_frame.pack(fill=tk.BOTH, expand=1)
        
        # Canvas for scrolling
        canvas = tk.Canvas(main_frame)
        canvas.pack(side=tk.LEFT, fill=tk.BOTH, expand=1)
        
        # Scrollbar
        scrollbar = ttk.Scrollbar(main_frame, orient=tk.VERTICAL, command=canvas.yview)
        scrollbar.pack(side=tk.RIGHT, fill=tk.Y)
        
        canvas.configure(yscrollcommand=scrollbar.set)
        
        # Frame inside canvas
        second_frame = ttk.Frame(canvas)
        window_id = canvas.create_window((0, 0), window=second_frame, anchor="nw")
        
        def on_canvas_configure(event):
            canvas.itemconfig(window_id, width=event.width)
            
        canvas.bind('<Configure>', on_canvas_configure)
        second_frame.bind('<Configure>', lambda e: canvas.configure(scrollregion=canvas.bbox("all")))
        
        # Enable Mousewheel scrolling
        def _on_mousewheel(event):
            canvas.yview_scroll(int(-1*(event.delta/120)), "units")
        canvas.bind_all("<MouseWheel>", _on_mousewheel)
    
    # Matplotlib Figure
    fig = plt.figure(figsize=(20, 16)) if args.output else plt.figure(figsize=(10, 16))
    
    ax1 = fig.add_subplot(411)
    line_path, = ax1.plot(path_x, path_y, 'b-', linewidth=2)
    ax1.set_aspect('equal')
    ax1.set_title("1. Spatial Path")
    ax1.set_xlabel("X Position (Steps)")
    ax1.set_ylabel("Y Position (Steps)")
    ax1.grid(True, linestyle="--", alpha=0.6)
    
    ax2 = fig.add_subplot(412)
    line_v_c, = ax2.plot(time_pts, vel_pts, color='darkviolet', linewidth=2.5, label='Combined')
    line_v_x, = ax2.plot(time_pts, vel_x_pts, 'r-', linewidth=1.5, label='X Axis')
    line_v_y, = ax2.plot(time_pts, vel_y_pts, 'b-', linewidth=1.5, label='Y Axis')
    ax2.set_title("2. Velocity Profile")
    ax2.set_xlabel("Time (s)")
    ax2.set_ylabel("Feedrate (steps/s)")
    ax2.grid(True, linestyle="--", alpha=0.6)
    ax2.legend(loc='upper right')
    
    ax3 = fig.add_subplot(413)
    line_a_c, = ax3.plot(time_pts, acc_pts, color='darkviolet', linewidth=2.5, label='Combined')
    line_a_x, = ax3.plot(time_pts, acc_x_pts, 'r-', linewidth=1.5, label='X Axis')
    line_a_y, = ax3.plot(time_pts, acc_y_pts, 'b-', linewidth=1.5, label='Y Axis')
    ax3.set_title("3. Acceleration Profile")
    ax3.set_xlabel("Time (s)")
    ax3.set_ylabel("Accel (steps/s²)")
    ax3.grid(True, linestyle="--", alpha=0.6)
    ax3.legend(loc='upper right')
    
    ax4 = fig.add_subplot(414)
    line_j_c, = ax4.plot(time_pts, jerk_pts, color='darkviolet', linewidth=2.5, label='Combined')
    line_j_x, = ax4.plot(time_pts, jerk_x_pts, 'r-', linewidth=1.5, label='X Axis')
    line_j_y, = ax4.plot(time_pts, jerk_y_pts, 'b-', linewidth=1.5, label='Y Axis')
    ax4.set_title("4. Jerk Profile")
    ax4.set_xlabel("Time (s)")
    ax4.set_ylabel("Jerk (steps/s³)")
    ax4.grid(True, linestyle="--", alpha=0.6)
    ax4.legend(loc='upper right')
    
    boundary_artists = []
    for i, b in enumerate(segment_boundaries):
        # Add scatter point and text to spatial path
        p1, = ax1.plot(b['x'], b['y'], 'ko', markersize=6, zorder=4)
        t1 = ax1.text(b['x'], b['y'], f" {i}: {b['type']}", fontsize=9, color='green', zorder=5)
        boundary_artists.extend([p1, t1])
        
        # Add vertical dashed lines to time series plots
        for ax in [ax2, ax3, ax4]:
            l = ax.axvline(x=b['time'], color='k', linestyle='--', alpha=0.5)
            boundary_artists.append(l)
            # Only add text to the top of ax2 to avoid clutter
            if ax == ax2:
                t2 = ax.text(b['time'], ax.get_ylim()[1], f" {b['type']}", rotation=90, verticalalignment='top', fontsize=8, color='k', alpha=0.7)
                boundary_artists.append(t2)

    fig.tight_layout()
    
    # Markers for synchronization
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
        # Embed figure in Tkinter
        canvas_fig = FigureCanvasTkAgg(fig, second_frame)
        canvas_fig.draw()
        canvas_fig.get_tk_widget().pack(fill=tk.BOTH, expand=1)
        
        toolbar = NavigationToolbar2Tk(canvas_fig, second_frame)
        toolbar.update()
        
        # Bottom control frame (Fixed, doesn't scroll)
        control_frame = ttk.Frame(root)
        control_frame.pack(side=tk.BOTTOM, fill=tk.X, pady=10, padx=20)
    
    if not args.output:
        c_var = tk.BooleanVar(value=False)
        x_var = tk.BooleanVar(value=True)
        y_var = tk.BooleanVar(value=True)
        abs_var = tk.BooleanVar(value=False)
        bounds_var = tk.BooleanVar(value=True)
    
        def update_plots(*args):
            c_on = c_var.get()
            x_on = x_var.get()
            y_on = y_var.get()
            abs_on = abs_var.get()
            bounds_on = bounds_var.get()
            
            for art in boundary_artists:
                art.set_visible(bounds_on)
        
            # Spatial Path Logic
            px = path_x if x_on else [path_x[0]] * len(path_x)
            py = path_y if y_on else [path_y[0]] * len(path_y)
            if not x_on and not y_on:
                px = [path_x[0]] * len(path_x)
                py = [path_y[0]] * len(path_y)
            line_path.set_data(px, py)
        
            if c_on:
                if x_on and y_on:
                    v_data = vel_pts
                    a_data = acc_pts
                    j_data = jerk_pts
                elif x_on:
                    v_data = np.abs(vel_x_pts)
                    a_data = np.abs(acc_x_pts)
                    j_data = np.gradient(a_data, time_pts)
                elif y_on:
                    v_data = np.abs(vel_y_pts)
                    a_data = np.abs(acc_y_pts)
                    j_data = np.gradient(a_data, time_pts)
                else:
                    v_data = np.zeros_like(vel_pts)
                    a_data = np.zeros_like(acc_pts)
                    j_data = np.zeros_like(jerk_pts)
                
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
        
        def update_markers(*args):
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
        
        # Bind text entry to update slider
        def on_entry_change(*args):
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
        # We need to manually set data to update plots since we don't have tk loop
        # Show absolute X and Y instead of Combined
        line_v_c.set_visible(False)
        line_a_c.set_visible(False)
        line_j_c.set_visible(False)
        
        line_v_x.set_ydata(np.abs(vel_x_pts))
        line_a_x.set_ydata(np.abs(acc_x_pts))
        line_j_x.set_ydata(np.abs(jerk_x_pts))
        
        line_v_y.set_ydata(np.abs(vel_y_pts))
        line_a_y.set_ydata(np.abs(acc_y_pts))
        line_j_y.set_ydata(np.abs(jerk_y_pts))
        
        line_v_x.set_visible(True)
        line_a_x.set_visible(True)
        line_j_x.set_visible(True)
        line_v_y.set_visible(True)
        line_a_y.set_visible(True)
        line_j_y.set_visible(True)
        
        # Hide markers if just saving
        marker1.set_visible(False)
        marker_v_c.set_visible(False)
        marker_v_x.set_visible(False)
        marker_v_y.set_visible(False)
        marker_a_c.set_visible(False)
        marker_a_x.set_visible(False)
        marker_a_y.set_visible(False)
        marker_j_c.set_visible(False)
        marker_j_x.set_visible(False)
        marker_j_y.set_visible(False)
        
        plt.savefig(args.output, dpi=150, bbox_inches='tight')
        print(f"Plot saved to {args.output}")

if __name__ == "__main__":
    main()
