#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir="$(mktemp -d /tmp/moonlight-pin-test.XXXXXX)"
trap 'rm -rf "$test_dir"' EXIT
gcc -Ilibgamestream -c libgamestream/public_key_pin.c -o "$test_dir/public_key_pin.o"
g++ -std=c++17 -Ilibgamestream -Isrc/system \
  -DMOONLIGHT_3DS_PATH=\"$test_dir/data\" \
  tests/test_public_key_pin.cpp src/system/pair_record.cpp \
  "$test_dir/public_key_pin.o" -lcrypto -o "$test_dir/test_public_key_pin"
"$test_dir/test_public_key_pin"
