# zsh only. Sourced by the scripts in tools/ and by probes/*/run_qemu.sh.
#
# Finds ESP-IDF v6.0.2 and sets PY (the python3 of the IDF virtual environment) and IDF_PY (PY on
# idf.py, an array). The scripts run idf.py through PY and do not source export.sh: CMake records
# the interpreter path in the build directory and refuses another path until fullclean.
#
# Search order:
#   1. IDF_PATH and IDF_PYTHON_ENV_PATH from the environment, when set.
#   2. An eim install: its activation script activate_idf_v6.0.2.sh in IDF_TOOLS_PATH or in
#      ~/.espressif/tools. It sets the IDF variables and PATH for this script only.
#   3. An install.sh install: IDF_PATH in ~/esp/esp-idf, ~/esp/v6.0.2/esp-idf or
#      ~/.espressif/v6.0.2/esp-idf, the environment in python_env/idf6.0_py*_env under IDF_TOOLS_PATH
#      or ~/.espressif.
# When the Xtensa toolchain is still not on PATH, idf_tools.py export adds the IDF tools to PATH for
# this script only. Any failure stops the script with a message.

IDF_VER=v6.0.2

_idf_fail() {
  print -u2 "idf_env.sh: $1"
  print -u2 "Install ESP-IDF $IDF_VER with eim or install.sh, or set IDF_PATH and IDF_PYTHON_ENV_PATH."
  exit 1
}

# Candidate tool roots. eim sets IDF_TOOLS_PATH to ~/.espressif/tools, install.sh uses ~/.espressif.
_idf_roots=(${IDF_TOOLS_PATH:+$IDF_TOOLS_PATH} ${IDF_TOOLS_PATH:+${IDF_TOOLS_PATH:h}} $HOME/.espressif)
_idf_roots=(${(u)_idf_roots})

# 2. eim. Values set in the environment win over the activation script.
if [[ -z $IDF_PATH || -z $IDF_PYTHON_ENV_PATH ]]; then
  for _r in $_idf_roots; do
    _act=($_r/activate_idf_$IDF_VER.sh(N) $_r/tools/activate_idf_$IDF_VER.sh(N))
    (( $#_act )) || continue
    _keep_path=$IDF_PATH _keep_env=$IDF_PYTHON_ENV_PATH
    source $_act[1] >/dev/null 2>&1 || true
    [[ -n $_keep_path ]] && export IDF_PATH=$_keep_path
    [[ -n $_keep_env ]] && export IDF_PYTHON_ENV_PATH=$_keep_env
    break
  done
fi

# 3. install.sh.
if [[ -z $IDF_PATH ]]; then
  for _d in $HOME/esp/esp-idf $HOME/esp/$IDF_VER/esp-idf $HOME/.espressif/$IDF_VER/esp-idf; do
    [[ -f $_d/tools/idf.py ]] && { export IDF_PATH=$_d; break }
  done
fi
[[ -n $IDF_PATH ]] || _idf_fail "ESP-IDF not found."
[[ -f $IDF_PATH/tools/idf.py ]] || _idf_fail "IDF_PATH=$IDF_PATH has no tools/idf.py."
if [[ -z $IDF_PYTHON_ENV_PATH ]]; then
  _minor=${${IDF_VER#v}%.*}
  for _r in $_idf_roots; do
    _envs=($_r/tools/python/$IDF_VER/venv(N/) $_r/python/$IDF_VER/venv(N/) $_r/python_env/idf${_minor}_py*_env(N/nOn))
    (( $#_envs )) && { export IDF_PYTHON_ENV_PATH=$_envs[1]; break }
  done
fi
[[ -n $IDF_PYTHON_ENV_PATH ]] || _idf_fail "no Python environment of ESP-IDF $IDF_VER found."

PY=$IDF_PYTHON_ENV_PATH/bin/python3
[[ -x $PY ]] || _idf_fail "$PY not found."
# Array, so $IDF_PY expands to two words under zsh.
IDF_PY=($PY $IDF_PATH/tools/idf.py)

_found=$(sed -nE 's/^set\(IDF_VERSION_(MAJOR|MINOR|PATCH) ([0-9]+)\)/\2/p' $IDF_PATH/tools/cmake/version.cmake | paste -sd. -)
[[ v$_found == $IDF_VER ]] || print -u2 "idf_env.sh: warning: IDF_PATH holds ESP-IDF $_found, the project is tested with $IDF_VER."

# The toolchain and the other IDF tools, when nothing above put them on PATH.
if (( ! $+commands[xtensa-esp32s3-elf-gcc] )); then
  for _r in $_idf_roots; do
    [[ -d $_r/tools/xtensa-esp-elf ]] || continue
    _exp=$(IDF_TOOLS_PATH=$_r $PY $IDF_PATH/tools/idf_tools.py export --format key-value 2>/dev/null) || continue
    for _l in ${(f)_exp}; do
      _k=${_l%%=*} _v=${_l#*=}
      case $_k in
        PATH) export PATH=${_v//\$PATH/$PATH} ;;
        IDF_DEACTIVATE_FILE_PATH) rm -f $_v ;;
        [A-Z_]*) export $_k=$_v ;;
      esac
    done
    rehash
    break
  done
  (( $+commands[xtensa-esp32s3-elf-gcc] )) || _idf_fail "the ESP-IDF tools are not installed (no xtensa-esp32s3-elf-gcc)."
fi

# Puts qemu-system-xtensa on PATH for idf.py qemu: the PATH copy, else the newest under a tool root.
idf_need_qemu() {
  (( $+commands[qemu-system-xtensa] )) && return 0
  local r q
  for r in $_idf_roots; do
    q=($r/tools/qemu-xtensa/*/qemu/bin(N/nOn))
    (( $#q )) && { export PATH=$q[1]:$PATH; rehash; return 0 }
  done
  print -u2 "qemu-system-xtensa not found. Install it with: $PY $IDF_PATH/tools/idf_tools.py install qemu-xtensa"
  exit 1
}
