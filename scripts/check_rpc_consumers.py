#!/usr/bin/env python3
"""
AF-C4 — "소비 메서드 ⊆ 등록 route" 불변식 + 고아(등록−소비) 정적 게이트.

소비 RPC 메서드가 dispatcher.c 의 g_rpc_routes 등록 집합에 포함되는지 검사한다.
미등록 메서드를 소비하면(=런타임 -32601 method not found) 위반 목록을 출력하고
exit 1 → CI 에서 계약 파손을 구조적으로 가시화. 역방향으로, 등록만 되고 어떤
경로로도 소비되지 않는 고아(등록−소비)는 rpc_orphan_baseline.json 로만 허용하며
신규 고아·baseline 축소·dead-candidate 오분류(test-covered 를 dead 로 표기)를
FAIL 시킨다(ADR-0025 반사실 — self-test 는 tests/test_rpc_consumers_acceptance.py).

  등록 진리원 : src/api/dispatcher.c  g_hash_table_insert(g_rpc_routes, "x.y", ...)
  소비 추출(rpc_extract.py, 전 경로):
    - CLI 리터럴        purectl_send_request("x.y" ...)            (CLI_RE)
    - CLI security 래퍼 security_request("x.y" ...)               (SECREQ_RE)
    - FE generic /rpc   {jsonrpc:'2.0', method:'x.y'} · rpc()/_rpc('x.y')
    - FE REST 브릿지    rest_server.c _build_rpc("x.y" ...)        (extract_rest_methods)
    - gRPC              grpc_server.c 의 등록된 dotted 리터럴       (extract_grpc_methods)
    - tests/            테스트가 소비하는 메서드(test-covered 라벨) (extract_test_consumed)

한계(문서화):
  - 문자열 리터럴만 검출한다. 변수로 조립한 동적 메서드명은 baseline dynamic-cli 로 허용.
  - grpc 과잉매칭: grpc_server.c 의 우연한 dotted 문자열이 등록 메서드와 겹치면
    거짓 소비로 계산될 수 있다(현재 그런 사례 없음, informational).

check_rbac_policies.py 의 ROUTE_RE 와 동일한 등록 추출 규칙을 재사용한다.
"""
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
    add(SECREQ_RE.findall(strip_comments(read(CLI_C))), "cli")   # security_request 래퍼 소비
    # 생성 산출물(*.bundle.js, bundle.js, sw.js)은 제외 — 소스(app.js + modules)만
    # 스캔해 stale 번들 중복/노이즈를 배제한다.
    js_files = [p for p in (list(UI_DIR.glob("*.js"))
                            + list((UI_DIR / "modules").glob("*.js")))
                if is_source_js(p)]
    for js in js_files:
        txt = strip_comments(read(js))
        methods = set(FE_RE_A.findall(txt)) | set(FE_RE_B.findall(txt))
        add((m for m in methods if "." in m), f"fe:{js.name}")

    # REST 브릿지 소비 (rest_server.c의 _build_rpc / _build_rpc_name)
    add(extract_rest_methods(read(REST_C)), "rest")
    # gRPC 명시 매핑 소비 (등록 route에 속하는 dotted 리터럴만)
    add(extract_grpc_methods(read(GRPC_C), registered), "grpc")
    # Web UI passthrough 헬퍼 소비 (rpc('x.y')/EP.RPC('x.y'))
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

    # ── 고아 불변식 (ADR-0025): 등록 − 전소비 ⊆ baseline ──
    baseline_full = json.loads(ORPHAN_BASELINE.read_text())["orphans"]
    baseline = set(baseline_full)
    orphans = registered - set(consumers)
    new_orphans = sorted(orphans - baseline)
    stale_baseline = sorted(baseline - orphans)   # 이제 소비됨 → 래칫 하향 후보
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

    # ── ADR-0025 정직 self-check: dead-candidate는 test-covered 불가 ──
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
