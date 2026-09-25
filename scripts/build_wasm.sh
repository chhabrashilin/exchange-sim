#!/usr/bin/env bash
# Compiles the real engine to WebAssembly for the browser UI (needs Emscripten on PATH).
#   scripts/build_wasm.sh          ->  ui/engine.js  (single file: the .wasm is embedded)
#
# SINGLE_FILE embeds the module as base64, so ui/index.html works when opened straight from disk and
# on GitHub Pages, with no server or fetch() involved.
set -euo pipefail
cd "$(dirname "$0")/.."
em++ -std=c++20 -O3 -DNDEBUG -I include wasm/engine_wasm.cpp -o ui/engine.js \
  -sMODULARIZE=1 -sEXPORT_NAME=createEngine -sENVIRONMENT=web,node -sSINGLE_FILE=1 \
  -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=64MB -sSTACK_SIZE=1MB -sASSERTIONS=0 \
  -sEXPORTED_RUNTIME_METHODS=cwrap,HEAPU8,HEAPU32,HEAP32 \
  -sEXPORTED_FUNCTIONS=_malloc,_free,_ex_init,_ex_submit,_ex_events,_ex_event_size,_ex_best_bid,_ex_best_ask,_ex_order_count,_ex_check_invariants,_ex_depth,_ex_depth_ptr,_ex_queue,_ex_queue_ptr,_ex_find,_ex_bench
ls -la ui/engine.js
