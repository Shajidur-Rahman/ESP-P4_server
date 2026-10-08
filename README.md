# Waveshare ESP32-P4 High-Speed NAS Storage Server & Autonomous Downloader

A high-performance embedded NAS, media streaming server, and autonomous background download engine built for the **Waveshare ESP32-P4-NANO / DevKit** paired with an **ESP32-C6** co-processor.

---

## 🛠️ Hardware Specification

> **IMPORTANT HARDWARE NOTE:**
> This project is designed and tuned specifically for early silicon revision ESP32-P4 with an external wireless co-processor. Standard generic ESP32 build and flash settings **will hang or fail**.

| Component | Specification | Details / Purpose |
| :--- | :--- | :--- |
| **Primary SoC** | **ESP32-P4** | Silicon Revision **v1.0 (0.1 early stepping)**, Dual RISC-V @ 400 MHz |
| **Internal RAM** | 768 KB SRAM | Fast internal L1 cache & execution memory |
| **External PSRAM** | **32 MB High-Speed PSRAM** | Ring buffers, download thread chunks, media stream buffering |
| **SPI Flash** | **16 MB Flash** | Configured with custom 4MB factory app partition (`partitions.csv`) |
| **Wireless Co-Processor** | **ESP32-C6** | Communicates via **SDIO slave** (`esp-hosted`) providing Wi-Fi 6 & BLE 5.3 |
| **Wired Network** | **100 Mbps RMII Ethernet** | Onboard IP101GRI PHY for stable, low-latency gigabit-router LAN connection |
| **USB Host Controller** | **USB 2.0 High-Speed Host** | Dedicated OTG Port running FatFS MSC driver for external USB Flash Drives |
| **Dev Host OS** | Linux (Ubuntu / Debian x86_64) | Tested on Linux with ESP-IDF v5.5 |
| **Serial / JTAG Port** | `/dev/ttyACM0` | Direct USB-JTAG/Serial interface |

---

## ⚡ Features

1. **Dual Network Interfaces (Auto-Failover / Simultaneous):**
   - 100M Full-Duplex Wired Ethernet.
   - 802.11ax Wi-Fi 6 via ESP32-C6 SDIO slave coprocessor.
   - mDNS local discovery: access at `http://localserver.local` or direct LAN IP (e.g. `http://192.168.0.47`).
2. **High-Speed USB 2.0 Mass Storage:**
   - Mounts standard FAT32 / exFAT USB flash drives at `/usb0`.
   - Custom safe unmount & hot-remount routine preserving SCSI MSC handles without inquiry timeouts.
3. **Web Management Dashboard & File Explorer:**
   - Single-page dark mode responsive dashboard.
   - Breadcrumb navigation, file deletion, and direct file upload/download.
   - Full URL decoding supporting nested directories, spaces, and `%2F` paths.
4. **Built-In Media Streaming Player:**
   - HTML5 video and audio player with seekbar, duration tracking, and fullscreen mode.
   - HTTP partial content (`Range: bytes=X-Y`) support for smooth seeking.
5. **Autonomous Multi-Threaded Downloader:**
   - Download large files directly to the USB drive without keeping a PC/laptop awake.
   - Multi-threaded HTTP Range engine (up to 4 parallel segments).
   - Direct link resolvers for Google Drive large files (bypasses virus scan confirmation) and direct media streams.

---

## 🚀 Uploading & Flashing Instructions (Read Carefully!)

### 1. Serial Port Selection
The board identifies as **`/dev/ttyACM0`**.
> ⚠️ **DO NOT** let flashing tools scan `/dev/ttyS*` or serial ports automatically. Always pass `/dev/ttyACM0` explicitly.

If you encounter permission issues:
```bash
sudo chmod 666 /dev/ttyACM0
# Or add your user to dialout:
sudo usermod -a -G dialout $USER
```

---

### 2. How to Build (Compiling)

#### The `esp-hosted` Environment Pitfall:
In ESP-IDF v5.5, `esp-hosted` uses dynamic Kconfig evaluation macros (`$(ESP_IDF_VERSION) >= "6.1"`). If you run `ninja` or `idf.py` in a shell where the IDF environment variables are missing, Kconfig fails immediately with a syntax error.

**Always source ESP-IDF before compiling:**
```bash
bash -c "source /home/shajidur-rahman/esp/esp-idf/export.sh && idf.py build"
```
Or with Ninja:
```bash
bash -c "source /home/shajidur-rahman/esp/esp-idf/export.sh && ninja -C build"
```

---

### 3. How to Flash (The 100% Reliable Method)

Because the ESP32-P4 revision is **v1.0**, high-speed auto-reset sequences with default `idf.py flash` can hang the ROM bootloader or fail stub synchronization.

#### Flashing Option A: Using the included script (Recommended)
Simply run:
```bash
./flash_run.sh
```

#### Flashing Option B: Direct esptool command
Run:
```bash
bash -c "source /home/shajidur-rahman/esp/esp-idf/export.sh && python -m esptool --chip esp32p4 -b 460800 --before default_reset --after hard_reset write_flash 0x8000 build/partition_table/partition-table.bin 0x10000 build/esp32p4_storage_server.bin"
```

#### Why these parameters matter:
- **`-b 460800`**: Early P4 silicon is rock-solid at **460800 baud**. 921600 or 2M baud can cause packet drops or timeouts.
- **`0x8000`**: Custom partition table offset (`partition-table.bin`), setting factory app partition to 4MB.
- **`0x10000`**: Factory app binary (`esp32p4_storage_server.bin`).
- **`--before default_reset --after hard_reset`**: Sends DTR/RTS reset pulse and reboots straight into the application.

---

### 4. Serial Monitor
To monitor serial logs at 115200 baud:
```bash
bash -c "source /home/shajidur-rahman/esp/esp-idf/export.sh && python -m esp_idf_monitor -p /dev/ttyACM0 -b 115200 build/esp32p4_storage_server.elf"
```
Or:
```bash
python3 -m serial.tools.miniterm /dev/ttyACM0 115200
```

---

## 📂 Project Structure

```
├── CMakeLists.txt
├── partitions.csv             # Custom 4MB App partition layout
├── sdkconfig                  # Fully configured for P4 + C6 SDIO + High-Speed USB
├── sdkconfig.defaults
├── flash_run.sh               # One-click reliable flash script
├── ESP32P4_FLASHING_GUIDE.md  # Detailed technical flashing & silicon guide
├── components/
│   └── ethernet_init/         # IP101GRI RMII Ethernet driver
└── main/
    ├── main.c                 # System orchestrator & event loops
    ├── ethernet.c / .h        # Wired Ethernet PHY initialization
    ├── wifi_manager.c / .h    # Wi-Fi 6 connection management via C6 SDIO
    ├── mdns_manager.c / .h    # Local hostname resolution (localserver.local)
    ├── usb_storage.c / .h     # USB 2.0 MSC host driver with safe remount logic
    ├── http_server.c / .h     # Web server, REST API & video streaming handler
    ├── download_manager.c/.h  # 4-thread autonomous HTTP range downloader
    └── web_ui.h               # Embedded responsive web dashboard HTML/CSS/JS
```
