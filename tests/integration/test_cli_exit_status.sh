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

SUCCESS = {
    "jsonrpc": "2.0",
    "result": {"status": "running"},
    "id": 1,
}
RPC_ERROR = {
    "jsonrpc": "2.0",
    "error": {"code": -32602, "message": "invalid params"},
    "id": 1,
}


def invoke(args, *, socket_path=None, stdin=None):
    command = [cli]
    if socket_path is not None:
        command.append(f"--socket={socket_path}")
    command.extend(["--no-color", *args])
    return subprocess.run(
        command,
        input=stdin,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=10,
        check=False,
    )


def run_fake(responses, args, *, stdin=None):
    """응답 목록만큼 실제 AF_UNIX 요청을 받고 pcvctl 결과와 요청을 반환한다."""
    with tempfile.TemporaryDirectory(prefix="pcv-cli-exit-") as temp:
        path = os.path.join(temp, "daemon.sock")
        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        server.bind(path)
        server.listen(max(1, len(responses)))
        server.settimeout(0.1)
        requests = []
        stop = threading.Event()

        def serve():
            index = 0
            while not stop.is_set() and index < len(responses):
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
                    response = responses[index]
                    index += 1
                    if response is None:
                        continue
                    if isinstance(response, str):
                        wire = response.encode()
                    else:
                        wire = json.dumps(response).encode()
                    conn.sendall(wire)

        thread = threading.Thread(target=serve, daemon=True)
        thread.start()
        proc = invoke(args, socket_path=path, stdin=stdin)
        stop.set()
        thread.join(timeout=2)
        server.close()
        assert not thread.is_alive(), "fake UDS server did not stop"
        return proc, requests


failures = []


def expect(code, proc, label):
    if proc.returncode != code:
        failures.append(
            f"{label}: expected exit {code}, got {proc.returncode}\n"
            f"stdout:\n{proc.stdout}\nstderr:\n{proc.stderr}"
        )


                      
for fmt in ("table", "json", "plain", "csv"):
    proc, requests = run_fake(
        [SUCCESS], [f"--format={fmt}", "vm", "start", "demo"]
    )
    expect(0, proc, f"success/{fmt}")
    assert [request["method"] for request in requests] == ["vm.start"]

                                                          
mode_args = ["br-lan", "isolated", "192.0.2.73/24"]
proc, requests = run_fake([SUCCESS], ["network", "mode", *mode_args])
expect(0, proc, "network-mode-canonical")
assert len(requests) == 1
assert requests[0]["method"] == "network.mode_set"
assert requests[0]["params"] == {
    "name": mode_args[0], "mode": mode_args[1], "cidr": mode_args[2]
}
assert "DEPRECATED" not in proc.stderr

proc, alias_requests = run_fake(
    [SUCCESS],
    ["network", "edit", mode_args[0], "--mode", mode_args[1], "--cidr", mode_args[2]],
)
expect(0, proc, "network-edit-deprecated-alias")
assert len(alias_requests) == 1
assert alias_requests[0]["method"] == "network.mode_set"
assert alias_requests[0]["params"] == requests[0]["params"]
assert "DEPRECATED" in proc.stderr and "network mode" in proc.stderr

                                                             
proc, create_requests = run_fake(
    [SUCCESS],
    ["network", "create", "pcv-jumbo", "--mode", "nat",
     "--cidr", "10.254.253.1/24", "--mtu", "9000"],
)
expect(0, proc, "network-create-mtu")
assert len(create_requests) == 1
assert create_requests[0]["method"] == "network.create"
assert create_requests[0]["params"] == {
    "bridge_name": "pcv-jumbo", "mode": "nat",
    "cidr": "10.254.253.1/24", "mtu": 9000,
}
expect(
    2,
    invoke(["network", "create", "pcv-jumbo", "--mtu", "not-a-number"]),
    "network-create-invalid-mtu",
)

                                                                      
for label, response in (
    ("rpc-error", RPC_ERROR),
    ("method-not-found", {
        "jsonrpc": "2.0",
        "error": {"code": -32601, "message": "method not found"},
        "id": 1,
    }),
    ("malformed-json", "not-json"),
    ("empty-response", None),
    ("invalid-envelope", {"jsonrpc": "2.0", "id": 1}),
    ("invalid-error-shape", {"jsonrpc": "2.0", "error": "broken", "id": 1}),
):
    proc, requests = run_fake([response], ["vm", "start", "demo"])
    expect(1, proc, label)
    assert [request["method"] for request in requests] == ["vm.start"]

with tempfile.TemporaryDirectory(prefix="pcv-cli-missing-") as temp:
    proc = invoke(
        ["vm", "start", "demo"],
        socket_path=os.path.join(temp, "missing.sock"),
    )
    expect(1, proc, "missing-socket")

                                     
proc, _ = run_fake(
    [RPC_ERROR], ["--format=json", "vm", "start", "demo"]
)
expect(1, proc, "rpc-error/json")

                                                                  
proc, _ = run_fake(
    [{"jsonrpc": "2.0", "result": {"message": "literal -32601"}, "id": 1}],
    ["vm", "start", "demo"],
)
expect(0, proc, "method-not-found-string-in-result")

                                        
expect(2, invoke(["does-not-exist", "run"]), "unknown-route")
expect(2, invoke(["vm", "start"]), "known-route-missing-arg")
expect(2, invoke(["--format=bogus", "help"]), "invalid-global-format")
expect(2, invoke(["format", "bogus"]), "invalid-repl-format-command")
expect(2, invoke(["--socket=", "vm", "start", "demo"]), "empty-socket-option")
expect(2, invoke(["--not-a-real-flag"]), "unknown-global-option")

                       
expect(0, invoke(["help"]), "help")
expect(0, invoke(["version"]), "version")

                                    
proc, requests = run_fake(
    [SUCCESS, RPC_ERROR],
    ["--batch"],
    stdin="vm start first\nvm start second\n",
)
expect(1, proc, "batch-runtime-failure")
assert [request["method"] for request in requests] == ["vm.start", "vm.start"]

proc, requests = run_fake(
    [SUCCESS],
    ["--batch"],
    stdin="vm start first\nvm start\n",
)
expect(2, proc, "batch-usage-failure")
assert [request["method"] for request in requests] == ["vm.start"]

expect(
    2,
    invoke(["--batch"], stdin="vm start 'unterminated\n"),
    "batch-tokenization-failure",
)

if failures:
    raise AssertionError("\n\n".join(failures))

print("PASS: pcvctl exit status contract 0/1/2")
PY
