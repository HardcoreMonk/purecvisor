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
vpc_id = "11111111-1111-1111-1111-111111111111"
subnet_id = "22222222-2222-2222-2222-222222222222"
attachment_id = "33333333-3333-3333-3333-333333333333"
publish_id = "44444444-4444-4444-4444-444444444444"


def envelope(result):
    return {"jsonrpc": "2.0", "result": result, "id": 1}


def accepted(job_id="job-vpc-cli"):
    return envelope({"status": "accepted", "job_id": job_id, "method": "fixture"})


def terminal(status="completed", *, result='{"ok":true}', job_id="job-vpc-cli"):
    return envelope({
        "job_id": job_id,
        "type": "vpc.fixture",
        "target": "fixture",
        "status": status,
        "status_code": 2 if status == "completed" else 3,
        "progress": 100,
        "detail": "fixture terminal",
        "result": result,
    })


def invoke(args, *, socket_path):
    return subprocess.run(
        [cli, f"--socket={socket_path}", "--no-color", *args],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=12,
        check=False,
    )


def run_fake(responses, args):
    """응답열만큼 실제 요청을 받고 production CLI 결과와 JSON 요청을 돌려준다."""
    with tempfile.TemporaryDirectory(prefix="pcv-vpc-cli-") as temp:
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
                    conn.sendall(json.dumps(response).encode())

        thread = threading.Thread(target=serve, daemon=True)
        thread.start()
        proc = invoke(args, socket_path=path)
        stop.set()
        thread.join(timeout=2)
        server.close()
        assert not thread.is_alive(), "fake VPC CLI server did not stop"
        return proc, requests


def assert_ok(proc, label):
    assert proc.returncode == 0, (
        f"{label}: expected exit 0, got {proc.returncode}\n"
        f"stdout:\n{proc.stdout}\nstderr:\n{proc.stderr}"
    )


def assert_usage(args, label):
    with tempfile.TemporaryDirectory(prefix="pcv-vpc-cli-usage-") as temp:
        path = os.path.join(temp, "unused.sock")
        proc = invoke(args, socket_path=path)
    assert proc.returncode == 2, (
        f"{label}: expected exit 2, got {proc.returncode}\n"
        f"stdout:\n{proc.stdout}\nstderr:\n{proc.stderr}"
    )


                                                     
list_result = [{
    "id": vpc_id, "name": "prod", "tenant": "acme", "backend": "linux",
    "egress_mode": "nat",
    "state": "ACTIVE", "revision": 3, "last_error": None,
}]
for fmt in ("table", "json", "plain", "csv"):
    proc, requests = run_fake(
        [envelope(list_result)],
        [f"--format={fmt}", "vpc", "list", "--tenant", "acme"],
    )
    assert_ok(proc, f"list/{fmt}")
    assert [(r["method"], r["params"]) for r in requests] == [
        ("vpc.list", {"tenant": "acme"})
    ]
    if fmt == "json":
        assert json.loads(proc.stdout)["result"][0]["id"] == vpc_id
    elif fmt == "table":
        assert "ID" in proc.stdout and vpc_id in proc.stdout
    elif fmt == "plain":
        assert proc.stdout.startswith(f"{vpc_id}\tprod\tacme\t")
    elif fmt == "csv":
        assert proc.stdout.startswith("ID,NAME,TENANT,BACKEND,EGRESS,STATE,REV,ERROR\n")

read_cases = [
    (["vpc", "backends"], "vpc.backend.list", {}, [{
        "id": "linux", "label": "Linux bridge", "ready": True,
        "current_vpcs": 0, "allocatable_vpcs": None,
    }]),
    (["vpc", "get", vpc_id, "--tenant", "acme"], "vpc.get",
     {"vpc_id": vpc_id, "tenant": "acme"}, {"id": vpc_id, "subnets": []}),
    (["vpc", "status"], "vpc.status", {}, {"vpc_count": 0, "healthy": True}),
    (["vpc", "subnet-list", vpc_id], "vpc.subnet.list",
     {"vpc_id": vpc_id}, []),
    (["vpc", "attachment-list", vpc_id], "vpc.attachment.list",
     {"vpc_id": vpc_id}, []),
    (["vpc", "service-list", vpc_id], "vpc.service.list",
     {"vpc_id": vpc_id}, []),
]
for args, method, params, result in read_cases:
    proc, requests = run_fake([envelope(result)], args)
    assert_ok(proc, method)
    assert [(r["method"], r["params"]) for r in requests] == [(method, params)]


                                                    
mutation_cases = [
    (["vpc", "create", "prod", "--tenant", "acme", "--egress", "nat"],
     "vpc.create", {"name": "prod", "tenant": "acme", "egress_mode": "nat",
                    "backend": "linux"}),
    (["vpc", "create", "edge", "--tenant", "acme", "--egress", "isolated",
      "--backend", "ovn", "--subnet-name", "web", "--cidr", "10.60.20.0/24",
      "--mtu", "9000"],
     "vpc.create", {"name": "edge", "tenant": "acme", "egress_mode": "isolated",
                    "backend": "ovn",
                    "subnet_name": "web", "subnet_cidr": "10.60.20.0/24",
                    "subnet_mtu": 9000}),
    (["vpc", "delete", vpc_id, "--tenant", "acme"],
     "vpc.delete", {"vpc_id": vpc_id, "tenant": "acme"}),
    (["vpc", "egress-set", vpc_id, "--tenant", "acme", "--egress", "isolated",
      "--revision", "3"],
     "vpc.egress.set", {"vpc_id": vpc_id, "tenant": "acme",
                        "egress_mode": "isolated", "expected_revision": 3}),
    (["vpc", "subnet-create", vpc_id, "web", "--tenant", "acme",
      "--cidr", "10.60.10.0/24", "--mtu", "9000", "--revision", "3"],
     "vpc.subnet.create", {"vpc_id": vpc_id, "name": "web", "tenant": "acme",
                           "cidr": "10.60.10.0/24", "mtu": 9000,
                           "expected_revision": 3}),
    (["vpc", "subnet-delete", subnet_id, "--tenant", "acme"],
     "vpc.subnet.delete", {"subnet_id": subnet_id, "tenant": "acme"}),
    (["vpc", "attachment-create", subnet_id, "web-prod", "--tenant", "acme",
      "--ip", "10.60.10.10"],
     "vpc.attachment.create", {"subnet_id": subnet_id, "vm": "web-prod",
                               "tenant": "acme", "ip_address": "10.60.10.10"}),
    (["vpc", "attachment-delete", attachment_id, "--tenant", "acme"],
     "vpc.attachment.delete", {"attachment_id": attachment_id, "tenant": "acme"}),
    (["vpc", "service-publish", attachment_id, "--tenant", "acme",
      "--protocol", "tcp", "--listen-address", "0.0.0.0", "--listen-port", "8443",
      "--target-port", "443", "--allowed-source", "192.0.2.0/24",
      "--allowed-source", "198.51.100.10/32"],
     "vpc.service.publish", {"attachment_id": attachment_id, "tenant": "acme",
                             "protocol": "tcp", "listen_address": "0.0.0.0",
                             "listen_port": 8443, "target_port": 443,
                             "allowed_sources": ["192.0.2.0/24", "198.51.100.10/32"]}),
    (["vpc", "service-unpublish", publish_id, "--tenant", "acme"],
     "vpc.service.unpublish", {"publish_id": publish_id, "tenant": "acme"}),
    (["vpc", "reconcile"], "vpc.reconcile", {}),
]
for args, method, params in mutation_cases:
    proc, requests = run_fake([accepted(), terminal()], args)
    assert_ok(proc, method)
    assert [(r["method"], r["params"]) for r in requests] == [
        (method, params), ("jobs.get", {"job_id": "job-vpc-cli"}),
    ]

                                                            
proc, requests = run_fake(
    [accepted("job-json-terminal"), terminal(job_id="job-json-terminal")],
    ["--format=json", "vpc", "create", "prod", "--tenant", "acme", "--egress", "nat"],
)
assert_ok(proc, "mutation-terminal-json")
terminal_json = json.loads(proc.stdout)
assert terminal_json["result"]["job_id"] == "job-json-terminal"
assert terminal_json["result"]["status"] == "completed"
assert proc.stdout.count("\n") == 1


                                                                             
proc, requests = run_fake(
    [accepted(), terminal("running", result=None), terminal("completed")],
    ["vpc", "create", "prod", "--tenant", "acme", "--egress", "nat"],
)
assert_ok(proc, "pending-then-completed")
assert [r["method"] for r in requests] == ["vpc.create", "jobs.get", "jobs.get"]

proc, requests = run_fake(
    [accepted(), terminal("failed", result='{"error":"nft failed"}')],
    ["vpc", "create", "prod", "--tenant", "acme", "--egress", "nat"],
)
assert proc.returncode == 1, proc
assert [r["method"] for r in requests] == ["vpc.create", "jobs.get"]
assert "failed" in (proc.stdout + proc.stderr).lower()
assert "fixture terminal" in proc.stderr

proc, requests = run_fake(
    [accepted(), terminal("cancelled", result='{"cancelled":true}')],
    ["vpc", "reconcile"],
)
assert proc.returncode == 1, proc
assert [r["method"] for r in requests] == ["vpc.reconcile", "jobs.get"]
assert "cancelled" in (proc.stdout + proc.stderr).lower()

                                                   
proc, requests = run_fake(
    [envelope({"status": "accepted", "method": "vpc.create"})],
    ["vpc", "create", "prod", "--tenant", "acme", "--egress", "nat"],
)
assert proc.returncode == 1, proc
assert [r["method"] for r in requests] == ["vpc.create"]


                                                  
proc, requests = run_fake(
    [accepted("job-no-wait")],
    ["--format=json", "vpc", "create", "prod", "--tenant", "acme",
     "--egress", "nat", "--no-wait"],
)
assert_ok(proc, "no-wait")
assert [r["method"] for r in requests] == ["vpc.create"]
assert json.loads(proc.stdout)["result"]["job_id"] == "job-no-wait"


                                   
usage_cases = [
    (["vpc", "create", "prod", "--egress", "nat"], "create-missing-tenant"),
    (["vpc", "create", "prod", "--tenant", "acme"], "create-missing-egress"),
    (["vpc", "create", "prod", "--tenant", "acme", "--egress", "nat",
      "--subnet-name", "web"], "create-subnet-missing-cidr"),
    (["vpc", "create", "prod", "--tenant", "acme", "--egress", "nat",
      "--cidr", "10.60.10.0/24"], "create-subnet-missing-name"),
    (["vpc", "create", "prod", "--tenant", "acme", "--egress", "nat",
      "--backend", "vxlan"], "create-invalid-backend"),
    (["vpc", "egress-set", vpc_id, "--tenant", "acme", "--egress", "nat",
      "--revision", "zero"], "bad-revision"),
    (["vpc", "egress-set", vpc_id, "--tenant", "acme", "--egress", "nat",
      "--revision", "9223372036854775808"], "overflow-revision"),
    (["vpc", "subnet-create", vpc_id, "web", "--tenant", "acme", "--cidr",
      "10.60.10.0/24", "--revision", "1", "--mtu", "huge"], "bad-mtu"),
    (["vpc", "service-publish", attachment_id, "--tenant", "acme", "--protocol",
      "tcp", "--listen-address", "0.0.0.0", "--listen-port", "8443",
      "--target-port", "443"], "missing-allowed-source"),
    (["vpc", "service-publish", attachment_id, "--tenant", "acme", "--protocol",
      "tcp", "--listen-address", "0.0.0.0", "--listen-port", "0",
      "--target-port", "443", "--allowed-source", "192.0.2.0/24"], "bad-port"),
    (["vpc", "status", "--bogus"], "unknown-option"),
    (["vpc", "status", "--no-wait"], "read-no-wait"),
    (["vpc", "list", "--egress", "nat"], "known-but-irrelevant-option"),
]
for args, label in usage_cases:
    assert_usage(args, label)

                                                               
with tempfile.TemporaryDirectory(prefix="pcv-vpc-cli-help-") as temp:
    help_proc = invoke(["help", "vpc"], socket_path=os.path.join(temp, "unused.sock"))
assert_ok(help_proc, "help-vpc")
for action in (
    "list", "get", "status", "backends", "create", "delete", "egress-set",
    "subnet-list", "subnet-create", "subnet-delete",
    "attachment-list", "attachment-create", "attachment-delete",
    "service-list", "service-publish", "service-unpublish", "reconcile",
):
    assert f"pcvctl vpc {action}" in help_proc.stdout, action

                                                                      
                                                           
proc, requests = run_fake(
    [envelope(True)],
    ["--format=json", "security-group", "rule", "add", "ovn-icmp",
     "--direction", "ingress", "--proto", "icmp"],
)
assert_ok(proc, "security-group-protocol-contract")
assert [(r["method"], r["params"]) for r in requests] == [
    ("security_group.rule.add", {
        "name": "ovn-icmp", "direction": "ingress", "protocol": "icmp",
    })
]

print("PASS: pcvctl Local VPC surface, SG params, terminal Job and exit contract")
PY
