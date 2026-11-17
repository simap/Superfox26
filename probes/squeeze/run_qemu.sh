#!/bin/zsh
# Runs the built probe under QEMU (idf.py qemu) and prints the console until the line
# SQUEEZE PROBE DONE, a crash or the time limit. Build first, from this directory, with $IDF_PY build
# after sourcing ../../tools/idf_env.sh. Usage: ./run_qemu.sh [seconds], default 120.
setopt null_glob
here=${0:A:h}
source $here/../../tools/idf_env.sh
limit=${1:-120}
# The newest installed qemu-xtensa, put on PATH for idf.py.
qemus=($HOME/.espressif/tools/qemu-xtensa/*/qemu/bin(N/))
(( $#qemus )) || { print -u2 "no qemu-xtensa under $HOME/.espressif/tools"; exit 1 }
export PATH=${qemus[-1]}:$PATH
log=$here/build/qemu_console.txt
cd $here
$IDF_PY qemu </dev/null >$log 2>&1 &
pid=$!
for (( t = 0; t < limit; t++ )); do
  sleep 1
  if grep -q "SQUEEZE PROBE DONE\|Guru Meditation\|abort()" $log; then break; fi
  kill -0 $pid 2>/dev/null || break
done
pkill -P $pid 2>/dev/null
kill $pid 2>/dev/null
pkill -f "qemu-system-xtensa.*$here/build" 2>/dev/null
cat $log
