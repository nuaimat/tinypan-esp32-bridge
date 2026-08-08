#!/bin/bash
# TinyPAN Bridge — Build & Flash
# Usage: ./flash.sh [PORT]

set -e

PORT="${1:-}"
if [ -z "$PORT" ]; then
    echo "Usage: ./flash.sh <PORT>"
    echo "  macOS example:  ./flash.sh /dev/cu.usbserial-XXXX"
    echo "  Linux example:  ./flash.sh /dev/ttyUSB0"
    exit 1
fi
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "=== TinyPAN Bridge — Build & Flash ==="
echo "Port: $PORT"

# Source ESP-IDF
if [ -f ~/esp/esp-idf/export.sh ]; then
    . ~/esp/esp-idf/export.sh
else
    echo "ERROR: ESP-IDF not found at ~/esp/esp-idf/"
    echo "Install: mkdir -p ~/esp && cd ~/esp && git clone -b v5.4 https://github.com/espressif/esp-idf.git && cd esp-idf && ./install.sh esp32"
    exit 1
fi

cd "$SCRIPT_DIR"

echo "=== Building..."
idf.py build

echo "=== Flashing to $PORT..."
idf.py -p "$PORT" flash

echo "=== Done! WiFi AP: TinyPAN-Bridge / bridge1234 | Web UI: http://192.168.99.1"
