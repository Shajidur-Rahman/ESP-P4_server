# Waveshare ESP32-P4 + ESP32-C6 Board: Build, Flashing & Hardware Guide

> **Notice for AI Assistants & Developers:**
> This repository contains firmware for the **Waveshare ESP32-P4-NANO / DevKit** paired with an **ESP32-C6** co-processor via SDIO (running `esp-hosted`).
> Read this document completely BEFORE attempting to compile or flash this board. Following standard generic ESP32 instructions will fail or hang.

---

## 1. Hardware Architecture & Environment

| Component | Specification |
| :--- | :--- |
| **Primary SoC** | ESP32-P4 (**Silicon Revision: v1.0 / 0.1 early stepping**) |
| **Crystal / Frequency** | 40 MHz Crystal, 400 MHz Dual RISC-V Core |
| **Internal RAM** | 768 KB SRAM |
| **External PSRAM** | 32 MB High-Speed PSRAM |
| **SPI Flash** | 16 MB Flash (Configured with custom `partitions.csv`) |
| **Wireless Co-Processor** | ESP32-C6 communicating via SDIO slave (`esp-hosted`) for 802.11ax Wi-Fi 6 & BLE 5.3 |
| **Wired Network** | Onboard 100 Mbps RMII Ethernet PHY (IP101GRI) |
| **USB Controller** | High-Speed USB 2.0 PHY Host (OTG Port) for Flash Drives / Storage |
| **Host Development OS** | Linux (Ubuntu / Debian x86_64) |
| **ESP-IDF Version** | **ESP-IDF v5.5** (located at `/home/shajidur-rahman/esp/esp-idf`) |

---

## 2. Serial Port & Flashing Port Identification

The board exposes a USB-Serial / JTAG interface directly to Linux.

- **Primary Interface Port:** `/dev/ttyACM0`
- **CRITICAL PITFALL:**
  - **NEVER** let tools scan generic `/dev/ttyS*` or serial ranges without specifying the port. It causes multi-minute timeouts and permissions failures.
  - Always target `/dev/ttyACM0` directly.
  - If you encounter permission errors accessing `/dev/ttyACM0`, the user's password for sudo in this environment is `29161500`.

---

## 3. How to Build (Compiling)

### The `esp-hosted` Kconfig Macro Trap
In ESP-IDF v5.5, `managed_components/espressif__esp_hosted` uses dynamic IDF version evaluation macros in Kconfig (such as `$(ESP_IDF_VERSION) >= "6.1"`). If you run `ninja` or `idf.py` in a shell that has **not loaded the IDF environment variables**, Kconfig evaluates `ESP_IDF_VERSION` as an empty string, triggering immediate syntax errors.

### The Correct Build Command
Always source the IDF export script in the same subshell execution:

```bash
bash -c "source /home/shajidur-rahman/esp/esp-idf/export.sh && idf.py build"
```
Or with Ninja:
```bash
bash -c "source /home/shajidur-rahman/esp/esp-idf/export.sh && ninja -C build"
```

---

## 4. How to Flash (Battle-Tested Procedure)

Because the ESP32-P4 silicon revision is **v1.0** (early silicon), ROM auto-reset sequences with standard `idf.py flash` can occasionally hang or mismatch stub communication if the baud rate is too high or if partition table addresses are not specified.

### Partition Table Layout (`partitions.csv`)
The factory app partition has been expanded to **4 MB** (`0x400000`) to accommodate mbedTLS, HTTP client, FatFS, Wi-Fi 6 hosted stack, and the web interface:
```csv
# Name,   Type, SubType, Offset,  Size, Flags
nvs,      data, nvs,     0x9000,  0x6000,
phy_init, data, phy,     0xf000,  0x1000,
factory,  app,  factory, 0x10000, 4M,
```

### The 100% Reliable Flash Command
Run the included `./flash_run.sh` script or execute directly:

```bash
bash -c "source /home/shajidur-rahman/esp/esp-idf/export.sh && python -m esptool --chip esp32p4 -b 460800 --before default_reset --after hard_reset write_flash 0x8000 build/partition_table/partition-table.bin 0x10000 build/esp32p4_storage_server.bin"
```

### Parameter Breakdown
- `--chip esp32p4`: Explicitly specifies the P4 target architecture.
- `-b 460800`: **460800 baud** is the golden stable baud rate for early P4 silicon. Higher rates (e.g. 921600 or 2M) can drop packets or fail handshake.
- `--before default_reset --after hard_reset`: Performs the standard DTR/RTS pulse before flashing and a clean hardware reboot into the new firmware afterwards.
- `0x8000`: Flash offset for the partition table.
- `0x10000`: Flash offset for the application binary (`factory`).

---

## 5. Serial Monitoring & Verification

To monitor output after flashing:
```bash
bash -c "source /home/shajidur-rahman/esp/esp-idf/export.sh && python -m esp_idf_monitor -p /dev/ttyACM0 -b 115200 build/esp32p4_storage_server.elf"
```
Or using python miniterm:
```bash
python3 -m serial.tools.miniterm /dev/ttyACM0 115200
```

### Expected Startup Log Signals
Upon boot, look for:
1. `ESP-Hosted: Initializing ESP-Hosted transport (SDIO)` -> ESP32-C6 handshake OK.
2. `Ethernet: Ethernet Link Up` -> 100M Full Duplex IP assigned (e.g., `192.168.0.47`).
3. `Wi-Fi: Connected to AP` -> Wi-Fi station connected with IP.
4. `USB Storage: Flash drive mounted at /usb0` -> High-Speed MSC drive recognized.
5. `mDNS: Service started at http://localserver.local` -> Local domain resolution active.

---

## 6. Software Architecture Overview

- **`main/main.c`**: Initializes NVS, Event Loops, Ethernet, Wi-Fi Station (via ESP32-C6 SDIO slave), mDNS, USB Host MSC, and HTTP Server.
- **`main/usb_storage.c`**: USB Host Mass Storage Controller. Features safe unmount and hot-remount logic that retains the USB device handle, preventing SCSI inquiry timeout panics.
- **`main/http_server.c`**: Web UI, JSON API for file browsing, chunked file upload/download, video streaming with seek range support, and in-place URL decoding (`%2F` path parser).
- **`main/download_manager.c`**: Autonomous multi-threaded background download engine supporting HTTP Range splitting, Google Drive extraction, and social platform resolution directly to `/usb0`.
- **`main/web_ui.h`**: Self-contained dark-mode single-page dashboard containing file explorer, direct video player modal, and download manager controls.

---

## 7. Golden Rules for Modifying This Codebase

1. **GCC Stringop Truncation**: GCC 13/14 treats `-Werror=stringop-truncation` as fatal in ESP-IDF v5.5. Never use `strncpy(dst, src, sizeof(dst) - 1);`. Always use `strlcpy(dst, src, sizeof(dst));` or `snprintf()`.
2. **FatFS Buffer Sizing**: High-speed USB transfers should use transfer chunks between 16 KB and 64 KB allocated from PSRAM. Writing byte-by-byte or unbuffered causes significant performance degradation.
3. **Partition Boundary**: The binary is currently ~1.3 MB. With 4 MB allocated in `partitions.csv`, plenty of headroom is available, but never flash to `0x10000` without also flashing the matching `partition-table.bin` at `0x8000` if partition boundaries change.
