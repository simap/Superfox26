# Sourced by the tools scripts. Sets PY (IDF venv python3) and IDF_PY
# (that python3 on idf.py). Activates ESP-IDF v6.0.2 if the shell has not.
IDF_VER=v6.0.2
if [[ -z $IDF_PYTHON_ENV_PATH || -z $IDF_PATH ]]; then
  source $HOME/.espressif/tools/activate_idf_$IDF_VER.sh >/dev/null 2>&1 || true
fi
export IDF_PATH=${IDF_PATH:-$HOME/.espressif/$IDF_VER/esp-idf}
export IDF_PYTHON_ENV_PATH=${IDF_PYTHON_ENV_PATH:-$HOME/.espressif/tools/python/$IDF_VER/venv}
PY=$IDF_PYTHON_ENV_PATH/bin/python3
# Array, so $IDF_PY expands to two words under zsh.
IDF_PY=($PY $IDF_PATH/tools/idf.py)
[[ -x $PY ]] || { print -u2 "$PY not found. Install ESP-IDF $IDF_VER with eim."; exit 1 }
