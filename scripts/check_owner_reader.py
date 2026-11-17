#!/usr/bin/env python3
                          
                                                                                                     
                                                                                                        
                                                   
                                                                                            
                                                                                                           
                                                                                                         
                                                                                   
 
                      
                                                                           
                                        
                                                           
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ALLOWED = {"src/modules/virt/vm_owner.c"}
CALL_RE = re.compile(r"\b(?:xmlStr(?:cmp|Equal|ncmp|casecmp)|xmlXPath\w*|xmlGetProp|xmlHasProp|xmlGetNsProp|"
                     r"node_is|child_named)\s*\(([^;{}]*)")
OWNER_LIT_RE = re.compile(r"[\"']owner[\"']")
XPATH_RE = re.compile(r"local-name\(\)\s*=\s*['\"]owner['\"]|/owner\b")


def _mask_comments(text: str) -> str:
                                                            
    out, i, n = [], 0, len(text)
    in_str = None
    while i < n:
        c = text[i]
        if in_str:
            out.append(c)
            if c == "\\" and i + 1 < n:
                out.append(text[i + 1])
                i += 2
                continue
            if c == in_str or c == "\n":
                in_str = None
            i += 1
            continue
        if c in "\"'":
            in_str = c
            out.append(c)
            i += 1
            continue
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(ch if ch == "\n" else " " for ch in text[i:j]))
            i = j
            continue
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
            continue
        out.append(c)
        i += 1
    return "".join(out)


def violations(texts: dict[str, str]) -> list[str]:
                                                            
    out = []
    for rel, text in sorted(texts.items()):
        if rel in ALLOWED:
            continue
        masked = _mask_comments(text)
        for m in CALL_RE.finditer(masked):
            if OWNER_LIT_RE.search(m.group(1)) or XPATH_RE.search(m.group(1)):
                line = masked.count("\n", 0, m.start()) + 1
                out.append(f"{rel}:{line}: 소유자 요소를 이름으로 찾는다 — src/modules/virt/vm_owner.c 의 함수를 쓴다")
    return out


def _tree_texts(root: Path) -> dict[str, str]:
    return {str(p.relative_to(root)): p.read_text(encoding="utf-8", errors="replace")
            for p in sorted((root / "src").rglob("*")) if p.suffix in (".c", ".h")}


def self_test() -> int:
    good = {
        "src/modules/virt/vm_owner.c": 'static int f(void){ return xmlStrEqual(n->name, BAD_CAST "owner"); }',
        "src/a.c": 'static void w(void){ g_strdup_printf("<pcv:owner xmlns:pcv=\'%s\'>%s</pcv:owner>", u, o); }\n'
                   '/* xmlStrcmp(cur->name, BAD_CAST "owner") 는 주석이다 */\n'
                   'static void j(void){ json_object_set_string_member(o, "owner", v); }',
    }
    if violations(good):
        print("[SELF-TEST FAIL] 허용 파일·쓰는 쪽·주석·JSON 키가 위반으로 잡혔다", file=sys.stderr)
        return 1
    bads = (
        'static int f(void){ return xmlStrcmp(cur->name, BAD_CAST "owner") == 0; }',
        'static int f(void){ return node_is(cur, "owner"); }',
        'static void f(void){ xmlXPathEvalExpression(BAD_CAST "/domain/metadata/*[local-name()=\'owner\']", c); }',
        'static int f(void){ return xmlStrEqual(n->name, (const xmlChar *)"owner"); }',
    )
    for b in bads:
        if not violations({"src/b.c": b}):
            print(f"[SELF-TEST FAIL] 잡아야 할 꼴을 놓쳤다: {b}", file=sys.stderr)
            return 1
    print("[PASS] check_owner_reader --self-test")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=str(ROOT))
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    root = Path(args.root)
    try:
        texts = _tree_texts(root)
    except OSError as e:
        print(f"[check-owner-reader] 읽기 실패: {e}", file=sys.stderr)
        return 2
    if not any(rel in ALLOWED for rel in texts):
        print("[check-owner-reader] 허용 TU(src/modules/virt/vm_owner.c)가 없다", file=sys.stderr)
        return 2
    bad = violations(texts)
    if bad:
        print("[check-owner-reader] FAIL")
        for b in bad:
            print(f"  - {b}")
        return 1
    print(f"[check-owner-reader] PASS: 소스 {len(texts)}개, 소유자 요소 해석은 vm_owner.c 한 곳")
    return 0


if __name__ == "__main__":
    sys.exit(main())
