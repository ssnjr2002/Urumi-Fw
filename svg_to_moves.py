# /// script
# requires-python = ">=3.11"
# dependencies = [
#     "svgpathtools",
#     "numpy"
# ]
# ///

import sys
import numpy as np
from svgpathtools import svg2paths, Path, Line, QuadraticBezier, CubicBezier, Arc

# Machine Configuration
STEPS_PER_MM = 160.0
NODE_X = 1
NODE_Y = 2

# Kinematic Defaults
V_CRUISE = 2000.0
ACCEL = 5000.0

# Slicing Configuration
SEGMENT_LENGTH_MM = 1.0
JUNCTION_DEVIATION_MM = 0.05

class Block:
    def __init__(self, dx, dy):
        self.dx = dx
        self.dy = dy
        # Distance of the major axis, as this dictates the timing!
        self.distance = max(abs(dx), abs(dy))
        self.max_junction_speed = 0.0
        self.v_entry = 0.0
        self.v_cruise = V_CRUISE
        self.v_exit = 0.0

def main():
    if len(sys.argv) < 3:
        print("Usage: uv run svg_to_moves.py <input.svg> <output.nc>")
        sys.exit(1)
        
    input_svg = sys.argv[1]
    output_nc = sys.argv[2]
    
    try:
        paths, attributes = svg2paths(input_svg)
    except Exception as e:
        print(f"Error reading SVG: {e}")
        sys.exit(1)
        
    # Phase 1: Rasterize SVG to integer steps
    current_x_steps = 0
    current_y_steps = 0
    current_point = 0j
    blocks = []
    
    # 1. Issue a move to the start of the path
    start_point = paths[0].start
    target_x_steps = int(start_point.real * STEPS_PER_MM)
    target_y_steps = int(start_point.imag * STEPS_PER_MM)
    
    dx_steps = target_x_steps - current_x_steps
    dy_steps = target_y_steps - current_y_steps
    
    if dx_steps != 0 or dy_steps != 0:
        b = Block(dx_steps, dy_steps)
        comb_dist_steps = abs(start_point - current_point) * STEPS_PER_MM
        if comb_dist_steps > 0:
            b.v_cruise = V_CRUISE * (b.distance / comb_dist_steps)
        else:
            b.v_cruise = V_CRUISE
        blocks.append(b)
        
    current_x_steps = target_x_steps
    current_y_steps = target_y_steps
    current_point = start_point

    for path in paths:
        # Iterate over individual geometric segments (Line, Bezier, etc.) to preserve corners!
        for sub_segment in path:
            length_mm = sub_segment.length()
            num_segments = max(1, int(np.ceil(length_mm / SEGMENT_LENGTH_MM)))
            
            # Skip u=0 because we are already sitting at the start of the sub-segment
            u_vals = np.linspace(0, 1, num_segments + 1)[1:]
            
            for u in u_vals:
                point = sub_segment.point(u)
                
                target_x_steps = int(point.real * STEPS_PER_MM)
                target_y_steps = int(point.imag * STEPS_PER_MM)
                
                dx_steps = target_x_steps - current_x_steps
                dy_steps = target_y_steps - current_y_steps
                
                if dx_steps != 0 or dy_steps != 0:
                    b = Block(dx_steps, dy_steps)
                    comb_dist_steps = abs(point - current_point) * STEPS_PER_MM
                    if comb_dist_steps > 0:
                        b.v_cruise = V_CRUISE * (b.distance / comb_dist_steps)
                    else:
                        b.v_cruise = V_CRUISE
                    blocks.append(b)
                
                current_x_steps = target_x_steps
                current_y_steps = target_y_steps
                current_point = point

    if not blocks:
        print("No paths found.")
        sys.exit(0)

    # Phase 2: Junction Deviation Algorithm
    jd_steps = JUNCTION_DEVIATION_MM * STEPS_PER_MM
    
    for i in range(len(blocks) - 1):
        b1 = blocks[i]
        b2 = blocks[i+1]
        
        len1 = np.sqrt(b1.dx**2 + b1.dy**2)
        len2 = np.sqrt(b2.dx**2 + b2.dy**2)
        
        if len1 == 0 or len2 == 0:
            b1.max_junction_speed = 0.0
            continue
            
        u1_x, u1_y = b1.dx / len1, b1.dy / len1
        u2_x, u2_y = b2.dx / len2, b2.dy / len2
        
        # Turn angle alpha (0 for straight, 180 for complete reversal)
        cos_alpha = u1_x * u2_x + u1_y * u2_y
        cos_alpha = max(-1.0, min(1.0, cos_alpha))
        
        if cos_alpha > 0.9999:
            # Perfectly straight line
            b1.max_junction_speed = b1.v_cruise
        elif cos_alpha < -0.9999:
            # Complete 180 reversal
            b1.max_junction_speed = 0.0
        else:
            # Junction Deviation Math
            cos_alpha_half = np.sqrt((1.0 + cos_alpha) / 2.0)
            # Prevent divide by zero just in case
            denominator = 1.0 - cos_alpha_half
            if denominator < 1e-6:
                b1.max_junction_speed = b1.v_cruise
            else:
                radius = jd_steps * cos_alpha_half / denominator
                v_junct_comb = np.sqrt(ACCEL * radius)
                v_junct_major = v_junct_comb * (b1.distance / len1)
                b1.max_junction_speed = min(v_junct_major, b1.v_cruise)
            
    # The last block always ends at 0
    blocks[-1].max_junction_speed = 0.0

    # Phase 3: The Forward Pass
    current_entry_speed = 0.0
    for b in blocks:
        b.v_entry = current_entry_speed
        
        # Calculate maximum possible exit speed based on acceleration limit
        max_exit_sq = b.v_entry**2 + 2 * ACCEL * b.distance
        max_exit = np.sqrt(max_exit_sq)
        
        # Cap exit speed to junction limit and cruise limit
        b.v_exit = min(max_exit, b.max_junction_speed, b.v_cruise)
        
        # Pass to next block
        current_entry_speed = b.v_exit

    # Phase 4: The Backward Pass
    blocks[-1].v_exit = 0.0
    current_exit_speed = 0.0
    
    for i in range(len(blocks) - 1, -1, -1):
        b = blocks[i]
        
        # Cap exit speed to the backward limit (if it's stricter than the forward limit)
        b.v_exit = min(b.v_exit, current_exit_speed)
        
        # Calculate maximum allowable entry speed to be able to safely decelerate
        max_entry_sq = b.v_exit**2 + 2 * ACCEL * b.distance
        max_entry = np.sqrt(max_entry_sq)
        
        # Cap entry speed to the safe limit
        b.v_entry = min(b.v_entry, max_entry, b.v_cruise)
        
        # Pass to previous block
        current_exit_speed = b.v_entry
        
        # Force the previous block's exit to perfectly match this entry to prevent mathematical jitter
        if i > 0:
            blocks[i-1].v_exit = b.v_entry

    # Phase 5: Trapezoid Geometry Enforcement
    # Ensure no block requires an impossible triangle by capping v_cruise.
    # This guarantees the firmware only ever has to execute trapezoids!
    for b in blocks:
        # Check if the block is a triangle by seeing if accel+decel dist exceeds total dist
        accel_dist = max(0.0, (b.v_cruise**2 - b.v_entry**2) / (2.0 * ACCEL))
        decel_dist = max(0.0, (b.v_cruise**2 - b.v_exit**2) / (2.0 * ACCEL))
        
        if accel_dist + decel_dist > b.distance:
            # It's a triangle! Calculate the geometric peak reachable velocity
            peak_v2 = (2.0 * ACCEL * b.distance + b.v_entry**2 + b.v_exit**2) / 2.0
            if peak_v2 > 0:
                b.v_cruise = min(b.v_cruise, np.sqrt(peak_v2))

    # Generate Output
    moves = []
    for b in blocks:
        # Round velocities to integers for firmware ingestion
        v_in = int(round(b.v_entry))
        v_cr = int(round(b.v_cruise))
        v_out = int(round(b.v_exit))
        a_max = int(ACCEL)
        moves.append(f"move 2 {NODE_X} {NODE_Y} {b.dx} {b.dy} {v_in} {v_cr} {v_out} {a_max}")

    # Write output
    with open(output_nc, 'w') as f:
        for move in moves:
            f.write(move + '\n')
            
    print(f"Generated {len(moves)} accelerated move commands in {output_nc}")

if __name__ == "__main__":
    main()
