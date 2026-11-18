#!/bin/zsh
# Build the project and merge its flash images into webinstall/firmware.bin, the one image that
# webinstall/index.html writes to a badge.
# Usage: tools/build_webinstall.sh
set -e

ROOT=${0:A:h:h}

source $ROOT/tools/idf_env.sh

cd $ROOT
$IDF_PY build
cd build
$PY -m esptool --chip esp32s3 merge-bin -o $ROOT/webinstall/firmware.bin @flash_args
