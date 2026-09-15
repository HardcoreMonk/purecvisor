#!/usr/bin/env python3
                          
                                                           
                                                                                      
                                                                                                                            
 
                      
                                                                                                                    
                                                                    
                                                                
   
import json
import re, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BASELINE_FILE = ROOT / "scripts" / "dead_exports_baseline.txt"
WAIVERS_FILE = ROOT / "contracts" / "dead_export_waivers.json"
WAIVER = "PCV_DEAD_EXPORT_OK"
DECL_RE = re.compile(r'\b(pcv_[a-z0-9_]+)\s*\(')


def _splice_continuations(text: str) -> str:
                                                               

                                                    
                                              
                                          
                                                  
                                                       
                          
       
    lines = text.split('\n')
    out = []
    buf = ''
    for line in lines:
        if line.endswith('\\'):
            buf += line[:-1]
        else:
            out.append(buf + line)
            buf = ''
    if buf:                                                
        out.append(buf)
    return '\n'.join(out)


def strip_code(text: str) -> str:
                           

                                      
                                                     
                                                                  
                                                  
                                                    
                                                  
                                                 
                                                      
       
    text = _splice_continuations(text)
    out = []
    in_block = False
    for line in text.split('\n'):
        res = []
        i, n = 0, len(line)
        while i < n:
            ch = line[i]
            if in_block:                                                  
                if ch == '*' and i + 1 < n and line[i + 1] == '/':
                    in_block = False
                    i += 2
                else:
                    i += 1
                continue
            if ch == '/' and i + 1 < n and line[i + 1] == '*':            
                in_block = True
                i += 2
                continue
            if ch == '/' and i + 1 < n and line[i + 1] == '/':                 
                break
            if ch == '"' or ch == "'":                                             
                quote = ch
                i += 1
                while i < n:
                    if line[i] == '\\':                                          
                        i += 2
                        continue
                    if line[i] == quote:
                        i += 1
                        break
                    i += 1
                res.append(' ')
                continue
            res.append(ch)
            i += 1
        out.append(''.join(res))
    return '\n'.join(out)


def collect_declared(header_texts) -> set:
    names = set()
    for t in header_texts:
                                                               
                                                             
        names.update(DECL_RE.findall(strip_code(t)))
    return names


def count_uses(name: str, c_texts_stripped) -> int:
    pat = re.compile(r'\b' + re.escape(name) + r'\b')
    return sum(len(pat.findall(t)) for t in c_texts_stripped)


def _waived(name: str, c_texts_raw) -> bool:
    pat = re.compile(r'\b' + re.escape(name) + r'\b')
    for t in c_texts_raw:
        for m in pat.finditer(t):
            if WAIVER in t[max(0, m.start() - 200): m.start() + 50]:
                return True
    return False


def find_dead(header_texts, c_texts_raw) -> set:
    declared = collect_declared(header_texts)
    stripped = [strip_code(t) for t in c_texts_raw]
    dead = set()
    for name in declared:
        if count_uses(name, stripped) == 1 and not _waived(name, c_texts_raw):
            dead.add(name)                               
    return dead


def _load_baseline() -> set:
    if not BASELINE_FILE.exists():
        return set()
    return {ln.strip() for ln in BASELINE_FILE.read_text().splitlines()
            if ln.strip() and not ln.lstrip().startswith("#")}


def validate_waivers(entries, declared, dead, evidence_texts) -> set:
    if not isinstance(entries, dict):
        raise ValueError("예외 계약은 심볼별 객체여야 한다")
    for name, spec in entries.items():
        if not re.fullmatch(r"pcv_[a-z0-9_]+", name) or name not in declared or name not in dead:
            raise ValueError(f"현재 선언·dead 후보와 일치하지 않는 예외: {name}")
        if not isinstance(spec, dict) or set(spec) != {"reason", "evidence"}:
            raise ValueError(f"예외 계약 필드 오류: {name}")
        if not isinstance(spec["reason"], str) or not spec["reason"].strip():
            raise ValueError(f"예외 사유 누락: {name}")
        paths = spec["evidence"]
        if not isinstance(paths, list) or not paths:
            raise ValueError(f"예외 근거 누락: {name}")
        for path in paths:
            if not isinstance(path, str) or path not in evidence_texts:
                raise ValueError(f"공개 코드 근거 없음: {name}: {path}")
            if not count_uses(name, [strip_code(evidence_texts[path])]):
                raise ValueError(f"근거 파일의 실제 심볼 참조 없음: {name}: {path}")
    return set(entries)


def main() -> int:
    header_paths = list((ROOT / "src").rglob("*.h")) + list((ROOT / "include").rglob("*.h"))
    source_paths = list((ROOT / "src").rglob("*.c"))
    evidence = {str(p.relative_to(ROOT)): p.read_text(errors="replace")
                for p in header_paths + source_paths + list((ROOT / "tests").rglob("*.c"))}
    headers = [evidence[str(p.relative_to(ROOT))] for p in header_paths]
    c_raw = [evidence[str(p.relative_to(ROOT))] for p in source_paths]
    dead = find_dead(headers, c_raw)
    try:
        entries = json.loads(WAIVERS_FILE.read_text())
        waived = validate_waivers(entries, collect_declared(headers), dead, evidence)
    except (OSError, ValueError) as error:
        print(f"[FAIL] 공개 dead export 예외 계약: {error}", file=sys.stderr)
        return 1
    dead -= waived
    baseline = _load_baseline()
    new_dead = sorted(dead - baseline)
    stale = sorted(baseline - dead)
    print(f"[check-dead-exports] dead export 후보 {len(dead)} / baseline {len(baseline)}")
    print(f"[check-dead-exports] 공개 근거 검증 예외 {len(waived)}")
    if stale:
        print(f"[INFO] baseline {len(stale)}건 이제 사용됨(시정) — baseline에서 제거 권장: "
              f"{', '.join(stale[:10])}")
    if new_dead:
        print(f"[FAIL] 신규 dead export {len(new_dead)}건 (헤더 선언·사용처0):", file=sys.stderr)
        for n in new_dead:
            print(f"  - {n}  (배선 or 삭제; 의도적이면 contracts/dead_export_waivers.json 근거 등록)", file=sys.stderr)
        return 1
    print("[PASS] 신규 dead export 없음")
    return 0


if __name__ == "__main__":
    sys.exit(main())
