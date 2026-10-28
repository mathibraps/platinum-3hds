#!/bin/sh
# Runs a host test build (.wasm) with wasmtime, giving it access to the
# current directory tree (romfs/ and output folders).
#   tools/run_host.sh <program.wasm> [args...]
WASMTIME=${WASMTIME:-../tools-host/wasmtime-v49.0.3-aarch64-macos/wasmtime}
prog=$1
shift
exec "$WASMTIME" run --dir=. "$prog" "$@"
