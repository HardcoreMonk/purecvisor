#!/usr/bin/env python3
                          
                                                      
                                                                                      
                                                                      
                                                                     
                                                                                  
 
                      
                                                                 
                                       

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

START_ASYNC = "purecvisor_vm_manager_start_vm_async"
STOP_ASYNC = "purecvisor_vm_manager_stop_vm_async"
START_ASYNC_ALLOWED = {"src/modules/virt/vm_ops.c", "src/modules/virt/vm_manager.h"}
STOP_ASYNC_ALLOWED = START_ASYNC_ALLOWED | {"src/modules/dispatcher/handler_vm_lifecycle.c"}
                                                                    
CREATE_BASELINE = {
    "src/modules/virt/vm_ops.c": 1,                                                 
    "src/modules/virt/vm_start_capacity.c": 1,                                
}
CREATE_RE = re.compile(r"\bvirDomainCreate(?:XML|WithFlags|WithFiles|XMLWithFiles)?\s*\(")


def strip_c(text: str) -> str:
                                                     
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j])); i = j
        elif text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i)); i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(re.sub(r"[^\n]", " ", text[i:j])); i = j
        else:
            out.append(c); i += 1
    return "".join(out)


def scan(files: dict[str, str]) -> list[str]:
                                 
    violations: list[str] = []
    for rel, raw in sorted(files.items()):
        code = strip_c(raw)
        if re.search(rf"\b{START_ASYNC}\b", code) and rel not in START_ASYNC_ALLOWED:
            violations.append(f"{rel}: {START_ASYNC} 참조 — 정식 vm.start 진입점(pcv_vm_start_submit)을 쓴다")
        if re.search(rf"\b{STOP_ASYNC}\b", code) and rel not in STOP_ASYNC_ALLOWED:
            violations.append(f"{rel}: {STOP_ASYNC} 참조 — 배치 정상 종료는 pcv_vm_graceful_stop_submit을 쓴다")
        count = len(CREATE_RE.findall(code))
        limit = CREATE_BASELINE.get(rel, 0)
        if count > limit:
            violations.append(f"{rel}: virDomainCreate* 호출 {count}건 > 허용 {limit}건 — 정식 vm.start 진입점을 쓴다")
    return violations


def load_tree(root: Path) -> dict[str, str]:
    return {p.relative_to(root).as_posix(): p.read_text(encoding="utf-8", errors="replace")
            for p in sorted((root / "src").rglob("*")) if p.suffix in (".c", ".h")}


def self_test() -> int:
    clean = {"src/modules/virt/vm_ops.c": f"void {START_ASYNC}(void){{ virDomainCreate(d); }}",
             "src/api/dispatcher.c": "/* virDomainCreate( 와 " + START_ASYNC + " 는 주석이라 무시 */ int x;"}
    cases = {
        "clean": (clean, 0),
        "start-async-outside": ({**clean, "src/api/dispatcher.c": f"x = {START_ASYNC};"}, 1),
        "stop-async-outside": ({**clean, "src/api/dispatcher.c": f"{STOP_ASYNC}(m, v, cb, d);"}, 1),
        "create-new-file": ({**clean, "src/modules/dispatcher/handler_snapshot.c": "virDomainCreate(dom);"}, 1),
        "create-over-limit": ({**clean, "src/modules/virt/vm_start_capacity.c": "virDomainCreate(a); virDomainCreateWithFlags(b, 0);"}, 1),
        "create-lifecycle-reintroduced": ({**clean, "src/modules/dispatcher/handler_vm_lifecycle.c": "virDomainCreate(dom);"}, 1),
        "create-self-healing-reintroduced": ({**clean, "src/modules/ai/self_healing.c": "virDomainCreate(dom);"}, 1),
        "create-backup-restore-reintroduced": ({**clean, "src/modules/backup/backup_scheduler.c": "virDomainCreate(dom);"}, 1),
        "string-literal-ignored": ({**clean, "src/api/x.c": 'log("virDomainCreate(");'}, 0),
    }
    failed = [name for name, (files, want) in cases.items() if len(scan(files)) != want]
    if failed:
        print(f"[check-vm-start-single-entry] SELF-TEST FAIL: {failed}")
        return 1
    print(f"[check-vm-start-single-entry] SELF-TEST PASS: {len(cases)} cases")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    violations = scan(load_tree(ROOT))
    if violations:
        print("[check-vm-start-single-entry] FAIL (ADR-0080 VM 시작 단일 진입점):")
        for v in violations:
            print(f"  - {v}")
        return 1
    print("[check-vm-start-single-entry] PASS: VM 시작·배치 정지 우회 없음")
    return 0


if __name__ == "__main__":
    sys.exit(main())
