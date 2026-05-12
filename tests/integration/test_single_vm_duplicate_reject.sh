#!/usr/bin/env bash
set -euo pipefail

SOCK="/var/run/purecvisor/daemon.sock"
VM="se-dupcheck-001"

rpc() {
  local payload="$1"
  printf '%s\n' "$payload" | socat - "UNIX-CONNECT:$SOCK"
}

cleanup() {
  if /usr/local/bin/pcvctl --format=json vm list | jq -e --arg vm "$VM" '.result[]? | select(.name == $vm)' >/dev/null 2>&1; then
    yes y | /usr/local/bin/pcvctl vm delete "$VM" >/dev/null 2>&1 || true
    sleep 2
  fi
}

trap cleanup EXIT

cleanup

first_resp="$(rpc "{\"jsonrpc\":\"2.0\",\"method\":\"vm.create\",\"params\":{\"name\":\"$VM\",\"vcpu\":1,\"memory_mb\":512,\"disk_size_gb\":5,\"network_bridge\":\"virbr0\"},\"id\":7001}")"
printf '%s\n' "$first_resp" | jq -e '.result.accepted == true and .result.name == "'"$VM"'"' >/dev/null
sleep 2

second_resp="$(rpc "{\"jsonrpc\":\"2.0\",\"method\":\"vm.create\",\"params\":{\"name\":\"$VM\",\"vcpu\":1,\"memory_mb\":512,\"disk_size_gb\":5,\"network_bridge\":\"virbr0\"},\"id\":7002}")"

if printf '%s\n' "$second_resp" | jq -e '.error != null' >/dev/null 2>&1; then
  printf 'PASS: duplicate vm.create request was rejected immediately\n'
  exit 0
fi

printf 'FAIL: duplicate vm.create request was accepted instead of rejected\n' >&2
printf 'second response: %s\n' "$second_resp" >&2
exit 1
