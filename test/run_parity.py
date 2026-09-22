import subprocess
import os
import sys

def main():
    # 1. Build and run host_dump
    print("Building and running host_dump...")
    subprocess.run(["pio", "run", "-e", "host_dump"], check=True)
    host_exe = os.path.join(".pio", "build", "host_dump", "program.exe")
    
    with open("host_parity.bin", "wb") as f:
        subprocess.run([host_exe], stdout=f, check=True)
    
    with open("host_parity.bin", "rb") as f:
        host_data = f.read()
    
    print(f"Host generated {len(host_data)} bytes of segments.")
    
    # 2. Build and upload pico_dump
    print("Building and uploading pico_dump...")
    subprocess.run(["pio", "run", "-e", "pico_dump", "-t", "upload"], check=True)
    
    # 3. Find serial port and read
    import serial
    import serial.tools.list_ports
    import time
    
    ports = list(serial.tools.list_ports.comports())
    if not ports:
        print("No serial ports found!")
        sys.exit(1)
        
    port = None
    for p in ports:
        if "2E8A" in p.hwid or "Pico" in p.description:
            port = p.device
            break
    if not port:
        port = ports[-1].device
        
    print(f"Connecting to Pico on {port}...")
    
    time.sleep(2)
    
    with serial.Serial(port, 115200, timeout=5) as ser:
        # Trigger
        ser.write(b'x')
        ser.flush()
        
        print("Reading from Pico...")
        pico_data = ser.read(len(host_data))
        
    if len(pico_data) != len(host_data):
        print(f"ERROR: Expected {len(host_data)} bytes, but got {len(pico_data)} bytes from Pico.")
        sys.exit(1)
        
    # 4. Compare
    print("Comparing data...")
    if host_data == pico_data:
        print("\nSUCCESS: PARITY PASSED. Exact byte-for-byte match!")
    else:
        print("\nFAILURE: PARITY MISMATCH!")
        SEG_SIZE = 44
        for i in range(0, len(host_data), SEG_SIZE):
            h_seg = host_data[i:i+SEG_SIZE]
            p_seg = pico_data[i:i+SEG_SIZE]
            if h_seg != p_seg:
                print(f"Mismatch at segment index {i // SEG_SIZE}")
                break
        sys.exit(1)

if __name__ == "__main__":
    main()
