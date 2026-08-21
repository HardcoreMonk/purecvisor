#!/usr/bin/env python3
                          
                                                                             
                                                                                      
                                                                                                                            
 
                      
                                                                                                                                      
                                                                      
                                                                                    

              
                                                                 
                                               
                                                                   
                                                              
                                                   
                                                       
                                                     
   
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BASELINE_FILE = ROOT / "scripts" / "error_codes_baseline.txt"
ENUM_DEF_FILE = "src/modules/dispatcher/rpc_utils.h"                          

RAW_RE = re.compile(r'-32[0-9]{3}\b')
PCV_ERR_RE = re.compile(r'\bPCV_ERR_[A-Z][A-Z0-9_]*\b')


def strip_code(text: str) -> str:
                                                          

                                                               
                                                      
                                                 
                                                       
                                                    
                                       
       
    out = []
    i, n = 0, len(text)
    in_block = False
    in_line = False
    while i < n:
        ch = text[i]
        if in_line:                                                       
            if ch == '\n':
                in_line = False
                out.append('\n')
            else:
                out.append(' ')
            i += 1
            continue
        if in_block:                                                    
            if ch == '*' and i + 1 < n and text[i + 1] == '/':
                out.append('  ')
                i += 2
                in_block = False
            else:
                out.append('\n' if ch == '\n' else ' ')
                i += 1
            continue
        if ch == '/' and i + 1 < n and text[i + 1] == '*':             
            in_block = True
            out.append('  ')
            i += 2
            continue
        if ch == '/' and i + 1 < n and text[i + 1] == '/':             
            in_line = True
            out.append('  ')
            i += 2
            continue
        if ch == '"' or ch == "'":                                 
            quote = ch
            out.append(' ')
            i += 1
            while i < n:
                c2 = text[i]
                if c2 == '\\' and i + 1 < n:                                    
                    out.append(' \n' if text[i + 1] == '\n' else '  ')
                    i += 2
                    continue
                if c2 == quote:
                    out.append(' ')
                    i += 1
                    break
                out.append('\n' if c2 == '\n' else ' ')
                i += 1
            continue
        out.append(ch)
        i += 1
    return ''.join(out)


def find_raw_literals_in_text(rel_path: str, text: str) -> list:
                                                                  
    stripped = strip_code(text)
    ids = []
    for i, line in enumerate(stripped.split('\n'), start=1):
        if RAW_RE.search(line):
            ids.append(f"{rel_path}:{i}")
    return ids


def find_pcv_err_in_text(rel_path: str, text: str) -> list:
                                                       
                                                            
    if rel_path == ENUM_DEF_FILE:
        return []
    stripped = strip_code(text)
    return [f"{rel_path}:{m.group()}" for m in PCV_ERR_RE.finditer(stripped)]


def _load_baseline() -> set:
    if not BASELINE_FILE.exists():
        return set()
    return {ln.strip() for ln in BASELINE_FILE.read_text().splitlines()
            if ln.strip() and not ln.lstrip().startswith("#")}


def main() -> int:
    c_files = sorted((ROOT / "src").rglob("*.c"))
    raw_ids, pcv_err_hits = [], []
    for p in c_files:
        rel = str(p.relative_to(ROOT))
        text = p.read_text(errors="replace")
        raw_ids.extend(find_raw_literals_in_text(rel, text))
        pcv_err_hits.extend(find_pcv_err_in_text(rel, text))

    baseline = _load_baseline()
    new_raw = sorted(set(raw_ids) - baseline)
    stale = sorted(baseline - set(raw_ids))

    print(f"[check-error-codes] raw -32xxx 리터럴 {len(raw_ids)} / baseline {len(baseline)} / "
          f"PCV_ERR_ 재도입 {len(pcv_err_hits)}")
    if stale:
        print(f"[INFO] baseline {len(stale)}건 이제 정리됨 — baseline에서 제거 권장: "
              f"{', '.join(stale[:10])}")

    fails = []
    if new_raw:
        fails.append(f"신규 raw 에러코드 리터럴 {len(new_raw)}건 (PURE_RPC_ERR_* 상수 사용 필요)")
    if pcv_err_hits:
        fails.append(f"PCV_ERR_ 재도입 {len(pcv_err_hits)}건 (canonical PURE_RPC_ERR_*만 허용)")

    if fails:
        print(f"[FAIL] 에러코드 계약 위반 {len(fails)}건:", file=sys.stderr)
        for rid in new_raw:
            print(f"  - raw 리터럴: {rid}", file=sys.stderr)
        for h in pcv_err_hits:
            print(f"  - PCV_ERR_ 재도입: {h}", file=sys.stderr)
        return 1
    print("[PASS] raw -32xxx 리터럴 신규 없음(baseline 밖) + PCV_ERR_ 재도입 0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
