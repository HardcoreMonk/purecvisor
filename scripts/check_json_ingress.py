#!/usr/bin/env python3
                          
                                                                       
                                                                                      
                                                                                                                            
 
                      
                                                                                                                                
                                             
                                                           
                                                                            
   
import re, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
INGRESS = ["src/api/ws_server.c", "src/api/uds_server.c", "src/api/grpc_server.c",
           "src/api/dispatcher.c", "src/api/rest_server.c"]
RAW_RE = re.compile(r'\bjson_parser_load_from_data\s*\(')
WAIVER = "PCV_PARSE_TRUSTED"
BASELINE_FILE = ROOT / "scripts" / "json_ingress_baseline.txt"


def _strip_block_comments(text: str) -> str:
                                               
                                       
    return re.sub(r'/\*.*?\*/',
                  lambda m: '\n' * m.group().count('\n'),
                  text, flags=re.DOTALL)


                        
                                                                 
                                                                    
                                                          
                                                        
def count_violations(text: str) -> int:
    orig = text.splitlines()
                                                       
                                                                     
                          
    stripped = _strip_block_comments(text).splitlines()
    viol = 0
    for i, sline in enumerate(stripped):
        code = re.sub(r'//.*', '', sline)                    
        if not RAW_RE.search(code):                                     
            continue
        line = orig[i]
        prev = orig[i - 1] if i > 0 else ""
        if WAIVER in line or WAIVER in prev:                                 
            continue
        viol += 1
    return viol


def main() -> int:
    total = 0
    per_file = {}
    for rel in INGRESS:
        p = ROOT / rel
        if not p.exists():
            continue
        v = count_violations(p.read_text(errors="replace"))
        per_file[rel] = v
        total += v
    baseline = int(BASELINE_FILE.read_text().strip()) if BASELINE_FILE.exists() else 0
    print(f"[check-json-ingress] 경계 파일 raw 파싱(waiver 제외) {total} / baseline {baseline}")
    for rel, v in per_file.items():
        if v:
            print(f"    {rel}: {v}")
    if total > baseline:
        print(f"[FAIL] 무가드 파싱 {total} > baseline {baseline} — "
              f"신규 raw json_parser_load_from_data. 래퍼(pcv_rpc_parse_guarded) 경유 또는 "
              f"waiver(PCV_PARSE_TRUSTED) 필요.", file=sys.stderr)
        return 1
    if total < baseline:
        print(f"[INFO] 위반 {total} < baseline {baseline} — baseline을 {total}로 낮추세요(래칫).")
    print("[PASS] 신규 무가드 경계 파싱 없음")
    return 0


if __name__ == "__main__":
    sys.exit(main())
