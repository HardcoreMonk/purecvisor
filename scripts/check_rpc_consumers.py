#!/usr/bin/env python3
                          
                                                                       
                                                                                      
                                                                                                                            
 
                      
                                                                                                                                
   
                                                   

                                                          
                                                       
                                                 
                                                        
                                                               
                                                                            

                                                                              
                              
                                                                        
                                                                           
                                                                         
                                                                                      
                                                                                   
                                                                              

        
                                                              
                                                          
                                                

                                                        
   
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from rpc_extract import (
    ROUTE_RE, SPECIAL_RE, CLI_RE, SECREQ_RE, FE_RE_A, FE_RE_B,
    extract_rest_methods, extract_fe_helper, extract_grpc_methods,
    extract_test_consumed,
    strip_comments, read, is_source_js,
)

REPO_ROOT = Path(__file__).resolve().parent.parent
DISPATCHER_C = REPO_ROOT / "src" / "api" / "dispatcher.c"
CLI_C = REPO_ROOT / "src" / "cli" / "purecvisorctl.c"
UI_DIR = REPO_ROOT / "ui"
REST_C = REPO_ROOT / "src" / "api" / "rest_server.c"
GRPC_C = REPO_ROOT / "src" / "api" / "grpc_server.c"
TESTS_DIR = REPO_ROOT / "tests"
ORPHAN_BASELINE = REPO_ROOT / "contracts" / "rpc_orphan_baseline.json"


def main() -> int:
    if not DISPATCHER_C.exists():
        print(f"ERROR: {DISPATCHER_C} 미존재", file=sys.stderr)
        return 2

    disp = read(DISPATCHER_C)
    registered = set(ROUTE_RE.findall(disp)) | set(SPECIAL_RE.findall(disp))
    if not registered:
        print("ERROR: 등록 route 0건 — dispatcher.c 파싱 실패", file=sys.stderr)
        return 2

    consumers: dict[str, set[str]] = {}

    def add(methods, label):
        for m in methods:
            consumers.setdefault(m, set()).add(label)

    add(CLI_RE.findall(strip_comments(read(CLI_C))), "cli")
    add(SECREQ_RE.findall(strip_comments(read(CLI_C))), "cli")                           
                                                                       
                                
    js_files = [p for p in (list(UI_DIR.glob("*.js"))
                            + list((UI_DIR / "modules").glob("*.js")))
                if is_source_js(p)]
    for js in js_files:
        txt = strip_comments(read(js))
        methods = set(FE_RE_A.findall(txt)) | set(FE_RE_B.findall(txt))
        add((m for m in methods if "." in m), f"fe:{js.name}")

                                                               
    add(extract_rest_methods(read(REST_C)), "rest")
                                               
    add(extract_grpc_methods(read(GRPC_C), registered), "grpc")
                                                         
    for js in js_files:
        add(extract_fe_helper(strip_comments(read(js))), f"fe-rpc:{js.name}")

    missing = sorted(m for m in consumers if m not in registered)

    print(f"[check-rpc-consumers] 등록 route {len(registered)} / "
          f"소비 메서드 {len(consumers)}")

    fail = False
    if missing:
        fail = True
        print(f"[FAIL] 미등록 소비 메서드 {len(missing)}건 "
              f"(런타임 -32601 method not found):", file=sys.stderr)
        for m in missing:
            print(f"  - {m}  (소비: {', '.join(sorted(consumers[m]))})", file=sys.stderr)
        print("  * 한계: 리터럴만 검출(동적 메서드명 스킵), FE REST 경로 소비는 "
              "Stage 2(미구현).", file=sys.stderr)

                                                  
    baseline_full = json.loads(ORPHAN_BASELINE.read_text())["orphans"]
    baseline = set(baseline_full)
    orphans = registered - set(consumers)
    new_orphans = sorted(orphans - baseline)
    stale_baseline = sorted(baseline - orphans)                      
    test_consumed = extract_test_consumed(TESTS_DIR, registered)
    test_covered = sorted(set(orphans) & test_consumed)
    print(f"[check-rpc-consumers] 고아 {len(orphans)} / baseline {len(baseline)} "
          f"(test-covered {len(test_covered)})")
    for m in test_covered:
        print(f"  TEST-COVERED: {m} (production 미배선이나 test 소비 — dead 아님)")
    for s in stale_baseline:
        print(f"  RATCHET: '{s}' 이제 소비됨 → baseline에서 제거 가능")
    if new_orphans:
        fail = True
        print(f"[FAIL] baseline 밖 신규 고아 {len(new_orphans)}건 "
              f"(등록됐으나 어떤 인터페이스로도 미소비 — 배선 누락 또는 baseline 주석 등재 필요):",
              file=sys.stderr)
        for m in new_orphans:
            print(f"  - ORPHAN {m}", file=sys.stderr)

                                                                   
    mislabeled = sorted(m for m in baseline
                        if baseline_full.get(m, {}).get("reason") == "dead-candidate"
                        and m in test_consumed)
    if mislabeled:
        fail = True
        print(f"[FAIL] dead-candidate 오분류 {len(mislabeled)}건 "
              f"(test가 소비 → dead 아님, reason 재분류 필요):", file=sys.stderr)
        for m in mislabeled:
            print(f"  - MISLABEL {m}", file=sys.stderr)

    if fail:
        return 1

    print("[PASS] 모든 소비 메서드가 등록 route 에 존재 (소비 ⊆ 등록) · 신규 고아 없음")
    return 0


if __name__ == "__main__":
    sys.exit(main())
