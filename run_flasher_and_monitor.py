#!/usr/bin/env python3
import sys
import time
import subprocess
import serial

PORT = "/dev/ttyACM0"
BOOTLOADER = "/home/shajidur-rahman/.gemini/antigravity/scratch/esp32p4_storage_server/storage_nas/build/bootloader/bootloader.bin"
PARTITION = "/home/shajidur-rahman/.gemini/antigravity/scratch/esp32p4_storage_server/storage_nas/build/partition_table/partition-table.bin"
APP = "/home/shajidur-rahman/.gemini/antigravity/scratch/esp32p4_storage_server/storage_nas/build/esp32p4_storage_server.bin"

print("==================================================================", flush=True)
print("  ESP32-P4 FLASH & LIVE MONITOR LISTENER", flush=True)
print("==================================================================", flush=True)
print(f"Target Port : {PORT}", flush=True)
print("Listening... Press and hold BOOT, tap RESET, then release BOOT now!", flush=True)
print("------------------------------------------------------------------", flush=True)

flashed = False

while not flashed:
    try:
        # Check if the chip is in download mode
        probe = subprocess.run(
            ["python3", "-m", "esptool", "--chip", "esp32p4", "-p", PORT, "-b", "115200", "--before", "no_reset", "chip_id"],
            capture_output=True,
            text=True,
            timeout=2
        )
        if probe.returncode == 0:
            print("\n[+] DOWNLOAD MODE DETECTED!", flush=True)
            print("[+] Starting high-speed flash upload now...", flush=True)
            print("------------------------------------------------------------------", flush=True)
            
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
                print("\n[✔] FLASH COMPLETED SUCCESSFULLY!", flush=True)
                print("==================================================================", flush=True)
                print("  NOW MONITORING LIVE SERIAL OUTPUT (Storage Server Boot)...", flush=True)
                print("==================================================================", flush=True)
                flashed = True
                break
            else:
                print("[-] Flash encountered an error, will retry...", flush=True)
    except subprocess.TimeoutExpired:
        pass
    except Exception as e:
        time.sleep(0.5)
    time.sleep(0.3)

# Monitor the board live
time.sleep(0.5)
try:
    ser = serial.Serial(PORT, 115200, timeout=1)
    while True:
        line = ser.readline()
        if line:
            try:
                print(line.decode('utf-8', errors='replace').rstrip(), flush=True)
            except Exception:
                pass
except KeyboardInterrupt:
    print("\nExiting monitor.")
except Exception as e:
    print(f"\nSerial monitor ended: {e}")
