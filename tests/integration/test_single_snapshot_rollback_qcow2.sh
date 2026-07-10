#!/usr/bin/env bash
set -euo pipefail

# Host integration smoke for qcow2 snapshot rollback.
# Requires a running daemon, qemu-guest-agent, and the named validation VM.
# The guest file mutation is the proof: command success alone is not enough.

SOCK="/var/run/purecvisor/daemon.sock"
VM="se-validate-001"
SNAP="itest-snap-001"
STATE_FILE="/tmp/itest-vmlc-state.txt"

rpc() {
  local payload="$1"
  printf '%s\n' "$payload" | socat - "UNIX-CONNECT:$SOCK"
}

qga_exec_status() {
  local cmd="$1"
  local req pid
  req="$(python3 - <<'PY' "$cmd"
import json, sys
cmd = sys.argv[1]
print(json.dumps({
    "execute": "guest-exec",
    "arguments": {
        "path": "/bin/sh",
        "arg": ["-lc", cmd],
        "capture-output": True,
    },
}))
PY
)"
  pid="$(virsh -c qemu:///system qemu-agent-command "$VM" "$req" | python3 -c 'import sys, json; print(json.load(sys.stdin)["return"]["pid"])')"
  sleep 1
  virsh -c qemu:///system qemu-agent-command "$VM" "{\"execute\":\"guest-exec-status\",\"arguments\":{\"pid\":$pid}}"
}

cleanup() {
  /usr/local/bin/pcvctl snapshot delete "$VM" "$SNAP" >/dev/null 2>&1 || true
  sleep 1
}

trap cleanup EXIT

/usr/local/bin/pcvctl vm guest-ping "$VM" >/dev/null

cleanup

create_resp="$(rpc "{\"jsonrpc\":\"2.0\",\"method\":\"vm.snapshot.create\",\"params\":{\"name\":\"$VM\",\"snapshot_name\":\"$SNAP\"},\"id\":7101}")"
printf '%s\n' "$create_resp" | jq -e '.result == true' >/dev/null

qga_exec_status "rm -f $STATE_FILE && sync" >/dev/null
qga_exec_status "printf mutated > $STATE_FILE && sync" >/dev/null

read_before="$(qga_exec_status "cat $STATE_FILE")"
printf '%s\n' "$read_before" | jq -e '.return["out-data"] == "bXV0YXRlZA=="' >/dev/null

rollback_resp="$(/usr/local/bin/pcvctl snapshot rollback "$VM" "$SNAP" 2>&1)"

printf '%s\n' "$rollback_resp" | grep -q 'SNAPSHOT_ROLLBACK SEQUENCE INITIATED SUCCESSFULLY' || {
  printf 'FAIL: rollback command did not report success\n' >&2
  printf 'rollback response: %s\n' "$rollback_resp" >&2
  exit 1
}

sleep 5
/usr/local/bin/pcvctl vm guest-ping "$VM" >/dev/null

read_after="$(qga_exec_status "test -f $STATE_FILE && cat $STATE_FILE || echo __MISSING__")"
printf '%s\n' "$read_after" | jq -e '.return["out-data"] == "X19NSVNTSU5HX18K"' >/dev/null || {
  printf 'FAIL: rollback did not restore guest file state\n' >&2
  printf 'guest state after rollback: %s\n' "$read_after" >&2
  exit 1
}

printf 'PASS: qcow2 snapshot rollback returned success and restored guest state\n'
