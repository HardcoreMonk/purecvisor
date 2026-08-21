#!/usr/bin/env bash
                                                                             
                                                  
                                                                
                                                    

set -euo pipefail

if [[ ! -x bin/pcvctl ]]; then
    echo "missing bin/pcvctl; run make cli first" >&2
    exit 1
fi

python3 - "$PWD/bin/pcvctl" "$PWD/src/cli/purecvisorctl.c" <<'PY'
import collections
import pathlib
import re
import subprocess
import sys

cli = sys.argv[1]
source = pathlib.Path(sys.argv[2]).read_text(encoding="utf-8")
route_block = source.split("static CommandRoute routes[]", 1)[1].split(
    "{NULL,NULL,NULL,NULL}", 1
)[0]
routes = re.findall(r'\{"([^"]+)",\s*"([^"]+)"', route_block)
groups = collections.OrderedDict()
for obj, action in routes:
    groups.setdefault(obj, []).append(action)


def invoke(*args, input_text=None):
    return subprocess.run(
        [cli, "--no-color", *args],
        input=input_text,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=10,
        check=False,
    )


root = invoke("help")
assert root.returncode == 0, root.stderr
assert "\x1b[" not in root.stdout
assert f"명령 그룹 ({len(groups)}개)" in root.stdout
assert "pcvctl help <그룹>" in root.stdout
assert "Create a new VM" not in root.stdout, "기본 메뉴가 전체 상세를 다시 펼쳤다"
assert len(root.stdout.splitlines()) < 80, "기본 메뉴가 한 화면 탐색 수준을 벗어났다"
for obj, actions in groups.items():
    pattern = rf"^  {re.escape(obj)}\s+{len(actions)}\s+"
    assert re.search(pattern, root.stdout, re.MULTILINE), (obj, len(actions))

vpc = invoke("help", "vpc")
assert vpc.returncode == 0, vpc.stderr
assert f"vpc 명령 ({len(groups['vpc'])}개)" in vpc.stdout
assert "NEURAL LINK ESTABLISHED" not in vpc.stdout, "그룹 상세에는 대형 배너를 반복하지 않는다"
rows = [line for line in vpc.stdout.splitlines() if line.startswith("  pcvctl vpc ")]
assert len(rows) == len(groups["vpc"]), rows
assert len({row.index("│") for row in rows}) == 1, "긴 action 때문에 설명 열 정렬이 깨졌다"
for action in groups["vpc"]:
    assert any(f"pcvctl vpc {action}" in row for row in rows), action

                                        
for args in (("vpc",), ("vpc", "--help"), ("vpc", "-h"), ("vpc", "help")):
    proc = invoke(*args)
    assert proc.returncode == 0, (args, proc.returncode, proc.stderr)
    assert f"vpc 명령 ({len(groups['vpc'])}개)" in proc.stdout, args

unknown = invoke("not-a-group")
assert unknown.returncode == 2, (unknown.returncode, unknown.stderr)

vm = invoke("help", "vm")
assert vm.returncode == 0, vm.stderr
assert max(map(len, vm.stdout.splitlines())) <= 100, "상세 메뉴가 100열을 넘었다"

repl = invoke("--interactive", input_text="exit\n")
assert repl.returncode == 0, repl.stderr
assert "도움말: help │ 종료: exit" in repl.stdout

all_help = invoke("help", "all")
assert all_help.returncode == 0, all_help.stderr
all_rows = [line for line in all_help.stdout.splitlines() if line.startswith("  pcvctl ")]
assert len(all_rows) == len(routes), (len(all_rows), len(routes))
assert all_help.stdout.count(f"vm 명령 ({len(groups['vm'])}개)") == 1

search = invoke("help", "guest-agent")
assert search.returncode == 0, search.stderr
assert "검색 결과: guest-agent" in search.stdout
assert "pcvctl vm guest-agent-status" in search.stdout

missing = invoke("help", "not-a-command")
assert missing.returncode == 0, missing.stderr
assert "일치하는 명령이 없습니다" in missing.stdout

print(f"PASS: pcvctl help readability ({len(groups)} groups, {len(routes)} routes)")
PY
