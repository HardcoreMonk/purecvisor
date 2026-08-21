#!/usr/bin/env python3
                          
                                                                            
                                                                                      
                                                                                                                            
 
                      
                                                                                                                                     
                                                                                      

    
                                                             
                                                                   
                                                    
                                                         
                                                           
                                                                    

                                                             
                                                                
              

    
                                                                              
                                                                 
                                                    
                    

     
                                                           
                                                               
                                                                    
                                                           
                                        

     
                                                  
                                          
                                                                         

              
                                              
                                                                                             
                                                                        
                                                       
                

       
                    
          
                        
   
from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MANIFEST_REL = "contracts/vendor_assets.json"
VENDOR_REL = "ui/vendor"

REQUIRED_KEYS = ("path", "sha256", "bytes", "component", "version", "license", "source")
                                                       
NULLABLE_KEYS = ("version", "license")


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def scan_vendor(vendor_dir: Path) -> list[Path]:
                                           

                                                 
                                                 
                                      
       
    files = []
    for p in sorted(vendor_dir.rglob("*")):
        if p.is_symlink() or not p.is_file():
            continue
        files.append(p)
    return files


def find_symlinks(vendor_dir: Path) -> list[Path]:
    return [p for p in sorted(vendor_dir.rglob("*")) if p.is_symlink()]


def build_actual(root: Path, vendor_dir: Path, previous: dict | None) -> dict:
                                              

                                                                           
       
    carry = {}
    if previous:
        for entry in previous.get("assets", []):
            if isinstance(entry, dict) and isinstance(entry.get("path"), str):
                carry[entry["path"]] = entry

    assets = []
    for p in scan_vendor(vendor_dir):
        rel = p.relative_to(root).as_posix()
        old = carry.get(rel, {})
        assets.append({
            "path": rel,
            "sha256": sha256_of(p),
            "bytes": p.stat().st_size,
            "component": old.get("component"),
            "version": old.get("version"),
            "license": old.get("license"),
            "source": old.get("source"),
        })
    doc = {}
    if previous:
        for k, v in previous.items():
            if k != "assets":
                doc[k] = v
    doc["assets"] = assets
    return doc


def check(root: Path, manifest: dict, vendor_dir: Path) -> list[str]:
    bad: list[str] = []

    assets = manifest.get("assets")
    if not isinstance(assets, list) or not assets:
        return ["[불변식1] 매니페스트에 비어있지 않은 assets 배열이 없다 — "
                "벤더 핀이 통째로 사라졌다"]

                                                       
    seen: set[str] = set()
    entries: dict[str, dict] = {}
    for i, entry in enumerate(assets):
        if not isinstance(entry, dict):
            bad.append(f"[불변식1] assets[{i}] 가 객체가 아니다")
            continue
        missing = [k for k in REQUIRED_KEYS if k not in entry]
        if missing:
            bad.append(f"[불변식1] assets[{i}] 에 필수 키 누락: {', '.join(missing)}")
            continue
        rel = entry["path"]
        if not isinstance(rel, str) or not rel:
            bad.append(f"[불변식2] assets[{i}].path 가 문자열이 아니다")
            continue
        if rel.startswith("/") or ".." in Path(rel).parts:
            bad.append(f"[불변식2] {rel}: 절대경로 또는 '..' 탈출 — 핀 우회 가능")
            continue
        if not rel.startswith(VENDOR_REL + "/"):
            bad.append(f"[불변식2] {rel}: {VENDOR_REL}/ 하위가 아니다")
            continue
        if rel in seen:
            bad.append(f"[불변식2] {rel}: 매니페스트에 중복 등재 — 어느 핀이 유효한지 모호")
            continue
        seen.add(rel)
        for k in REQUIRED_KEYS:
            if k in NULLABLE_KEYS:
                continue
            if entry[k] is None or entry[k] == "":
                bad.append(f"[불변식1] {rel}: '{k}' 가 비어 있다")
        if not isinstance(entry["sha256"], str) or len(entry["sha256"]) != 64:
            bad.append(f"[불변식1] {rel}: sha256 이 64자 hex 가 아니다")
        if not isinstance(entry["bytes"], int) or entry["bytes"] < 0:
            bad.append(f"[불변식1] {rel}: bytes 가 음이 아닌 정수가 아니다")
        entries[rel] = entry

                                                       
    if not vendor_dir.is_dir():
        bad.append(f"[불변식3] 벤더 디렉토리가 없다: {vendor_dir}")
        return bad

    actual = {p.relative_to(root).as_posix(): p for p in scan_vendor(vendor_dir)}
    for rel in sorted(actual):
        if rel not in entries:
            bad.append(f"[불변식3] {rel}: 매니페스트에 없는 벤더 자산 — "
                       "무단 반입이거나 갱신 누락(--print-actual 로 재생성 후 출처 기재)")

    for p in find_symlinks(vendor_dir):
        bad.append(f"[불변식3] {p.relative_to(root).as_posix()}: 벤더 디렉토리의 심링크 — "
                   "트리 밖 바이트를 가리켜 핀을 우회할 수 있다")

                                                       
    for rel in sorted(entries):
        entry = entries[rel]
        p = actual.get(rel)
        if p is None:
            bad.append(f"[불변식4] {rel}: 매니페스트에 있으나 파일이 없다 — "
                       "삭제됐거나 경로가 바뀌었다")
            continue
        size = p.stat().st_size
        if isinstance(entry["bytes"], int) and size != entry["bytes"]:
            bad.append(f"[불변식5] {rel}: 크기 {size}B ≠ 핀 {entry['bytes']}B")
        digest = sha256_of(p)
        if isinstance(entry["sha256"], str) and digest != entry["sha256"]:
            bad.append(f"[불변식5] {rel}: SHA-256 불일치 "
                       f"(실제 {digest[:16]}… ≠ 핀 {entry['sha256'][:16]}…)")

    return bad


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="벤더링 자산 무결성 핀 게이트 (A03)")
    ap.add_argument("--manifest", default=str(ROOT / MANIFEST_REL))
    ap.add_argument("--vendor-dir", default=str(ROOT / VENDOR_REL))
    ap.add_argument("--root", default=str(ROOT),
                    help="path 계산 기준 루트(self-test 가 temp 트리를 검사할 때 사용)")
    ap.add_argument("--print-actual", action="store_true",
                    help="현재 트리 상태로 매니페스트를 재생성해 stdout 에 출력한다")
    args = ap.parse_args(argv)

    root = Path(args.root).resolve()
    vendor_dir = Path(args.vendor_dir).resolve()
    manifest_path = Path(args.manifest)

    previous = None
    if manifest_path.is_file():
        try:
            previous = json.loads(manifest_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            if args.print_actual:
                previous = None
            else:
                print(f"\033[31m[FAIL]\033[0m 매니페스트를 읽을 수 없다 "
                      f"({manifest_path}): {exc}", file=sys.stderr)
                return 2
    elif not args.print_actual:
        print(f"\033[31m[FAIL]\033[0m 벤더 매니페스트가 없다: {manifest_path} — "
              "벤더 자산 핀이 통째로 제거됐다", file=sys.stderr)
        return 2

    if args.print_actual:
        if not vendor_dir.is_dir():
            print(f"벤더 디렉토리가 없다: {vendor_dir}", file=sys.stderr)
            return 2
        doc = build_actual(root, vendor_dir, previous)
        print(json.dumps(doc, indent=2, ensure_ascii=False))
        return 0

    if not isinstance(previous, dict):
        print("\033[31m[FAIL]\033[0m 매니페스트 최상위가 객체가 아니다", file=sys.stderr)
        return 2

    bad = check(root, previous, vendor_dir)
    n_pinned = len(previous.get("assets") or [])
    total_bytes = sum(e.get("bytes", 0) for e in (previous.get("assets") or [])
                      if isinstance(e, dict) and isinstance(e.get("bytes"), int))
    print(f"[check-vendor-integrity] 핀 {n_pinned}개 자산 / {total_bytes}B 대조 "
          f"({VENDOR_REL})")

    if bad:
        print("\033[31m[FAIL]\033[0m 벤더 자산 무결성 계약 위반:", file=sys.stderr)
        for b in bad:
            print(f"  - {b}", file=sys.stderr)
        print("       정당한 업스트림 갱신이라면: "
              "python3 scripts/check_vendor_integrity.py --print-actual "
              "> contracts/vendor_assets.json (출처 기재 후 커밋)", file=sys.stderr)
        return 1

    print(f"\033[32m[PASS]\033[0m 벤더 자산 {n_pinned}개 SHA-256 핀 일치 "
          "(전수 등재 · 유령 없음 · 심링크 없음)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
