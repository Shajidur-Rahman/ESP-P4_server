#!/usr/bin/env python3
import time
import subprocess
import serial

PORT = "/dev/ttyACM0"
FLASH_CMD = [
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
    "0x2000", "/home/shajidur-rahman/.gemini/antigravity/scratch/esp32p4_storage_server/storage_nas/build/bootloader/bootloader.bin",
    "0x8000", "/home/shajidur-rahman/.gemini/antigravity/scratch/esp32p4_storage_server/storage_nas/build/partition_table/partition-table.bin",
    "0x10000", "/home/shajidur-rahman/.gemini/antigravity/scratch/esp32p4_storage_server/storage_nas/build/esp32p4_storage_server.bin"
]

print(">>> Flasher is ready and listening on /dev/ttyACM0...")
print(">>> Waiting for ESP32-P4 to enter Download Mode (BOOT button held during RESET)...")

while True:
    try:
        # Try probing the chip in download mode
        res = subprocess.run(
            ["python3", "-m", "esptool", "--chip", "esp32p4", "-p", PORT, "-b", "115200", "--before", "no_reset", "chip_id"],
            capture_output=True,
            text=True,
            timeout=3
        )
        if res.returncode == 0:
            print("\n>>> ESP32-P4 IN DOWNLOAD MODE DETECTED! Starting high-speed flash now...")
            proc = subprocess.run(FLASH_CMD)
            if proc.returncode == 0:
                print("\n>>> FLASH COMPLETED SUCCESSFULLY! <<<")
                break
            else:
                print("Flash error, retrying...")
    except subprocess.TimeoutExpired:
        pass
    except Exception as e:
        print(f"Waiting... ({e})")
    time.sleep(0.5)
