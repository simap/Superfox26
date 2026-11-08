#!/bin/zsh
# Build and flash the project with idf.py.
# Usage: tools/flash.sh [PORT]
# PORT defaults to the first /dev/cu.usbmodem* device.
set -e
setopt null_glob

ROOT=${0:A:h:h}

source $ROOT/tools/idf_env.sh

ports=(/dev/cu.usbmodem*)
PORT=${1:-${ports[1]}}
[[ -n $PORT ]] || { print -u2 "no /dev/cu.usbmodem* port found"; exit 1 }

print -u2 "Port $PORT. Hold BOOT during reset to enter the bootloader."
cd $ROOT
$IDF_PY -p $PORT flash
