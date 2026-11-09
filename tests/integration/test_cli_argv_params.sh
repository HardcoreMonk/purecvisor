#!/usr/bin/env bash
                                                                                        
                                                                                                     
                                                                                      
                                                                            
                                                                     
                                                             
 
                                                                  
                                                        
                                                                
 
                                                                    
                     

set -euo pipefail

if [[ ! -x bin/pcvctl ]]; then
    echo "missing bin/pcvctl; run make cli first" >&2
    exit 1
fi

python3 - "$PWD/bin/pcvctl" <<'PY'
import json
import os
import re
import shlex
import socket
import subprocess
import sys
import tempfile
import threading

cli = sys.argv[1]
                                           
SENTINEL_KEY = "-".join(("fixture", "sentinel", "not", "a", "real", "value"))
SUCCESS = {"jsonrpc": "2.0", "result": {"status": "ok"}, "id": 1}
RPC_ERROR = {"jsonrpc": "2.0", "error": {"code": -32602, "message": "invalid params"}, "id": 1}
failures = []


def run_fake(args, response=SUCCESS, *, env=None, stdin=None, global_flags=()):
    """가짜 UDS에 pcvctl을 붙여 (프로세스 결과, 받은 요청들)을 돌려준다. 응답은 요청마다 같다."""
    with tempfile.TemporaryDirectory(prefix="pcv-cli-argv-") as temp:
        path = os.path.join(temp, "daemon.sock")
        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        server.bind(path)
        server.listen(8)
        server.settimeout(0.05)
        requests = []
        stop = threading.Event()

        def serve():
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
                    conn.sendall(response if isinstance(response, bytes) else json.dumps(response).encode())

        thread = threading.Thread(target=serve, daemon=True)
        thread.start()
        proc = subprocess.run(
            [cli, f"--socket={path}", "--no-color", *global_flags, *args],
            input=stdin, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            timeout=15, check=False, env=env,
        )
        stop.set()
        thread.join(timeout=2)
        server.close()
        return proc, requests


def check(condition, label, proc=None):
    if not condition:
        detail = f"\nrc={proc.returncode}\nstdout:\n{proc.stdout}\nstderr:\n{proc.stderr}" if proc else ""
        failures.append(f"{label}{detail}")


def expect_rpc(label, args, method, params, response=SUCCESS, **kwargs):
    """정확히 한 건의 RPC가 기대한 method·params로 나가고 exit 0인지 본다."""
    proc, requests = run_fake(args, response, **kwargs)
    check(proc.returncode == 0, f"{label}: exit 0이어야 한다", proc)
    if len(requests) != 1:
        check(False, f"{label}: RPC 1건이어야 하는데 {len(requests)}건", proc)
        return proc
    check(requests[0]["method"] == method, f"{label}: method={requests[0]['method']!r}, 기대 {method!r}")
    check(requests[0].get("params", {}) == params, f"{label}: params={requests[0].get('params')!r}, 기대 {params!r}")
    return proc


def expect_usage(label, args, **kwargs):
    """RPC 없이 사용법 오류(exit 2)로 끝나는지 본다."""
    proc, requests = run_fake(args, **kwargs)
    check(proc.returncode == 2, f"{label}: exit 2여야 한다", proc)
    check(not requests, f"{label}: RPC를 보내면 안 되는데 {[r['method'] for r in requests]}", proc)
    return proc


                                                                           
expect_rpc("storage-pool-list", ["storage", "pool", "list"], "storage.pool.list", {},
           {"jsonrpc": "2.0", "result": [], "id": 1})
expect_rpc("zvol-create", ["storage", "zvol", "create", "tank/vm1-disk", "--size", "10G"],
           "storage.zvol.create", {"zvol_path": "tank/vm1-disk", "size": "10G"})
expect_rpc("zvol-delete", ["storage", "zvol", "delete", "tank/vm1-disk"],
           "storage.zvol.delete", {"zvol_path": "tank/vm1-disk"})
expect_rpc("zvol-list", ["storage", "zvol", "list"], "storage.zvol.list", {},
           {"jsonrpc": "2.0", "result": [], "id": 1})
expect_rpc("disk-attach", ["device", "disk", "attach", "vm1", "--source", "S", "--target", "vdb", "--bus", "scsi"],
           "device.disk.attach", {"vm_id": "vm1", "source": "S", "target": "vdb", "bus": "scsi"})
expect_rpc("disk-detach", ["device", "disk", "detach", "vm1", "--target", "vdb"],
           "device.disk.detach", {"vm_id": "vm1", "target": "vdb"})
expect_rpc("pool-forecast-named", ["storage", "pool-forecast", "tank"], "storage.pool.forecast", {"pool": "tank"})
expect_rpc("pool-forecast-default", ["storage", "pool-forecast"], "storage.pool.forecast", {"pool": "pcvpool"})

                                                
expect_usage("storage-pool-no-sub", ["storage", "pool", "bogus"])
for sub in ("create", "health", "scrub", "destroy", "forecast"):
    expect_usage(f"storage-pool-{sub}-absent", ["storage", "pool", sub, "tank"])                               
expect_usage("zvol-create-no-size-value", ["storage", "zvol", "create", "tank/x", "--size"])
expect_usage("zvol-create-wrong-flag", ["storage", "zvol", "create", "tank/x", "--bogus", "10G"])
expect_usage("disk-no-flags", ["device", "disk", "attach", "vm1"])

                                              
SNAP = {"jsonrpc": "2.0", "result": [{"snapshot": "tank/vm1@snap-a", "creation": "Mon"}, {"snapshot": "tank/vm1@snap-b"}], "id": 1}
METRICS = {"jsonrpc": "2.0", "result": {"cpu": 5, "mem": 6}, "id": 1}
for label, args, method, params, response, table_needle, plain_needle in (
    ("snapshot-list", ["snapshot", "list", "vm1"], "vm.snapshot.list", {"vm_id": "vm1"}, SNAP,
     "ZFS TIMELINES: vm1", "tank/vm1@snap-a\tMon\ntank/vm1@snap-b\t-\n"),
    ("monitor-metrics", ["monitor", "metrics", "vm1"], "vm.metrics", {"vm_id": "vm1"}, METRICS,
     "REALTIME TELEMETRY: vm1", "vm1\t5\t6"),
    ("vm-eject", ["vm", "eject", "vm1"], "vm.eject", {"vm_id": "vm1"}, SUCCESS,
     "TARGET VM : vm1", "eject\tvm1\tOK"),
):
    for fmt, needle in (("table", table_needle), ("plain", plain_needle), ("csv", plain_needle)):
        proc = expect_rpc(f"{label}/{fmt}", [f"--format={fmt}", *args], method, params, response)
        check(needle in proc.stdout, f"{label}/{fmt}: {needle!r} 없음", proc)
        check("(null)" not in proc.stdout + proc.stderr, f"{label}/{fmt}: (null) 출력", proc)


                                                                               
proc = expect_rpc("snapshot-list-table-body", ["snapshot", "list", "vm1"], "vm.snapshot.list", {"vm_id": "vm1"}, SNAP)
check("tank/vm1@snap-a\tMon" in proc.stdout, "snapshot-list-table-body: 항목이 없다", proc)
proc = expect_rpc("snapshot-list-object-result", ["--format=plain", "snapshot", "list", "vm1"], "vm.snapshot.list", {"vm_id": "vm1"}, SUCCESS)
check("SNAPSHOT_LIST" in proc.stdout, "snapshot-list-object-result: 배열이 아니면 액션 응답으로 표시", proc)

                                                                                             
for action in ("start", "stop", "pause", "resume"):
    proc = expect_usage(f"vm-{action}-usage", ["vm", action])
    check(f"pcvctl vm {action} <vm_id>" in proc.stdout, f"vm-{action}-usage: 동작 이름이 없다", proc)


                                                                
def providers(**entry):
    return {"providers": [entry]}


def first_provider(request):
    """요청의 첫 프로바이더 항목. 배열 형식이 아니면 빈 dict라 뒤따르는 단정이 실패한다."""
    entries = request.get("params", {}).get("providers")
    return entries[0] if isinstance(entries, list) and entries else {}


expect_rpc("agent-set-key", ["agent", "set", "--provider", "claude", "--api_key", SENTINEL_KEY],
           "agent.config.set", providers(name="Claude", api_key=SENTINEL_KEY))
expect_rpc("agent-set-key-alias", ["agent", "set", "--provider", "OpenAI", "--api-key", SENTINEL_KEY],
           "agent.config.set", providers(name="OpenAI", api_key=SENTINEL_KEY))
expect_rpc("agent-set-all", ["agent", "set", "--provider", "OLLAMA", "--endpoint", "http://localhost:11434",
                             "--model", "m1", "--api_key", "local"],
           "agent.config.set",
           providers(name="Ollama", endpoint="http://localhost:11434", model="m1", api_key="local"))
expect_rpc("agent-set-model-only", ["agent", "set", "--provider", "gemini", "--model", "m2"],
           "agent.config.set", providers(name="Gemini", model="m2"))
expect_rpc("agent-set-disable", ["agent", "set", "--provider", "claude", "--api_key", ""],
           "agent.config.set", providers(name="Claude", api_key=""))

BASE = ["agent", "set", "--provider", "claude", "--api_key", SENTINEL_KEY]
for label, args in (
    ("enabled", [*BASE, "--enabled", "true"]),                                     
    ("weight", [*BASE, "--weight", "0.3"]),                          
    ("enabled-first", ["agent", "set", "--enabled", "false", "--provider", "claude"]),
    ("enabled-eq", [*BASE, "--enabled=true"]),
    ("unknown-flag", [*BASE, "--bogus", "x"]),
    ("positional", [*BASE, "stray"]),
    ("key-eq-form", ["agent", "set", "--provider", "claude", f"--api_key={SENTINEL_KEY}"]),
    ("no-provider", ["agent", "set", "--api_key", SENTINEL_KEY]),
    ("unknown-provider", ["agent", "set", "--provider", "claud", "--api_key", SENTINEL_KEY]),                 
    ("provider-only", ["agent", "set", "--provider", "claude"]),
    ("missing-value", ["agent", "set", "--provider", "claude", "--api_key"]),
    ("masked-key", ["agent", "set", "--provider", "claude", "--api_key", f"fixture...{SENTINEL_KEY}"]),                          
    ("masked-short-key", ["agent", "set", "--provider", "claude", "--api_key", "***"]),                                  
    ("empty-endpoint", ["agent", "set", "--provider", "ollama", "--endpoint", ""]),
    ("empty-model", ["agent", "set", "--provider", "ollama", "--model", ""]),
    ("disable-plus-model", ["agent", "set", "--provider", "claude", "--api_key", "", "--model", "m"]),                      
):
    proc = expect_usage(f"agent-set-reject/{label}", args)
    check(SENTINEL_KEY not in proc.stdout + proc.stderr, f"agent-set-reject/{label}: 키가 출력됐다", proc)

                                                  
proc = expect_usage("agent-set-enabled-reason", [*BASE, "--enabled", "true"])
check("--enabled is not supported" in proc.stderr, "agent-set-enabled-reason: 이유가 없다", proc)
proc = expect_usage("agent-set-weight-reason", [*BASE, "--weight", "0.3"])
check("--weight is not supported" in proc.stderr, "agent-set-weight-reason: 이유가 없다", proc)

                                           
MASKED = {"jsonrpc": "2.0", "result": {"providers": [{"name": "Claude", "api_key": "fix...789"}]}, "id": 1}
for flags, response, code in (
    ((), MASKED, 0),
    (("--format=json",), MASKED, 0),
    (("--format=plain",), MASKED, 0),
    (("--format=csv",), MASKED, 0),
    (("--verbose",), MASKED, 0),
    ((), RPC_ERROR, 1),
    (("--verbose",), RPC_ERROR, 1),
):
    proc, requests = run_fake(BASE, response, global_flags=flags)
    check(proc.returncode == code, f"agent-set-output/{flags}/{code}: exit {code}", proc)
    check(len(requests) == 1 and first_provider(requests[0]).get("api_key") == SENTINEL_KEY,
          f"agent-set-output/{flags}: 원문 키가 전송돼야 한다")
    check(SENTINEL_KEY not in proc.stdout + proc.stderr, f"agent-set-output/{flags}: 키가 출력됐다", proc)

                                                                  
batch_line = f"agent set --provider claude --api_key {SENTINEL_KEY}"
proc, requests = run_fake(["--batch"], MASKED, stdin=f"{batch_line}\nvm start batch-vm\n", global_flags=("--verbose",))
check(proc.returncode == 0, "agent-set-batch-verbose: exit 0", proc)
check([r["method"] for r in requests] == ["agent.config.set", "vm.start"], f"agent-set-batch-verbose: {requests}")
check(SENTINEL_KEY not in proc.stdout + proc.stderr, "agent-set-batch-verbose: 키가 출력됐다", proc)
check("vm start batch-vm" in proc.stderr, "agent-set-batch-verbose: 일반 줄 표시가 사라졌다", proc)

                                                                                         
proc, requests = run_fake(["--batch"], RPC_ERROR, stdin=f"{batch_line}\nvm start batch-vm\n")
check(proc.returncode == 1 and len(requests) == 2, "agent-set-batch-rpc-error: 두 줄 모두 RPC 뒤 exit 1", proc)
check(SENTINEL_KEY not in proc.stdout + proc.stderr, "agent-set-batch-rpc-error: 키가 출력됐다", proc)
check("[ERR line 1] (secret line)" in proc.stderr, "agent-set-batch-rpc-error: 키 줄의 실패 표시가 없다", proc)
check("[ERR line 2] vm start batch-vm" in proc.stderr, "agent-set-batch-rpc-error: 일반 줄의 원문 표시가 사라졌다(반사실)", proc)

with tempfile.TemporaryDirectory(prefix="pcv-cli-argv-") as absent_dir:
    proc = subprocess.run([cli, f"--socket={absent_dir}/absent.sock", "--no-color", "--batch"], input=f"{batch_line}\n",
                          text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15, check=False)
check(proc.returncode != 0, "agent-set-batch-transport: 소켓이 없으면 실패", proc)
check("[ERR line 1] (secret line)" in proc.stderr, "agent-set-batch-transport: 실패 표시가 없다", proc)
check(SENTINEL_KEY not in proc.stdout + proc.stderr, "agent-set-batch-transport: 키가 출력됐다", proc)

                                                             
for label, line in (
    ("enabled", f"{batch_line} --enabled true"),
    ("unknown-flag", f"{batch_line} --bogus x"),
    ("eq-form", f"agent set --provider claude --api_key={SENTINEL_KEY}"),
    ("alias", f"agent set --provider claude --api-key {SENTINEL_KEY} --weight 1"),
    ("unbalanced-quote", f"agent set --provider claude --api_key '{SENTINEL_KEY}"),
    ("undefined-var", f"agent set --provider claude --api_key {SENTINEL_KEY}$PCV_UNDEFINED_VAR_FOR_TEST"),
    ("command-substitution", f"agent set --provider claude --api_key $(echo {SENTINEL_KEY})"),
):
    proc, requests = run_fake(["--batch"], stdin=f"{line}\n")
    check(proc.returncode == 2 and not requests, f"agent-set-batch-usage/{label}: RPC 없이 exit 2", proc)
    check("[ERR line 1] (secret line)" in proc.stderr, f"agent-set-batch-usage/{label}: 실패 표시가 없다", proc)
    check(SENTINEL_KEY not in proc.stdout + proc.stderr, f"agent-set-batch-usage/{label}: 키가 출력됐다", proc)

                                                                      
SENTINEL_PASSWORD = "-".join(("fixture", "sentinel", "user", "credential"))
auth_line = f"auth create ops-user {SENTINEL_PASSWORD} operator"
AUTH_PARAMS = {"username": "ops-user", "password": SENTINEL_PASSWORD, "role": "operator"}

proc, requests = run_fake(["--batch"], RPC_ERROR, stdin=f"{auth_line}\nauth list\nvm start batch-vm\nvm start auth-create-vm\nauth role ops-user viewer\n")
check(len(requests) == 5 and requests[0].get("params") == AUTH_PARAMS, f"auth-batch-fail: 원문 비밀번호가 전송돼야 한다: {requests}", proc)
check(SENTINEL_PASSWORD not in proc.stdout + proc.stderr, "auth-batch-fail: 비밀번호가 출력됐다", proc)
check("[ERR line 1] (secret line)" in proc.stderr, "auth-batch-fail: 실패 표시가 없다", proc)
for lineno, kept in ((2, "auth list"), (3, "vm start batch-vm"), (4, "vm start auth-create-vm"), (5, "auth role ops-user viewer")):
    check(f"[ERR line {lineno}] {kept}" in proc.stderr, f"auth-batch-fail: 비밀이 없는 줄이 가려졌다(반사실): {kept}", proc)

proc, requests = run_fake(["--batch"], stdin=f"{auth_line}\nauth list\n")
check(proc.returncode == 0 and len(requests) == 2, "auth-batch-ok: exit 0·RPC 2건", proc)
check(SENTINEL_PASSWORD not in proc.stdout + proc.stderr, "auth-batch-ok: 비밀번호가 출력됐다", proc)

proc, requests = run_fake(["--batch"], stdin=f"{auth_line}\nauth list\nvm start batch-vm\n", global_flags=("--verbose",))
check(proc.returncode == 0 and len(requests) == 3, "auth-batch-verbose: exit 0·RPC 3건", proc)
check(requests and requests[0].get("params") == AUTH_PARAMS, f"auth-batch-verbose: 원문 비밀번호가 전송돼야 한다: {requests}", proc)
check(SENTINEL_PASSWORD not in proc.stdout + proc.stderr, "auth-batch-verbose: 비밀번호가 출력됐다", proc)
check("[batch:1]" in proc.stderr and "(secret line)" in proc.stderr, "auth-batch-verbose: 되풀이 표시가 없다", proc)
check("auth list" in proc.stderr and "vm start batch-vm" in proc.stderr, "auth-batch-verbose: 비밀이 없는 줄이 가려졌다(반사실)", proc)

with tempfile.TemporaryDirectory(prefix="pcv-cli-argv-") as absent_dir:
    proc = subprocess.run([cli, f"--socket={absent_dir}/absent.sock", "--no-color", "--batch"], input=f"{auth_line}\n",
                          text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15, check=False)
check(proc.returncode != 0 and "[ERR line 1] (secret line)" in proc.stderr, "auth-batch-transport: 실패 표시가 없다", proc)
check(SENTINEL_PASSWORD not in proc.stdout + proc.stderr, "auth-batch-transport: 비밀번호가 출력됐다", proc)

                                                                    
for label, line, code in (
    ("typo-action", f"auth creat ops-user {SENTINEL_PASSWORD} operator", 2),
    ("no-role", f"auth create ops-user {SENTINEL_PASSWORD}", 2),
    ("quoted-words", f"\"auth\"   'create' ops-user {SENTINEL_PASSWORD} operator", 0),
    ("tabs", f"auth\tcreate\tops-user\t{SENTINEL_PASSWORD}\toperator", 0),
    ("unbalanced-quote", f"auth create ops-user '{SENTINEL_PASSWORD} operator", 2),
    ("undefined-var", f"auth create ops-user {SENTINEL_PASSWORD}$PCV_UNDEFINED_VAR_FOR_TEST operator", 2),
    ("global-flag-first", f"--format=json auth create ops-user {SENTINEL_PASSWORD} operator", 2),
    ("password-option", f"auth create ops-user --password {SENTINEL_PASSWORD} operator", None),
    ("socket-value-first", f"--socket /tmp/x auth create ops-user {SENTINEL_PASSWORD} operator", 2),
    ("format-value-first", f"--format json auth create ops-user {SENTINEL_PASSWORD} operator", 2),
    ("flag-then-values", f"--no-color --format json --socket /tmp/x auth create ops-user {SENTINEL_PASSWORD} operator", 2),
    ("key-option", f"auth bogus --key {SENTINEL_PASSWORD}", 2),
    ("key-eq-option", f"--key={SENTINEL_PASSWORD}", 2),
    ("key-typo", f"agent set --provider claude --key {SENTINEL_PASSWORD}", 2),
):
    proc, _ = run_fake(["--batch"], stdin=f"{line}\n")
    if code is not None:
        check(proc.returncode == code, f"auth-batch-input/{label}: exit {code}", proc)
    check(SENTINEL_PASSWORD not in proc.stdout + proc.stderr, f"auth-batch-input/{label}: 비밀번호가 출력됐다", proc)

                                                                        
TRICKY = 'head"' + SENTINEL_PASSWORD
proc, requests = run_fake(["auth", "create", "ops-user", TRICKY, "operator"], global_flags=("--verbose",))
check(requests and requests[0]["params"]["password"] == TRICKY, f"auth-verbose-quote: 원문 전송: {requests}", proc)
check(SENTINEL_PASSWORD not in proc.stdout + proc.stderr, "auth-verbose-quote: 따옴표 뒤가 출력됐다", proc)
check('"username":"ops-user"' in proc.stderr, "auth-verbose-quote: 비밀이 아닌 값은 그대로 보여야 한다(반사실)", proc)

                                        
for label, args in (
    ("usage", ["auth", "creat", "ops-user", SENTINEL_PASSWORD, "operator"]),
    ("flag", ["--password", SENTINEL_PASSWORD]),
    ("eq", [f"--password={SENTINEL_PASSWORD}"]),
):
    for flags in ((), ("--verbose",)):
        proc, requests = run_fake(args, global_flags=flags)
        check(not requests, f"auth-router/{label}/{flags}: RPC를 보내면 안 된다", proc)
        check(SENTINEL_PASSWORD not in proc.stdout + proc.stderr, f"auth-router/{label}/{flags}: 비밀번호가 출력됐다", proc)
                                                                                       
for label, args in (("typo", ["auth", "lst"]), ("create-typo", ["auth", "creat", "ops-user", SENTINEL_PASSWORD, "operator"])):
    proc = expect_usage(f"auth-router-unknown-action/{label}", args)
    check("UNKNOWN COMMAND: auth (hidden)" in proc.stderr, f"auth-router-unknown-action/{label}: object가 안내에 없다", proc)
    check("pcvctl auth list" in proc.stdout and "빠른 시작" not in proc.stdout, f"auth-router-unknown-action/{label}: auth 도움말이 아니다", proc)
    check(args[1] not in proc.stderr and SENTINEL_PASSWORD not in proc.stdout + proc.stderr,                                          
          f"auth-router-unknown-action/{label}: 입력 토큰이 출력됐다", proc)
proc = expect_usage("agent-router-key-option", ["agent", f"--api_key={SENTINEL_KEY}"])
check("UNKNOWN COMMAND: agent (hidden)" in proc.stderr and SENTINEL_KEY not in proc.stdout + proc.stderr, "agent-router-key-option: object 안내·키 가림", proc)
proc = expect_usage("router-key-first", [SENTINEL_KEY, "--api_key"])
check("UNKNOWN COMMAND: (secret argument)" in proc.stderr and SENTINEL_KEY not in proc.stdout + proc.stderr, "router-key-first: 명령표에 없는 첫 낱말은 찍지 않는다", proc)
proc, _ = run_fake(["help", "auth"])
check("auth create" in proc.stdout, "auth-help-kept: help auth는 그 그룹 도움말이어야 한다(반사실)", proc)

                                                      
SECRET_VALUE = "-".join(("fixture", "sentinel", "env", "value"))
SECRET_URL = "https://hooks.example.invalid/" + "-".join(("fixture", "sentinel", "hook", "path"))
                                                                                          
ALERT_GET = {"jsonrpc": "2.0", "result": {"config_revision": 3, "cpu_warn": 80, "webhook_url": SECRET_URL, "webhook_crit_url": SECRET_URL}, "id": 1}
for label, line, marker, rpc in (
    ("env-set", f"container env-set c1 DB_PW {SECRET_VALUE}", SECRET_VALUE, SUCCESS),
    ("webhook", f"alert set --webhook {SECRET_URL}", SECRET_URL, ALERT_GET),
):
    for flags in ((), ("--verbose",)):
        proc, requests = run_fake(["--batch"], RPC_ERROR if not flags else rpc, stdin=f"{line}\n", global_flags=flags)
        check(requests, f"secret-input/{label}/{flags}: RPC를 보내야 한다", proc)
        check(marker not in proc.stdout + proc.stderr, f"secret-input/{label}/{flags}: 비밀 값이 출력됐다", proc)
for label, line in (("env-set-typo", f"container env_set c1 DB_PW {SECRET_VALUE}"), ("env-word", f"container envset c1 DB_PW {SECRET_VALUE}")):
    proc, _ = run_fake(["--batch"], RPC_ERROR, stdin=f"{line}\n")
    check(SECRET_VALUE not in proc.stdout + proc.stderr, f"secret-input/{label}: 비밀 값이 출력됐다", proc)
kept_lines = ("container env-list c1", "container env-delete c1 DB_PW", "alert set --cpu_warn 80", "webhook list",
              "--socket /tmp/x auth list", "--format json vm start batch-vm", "--format json bogus-word",
              "audit search --user auth --limit 5", "vm start auth create", "vm start key-vm", "backup set vm1 --keep 3",
              "backup hostkey list", "vm start --path=/x/key.img")
proc, requests = run_fake(["--batch"], RPC_ERROR, stdin="".join(f"{line}\n" for line in kept_lines))
for lineno, kept in enumerate(kept_lines, 1):
    check(f"[ERR line {lineno}] {kept}" in proc.stderr, f"secret-input-kept: 비밀이 없는 줄이 가려졌다(반사실): {kept}", proc)

                                                                               
proc, requests = run_fake(["alert", "set", "--webhook", SECRET_URL], ALERT_GET, global_flags=("--verbose",))
check(proc.returncode == 0 and [r["method"] for r in requests] == ["alert.config.get", "alert.config.set"], f"verbose-response/alert: {requests}", proc)
check(requests and requests[1]["params"].get("webhook_url") == SECRET_URL, "verbose-response/alert: 원문 URL이 전송돼야 한다", proc)
check(SECRET_URL not in proc.stdout + proc.stderr, "verbose-response/alert: URL 원문이 출력됐다", proc)
check('"config_revision":3' in proc.stderr and '"cpu_warn":80' in proc.stderr, "verbose-response/alert: 비밀이 아닌 값은 보여야 한다(반사실)", proc)
check('"webhook_url":"****"' in proc.stderr, "verbose-response/alert: 가린 표시가 없다", proc)
KEPT_RESPONSE = {"jsonrpc": "2.0", "result": {"total_tokens": 7, "value": "kept-value", "access_token": "kept-off-screen", "api_key": ""}, "id": 1}
proc, _ = run_fake(["vm", "start", "v1"], KEPT_RESPONSE, global_flags=("--verbose",))
check('"total_tokens":7' in proc.stderr and '"value":"kept-value"' in proc.stderr and '"api_key":""' in proc.stderr, "verbose-response/kept: 숫자·value·빈 값은 그대로다(반사실)", proc)
check("kept-off-screen" not in proc.stderr, "verbose-response/kept: 응답의 토큰이 출력됐다", proc)
proc, _ = run_fake(["vm", "start", "v1"], b'not json {"webhook_url":"' + SECRET_URL.encode(), global_flags=("--verbose",))
check(SECRET_URL not in proc.stderr and "[\u2190 (unprintable response)]" in proc.stderr, "verbose-response/raw: 해석이 안 되는 응답은 찍지 않는다", proc)
                                                    
proc, _ = run_fake(["container", "env-set", "c1", "DB_PW", SECRET_VALUE], global_flags=("--verbose",))
check(SECRET_VALUE not in proc.stdout + proc.stderr and '"value":"****"' in proc.stderr, "verbose-request/value: 요청의 value가 출력됐다", proc)
                                                                                 
proc, _ = run_fake(["alert", "set", "--webhook", SECRET_URL], ALERT_GET, global_flags=("--format=json",))
check(SECRET_URL in proc.stdout and SECRET_URL not in proc.stderr, "format-json-exception: 문서의 예외가 코드와 다르다", proc)

                                                                                           
for label, tokens in (
    ("flag-first", ["--api_key", SENTINEL_KEY]),
    ("eq-alone", [f"--api_key={SENTINEL_KEY}"]),
    ("alias-eq-alone", [f"--api-key={SENTINEL_KEY}"]),
    ("key-first", [SENTINEL_KEY, "--api_key"]),
    ("object-then-eq", ["agent", f"--api_key={SENTINEL_KEY}"]),
    ("help", ["help", f"--api_key={SENTINEL_KEY}"]),
    ("format", ["format", f"--api_key={SENTINEL_KEY}"]),
):
    for mode, (args, stdin) in (("one-shot", (tokens, None)), ("batch", (["--batch"], " ".join(tokens) + "\n"))):
        proc, requests = run_fake(args, stdin=stdin)
        check(not requests, f"agent-key-router/{label}/{mode}: RPC를 보내면 안 된다", proc)
        check(SENTINEL_KEY not in proc.stdout + proc.stderr, f"agent-key-router/{label}/{mode}: 키가 출력됐다", proc)
proc = expect_usage("router-echo-kept", ["vm", "bogus"])
check("UNKNOWN COMMAND: vm bogus" in proc.stderr, "router-echo-kept: 키가 없는 입력의 안내가 사라졌다(반사실)", proc)
proc = expect_usage("router-format-kept", ["format", "bogus"])
check("Invalid format: bogus" in proc.stderr, "router-format-kept: 키가 없는 입력의 안내가 사라졌다(반사실)", proc)

                                               
version = subprocess.run([cli, "--version"], text=True, capture_output=True, check=True).stdout
if "readline: enabled" in version:
    with tempfile.TemporaryDirectory(prefix="pcv-cli-home-") as home:
        env = {**os.environ, "HOME": home}
        env.pop("TERM", None)
        proc, requests = run_fake([], MASKED, env=env, global_flags=("--interactive",),
                                  stdin=f"{batch_line}\n{auth_line}\ncontainer env-set c1 DB_PW {SECRET_VALUE}\n"
                                        f"alert set --webhook {SECRET_URL}\nauth list\nvm start history-vm\nexit\n")
        check(len(requests) >= 5, f"secret-history: RPC가 빠졌다: {requests}", proc)
        history = open(os.path.join(home, ".pcvctl_history"), encoding="utf-8").read()
        for marker in (SENTINEL_KEY, SENTINEL_PASSWORD, SECRET_VALUE, SECRET_URL):
            check(marker not in history, f"secret-history: 비밀 값이 이력에 남았다: {marker[:12]}")                                              
        check("vm start history-vm" in history, "secret-history: 일반 줄이 이력에 없다")
        check("auth list" in history, "secret-history: 비밀이 없는 auth 줄이 이력에 없다(반사실)")


                                                            
expect_rpc("container-create", ["container", "create", "app-ctr", "--image", "ubuntu:24.04"],
           "container.create", {"name": "app-ctr", "image": "ubuntu:24.04"})
expect_rpc("container-create-inner-dash", ["container", "create", "a-b-"], "container.create", {"name": "a-b-"})
expect_usage("container-create-old-doc", ["container", "create", "--name", "app-ctr", "--dist", "ubuntu", "--release", "noble"])
expect_usage("container-create-dash", ["container", "create", "-x"])
expect_usage("container-create-help", ["container", "create", "--help"])
                                                 
expect_rpc("container-destroy-dash-name", ["container", "destroy", "--name"], "container.destroy", {"name": "--name"})


                                                    
FAMILIES = ("storage ", "device ", "container create", "agent set")
PLACEHOLDERS = {"<p>": "claude", "<k>": "fixture-key", "<name>": "ctr1", "<img>": "ubuntu:24.04"}
DOC_METHODS = {                                                     
    "storage pool list": ("storage.pool.list", {}),
    "storage pool-forecast pcvpool": ("storage.pool.forecast", {"pool": "pcvpool"}),
    "storage zvol list": ("storage.zvol.list", {}),
    "storage zvol create pcvpool/vms/new-disk --size 50G":
        ("storage.zvol.create", {"zvol_path": "pcvpool/vms/new-disk", "size": "50G"}),
    "storage zvol delete pcvpool/vms/old-disk": ("storage.zvol.delete", {"zvol_path": "pcvpool/vms/old-disk"}),
    "storage health": ("storage.pool.health", {}),
    "storage health pcvpool": ("storage.pool.health", {"pool": "pcvpool"}),
    "device disk attach web-prod --source /dev/zvol/pcvpool/vms/new-disk --target vdb":
        ("device.disk.attach", {"vm_id": "web-prod", "source": "/dev/zvol/pcvpool/vms/new-disk", "target": "vdb"}),
    "device disk detach web-prod --target vdb": ("device.disk.detach", {"vm_id": "web-prod", "target": "vdb"}),
}
LIST_RESULT = {"jsonrpc": "2.0", "result": [], "id": 1}


def doc_commands(path):
    """코드 블록의 `pcvctl ...` 줄과 명령표의 `...` 칸에서 대상 명령을 뽑는다(주석·인라인 설명은 제외)."""
    found = set()
    in_fence = False
    for line in open(path, encoding="utf-8"):
        stripped = line.strip()
        if stripped.startswith("```"):
            in_fence = not in_fence
            continue
        if in_fence and stripped.startswith("pcvctl "):
            command = stripped[len("pcvctl "):]
        else:
            row = re.match(r"^\| `([^`]+)` \|", stripped)
            command = row.group(1) if row else None
        if command and command.startswith(FAMILIES):
            for token, value in PLACEHOLDERS.items():
                command = command.replace(token, value)
            found.add(command)
    return found


guide = doc_commands("docs/GUIDE.md")
in_app = doc_commands("ui/guide-content.md")
check(guide == in_app, f"두 가이드의 명령이 다르다: {sorted(guide ^ in_app)}")
for family, minimum in (("storage ", 6), ("device ", 2), ("container create", 2), ("agent set", 4)):
    count = sum(1 for command in guide if command.startswith(family))
    check(count >= minimum, f"문서 추출이 줄었다({family!r}: {count} < {minimum}) — 정규식이 낡았는지 본다")

for command in sorted(guide | in_app):
    args = shlex.split(command)
    if command.startswith(("storage ", "device ")):
        if command not in DOC_METHODS:
            check(False, f"문서 명령 {command!r}이 시험 정본에 없다")
            continue
        method, params = DOC_METHODS[command]
        expect_rpc(f"doc/{command}", args, method, params, LIST_RESULT)
    elif command.startswith("container create"):
        proc, requests = run_fake(args)
        check(proc.returncode == 0 and len(requests) == 1, f"doc/{command}: exit 0·RPC 1건", proc)
        if requests:
            sent = requests[0]["params"]
            check(sent.get("name") == args[2] and not args[2].startswith("-"), f"doc/{command}: name={sent}")
    else:
        proc, requests = run_fake(args)
        check(proc.returncode == 0 and len(requests) == 1, f"doc/{command}: exit 0·RPC 1건", proc)
        if requests:
            entry = first_provider(requests[0])
            check(str(entry.get("name")).lower() == args[args.index("--provider") + 1],
                  f"doc/{command}: provider={entry}")

if failures:
    raise AssertionError("\n\n".join(failures))

print("PASS: pcvctl argv -> RPC params contract")
PY
