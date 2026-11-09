#!/usr/bin/env python3
                          
                                                                  
                                                           
                                                                            
                                             
                                                                             
                                                                         
                                                                   
 
                      
                                                              
                                                            
                                                                  
                                                                  

                                                                  
   
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

                                                   
IDENT_TOKENS = (
    "VIR_DOMAIN_INTERFACE_ADDRESSES_SRC_AGENT", "virDomainQemuAgentCommand",
    "virDomainAgentSetResponseTimeout", "VIR_DOMAIN_SHUTDOWN_GUEST_AGENT",
    "VIR_DOMAIN_REBOOT_GUEST_AGENT", "virDomainGetGuestInfo", "virDomainGetFSInfo",
    "virDomainFSFreeze", "virDomainFSThaw", "virDomainSetTime", "virDomainGetTime",
    "virDomainGetHostname", "virDomainSetUserPassword",
)
                                             
CALL_TOKENS = ("virDomainShutdown", "virDomainShutdownFlags")
TOKEN_RE = re.compile(
    r"\b(" + "|".join(IDENT_TOKENS) + r")\b|\b(" + "|".join(CALL_TOKENS) + r")\s*\(")

_H = "src/modules/dispatcher/"
                                                                              
                                           
ALLOWLIST: dict[tuple[str, str], str] = {
    (_H + "handler_vm_guest.c", "_guest_fsinfo_worker"):
        "g_task_run_in_thread(task, _guest_fsinfo_worker) handler_vm_guest.c:490 — GTask 스레드 함수",
    (_H + "handler_vm_guest.c", "_guest_ping_worker"):
        "g_task_run_in_thread(task, _guest_ping_worker) handler_vm_guest.c:616 — GTask 스레드 함수",
    (_H + "handler_vm_guest.c", "_guest_exec_worker"):
        "g_task_run_in_thread(task, _guest_exec_worker) handler_vm_guest.c:932 — GTask 스레드 함수",
    (_H + "handler_vm_guest.c", "_guest_shutdown_worker"):
        "g_task_run_in_thread(task, _guest_shutdown_worker) handler_vm_guest.c:1070 — GTask 스레드 함수",
    (_H + "handler_snapshot.c", "_rollback_worker"):
        "g_task_run_in_thread(task, _rollback_worker) handler_snapshot.c:1119 — GTask 스레드 함수(virDomainShutdown :528)",
    (_H + "handler_snapshot.c", "_libvirt_rollback_worker"):
        "g_task_run_in_thread(task, _libvirt_rollback_worker) handler_snapshot.c:775 — GTask 스레드 함수(virDomainShutdown :683)",
    (_H + "guest_agent_exec.c", "_ga_real_agent_command"):
        "ops 표 k_real_ops(:266)로만 도달, 호출은 _ga_exec_job(:508) — GThreadPool 함수(g_thread_pool_new :530)",
    (_H + "guest_agent_exec.c", "_ga_real_status_probe"):
        "ops 표 k_real_ops(:268)로만 도달, 호출은 _ga_exec_status(:479) ← _ga_exec_job(:498) — GThreadPool 함수",
    ("src/modules/virt/vm_ops.c", "stop_vm_thread_impl"):
        "g_task_run_in_thread(task, stop_vm_thread_impl) vm_ops.c:316 — GTask 스레드 함수(유일한 참조)",
    ("src/modules/backup/backup_scheduler.c", "pcv_backup_restore"):
        "src 내 유일한 호출자 _restore_worker handler_backup.c:501 ← g_task_run_in_thread handler_backup.c:741 — GTask 스레드 함수",
}


def mask_code(text: str) -> str:
                                                          
    out: list[str] = []
    i, n = 0, len(text)
    state, quote = "code", ""
    while i < n:
        ch = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if state == "code":
            if ch == "/" and nxt == "/":
                state = "line"; out.append("  "); i += 2; continue
            if ch == "/" and nxt == "*":
                state = "block"; out.append("  "); i += 2; continue
            if ch in ('"', "'"):
                state, quote = "literal", ch
            out.append(ch)
            i += 1
        elif state == "literal":
            if ch == "\\" and i + 1 < n:
                out.append("  "); i += 2; continue
            if ch == quote or ch == "\n":
                state = "code"; out.append(ch)
            else:
                out.append(" ")
            i += 1
        elif state == "line":
            if ch == "\n":
                state = "code"; out.append("\n")
            else:
                out.append(" ")
            i += 1
        else:
            if ch == "*" and nxt == "/":
                state = "code"; out.append("  "); i += 2; continue
            out.append("\n" if ch == "\n" else " ")
            i += 1
    return "".join(out)


_NAME_RE = re.compile(r"\b([A-Za-z_]\w*)\s*\(")
_SKIP_NAMES = {"__attribute__", "if", "for", "while", "switch", "sizeof"}


def _strip_preprocessor(text: str) -> str:
                                            
    out, cont = [], False
    for ln in text.split("\n"):
        if cont or ln.lstrip().startswith("#"):
            cont = ln.rstrip().endswith("\\")
            continue
        out.append(ln)
    return "\n".join(out)


def function_spans(code: str) -> list[tuple[str, int, int]]:
                                                                     
    spans: list[tuple[str, int, int]] = []
    depth, header_start, i, n = 0, 0, 0, len(code)
    name, start, header = None, 0, ""
    while i < n:
        ch = code[i]
        if ch == "{":
            if depth == 0:
                header = _strip_preprocessor(code[header_start:i])
                name = next((m.group(1) for m in _NAME_RE.finditer(header)
                             if m.group(1) not in _SKIP_NAMES), None)
                start = i
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth < 0:
                raise ValueError(f"닫는 중괄호가 더 많다(줄 {code.count(chr(10), 0, i) + 1})")
            if depth == 0:
                if name and "=" not in header.split("(")[0]:
                    spans.append((name, start, i))
                header_start = i + 1
        elif ch == ";" and depth == 0:
            header_start = i + 1
        i += 1
    if depth != 0:                                                                 
        raise ValueError(f"파일 끝에서 중괄호 깊이가 {depth} 이다")
    return spans


def find_hits(text: str) -> list[tuple[str, str, int]]:
                                               
    code = mask_code(text)
    spans = function_spans(code)
    hits = []
    for m in TOKEN_RE.finditer(code):
        pos = m.start()
        func = next((nm for nm, s, e in spans if s <= pos <= e), "<file-scope>")
        hits.append((func, m.group(1) or m.group(2), code.count("\n", 0, pos) + 1))
    return hits


def check_files(files: dict[str, str],
                allowlist: dict[tuple[str, str], str] | None = None) -> list[str]:
                                                                 
    allow = ALLOWLIST if allowlist is None else allowlist
    reasons: list[str] = []
    used: set[tuple[str, str]] = set()
    for rel, text in sorted(files.items()):
        try:
            hits = find_hits(text)
        except ValueError as exc:
            reasons.append(f"{rel}: 함수 경계를 해석할 수 없다(fail-closed) — {exc}")
            used |= {k for k in allow if k[0] == rel}
            continue
        for func, token, line in hits:
            if (rel, func) in allow:
                used.add((rel, func))
            else:
                reasons.append(f"{rel}:{line} {func}() 가 {token} 을 쓴다 — 메인 루프에서 게스트 에이전트를 "
                               "기다릴 수 있다(워커/실행기 함수로 옮기거나 근거와 함께 ALLOWLIST 에 추가)")
    for entry in sorted(set(allow) - used):
        rel, func = entry
        why = "파일이 없다" if rel not in files else "함수가 없거나 토큰을 더 이상 쓰지 않는다"
        reasons.append(f"stale 허용 목록: {rel} {func}() — {why}")
    return reasons


def load_tree(root: Path) -> dict[str, str]:
                                                                       
    files: dict[str, str] = {}
    allow_files = {f for f, _ in ALLOWLIST}
    for path in sorted((root / "src").rglob("*.c")):
        rel = path.relative_to(root).as_posix()
        text = path.read_text(encoding="utf-8", errors="replace")
        if rel in allow_files or TOKEN_RE.search(text):
            files[rel] = text
    return files


def check_tree(root: Path) -> list[str]:
    return check_files(load_tree(root))


def self_test() -> int:
                              
    bad = 'static void m(void)\n{\n    char *r = virDomainQemuAgentCommand(d, c, 5, 0);\n}\n'
    ok_cases = {
        "주석·문자열 무시": ('/* virDomainShutdown( */ static void f(void)\n{\n'
                           '    s = "VIR_DOMAIN_INTERFACE_ADDRESSES_SRC_AGENT"; // virDomainGetTime\n}\n', {}),
        "허용 함수": (bad, {("src/a.c", "m"): "x"}),
    }
    for label, (src, allow) in ok_cases.items():
        if check_files({"src/a.c": src}, allow):
            print(f"[SELF-TEST FAIL] {label}", file=sys.stderr)
            return 1
    if not check_files({"src/a.c": bad}, {}):
        print("[SELF-TEST FAIL] 허용 밖 호출을 못 잡음", file=sys.stderr)
        return 1
    if not check_files({"src/a.c": bad.replace("m(", "m2(")}, {("src/a.c", "m"): "x"}):
        print("[SELF-TEST FAIL] 낡은 허용 항목을 못 잡음", file=sys.stderr)
        return 1
    unbalanced = "static void a(void)\n{\n#if X\n    if (1) {\n#else\n    if (2) {\n#endif\n    }\n}\n"
    unbalanced2 = unbalanced.replace("    }\n}\n", "    }\n")
    if not check_files({"src/a.c": unbalanced2}, {}):
        print("[SELF-TEST FAIL] 깊이 불일치를 fail-closed 로 못 잡음", file=sys.stderr)
        return 1
    cont = "#define M(x) \\\n    do { \\\n    } while (0)\nstatic void c(void)\n{\n    virDomainShutdown(d);\n}\n"
    if [h[0] for h in find_hits(cont)] != ["c"]:
        print("[SELF-TEST FAIL] #define 줄 이음 뒤 줄을 전처리로 못 건너뜀", file=sys.stderr)
        return 1
    if not check_files({"src/a.c": "static int q = (virDomainShutdown(d));\n"}, {}):
        print("[SELF-TEST FAIL] file-scope 호출을 못 잡음", file=sys.stderr)
        return 1
    print("[PASS] check_mainloop_guest_agent --self-test")
    return 0


def main(argv: list[str]) -> int:
    if "--self-test" in argv:
        return self_test()
    root = ROOT
    if "--root" in argv:
        root = Path(argv[argv.index("--root") + 1])
    try:
        reasons = check_tree(root)
    except OSError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2
    if reasons:
        print("[FAIL] 메인 루프 게스트 에이전트 호출 금지 계약 위반 (ML14):", file=sys.stderr)
        for r in reasons:
            print(f"  - {r}", file=sys.stderr)
        return 1
    print(f"[PASS] 게스트 에이전트 API 는 허용 목록 워커 {len(ALLOWLIST)}개 함수 안에만 있다")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
