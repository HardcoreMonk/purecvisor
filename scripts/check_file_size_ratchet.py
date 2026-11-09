#!/usr/bin/env python3
                          
                                                                               
                                                                                            
                                                                       
                                   
 
                      
                                                         
                                                 
                   
from __future__ import annotations

import argparse
import fnmatch
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(os.environ.get("PCV_RATCHET_ROOT") or Path(__file__).resolve().parents[1])
BASELINE_REL = "scripts/file_size_baseline.txt"
LIMIT = 1500
INCLUDE = ("src/*.c", "src/*.h", "ui/app.js", "ui/modules/*.js")
EXCLUDE = ("ui/vendor/*", "ui/app.bundle.js", "ui/sw.js")


def tracked(root: Path) -> list[str]:
    out = subprocess.run(["git", "ls-files"], cwd=root, capture_output=True, text=True, check=True).stdout.split()
    return sorted(p for p in out if any(fnmatch.fnmatch(p, g) for g in INCLUDE)
                  and not any(fnmatch.fnmatch(p, g) for g in EXCLUDE))


def count_lines(path: Path) -> int:
    with path.open("rb") as f:
        return sum(1 for _ in f)


def load_baseline(root: Path) -> dict[str, int]:
    base = {}
    p = root / BASELINE_REL
    if p.exists():
        for line in p.read_text().splitlines():
            if line.strip() and not line.startswith("#"):
                n, rel = line.split("\t")
                base[rel] = int(n)
    return base


def write_baseline(root: Path, base: dict[str, int]) -> None:
    head = ("# 파일 크기 래칫 기준선 — 상한 1,500줄을 넘는 기존 파일의 현재 줄 수.\n"
            "# 줄이는 방향만 허용한다. 갱신은 scripts/check_file_size_ratchet.py --tighten (값을 올리지 않는다).\n")
    body = "".join(f"{n}\t{rel}\n" for rel, n in sorted(base.items(), key=lambda kv: (-kv[1], kv[0])))
    (root / BASELINE_REL).write_text(head + body)


def check(root: Path) -> tuple[list[str], list[str], dict[str, int]]:
    base = load_baseline(root)
    files = tracked(root)
    sizes = {rel: count_lines(root / rel) for rel in files}
    fails, warns = [], []
    for rel, n in sizes.items():
        if rel in base:
            if n > base[rel]:
                fails.append(f"{rel}: {n}줄 > 기준선 {base[rel]}줄(커지면 안 된다)")
            elif n < base[rel]:
                warns.append(f"{rel}: {n}줄 < 기준선 {base[rel]}줄 — --tighten으로 기준선을 낮춘다")
        elif n > LIMIT:
            fails.append(f"{rel}: {n}줄 > 상한 {LIMIT}줄(새로 커진 파일)")
    for rel in sorted(set(base) - set(sizes)):
        fails.append(f"{rel}: 기준선에 있으나 추적 파일이 아니다 — 기준선에서 지운다")
    return fails, warns, sizes


def tighten(root: Path) -> int:
    base = load_baseline(root)
    sizes = {rel: count_lines(root / rel) for rel in tracked(root)}
    new = {rel: min(n, sizes[rel]) for rel, n in base.items() if rel in sizes and sizes[rel] > LIMIT}
    write_baseline(root, new)
    print(f"[file-size-ratchet] 기준선 {len(base)} → {len(new)}개")
    return 0


def self_test() -> int:
    ok = True
    with tempfile.TemporaryDirectory() as d:
        r = Path(d)
        (r / "src").mkdir()
        (r / "scripts").mkdir()
        (r / "src/small.c").write_text("x\n" * 10)
        (r / "src/big.c").write_text("x\n" * 1600)
        subprocess.run(["git", "init", "-q"], cwd=r, check=True)
        subprocess.run(["git", "add", "-A"], cwd=r, check=True)
        write_baseline(r, {"src/big.c": 1600})
        cases = []
        cases.append(("기준선 그대로", check(r)[0] == []))
        (r / "src/new.c").write_text("x\n" * 1501)
        subprocess.run(["git", "add", "-A"], cwd=r, check=True)
        cases.append(("상한 초과 새 파일 FAIL", any("new.c" in f for f in check(r)[0])))
        (r / "src/new.c").write_text("x\n")
        (r / "src/big.c").write_text("x\n" * 1601)
        cases.append(("기준선 파일 증가 FAIL", any("big.c" in f for f in check(r)[0])))
        (r / "src/big.c").write_text("x\n" * 1550)
        cases.append(("기준선 파일 감소는 WARN만", check(r)[0] == [] and check(r)[1] != []))
        tighten(r)
        cases.append(("--tighten은 낮추기만", load_baseline(r) == {"src/big.c": 1550}))
        write_baseline(r, {"src/big.c": 1550, "src/gone.c": 2000})
        cases.append(("사라진 기준선 경로 FAIL", any("gone.c" in f for f in check(r)[0])))
    for name, passed in cases:
        if not passed:
            ok = False
            print(f"[SELF-TEST FAIL] {name}")
    print("[file-size-ratchet] self-test " + ("PASS" if ok else "FAIL") + f" ({len(cases)} cases)")
    return 0 if ok else 1


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--tighten", action="store_true", help="기준선을 현재 줄 수로 낮춘다(올리지 않는다)")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--init", action="store_true", help="기준선이 없을 때 한 번만: 상한 초과 파일을 현재 값으로 고정")
    a = ap.parse_args(argv)
    if a.self_test:
        return self_test()
    if a.init:
        if (ROOT / BASELINE_REL).exists():
            print("기준선이 이미 있다 — --init은 처음 한 번만 쓴다", file=sys.stderr)
            return 2
        sizes = {rel: count_lines(ROOT / rel) for rel in tracked(ROOT)}
        write_baseline(ROOT, {rel: n for rel, n in sizes.items() if n > LIMIT})
        print(f"[file-size-ratchet] 기준선 생성: {sum(1 for n in sizes.values() if n > LIMIT)}개")
        return 0
    if a.tighten:
        return tighten(ROOT)
    fails, warns, sizes = check(ROOT)
    for w in warns:
        print("WARN " + w)
    if fails:
        print(f"[file-size-ratchet] FAIL: {len(fails)}건")
        for f in fails:
            print(" - " + f)
        return 1
    print(f"[file-size-ratchet] PASS: {len(sizes)}개 파일, 상한 {LIMIT}줄, 기준선 {len(load_baseline(ROOT))}개")
    return 0


if __name__ == "__main__":
    sys.exit(main())
