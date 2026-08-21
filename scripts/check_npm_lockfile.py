#!/usr/bin/env python3
                          
                                                                              
                                                                                      
                                                                                                                            
 
                      
                                                                                                                                       
                                                                               

    
                                                                 
                                                                 
                                                                   
                                                       
           

                                                                 
                                                           
                                                        

       
                                                                     

     
                                                                   
                                                                             
                                                           
                                                                                  
                                                                     
                                                                  
                                                                
                                                                    
                                       
                                                                          
                                         
                                                                      
                                      
                                                                 
                                                          
                                                                      
                                                  

     
                                                                         
                                                                        
                        
                                                                     

       
                    
          
                
   
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PKG_REL = "package.json"
LOCK_REL = "package-lock.json"

MIN_LOCKFILE_VERSION = 2
ALLOWED_REGISTRY_HOSTS = ("registry.npmjs.org",)
                                    
SRI_RE = re.compile(r"^sha(256|384|512)-[A-Za-z0-9+/]+={0,2}$")
SEMVER_RE = re.compile(r"^(\d+)\.(\d+)\.(\d+)$")

                                                             
                                  
NPX_RE = re.compile(r"\bnpx\b([^\n;|&]*)")
                                            
NPX_PKG_RE = re.compile(r"^-")

NPX_SOURCES = ("Makefile",)
NPX_GLOBS = ("scripts/*.sh",)


def parse_semver(v: str):
    m = SEMVER_RE.match(v)
    return tuple(int(x) for x in m.groups()) if m else None


def satisfies(version: str, spec: str):
                                        

                                                       
                                                                   
                                                        
                                      
       
    v = parse_semver(version)
    if v is None:
        return None
    spec = spec.strip()
    if spec in ("", "*", "latest"):
                                                    
                           
        return False
    if SEMVER_RE.match(spec):
        return v == parse_semver(spec)
    if spec.startswith("^") or spec.startswith("~"):
        base = parse_semver(spec[1:])
        if base is None:
            return None
        if v < base:
            return False
        major, minor, _ = base
        if spec[0] == "~":
            return v[:2] == (major, minor)
                                       
        if major != 0:
            return v[0] == major
        if minor != 0:
            return v[:2] == (0, minor)
        return v == base
    if spec.startswith(">="):
        base = parse_semver(spec[2:].strip())
        if base is None:
            return None
        return v >= base
    return None


def lock_entry_for(lock_packages: dict, name: str):
                                                       
    return lock_packages.get(f"node_modules/{name}")


def check_lock(pkg: dict, lock: dict) -> list[str]:
    bad: list[str] = []

                                                           
    ver = lock.get("lockfileVersion")
    if not isinstance(ver, int) or ver < MIN_LOCKFILE_VERSION:
        bad.append(f"[불변식2] lockfileVersion={ver!r} — >= {MIN_LOCKFILE_VERSION} "
                   "이어야 한다(v1 다운그레이드는 전이 의존 무결성 표현을 잃는다)")
    packages = lock.get("packages")
    if not isinstance(packages, dict) or "" not in packages:
        bad.append("[불변식2] lock 에 packages 맵(루트 항목 포함)이 없다")
        return bad

                                                         
    root = packages[""] if isinstance(packages[""], dict) else {}
    for field in ("dependencies", "devDependencies"):
        declared = pkg.get(field) or {}
        locked = root.get(field) or {}
        if declared != locked:
            only_pkg = {k: v for k, v in declared.items() if locked.get(k) != v}
            only_lock = {k: v for k, v in locked.items() if declared.get(k) != v}
            bad.append(f"[불변식3] package.json 의 {field} 가 lock 루트와 다르다 — "
                       f"lock 재생성 누락(package.json만={only_pkg}, lock만={only_lock}). "
                       "`npm install` 로 lock 을 갱신해 함께 커밋할 것")

                                                         
    n_declared = 0
    for field in ("dependencies", "devDependencies"):
        for name, spec in (pkg.get(field) or {}).items():
            n_declared += 1
            entry = lock_entry_for(packages, name)
            if not isinstance(entry, dict):
                bad.append(f"[불변식4] {name}: package.json 에 선언됐으나 lock 에 "
                           "`node_modules/{0}` 항목이 없다".format(name))
                continue
            locked_version = entry.get("version")
            if not isinstance(locked_version, str):
                bad.append(f"[불변식4] {name}: lock 항목에 version 이 없다")
                continue
            if not isinstance(spec, str):
                bad.append(f"[불변식4] {name}: 선언 range 가 문자열이 아니다")
                continue
            ok = satisfies(locked_version, spec)
            if ok is None:
                bad.append(f"[불변식4] {name}: range '{spec}' 를 이 게이트가 해석하지 "
                           "못한다(fail-closed) — 지원 문법(정확·^·~·>=)으로 바꾸거나 "
                           "scripts/check_npm_lockfile.py 의 satisfies() 를 확장할 것")
            elif ok is False:
                bad.append(f"[불변식4] {name}: 핀 {locked_version} 이 선언 range "
                           f"'{spec}' 를 만족하지 않는다(드리프트)")

                                                       
    n_pinned = 0
    n_link = 0
    for key, entry in packages.items():
        if key == "":
            continue
        if not isinstance(entry, dict):
            bad.append(f"[불변식5] {key}: lock 항목이 객체가 아니다")
            continue
        if entry.get("link") is True:
            n_link += 1
            continue
        n_pinned += 1
        if not isinstance(entry.get("version"), str):
            bad.append(f"[불변식5] {key}: version 없음")
        resolved = entry.get("resolved")
        integrity = entry.get("integrity")
        if not isinstance(resolved, str) or not resolved:
            bad.append(f"[불변식5] {key}: resolved 없음 — 출처 불명 의존")
            continue
        if not isinstance(integrity, str) or not integrity:
            bad.append(f"[불변식5] {key}: integrity 없음 — 내용 검증 없이 설치된다")
        elif not SRI_RE.match(integrity):
            bad.append(f"[불변식6] {key}: integrity '{integrity[:24]}…' 가 "
                       "sha256/384/512 SRI 형식이 아니다")
        if not resolved.startswith("https://"):
            bad.append(f"[불변식7] {key}: resolved 가 https 가 아니다 ({resolved[:60]})")
            continue
        host = resolved.split("/", 3)[2].split("@")[-1]
        if host not in ALLOWED_REGISTRY_HOSTS:
            bad.append(f"[불변식7] {key}: 허용되지 않은 레지스트리 호스트 '{host}' "
                       f"(허용: {', '.join(ALLOWED_REGISTRY_HOSTS)})")

    if n_pinned == 0:
        bad.append("[불변식5] lock 에 핀된 의존이 하나도 없다 — 파일이 비워졌다")

    return bad


def collect_npx_sources(root: Path) -> list[tuple[str, str]]:
    out = []
    for rel in NPX_SOURCES:
        p = root / rel
        if p.is_file():
            out.append((rel, p.read_text(encoding="utf-8", errors="replace")))
    for pattern in NPX_GLOBS:
        for p in sorted(root.glob(pattern)):
            if p.is_file():
                out.append((p.relative_to(root).as_posix(),
                            p.read_text(encoding="utf-8", errors="replace")))
    return out


def check_npx(pkg: dict, sources: list[tuple[str, str]]) -> tuple[list[str], int]:
                                       
    bad: list[str] = []
    declared = set(pkg.get("dependencies") or {}) | set(pkg.get("devDependencies") or {})
    n_calls = 0
    for rel, text in sources:
        for lineno, line in enumerate(text.split("\n"), 1):
            stripped = line.lstrip()
            if stripped.startswith("#"):
                continue
            for m in NPX_RE.finditer(line):
                n_calls += 1
                tail = m.group(1)
                tokens = tail.split()
                if not any(t in ("--no-install", "--offline") for t in tokens):
                    bad.append(f"[불변식8] {rel}:{lineno}: `npx` 호출에 --no-install 이 "
                               "없다 — 빌드 중 레지스트리에서 임의 버전을 받아 실행한다"
                               "(lock 핀 우회)")
                    continue
                target = next((t for t in tokens if not NPX_PKG_RE.match(t)), None)
                if target is None:
                    bad.append(f"[불변식9] {rel}:{lineno}: `npx` 실행 패키지를 찾지 못했다")
                elif target not in declared:
                    bad.append(f"[불변식9] {rel}:{lineno}: npx 가 실행하는 '{target}' 이 "
                               "package.json 에 선언돼 있지 않다 — 로컬 해소 실패 시 "
                               "전역 설치본(무핀)을 집는다")
    return bad, n_calls


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="npm 의존 핀·무결성 게이트 (A03)")
    ap.add_argument("--package", default=str(ROOT / PKG_REL))
    ap.add_argument("--lock", default=str(ROOT / LOCK_REL))
    ap.add_argument("--root", default=str(ROOT),
                    help="npx 호출 스캔 루트(self-test 가 temp 트리를 검사할 때 사용)")
    args = ap.parse_args(argv)

    pkg_path, lock_path = Path(args.package), Path(args.lock)
    for label, p in (("package.json", pkg_path), ("package-lock.json", lock_path)):
        if not p.is_file():
            print(f"\033[31m[FAIL]\033[0m [불변식1] {label} 이 없다: {p} — "
                  "npm 의존 핀이 통째로 사라졌다", file=sys.stderr)
            return 2
    try:
        pkg = json.loads(pkg_path.read_text(encoding="utf-8"))
        lock = json.loads(lock_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        print(f"\033[31m[FAIL]\033[0m [불변식1] JSON 파싱 실패: {exc}", file=sys.stderr)
        return 2
    if not isinstance(pkg, dict) or not isinstance(lock, dict):
        print("\033[31m[FAIL]\033[0m [불변식1] package.json/lock 최상위가 객체가 아니다",
              file=sys.stderr)
        return 2

    bad = check_lock(pkg, lock)
    npx_bad, n_npx = check_npx(pkg, collect_npx_sources(Path(args.root)))
    bad += npx_bad

    packages = lock.get("packages") if isinstance(lock.get("packages"), dict) else {}
    n_locked = max(len(packages) - 1, 0)
    n_declared = len(pkg.get("dependencies") or {}) + len(pkg.get("devDependencies") or {})
    print(f"[check-npm-lockfile] 선언 {n_declared} / 핀 {n_locked} 항목 / "
          f"npx 호출 {n_npx}건 (lockfileVersion {lock.get('lockfileVersion')})")

    if bad:
        print("\033[31m[FAIL]\033[0m npm 공급망 핀 계약 위반:", file=sys.stderr)
        for b in bad:
            print(f"  - {b}", file=sys.stderr)
        print("       근거: OWASP Top 10:2025 A03 — "
              "docs/DEVELOPMENT_VERIFICATION_POLICY.md", file=sys.stderr)
        return 1

    print(f"\033[32m[PASS]\033[0m npm 의존 {n_locked}개 SRI 핀 + 레지스트리 단일 출처 + "
          f"npx --no-install {n_npx}건 (드리프트 0)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
