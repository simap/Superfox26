#!/usr/bin/env zsh
# idf.py for a shell that has not sourced the ESP-IDF activation or export script.
# Runs the IDF venv python3 on idf.py (tools/idf_env.sh finds both) from the project root, with
# idf.py's default build directory and sdkconfig. Usage: tools/idf.sh ARGS, for example
# tools/idf.sh build.
set -e
source ${0:A:h}/idf_env.sh
cd ${0:A:h:h}
exec $IDF_PY "$@"
