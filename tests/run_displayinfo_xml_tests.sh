#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir="$(mktemp -d /tmp/moonlight-displayinfo-test.XXXXXX)"
trap 'rm -rf "$test_dir"' EXIT
gcc -std=c99 -Wall -Wextra -Ilibgamestream \
  tests/test_displayinfo_xml.c libgamestream/xml.c \
  -lexpat -o "$test_dir/test_displayinfo_xml"
"$test_dir/test_displayinfo_xml"
