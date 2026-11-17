#!/usr/bin/env python3
                          
                                                                                                      
                                                                            
                                                                                                  
                                                                                      
                                   
                                                                                                                       
                                                                         
                                                                                      
 
                                                                                 
                                                               
                                        
from __future__ import annotations

import argparse
import json
import re
import sys

HIST_SUFFIX = ("_bucket", "_sum", "_count")
_NAME = r"[a-zA-Z_:][a-zA-Z0-9_:]*"
_LVAL = r'"(?:[^"\\\n]|\\[\\"n])*"'                                          
_LABEL = r"[a-zA-Z_][a-zA-Z0-9_]*=" + _LVAL
SAMPLE = re.compile(r"^(" + _NAME + r")(\{(?:" + _LABEL + r"(?:," + _LABEL + r")*,?)?\})?[ \t]+(\S+)(?:[ \t]+-?\d+)?$")
LABEL_PAIR = re.compile(r"([a-zA-Z_][a-zA-Z0-9_]*)=(" + _LVAL + r")")
TYPE_LINE = re.compile(r"^# TYPE (" + _NAME + r") (counter|gauge|histogram|summary|untyped)$")


def _family(name: str, types: dict[str, str]) -> str:
                                                             
    for suf in HIST_SUFFIX:
        if name.endswith(suf) and types.get(name[: -len(suf)]) in ("histogram", "summary"):
            return name[: -len(suf)]
    return name


def _value_ok(v: str) -> bool:
    try:
        float(v)                     
        return True
    except ValueError:
        return False


def check(text: str) -> list[str]:
                                      
    errs: list[str] = []
    types: dict[str, str] = {}
    started: set[str] = set()                                
    closed: set[str] = set()                   
    seen: set[tuple[str, tuple]] = set()
    current = None
    for no, line in enumerate(text.splitlines(), 1):
        if not line.strip():
            continue
        if line.startswith("#"):
            if line.startswith("# TYPE"):
                m = TYPE_LINE.match(line)
                if not m:
                    errs.append(f"{no}: TYPE 줄 문법 오류: {line[:160]}")
                    continue
                name, kind = m.groups()
                if name in types:
                    errs.append(f"{no}: {name} 의 TYPE 이 두 번 나온다")
                elif name in started:
                    errs.append(f"{no}: {name} 의 TYPE 이 첫 시계열 뒤에 나온다")
                else:
                    types[name] = kind
            continue
        m = SAMPLE.match(line)
        if not m:
            errs.append(f"{no}: 시계열 줄 문법 오류: {line[:160]}")
            continue
        name, labels, value = m.group(1), m.group(2) or "", m.group(3)
        if not _value_ok(value):
            errs.append(f"{no}: {name} 값이 숫자가 아니다: {value!r}")
        fam = _family(name, types)
        if fam not in types:
            errs.append(f"{no}: {name} 앞에 TYPE 이 없다")
            types[fam] = "untyped"                            
        if fam != current:
            if fam in closed:
                errs.append(f"{no}: {fam} 시계열이 한 덩어리가 아니다(흩어져 다시 나온다)")
            if current is not None:
                closed.add(current)
            current = fam
        started.add(fam)
        key = (name, tuple(sorted(LABEL_PAIR.findall(labels))))
        if key in seen:
            errs.append(f"{no}: 같은 (이름, 라벨) 이 두 번 나온다: {name}{labels}")
        seen.add(key)
    for name, kind in types.items():
        if kind == "untyped":
            continue
        if name.endswith("_total") and kind != "counter":
            errs.append(f"{name}: `_total` 이름인데 TYPE {kind}")
        if kind == "counter" and not (name.endswith("_total") or name.endswith(HIST_SUFFIX)):
            errs.append(f"{name}: counter 인데 이름이 `_total` 로 끝나지 않는다")
    return errs


def bodies_from_jsonl(path: str, want_path: str, want_probe: str) -> list[str]:
    out = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            r = json.loads(line)
            if r.get("path") == want_path and r.get("probe") == want_probe and r.get("status") == 200:
                out.append(r.get("body", ""))
    return out


_GOOD = "# TYPE a_total counter\na_total{x=\"1\"} 1\na_total{x=\"2\"} 2\n# TYPE b gauge\nb 1\n"


def self_test() -> int:
    cases = {
        "good": (_GOOD, 0),
        "dup_type_and_series": ("# TYPE b gauge\nb 1\n# TYPE b gauge\nb 2\n", 2),
        "scattered": ("# TYPE a gauge\n# TYPE b gauge\na{x=\"1\"} 1\nb 1\na{x=\"2\"} 2\n", 1),
        "total_gauge": ("# TYPE c_total gauge\nc_total 1\n", 1),
        "counter_no_total": ("# TYPE d counter\nd 1\n", 1),
        "unclosed_quote": ("# TYPE e gauge\ne{x=\"1} 1\n", 1),
        "hist_child_counter": ("# TYPE h_bucket counter\nh_bucket{le=\"1\"} 1\n", 0),
    }
    bad = []
    for name, (text, want) in cases.items():
        got = check(text)
        if len(got) != want:
            bad.append(f"{name}: 위반 {len(got)} != 기대 {want}: {got}")
    if bad:
        print("[check-prom-exposition] 자기 시험 FAIL")
        for b in bad:
            print(" - " + b)
        return 1
    print(f"[check-prom-exposition] 자기 시험 PASS: {len(cases)}건")
    return 0


def main(argv: list[str]) -> int:
    if not argv:
        return self_test()
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("file", nargs="?", help="본문 파일(- = 표준 입력)")
    ap.add_argument("--jsonl", help="rest_public_compare.sh 의 JSONL")
    ap.add_argument("--path", default="/metrics")
    ap.add_argument("--probe", default="direct")
    a = ap.parse_args(argv)
    if a.jsonl:
        bodies = bodies_from_jsonl(a.jsonl, a.path, a.probe)
        if not bodies:
            print(f"[check-prom-exposition] {a.jsonl}: {a.path} {a.probe} 200 레코드 없음")
            return 2
    elif a.file:
        bodies = [sys.stdin.read() if a.file == "-" else open(a.file, encoding="utf-8").read()]
    else:
        ap.print_usage()
        return 2
    rc = 0
    for i, body in enumerate(bodies):
        errs = check(body)
        n = sum(1 for ln in body.splitlines() if ln.strip() and not ln.startswith("#"))
        if errs:
            rc = 1
            print(f"[check-prom-exposition] FAIL 본문 {i}: 위반 {len(errs)}건(시계열 {n}줄)")
            for e in errs[:200]:
                print(" - " + e)
        else:
            print(f"[check-prom-exposition] PASS 본문 {i}: 시계열 {n}줄, 이름 {len(set(m.group(1) for m in map(TYPE_LINE.match, body.splitlines()) if m))}개")
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
