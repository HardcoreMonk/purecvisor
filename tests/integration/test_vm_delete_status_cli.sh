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


def run(result, *args):
    with tempfile.TemporaryDirectory(prefix="pcv-delstatus-cli-") as temp:
        path = os.path.join(temp, "daemon.sock")
        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        server.bind(path)
        server.listen(1)
        requests = []

        def serve():
            conn, _ = server.accept()
            with conn:
                data = b""
                while True:
                    chunk = conn.recv(8192)
                    if not chunk:
                        break
                    data += chunk
                requests.append(json.loads(data.decode()))
                conn.sendall(json.dumps({"jsonrpc": "2.0", "result": result, "id": 1}).encode())

        thread = threading.Thread(target=serve, daemon=True)
        thread.start()
        proc = subprocess.run([cli, f"--socket={path}", "--no-color", *args, "vm", "delete-status", "web"],
                              text=True, capture_output=True, timeout=10, check=False)
        thread.join(5)
        server.close()
        assert requests and requests[0]["method"] == "vm.delete.status", requests
        assert requests[0]["params"] == {"name": "web"}, requests
        return proc


warn = "NVRAM cleanup failed: /var/lib/libvirt/qemu/nvram/web_VARS.fd (Is a directory); manual cleanup is required"
completed = {"name": "web", "status": "completed", "job_id": "job-0000beef", "outcome_known": True, "warnings": [warn]}
failed = {"name": "web", "status": "failed", "job_id": "job-0000cafe", "outcome_known": True,
          "error": "ZFS destroy failed (VM definition rolled back): dataset is busy"}

table = run(completed)
assert table.returncode == 0, table
assert "STATUS : completed" in table.stdout and f"WARN   : {warn}" in table.stdout, table.stdout
plain = run(failed, "--format=plain")
assert plain.stdout.splitlines() == ["web\tfailed", "web\terror\tZFS destroy failed (VM definition rolled back): dataset is busy"], plain.stdout
raw = run(completed, "--format=json")
assert json.loads(raw.stdout)["result"] == completed, raw.stdout                 
legacy = run("not_found", "--format=plain")
assert legacy.stdout.strip() == "web\tnot_found", legacy.stdout                        
print("[PASS] vm delete-status CLI: warnings/error lines, json passthrough, legacy string")
PY
