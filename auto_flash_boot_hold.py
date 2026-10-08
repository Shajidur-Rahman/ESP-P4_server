#!/usr/bin/env python3
import sys
import time
import subprocess
import serial

PORT = "/dev/ttyACM0"
BOOTLOADER = "/home/shajidur-rahman/.gemini/antigravity/scratch/esp32p4_storage_server/storage_nas/build/bootloader/bootloader.bin"
PARTITION = "/home/shajidur-rahman/.gemini/antigravity/scratch/esp32p4_storage_server/storage_nas/build/partition_table/partition-table.bin"
APP = "/home/shajidur-rahman/.gemini/antigravity/scratch/esp32p4_storage_server/storage_nas/build/esp32p4_storage_server.bin"

print("\n" + "=" * 60, flush=True)
print("  READY TO FLASH FOR REVISION v1.0 SILICON", flush=True)
print("=" * 60, flush=True)
print("Hold down the BOOT button now until you see 'DETECTED' below.\n", flush=True)

detected = False

for attempt in range(80): # 40 seconds
    try:
        s = serial.Serial(PORT, 115200, timeout=0.2)
        s.dtr = False
        s.rts = True
        time.sleep(0.08)
        s.rts = False
        time.sleep(0.15)
        raw = s.read(400)
        s.close()
        
        if b"DOWNLOAD" in raw:
            print("\n[✔] DOWNLOAD MODE DETECTED! You can release the BOOT button now!\n", flush=True)
            detected = True
            break
        elif b"boot:0x" in raw:
            for line in raw.split(b"\r\n"):
                if b"boot:" in line:
                    txt = line.decode('utf-8', errors='ignore')
                    print(f"Waiting for BOOT button... (Status: {txt})", flush=True)
    except Exception:
        time.sleep(0.3)
    time.sleep(0.4)

if not detected:
    print("\n[!] Timed out waiting for BOOT button.", flush=True)
    sys.exit(1)

print("Writing flash at 460800 baud...", flush=True)
cmd = [
    "python3", "-m", "esptool",
    "--chip", "esp32p4",
    "-p", PORT,
    "-b", "460800",
    "--before", "no_reset",
    "--after", "hard_reset",
    "write_flash",
    "--flash_mode", "dio",
    "--flash_size", "16MB",
    "--flash_freq", "80m",
    "0x2000", BOOTLOADER,
    "0x8000", PARTITION,
    "0x10000", APP
]

proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
for line in proc.stdout:
    print(line.rstrip(), flush=True)
proc.wait()

if proc.returncode == 0:
    print("\n" + "=" * 60, flush=True)
    print("  FLASH SUCCESSFUL! LIVE CONSOLE MONITOR:", flush=True)
    print("=" * 60 + "\n", flush=True)
    time.sleep(0.5)
    ser = serial.Serial(PORT, 115200, timeout=1)
    while True:
        l = ser.readline()
        if l:
            try:
                print(l.decode('utf-8', errors='replace').rstrip(), flush=True)
            except Exception:
                pass
else:
    print("\n[!] Flashing encountered an error.", flush=True)
