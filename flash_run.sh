#!/bin/bash
source /home/shajidur-rahman/esp/esp-idf/export.sh > /dev/null 2>&1
echo "Checking serial port /dev/ttyACM0..."
if [ ! -e /dev/ttyACM0 ]; then
    echo "ERROR: /dev/ttyACM0 not found. Is the board plugged in via UART?"
    exit 1
fi

echo "Attempting to flash /dev/ttyACM0 with baud 460800..."
python -m esptool --chip esp32p4 -b 460800 --before default_reset --after hard_reset write_flash \
  0x8000 /home/shajidur-rahman/.gemini/antigravity/scratch/esp32p4_storage_server/storage_nas/build/partition_table/partition-table.bin \
  0x10000 /home/shajidur-rahman/.gemini/antigravity/scratch/esp32p4_storage_server/storage_nas/build/esp32p4_storage_server.bin
