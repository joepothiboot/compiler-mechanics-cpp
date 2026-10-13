#!/bin/sh
# Compile every compiler/*/NN_*.cpp to web/wasm/NN_*.js (wasm embedded). Needs emsdk: https://emscripten.org
set -e
cd "$(dirname "$0")/.."
mkdir -p web/wasm
for f in compiler/*/[0-9][0-9]_*.cpp; do
  n=$(basename "$f" .cpp)
  emcc -std=c++2b -O1 -I. "$f" -o "web/wasm/$n.js" \
    -sSINGLE_FILE -sMODULARIZE -sEXPORT_NAME=createMod -sEXIT_RUNTIME=0 -sINVOKE_RUN=1
  echo "built $n"
done
