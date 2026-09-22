#!/usr/bin/env bash
# Builds and runs the host-side protocol tests. Requires a C++17 compiler.
set -euo pipefail
cd "$(dirname "$0")"
CXX="${CXX:-c++}"
OUT="$(mktemp -d)/protocol_test"
"$CXX" -std=c++17 -Wall -Wextra -O1 -o "$OUT" protocol_test.cpp ../components/goodnature_ble/protocol.cpp
"$OUT"
