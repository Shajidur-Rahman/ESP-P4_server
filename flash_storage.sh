#!/bin/bash
set -e

PORT="/dev/ttyACM0"
DIR="/home/shajidur-rahman/.gemini/antigravity/scratch/esp32p4_storage_server/storage_nas"

echo "=================================================================="
echo "  ESP32-P4 STORAGE SERVER FLASHER (REVISION v1.0 SILICON)"
echo "=================================================================="
echo "Target port: $PORT"
echo ""
echo ">>> PLEASE DO THE BUTTON COMBINATION NOW:"
echo "1. Press and HOLD the 'BOOT' button."
echo "2. While holding 'BOOT', click and release the 'RST' button."
echo "3. Release the 'BOOT' button."
echo "------------------------------------------------------------------"

for i in {1..30}; do
    echo "Attempt $i: Connecting to ESP32-P4..."
    if python3 -m esptool --chip esp32p4 -p "$PORT" -b 460800 \
        --before default_reset --after hard_reset write_flash \
        --flash_mode dio --flash_size 16MB --flash_freq 80m \
        0x2000 "$DIR/build/bootloader/bootloader.bin" \
        0x8000 "$DIR/build/partition_table/partition-table.bin" \
        0x10000 "$DIR/build/esp32p4_storage_server.bin"; then
        
        echo ""
        echo "=================================================================="
        echo " [✔] FLASH SUCCEEDED! BOOTING STORAGE SERVER..."
        echo "=================================================================="
        sleep 1
        python3 -c "
import serial
s = serial.Serial('$PORT', 115200, timeout=1)
print('>>> Live Storage Server Logs:')
while True:
    line = s.readline()
    if line:
        print(line.decode('utf-8', errors='replace').rstrip())
"
        exit 0
    fi
    sleep 0.5
done

echo "[!] Failed to connect after multiple attempts."
exit 1
