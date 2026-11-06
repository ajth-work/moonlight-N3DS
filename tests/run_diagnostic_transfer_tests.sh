#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d /tmp/moonlight-transfer-tests-XXXXXX)
trap 'rm -rf "$test_dir"' EXIT
g++ -std=c++17 -Wall -Wextra -Isrc tests/test_diagnostic_transfer.cpp src/diagnostic_transfer_client.cpp src/diagnostic_inventory.cpp -lcrypto -o "$test_dir/transfer"
"$test_dir/transfer" "$test_dir"
g++ -std=c++17 -Wall -Wextra -Isrc tests/test_diagnostic_inventory.cpp tests/diagnostic_spool_test_stubs.cpp src/diagnostic_inventory.cpp src/diagnostic_transfer_client.cpp -lcrypto -o "$test_dir/inventory"
"$test_dir/inventory"
