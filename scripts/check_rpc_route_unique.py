#!/usr/bin/env python3
                          
                                                       
                                                                                      
                                                                                                                            
 
                      
                                                                                                                
                                                                         
           

                                                               
                                                                          
                                                  
                                                          

                                                  

                                                         
                                                         
                                                        

          
                                              
                                                          

       
                                                         
                                                         
                                                     
                                                                
                                              
                                                       
                          
                                                            

   
                                                                                   
                                                                  
   
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DISPATCHER = ROOT / "src" / "api" / "dispatcher.c"
                                                     
                                                     
                                                
                                       
                                              
                           
ROUTE_FLOOR = 200
INSERT = re.compile(r'g_hash_table_insert\(\s*g_rpc_routes\s*,\s*"([a-z0-9_.]+)"')


def find_dups(text):
                                   
    seen = {}
    for m in INSERT.finditer(text):
        seen[m.group(1)] = seen.get(m.group(1), 0) + 1
    return {k: v for k, v in seen.items() if v > 1}


def run_check():
    text = DISPATCHER.read_text(encoding="utf-8", errors="replace")
    total = len(INSERT.findall(text))
    dups = find_dups(text)
    print(f"[check-rpc-route-unique] g_rpc_routes 등록 {total}건 검사")
                                                           
                                                     
                                                        
    if total < ROUTE_FLOOR:
        print(f"\n[FAIL] 등록을 {total}건밖에 못 찾았다(하한 {ROUTE_FLOOR}) — "
              "정규식이 낡았거나 라우트 등록이 다른 파일로 옮겨갔다. "
              "이 상태의 PASS 는 '위반 없음'이 아니라 '검사 못 함'이다.")
        return 1
    if dups:
        print("\n[FAIL] 같은 RPC 메서드가 두 번 이상 등록됐다 "
              "(g_hash_table_insert 는 replace — 나중 등록이 이긴다):")
        for k, v in sorted(dups.items()):
            print(f"  - {k}  ({v}회)")
        print("\n시정: 정본 구현 하나만 남기고 나머지 등록을 제거한다.")
        return 1
    print("[PASS] 중복 등록 없음")
    return 0


def self_test():
    ok = True
    dup = find_dups('g_hash_table_insert(g_rpc_routes, "a.b", (gpointer)f1);\n'
                    'g_hash_table_insert(g_rpc_routes, "a.b", (gpointer)f2);')
    if dup != {"a.b": 2}:
        print(f"[SELF-TEST FAIL] 중복을 못 잡았다: {dup}")
        ok = False
    uniq = find_dups('g_hash_table_insert(g_rpc_routes, "a.b", (gpointer)f1);\n'
                     'g_hash_table_insert(g_rpc_routes, "c.d", (gpointer)f2);')
    if uniq != {}:
        print(f"[SELF-TEST FAIL] 정상 등록을 중복으로 오판: {uniq}")
        ok = False
    print("[SELF-TEST PASS] 중복 판정 정확" if ok else "[SELF-TEST FAILED]")
    return 0 if ok else 1


def main():
    if "--self-test" in sys.argv:
        return self_test()
    return run_check()


if __name__ == "__main__":
    sys.exit(main())
