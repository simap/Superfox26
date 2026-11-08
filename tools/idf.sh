#!/bin/zsh
# idf.py for a shell that has not sourced the activation script. Runs the IDF
# venv python3 on tools/idf.py, the same interpreter the activation alias
# uses, with idf.py's default build dir and sdkconfig. A different interpreter
# name for the same venv makes CMake refuse the build dir until fullclean.
set -e
source ${0:A:h}/idf_env.sh
cd ${0:A:h:h}
exec $IDF_PY "$@"
