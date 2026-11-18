#!/usr/bin/env zsh
# Builds the project and flashes the bootloader, the partition table, the app and the
# data partition image with idf.py.
# Usage: tools/flash.sh [PORT]
# PORT defaults to the first USB serial port of the badge: /dev/cu.usbmodem* on macOS,
# /dev/ttyACM* on Linux.
set -e

ROOT=${0:A:h:h}

source $ROOT/tools/idf_env.sh

ports=(/dev/cu.usbmodem*(N) /dev/ttyACM*(N))
PORT=${1:-${ports[1]}}
[[ -n $PORT ]] || { print -u2 "No /dev/cu.usbmodem* or /dev/ttyACM* port found. Give the port: tools/flash.sh PORT"; exit 1 }

print -u2 "Port $PORT. Hold BOOT during reset to enter the bootloader."
cd $ROOT
$IDF_PY -p $PORT flash
