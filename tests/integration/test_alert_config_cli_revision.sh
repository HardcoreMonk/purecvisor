#!/usr/bin/env bash
                                                                                  
                                                             
                                                                         
                                                             
                                                                    

set -euo pipefail

if [[ ! -x bin/pcvctl ]]; then
    echo "missing bin/pcvctl; run make cli first" >&2
    exit 1
fi

python3 - "$PWD/bin/pcvctl" <<'PY'
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading

cli = sys.argv[1]


def run_fake(responses):
    with tempfile.TemporaryDirectory(prefix="pcv-alert-cli-") as temp:
        path = os.path.join(temp, "daemon.sock")
        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        server.bind(path)
        server.listen(8)
        server.settimeout(0.1)
        requests = []
        stop = threading.Event()

        def serve():
            index = 0
            while not stop.is_set():
                try:
                    conn, _ = server.accept()
                except socket.timeout:
                    continue
                with conn:
                    data = b""
                    while True:
                        chunk = conn.recv(8192)
                        if not chunk:
                            break
                        data += chunk
                    requests.append(json.loads(data.decode()))
                    response = responses[index] if index < len(responses) else {
                        "jsonrpc": "2.0",
                        "error": {"code": -32099, "message": "unexpected retry"},
                        "id": 1,
                    }
                    index += 1
                    if isinstance(response, str):
                        conn.sendall(response.encode())
                    else:
                        conn.sendall(json.dumps(response).encode())

        thread = threading.Thread(target=serve, daemon=True)
        thread.start()
        proc = subprocess.run(
            [
                cli,
                f"--socket={path}",
                "--no-color",
                "alert",
                "set",
                "--cpu_warn",
                "81",
            ],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=10,
            check=False,
        )
        stop.set()
        thread.join(timeout=2)
        server.close()
        return proc, requests


ok_get = {
    "jsonrpc": "2.0",
    "result": {"config_revision": 7, "cpu_warn": 80, "cpu_crit": 95},
    "id": 1,
}
ok_set = {
    "jsonrpc": "2.0",
    "result": {"config_revision": 8, "cpu_warn": 81, "cpu_crit": 95},
    "id": 1,
}

proc, requests = run_fake([ok_get, ok_set])
assert proc.returncode == 0, proc.stderr
assert [request["method"] for request in requests] == [
    "alert.config.get",
    "alert.config.set",
]
assert requests[1]["params"]["expected_revision"] == 7
assert requests[1]["params"]["cpu_warn"] == 81

get_error = {
    "jsonrpc": "2.0",
    "error": {"code": -32000, "message": "read failed"},
    "id": 1,
}
for response in (
    get_error,
    "not-json",
    {"jsonrpc": "2.0", "result": {"cpu_warn": 80}, "id": 1},
):
    proc, requests = run_fake([response])
    assert [request["method"] for request in requests] == ["alert.config.get"]
    assert "update not sent" in proc.stderr

with tempfile.TemporaryDirectory(prefix="pcv-alert-cli-missing-") as temp:
    missing = os.path.join(temp, "missing.sock")
    proc = subprocess.run(
        [
            cli,
            f"--socket={missing}",
            "--no-color",
            "alert",
            "set",
            "--cpu_warn",
            "81",
        ],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=10,
        check=False,
    )
    assert "update not sent" in proc.stderr

conflict = {
    "jsonrpc": "2.0",
    "error": {"code": -32002, "message": "Alert config revision conflict"},
    "id": 1,
}
proc, requests = run_fake([ok_get, conflict])
assert [request["method"] for request in requests] == [
    "alert.config.get",
    "alert.config.set",
]
assert "changed concurrently" in proc.stderr

print("PASS: alert set performs one GET, one revision-guarded SET, and never retries")
PY
