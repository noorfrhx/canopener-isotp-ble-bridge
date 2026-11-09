#!/bin/bash
# Build (and optionally flash) the CAN Opener SE ISO-TP bridge.
# ESP-IDF cannot build from a path that contains spaces, so this copies the
# project to a space-free build directory first.
#   ./build.sh            build only
#   ./build.sh flash      build and flash over USB-C, then open the serial monitor
set -e
SRC="$(cd "$(dirname "$0")" && pwd)"
OUT="${CANOPENER_BUILD_DIR:-$HOME/.cache/canopener-bridge}"
IDF="${IDF_PATH:-$HOME/esp/esp-idf}"
mkdir -p "$OUT"
rsync -a --delete --exclude build --exclude managed_components --exclude dependencies.lock --exclude sdkconfig --exclude dist "$SRC/" "$OUT/src/"
cd "$OUT/src"
# shellcheck disable=SC1091
. "$IDF/export.sh" >/dev/null
# Regenerate sdkconfig every time so edits to sdkconfig.defaults always apply.
rm -f sdkconfig
idf.py -DIDF_TARGET=esp32c5 build
mkdir -p "$SRC/dist"
cp build/canopener_isotp_bridge.bin build/bootloader/bootloader.bin build/partition_table/partition-table.bin "$SRC/dist/"
echo "Firmware copied to $SRC/dist"
if [ "$1" = "flash" ]; then
  idf.py flash monitor
fi
