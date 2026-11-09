#!/usr/bin/env python3
                          
                                                                  
                                                                                      
                                                                                                                            
 
                      
                                                                                                                           
   
                                           

                                                                              
                                                                           
                                                                             
              
   
from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
BACKUP_C = REPO_ROOT / "src" / "modules" / "backup" / "backup_scheduler.c"
HOSTKEY_C = REPO_ROOT / "src" / "modules" / "backup" / "replication_hostkey.c"

FUNCTIONS = (
    "_remote_snapshot_exists",
    "_verify_replication",
    "_enforce_remote_retention",
    "pcv_backup_replicate",
)

FORBIDDEN_PATTERNS = (
    (re.compile(r'"\s*/?bin/sh\s*"\s*,\s*"-c"'), "local /bin/sh -c"),
    (re.compile(r'"\s*sh\s*"\s*,\s*"-c"'), "local sh -c"),
    (re.compile(r"\bg_shell_quote\s*\("), "shell quoting in replication path"),
)

WEAK_OPTIONS = (
    "StrictHostKeyChecking=no",
    "StrictHostKeyChecking=accept-new",
    "UserKnownHostsFile=/dev/null",
)
SSH_LITERAL_RE = re.compile(r'"ssh"')
HELPER = "pcv_repl_ssh_argv"
REQUIRED_HELPER_LITERALS = (
    '"ssh"', '"-F"', '"/dev/null"', '"BatchMode=yes"', '"StrictHostKeyChecking=yes"',
    '"UserKnownHostsFile=%s"', '"GlobalKnownHostsFile=/dev/null"', '"HostKeyAlias=%s"',
    '"CheckHostIP=no"', '"UpdateHostKeys=no"', '"IdentitiesOnly=yes"', '"ConnectTimeout=%d"', '"-i"',
)

REQUIRED_ARGV_PAIRS = (
    ('"-F"', ('"/dev/null"',)),
    ('"-o"', ('"BatchMode=yes"',)),
    ('"-o"', ('"StrictHostKeyChecking=yes"',)),
    ('"-o"', ('"UserKnownHostsFile=%s"',)),
    ('"-o"', ('"GlobalKnownHostsFile=/dev/null"',)),
    ('"-o"', ('"HostKeyAlias=%s"',)),
    ('"-o"', ('"CheckHostIP=no"',)),
    ('"-o"', ('"UpdateHostKeys=no"',)),
    ('"-o"', ('"IdentitiesOnly=yes"',)),
    ('"-o"', ('"ConnectTimeout=%d"',)),
    ('"-i"', ("PCV_REPL_SSH_KEY_PATH", '"/etc/purecvisor/cluster_id_ed25519"')),
)
ARGV_ARRAY_RE = re.compile(r"\bconst\s+gchar\s*\*\s*\w+\s*\[\s*\]\s*=\s*\{(.*?)\}\s*;", re.DOTALL)
PRINTF_VAR_RE = re.compile(r'\b(\w+)\s*=\s*g_strdup_printf\s*\(\s*("(?:[^"\\]|\\.)*")')

PROBE_CALL = "pcv_repl_hostkey_probe("
PROBE_ANCHORS = ("_list_all_snapshots(", "_remote_snapshot_exists(", "pcv_spawn_pipe_sync(")

def strip_c_comments(text: str) -> str:

    out: list[str] = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c in ('"', "'"):
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
        elif text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join("\n" if ch == "\n" else " " for ch in text[i:j]))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)

def extract_c_function_body(text: str, name: str) -> str | None:
    match = re.search(rf"\b{name}\s*\([^;]*?\)\s*\{{", text, re.DOTALL)
    if not match:
        return None

    start = match.end() - 1
    depth = 0
    for pos in range(start, len(text)):
        if text[pos] == "{":
            depth += 1
        elif text[pos] == "}":
            depth -= 1
            if depth == 0:
                return text[start + 1:pos]
    return None


def helper_argv_tokens(helper_body: str) -> list[str] | None:

    fmts = dict(PRINTF_VAR_RE.findall(helper_body))
    for m in ARGV_ARRAY_RE.finditer(helper_body):
        toks = [t.strip() for t in m.group(1).split(",") if t.strip()]
        if toks and toks[0] == '"ssh"':
            return [fmts.get(t, t) for t in toks]
    return None

def missing_argv_pairs(tokens: list[str]) -> list[str]:

    adjacent = set(zip(tokens, tokens[1:]))
    return [f"{flag} {values[0]}" for flag, values in REQUIRED_ARGV_PAIRS
            if not any((flag, v) in adjacent for v in values)]

def probe_order_failures(replicate_code: str) -> list[str]:

    failures: list[str] = []
    probe = replicate_code.find(PROBE_CALL)
    if probe < 0:
        failures.append("pcv_backup_replicate: pcv_repl_hostkey_probe call missing (spec 4.5-4)")
    for anchor in PROBE_ANCHORS:
        pos = replicate_code.find(anchor)
        if pos < 0:
            failures.append(f"pcv_backup_replicate: probe-order anchor {anchor} missing — "
                            "update this gate together with the code")
        elif probe >= 0 and probe > pos:
            failures.append(f"pcv_backup_replicate: pcv_repl_hostkey_probe must run before {anchor} (spec 4.5-4)")
    return failures

def check(backup_text: str, hostkey_text: str | None) -> list[str]:
    failures: list[str] = []
    for fn in FUNCTIONS:
        body = extract_c_function_body(backup_text, fn)
        if body is None:
            failures.append(f"{fn}: function body not found")
            continue
        for pattern, label in FORBIDDEN_PATTERNS:
            if pattern.search(body):
                failures.append(f"{fn}: forbidden {label}")

    replicate = extract_c_function_body(backup_text, "pcv_backup_replicate")
    if replicate is None or "pcv_spawn_pipe_sync" not in replicate:
        failures.append("pcv_backup_replicate: must stream zfs send via pcv_spawn_pipe_sync")

    code_backup = strip_c_comments(backup_text)
    code_hostkey = strip_c_comments(hostkey_text) if hostkey_text is not None else ""
    if hostkey_text is None:
        failures.append("replication_hostkey.c missing — replication ssh argv helper required")
    for label, code in (("backup_scheduler.c", code_backup), ("replication_hostkey.c", code_hostkey)):
        for opt in WEAK_OPTIONS:
            if opt in code:
                failures.append(f"{label}: weak host-key option {opt}")
    if SSH_LITERAL_RE.search(code_backup):
        failures.append('backup_scheduler.c: "ssh" argv literal — use pcv_repl_ssh_argv')

    helper = extract_c_function_body(code_hostkey, HELPER) if hostkey_text is not None else None
    if hostkey_text is not None and helper is None:
        failures.append(f"replication_hostkey.c: {HELPER} body not found")
    for lit in REQUIRED_HELPER_LITERALS if helper is not None else ():
        if lit not in helper:
            failures.append(f"{HELPER}: required ssh option literal {lit} missing (spec 4.1)")
    if helper is not None:
        tokens = helper_argv_tokens(helper)
        if tokens is None:
            failures.append(f'{HELPER}: fixed argv array starting with "ssh" not found (spec 4.1)')
        else:
            for pair in missing_argv_pairs(tokens):
                failures.append(f"{HELPER}: argv pair {pair} missing from the fixed argv array (spec 4.1)")

    code_replicate = extract_c_function_body(code_backup, "pcv_backup_replicate") or ""
    failures.extend(probe_order_failures(code_replicate))
    return failures

def _has(failures: list[str], needle: str) -> bool:

    return any(needle in f for f in failures)

PROBE_STMT_RE = re.compile(r"\n[ \t]*if \(!pcv_repl_hostkey_probe\(.*?\n[ \t]*\}\n", re.DOTALL)

def _relocate_probe_after(text: str, anchor: str) -> str:

    head = re.search(r"\bpcv_backup_replicate\s*\([^;]*?\)\s*\{", text)
    if not head:
        return text
    probe = PROBE_STMT_RE.search(text, head.end())
    if not probe:
        return text
    rest = text[:probe.start()] + "\n" + text[probe.end():]
    pos = rest.find(anchor, head.end())
    end = rest.find(";\n", pos) if pos >= 0 else -1
    if end < 0:
        return text
    return rest[:end + 2] + probe.group(0).lstrip("\n") + rest[end + 2:]

def self_test() -> int:
    b = BACKUP_C.read_text(errors="replace")
    h = HOSTKEY_C.read_text(errors="replace")
    cases = [
        ("현재 트리 PASS", not check(b, h)),
        ("backup StrictHostKeyChecking=no FAIL", bool(check(b + '\nstatic const char *x = "StrictHostKeyChecking=no";\n', h))),
        ("backup accept-new FAIL", bool(check(b + '\nstatic const char *x = "-oStrictHostKeyChecking=accept-new";\n', h))),
        ("backup UserKnownHostsFile=/dev/null FAIL", bool(check(b + '\nstatic const char *x = "UserKnownHostsFile=/dev/null";\n', h))),
        ('backup "ssh" 리터럴 FAIL', bool(check(b + '\nstatic const char *y[] = { "ssh", NULL };\n', h))),
        ("헬퍼 IdentitiesOnly 제거 FAIL", bool(check(b, h.replace('"IdentitiesOnly=yes"', '"IdentitiesOnly=no"')))),
        ("헬퍼 -F /dev/null 제거 FAIL", bool(check(b, h.replace('"ssh", "-F", "/dev/null",', '"ssh",')))),
        ("헬퍼 GlobalKnownHostsFile 제거 FAIL", bool(check(b, h.replace('"GlobalKnownHostsFile=/dev/null"', '"X=1"')))),
        ("주석 속 옛 옵션은 무시", not check(b + "\n/* 옛 설정: StrictHostKeyChecking=no */\n", h)),
        ("탐침 제거 FAIL", bool(check(b.replace("pcv_repl_hostkey_probe(", "pcv_repl_hostkey_probe_gone("), h))),
        ("헬퍼 파일 없음 FAIL", bool(check(b, None))),

        ('헬퍼 argv 에서 "-o", khf 삭제 FAIL', bool(check(b, h.replace('"-o", khf,', '')))),
        ('헬퍼 argv 에서 "-o", alias 삭제 FAIL', bool(check(b, h.replace('"-o", alias,', '')))),
        ('헬퍼 khf 앞 "-o" 삭제 FAIL', bool(check(b, h.replace('"-o", khf,', 'khf,')))),
        ('헬퍼 alias 앞 "-o" → "-O" FAIL', bool(check(b, h.replace('"-o", alias,', '"-O", alias,')))),
        ('헬퍼 BatchMode 앞 "-o" 삭제 FAIL', bool(check(b, h.replace('"-o", "BatchMode=yes",', '"BatchMode=yes",')))),

        ("_list_all_snapshots 앵커 없음 FAIL",
         _has(check(b.replace("snaps = _list_all_snapshots(", "snaps = _list_snaps_renamed("), h), "anchor")),
        ("_remote_snapshot_exists 앵커 없음 FAIL",
         _has(check(b.replace("base && _remote_snapshot_exists(", "base && _remote_snap_renamed("), h), "anchor")),
        ("앵커 이름 바꾸고 탐침을 그 뒤로 FAIL",
         bool(check(_relocate_probe_after(b.replace("snaps = _list_all_snapshots(", "snaps = _list_snaps_renamed("),
                                          "_list_snaps_renamed("), h))),
        ("탐침을 _list_all_snapshots 뒤로 FAIL", _has(check(_relocate_probe_after(b, "_list_all_snapshots("), h), "must run before")),
        ("탐침을 _remote_snapshot_exists 뒤로 FAIL",
         _has(check(_relocate_probe_after(b, "_remote_snapshot_exists("), h), "must run before")),
        ("탐침을 pcv_spawn_pipe_sync 뒤로 FAIL", _has(check(_relocate_probe_after(b, "pcv_spawn_pipe_sync("), h), "must run before")),

        ("합성: 순서 맞음 PASS", not probe_order_failures(
            "pcv_repl_hostkey_probe(); _list_all_snapshots(); _remote_snapshot_exists(); pcv_spawn_pipe_sync();")),
        ("합성: _remote_snapshot_exists 뒤 탐침 FAIL", _has(probe_order_failures(
            "_remote_snapshot_exists(); pcv_repl_hostkey_probe(); _list_all_snapshots(); pcv_spawn_pipe_sync();"),
            "before _remote_snapshot_exists(")),
        ("합성: pcv_spawn_pipe_sync 뒤 탐침 FAIL", _has(probe_order_failures(
            "pcv_spawn_pipe_sync(); pcv_repl_hostkey_probe(); _list_all_snapshots(); _remote_snapshot_exists();"),
            "before pcv_spawn_pipe_sync(")),
    ]
    ok = True
    for name, passed in cases:
        if not passed:
            print(f"[SELF-TEST FAIL] {name}")
            ok = False
    print(f"[backup-replicate-shell-free] self-test {'PASS' if ok else 'FAIL'} ({len(cases)} cases)")
    return 0 if ok else 1

def main(argv: list[str] | None = None) -> int:
    args = sys.argv[1:] if argv is None else argv
    if args == ["--self-test"]:
        return self_test()
    hostkey = HOSTKEY_C.read_text(errors="replace") if HOSTKEY_C.exists() else None
    failures = check(BACKUP_C.read_text(errors="replace"), hostkey)
    if failures:
        print("[FAIL] backup.replicate shell-free / host-key pin contract violated")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print("[PASS] backup.replicate uses argv/splice without local shell and pinned host keys")
    return 0


if __name__ == "__main__":
    sys.exit(main())
