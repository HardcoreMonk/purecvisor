#!/usr/bin/env python3
                          
                                                              
                                                                                      
                                                                                                                            
 
                      
                                                                                                                       
                                                    

               
                                                                       
                                                                  
                                                                          
                                                      

              
                                                                
                                             
   
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TARGET = ROOT / "src/modules/storage/iscsi_manager.c"

ARRAY_RE = re.compile(
    r"(?:const\s+)?gchar\s*\*\s*[A-Za-z_]\w*\s*\[\s*\]\s*=\s*\{(?P<body>.*?)\};",
    re.S,
)
NODE_DB_GUARD_RE = re.compile(
    r"if\s*\(\s*!\s*pcv_iscsi_node_db_set_chap\s*\(", re.S
)
VALIDATE_GUARD_RE = re.compile(
    r"if\s*\(\s*!\s*pcv_iscsi_chap_validate\s*\(", re.S
)


def strip_comments(text: str) -> str:
                                   
    out: list[str] = []
    i = 0
    in_block = False
    in_line = False
    in_string: str | None = None
    while i < len(text):
        ch = text[i]
        if in_line:
            out.append("\n" if ch == "\n" else " ")
            if ch == "\n":
                in_line = False
            i += 1
            continue
        if in_block:
            if ch == "*" and i + 1 < len(text) and text[i + 1] == "/":
                out.extend("  ")
                i += 2
                in_block = False
            else:
                out.append("\n" if ch == "\n" else " ")
                i += 1
            continue
        if in_string:
            out.append(ch)
            if ch == "\\" and i + 1 < len(text):
                out.append(text[i + 1])
                i += 2
                continue
            if ch == in_string:
                in_string = None
            i += 1
            continue
        if ch == "/" and i + 1 < len(text) and text[i + 1] == "/":
            out.extend("  ")
            i += 2
            in_line = True
            continue
        if ch == "/" and i + 1 < len(text) and text[i + 1] == "*":
            out.extend("  ")
            i += 2
            in_block = True
            continue
        if ch in {'"', "'"}:
            in_string = ch
        out.append(ch)
        i += 1
    return "".join(out)


def scan_text(text: str) -> list[str]:
    code = strip_comments(text)
    failures: list[str] = []

    if not NODE_DB_GUARD_RE.search(code):
        failures.append("pcv_iscsi_node_db_set_chap 실패를 login 전에 거부하는 guard 없음")
    if not VALIDATE_GUARD_RE.search(code):
        failures.append("pcv_iscsi_chap_validate fail-closed guard 없음")
    if '"discoverydb"' not in code or '"--discover"' not in code:
        failures.append("영속 node record를 만드는 discoverydb --discover argv 없음")
    if '"--op=update"' in code or '"--op"' in code and '"update"' in code:
        failures.append("initiator manager에 iscsiadm node update argv가 다시 생김")
    if '"node.session.auth.password"' in code:
        failures.append("initiator manager에 password node key argv literal이 다시 생김")
    for match in ARRAY_RE.finditer(code):
        body = match.group("body")
        if re.search(r"\bchap_(?:password|user)\b", body):
            failures.append("CHAP 자격 변수가 argv initializer에 포함됨")
            break
    return failures


def main(argv: list[str] | None = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    target = Path(args[0]) if args else TARGET
    failures = scan_text(target.read_text(encoding="utf-8", errors="replace"))
    if failures:
        for failure in failures:
            print(f"FAIL {failure}")
        print(f"[check-iscsi-chap-argv] FAIL: {len(failures)}건")
        return 1
    print("[check-iscsi-chap-argv] PASS: node DB fail-closed + CHAP argv 0건")
    return 0


if __name__ == "__main__":
    sys.exit(main())
