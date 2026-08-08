# TinyPAN Bridge

ESP32 firmware that bridges Bluetooth PAN (phone tethering) to a WiFi hotspot — turn your phone's cellular data into a shareable WiFi network for laptops, tablets, and IoT devices.

```
Phone (BT Hotspot) ──▶ ESP32 ──▶ WiFi AP ──▶ Laptop / Tablet / IoT devices
```

Built on [TinyPAN](https://github.com/Akhil-Chaturvedi/TinyPAN), a zero-allocation Bluetooth PAN client library for microcontrollers.

> [!IMPORTANT]
> **This is an experimental, vibe-coded project.** The firmware works but is flaky and not bug-free — expect connection drops, packet loss, and rough edges. Bluetooth and WiFi sharing the same 2.4 GHz radio on the ESP32 is an inherently difficult coexistence problem. Pull requests are very welcome and encouraged — see [Contributing](#contributing).

## Features

- **Bluetooth PAN client** — connects to Android/iOS Personal Hotspot via BNEP (Bluetooth Classic)
- **WiFi Access Point** — creates a configurable WiFi hotspot (2.4 GHz, up to 10 clients)
- **lwIP NAT/routing** — transparently routes WiFi client traffic through the Bluetooth internet link
- **Web UI** — manage everything from `http://192.168.99.1`:
  - Real-time status dashboard (connection state, IP, uptime, client count)
  - Scan nearby Bluetooth devices and pair with one click
  - Configure WiFi SSID, password, and channel
  - Auto-reconnect to bonded phone on reboot
- **Captive portal DNS** — redirects all DNS queries to the web UI for zero-config access
- **Self-recovering** — resilience watchdog monitors upstream connectivity, refreshes DNS, repairs NAT/routes, and reboots as a last resort
- **Status LED** — GPIO2 shows connection state at a glance
- **Persistent config** — all settings survive reboots via NVS

## Hardware Requirements

- **ESP32** with Bluetooth Classic (BR/EDR) — e.g., ESP32-WROOM-32, ESP32-DevKitC, NodeMCU-32S
- ESP32-S3/C3/C6 are **not supported** (BLE-only chips lack BNEP/Classic BT)
- 4 MB flash minimum (partitions: 2 MB factory app + 2 MB SPIFFS web assets)
- USB-to-UART bridge for flashing (most dev boards have this built in)

## Prerequisites

### To build from source (recommended)

You need [ESP-IDF v5.4+](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/) with toolchain support for ESP32.

### To flash a pre-built binary (no build tools needed)

You only need Python 3.8+ and `esptool`:

| OS | Install command |
|-----|-----------------|
| macOS | `brew install esptool` or `pip install esptool` |
| Linux | `pip install esptool` |
| Windows | `pip install esptool` |

Pre-built binaries are distributed via [GitHub Releases](../../releases). Download `tinypan_bridge_merged.bin` from the latest release.

## Platform-Specific Setup

### Finding your serial port

<details>
<summary><b>macOS</b></summary>

```bash
ls /dev/cu.usbserial-* /dev/cu.usbmodem-*
```
Common ports: `/dev/cu.usbserial-110`, `/dev/cu.usbmodem-2101` (varies by adapter).

If nothing appears, install the appropriate driver:
- CP2102/CP210x adapters: [Silicon Labs VCP driver](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers)
- CH340/CH341 adapters: [WCH CH340 driver](https://www.wch-ic.com/downloads/CH34XSER_MAC_ZIP.html)
</details>

<details>
<summary><b>Linux</b></summary>

```bash
ls /dev/ttyUSB* /dev/ttyACM*
```
Common ports: `/dev/ttyUSB0`, `/dev/ttyACM0`.

If you get permission denied, add your user to the `dialout` group:
```bash
sudo usermod -aG dialout $USER
# Log out and back in (or reboot) for the change to take effect
```
</details>

<details>
<summary><b>Windows</b></summary>

Open **Device Manager** → **Ports (COM & LPT)**. Look for a COM port (e.g., `COM3`, `COM5`).

If no COM port appears, install the driver for your USB-to-UART chip:
- CP2102/CP210x: [Silicon Labs VCP driver](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers)
- CH340/CH341: [WCH CH340 driver](https://www.wch-ic.com/downloads/CH341SER_EXE.html)
</details>

## Quick Start

### Option A: Flash a pre-built binary

```bash
# 1. Download tinypan_bridge_merged.bin from GitHub Releases

# 2. Flash (replace PORT with your serial port)
# macOS / Linux:
esptool.py --chip esp32 --port /dev/cu.usbserial-XXXX --baud 460800 \
  write_flash 0x0 tinypan_bridge_merged.bin

# Windows (PowerShell):
esptool.py --chip esp32 --port COM5 --baud 460800 `
  write_flash 0x0 tinypan_bridge_merged.bin
```

The merged image contains bootloader, partition table, firmware, and web assets:

| Offset | Partition |
|--------|-----------|
| `0x1000` | bootloader |
| `0x8000` | partition table |
| `0x10000` | application firmware |
| `0x200000` | SPIFFS web assets |

### Option B: Build from source

#### 1. Install ESP-IDF

<details>
<summary><b>macOS / Linux</b></summary>

```bash
mkdir -p ~/esp
cd ~/esp
git clone --depth 1 -b v5.4 https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32
```
</details>

<details>
<summary><b>Windows</b></summary>

```powershell
mkdir $env:USERPROFILE\esp
cd $env:USERPROFILE\esp
git clone --depth 1 -b v5.4 https://github.com/espressif/esp-idf.git
cd esp-idf
.\install.bat esp32
```

Or use the [ESP-IDF Tools Installer](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/windows-setup.html) for a guided setup with GUI.
</details>

#### 2. Build and flash

<details>
<summary><b>macOS / Linux</b></summary>

```bash
# Source ESP-IDF environment (do this in every new terminal)
. ~/esp/esp-idf/export.sh

# Clone and build
git clone https://github.com/nuaimat/tinypan-esp32-bridge.git
cd tinypan-esp32-bridge

# Optional: customize settings
idf.py menuconfig  # → "TinyPAN Bridge Configuration" submenu

# Build
idf.py build

# Flash firmware + web assets (SPIFFS flashed automatically via FLASH_IN_PROJECT)
idf.py -p /dev/cu.usbserial-XXXX flash

# Monitor serial output (Ctrl+] to exit)
idf.py -p /dev/cu.usbserial-XXXX monitor
```
</details>

<details>
<summary><b>Windows</b></summary>

```powershell
# Source ESP-IDF environment (do this in every new terminal)
. $env:USERPROFILE\esp\esp-idf\export.ps1

# Clone and build
git clone https://github.com/nuaimat/tinypan-esp32-bridge.git
cd tinypan-esp32-bridge

# Optional: customize settings
idf.py menuconfig

# Build
idf.py build

# Flash firmware + web assets
idf.py -p COM5 flash

# Monitor serial output (Ctrl+] to exit)
idf.py -p COM5 monitor
```
</details>

<details>
<summary><b>Using the included flash.sh script (macOS / Linux)</b></summary>

```bash
./flash.sh /dev/cu.usbserial-XXXX
```

The script sources ESP-IDF, builds, and flashes in one step. Requires ESP-IDF at `~/esp/esp-idf/`.
</details>

#### 3. Recreate the merged binary (optional)

After building, you can create a single merged binary for distribution:

```bash
python -m esptool --chip esp32 merge_bin -o tinypan_bridge_merged.bin \
  --flash_mode dio --flash_size 4MB --flash_freq 40m \
  0x1000 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x10000 build/tinypan_bridge.bin \
  0x200000 build/www.bin
```

## First Use

1. **Power on** the ESP32 — LED blinks fast (booting), then slow (AP ready)
2. **Connect** your laptop/phone to WiFi: **TinyPAN-Bridge** (password: `bridge1234`)
3. **Open** `http://192.168.99.1` in a browser
4. **Click "Scan for Devices"** — your phone should appear
5. **Click "Connect"** on your phone, then accept the Bluetooth pairing prompt
6. **Enable Bluetooth tethering** on your phone:
   - **Android**: Settings → Network & Internet → Hotspot & tethering → Bluetooth tethering → ON
   - **iOS**: Settings → Personal Hotspot → Allow Others to Join (Bluetooth pairing must be accepted first)
7. The status dot turns **green** — you're online

### Changing WiFi Settings

Use the web UI (Settings panel) or run `idf.py menuconfig` → TinyPAN Bridge Configuration and reflash.

## Architecture

```
┌──────────┐   BT PAN/BNEP    ┌──────────────────┐    WiFi AP      ┌──────────┐
│  Phone   │◄────────────────►│     ESP32        │◄──────────────►│  Laptop  │
│ Hotspot  │   lwIP netif     │                  │   DHCP Server   │  Tablet  │
└──────────┘                  │  TinyPAN  BNEP   │  192.168.99.1  │  IoT dev │
                              │  lwIP NAT/Router │                 └──────────┘
                              │  HTTP Server     │
                              │  Web UI (SPIFFS) │
                              └──────────────────┘
```

- ESP32 connects to phone via Bluetooth BNEP (L2CAP PSM 0x000F), obtains IP via DHCP over PAN
- ESP32 runs WiFi SoftAP on channel 1 with WPA2-PSK
- NAPT bridges WiFi clients (`192.168.99.x`) to the PAN interface
- DNS forwarding: captive DNS on `192.168.99.1:53`, forwards to phone's DNS when PAN is online
- Static ARP entry for PAN gateway avoids ARP resolution over Bluetooth

### Source layout

| Component | Source |
|-----------|--------|
| Bluetooth init + GAP | `main/bt_manager.c` |
| Bluetooth state machine | `main/bt_state.c` |
| TinyPAN wrapper | `main/pan_manager.c` |
| WiFi softAP | `main/wifi_ap.c` |
| lwIP NAT/router | `main/nat_router.c` |
| HTTP server + REST API | `main/http_server.c` |
| Captive portal DNS | `main/captive_dns.c` |
| Resilience watchdog | `main/resilience.c` |
| LED indicator | `main/led_indicator.c` |
| Console REPL | `main/console_repl.c` |
| TinyPAN library | `components/tinypan/` |
| Web UI assets | `main/www/` |

## LED Patterns (GPIO2)

| State | Pattern |
|-------|---------|
| Booting | Fast blink (200 ms) |
| AP ready, no BT | Slow blink (1 s) |
| BT connecting | Medium blink (500 ms) |
| Connected + NAT | Solid on |
| Error | 3 fast blinks, pause, repeat |

## Configuration

### Kconfig (via `idf.py menuconfig`)

| Option | Default | Description |
|--------|---------|-------------|
| `BRIDGE_BT_MODE` | BNEP | Bluetooth transport (BNEP or BLE SLIP) |
| `BRIDGE_WIFI_SSID` | TinyPAN-Bridge | WiFi hotspot name |
| `BRIDGE_WIFI_PASSWORD` | bridge1234 | WiFi password (min 8 chars) |
| `BRIDGE_HTTP_PORT` | 80 | Web UI port |

### Compile-time tuning (in source headers)

| Define | Default | File | Description |
|--------|---------|------|-------------|
| `TINYPAN_MAX_FRAME_SIZE` | 1696 | `components/tinypan/include/tinypan_config.h` | Max BNEP frame size (must be >= 1696 for phone PAN MTU) |
| `TINYPAN_BNEP_KEEPALIVE_MS` | 2000 | `components/tinypan/src/tinypan_bnep_transport.c` | BNEP keepalive interval |
| `TINYPAN_BT_QOS_POLL` | 0x80 (80 ms) | `components/tinypan/ports/esp32_classic/tinypan_hal_esp32.c` | BT QoS poll interval |

### Partition table

| Name | Type | Offset | Size |
|------|------|--------|------|
| nvs | data | 0x9000 | 24 KB |
| phy_init | data | 0xf000 | 4 KB |
| factory | app | 0x10000 | ~2 MB |
| www | SPIFFS | 0x200000 | 2 MB |

## REST API

The web UI drives these endpoints. Useful for scripting:

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/status` | Connection state, IP, uptime, client count |
| GET | `/api/wifi` | Current WiFi settings |
| POST | `/api/wifi` | Update WiFi SSID/password/channel |
| GET | `/api/bt/status` | Bluetooth connection state |
| GET | `/api/bt/scan` | Scan for nearby BT devices |
| POST | `/api/bt/connect` | Connect to a device (JSON: `{"bda": "xx:xx:xx:xx:xx:xx"}`) |
| POST | `/api/bt/disconnect` | Disconnect current device |

## Troubleshooting

<details>
<summary><b>WiFi client can't complete the WPA2 handshake</b></summary>

This is a known BT/WiFi coexistence issue. The firmware already forces HT20 bandwidth and pauses BT keepalive during handshakes. If it persists:
- Move the ESP32 closer to the client device
- Reduce BT background traffic on the phone (close apps)
- Keep the phone screen on (Android throttles BT PAN when screen is off)
</details>

<details>
<summary><b>Phone disconnects immediately after BNEP setup</b></summary>

Bluetooth tethering must be **manually enabled** on the phone before or right after pairing:
- **Android**: Settings → Network & Internet → Hotspot & tethering → Bluetooth tethering → ON
- Without this, Android accepts the BNEP link but disconnects immediately (NAP service not active)
</details>

<details>
<summary><b>High packet loss / latency</b></summary>

Bluetooth and WiFi share the same 2.4 GHz radio on the ESP32, so some contention is expected. Best achieved: ~15% packet loss, ~217 ms average ping. To improve:
- Keep the phone close to the ESP32 (strong BT signal)
- Reduce phone background traffic
- Use channel 1 for WiFi AP (default)
</details>

<details>
<summary><b>ESP32 doesn't reconnect after phone goes out of range</b></summary>

The firmware tries reconnecting with backoff (5s → 15s → 30s, 6 attempts, then repeats). If the phone is out of range for an extended period, the supervisor goes to ERROR state. Use the web UI to manually reconnect, or reboot the ESP32.
</details>

<details>
<summary><b>Build fails after changing sdkconfig.defaults</b></summary>

Run `idf.py fullclean` (or `rm sdkconfig`) before rebuilding:
```bash
idf.py fullclean
idf.py build
```
</details>

<details>
<summary><b>Permission denied on serial port (Linux)</b></summary>

```bash
sudo usermod -aG dialout $USER
# Log out and back in for the change to take effect
```
</details>

## Known Limitations

- **BT/WiFi coexistence**: Both radios share the 2.4 GHz band. Packet loss of 15-60% is expected depending on phone background traffic and RF environment.
- **Android screen-off throttling**: Android throttles Bluetooth PAN when the screen is off. Keep the phone unlocked for best performance.
- **ESP32-S3/C3/C6 not supported**: These are BLE-only chips without Bluetooth Classic / BNEP support.
- **Max 4 WiFi clients**: The default AP configuration supports up to 4 connected stations.

## Contributing

Contributions are welcome! Please open an issue first to discuss what you'd like to change.

### Development setup

1. Fork and clone the repo
2. Install ESP-IDF v5.4+ (see [Build from source](#option-b-build-from-source))
3. Create a branch, make your changes
4. Test on real hardware if possible
5. Open a pull request

## License

MIT — see [LICENSE](LICENSE). The upstream [TinyPAN](https://github.com/Akhil-Chaturvedi/TinyPAN) library is also MIT licensed.
