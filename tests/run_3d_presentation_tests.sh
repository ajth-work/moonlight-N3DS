#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir="$(mktemp -d /tmp/moonlight-3d-presentation-test.XXXXXX)"
trap 'rm -rf "$test_dir"' EXIT
mkdir -p "$test_dir/ctru"
cat >"$test_dir/ctru/3ds.h" <<'EOF'
#pragma once
struct RecursiveLock {};
EOF
cat >"$test_dir/threadlock_stub.cpp" <<'EOF'
#include "ThreadLock.hpp"
ThreadLock::ThreadLock() {}
ThreadLock::~ThreadLock() {}
void ThreadLock::lock() {}
void ThreadLock::unlock() {}
EOF
g++ -std=c++17 -Wall -Wextra -I"$test_dir/ctru" -Isrc/system -Isrc \
  tests/test_3d_presentation.cpp \
  src/video/renderer/N3ds3DPresentation.cpp \
  "$test_dir/threadlock_stub.cpp" -o "$test_dir/test_3d_presentation"
"$test_dir/test_3d_presentation"
