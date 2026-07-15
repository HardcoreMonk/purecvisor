#!/usr/bin/env python3
"""
AF-C4 — "소비 메서드 ⊆ 등록 route" 불변식 정적 게이트 (Stage 1).

CLI/TUI 리터럴 및 FE generic /rpc 소비 RPC 메서드가 dispatcher.c 의 g_rpc_routes
등록 집합에 포함되는지 검사한다. 미등록 메서드를 소비하면(=런타임 -32601 method
not found) 위반 목록을 출력하고 exit 1 → CI 에서 계약 파손을 구조적으로 가시화.

  등록 진리원 : src/api/dispatcher.c  g_hash_table_insert(g_rpc_routes, "x.y", ...)
  소비 CLI    : src/cli/purecvisorctl.c  purectl_send_request("x.y" ...)
  소비 TUI    : src/tui/purecvisortui.c  tui_send_request/send_async_rpc("x.y" ...)
  소비 FE     : ui/**/*.js  {jsonrpc:'2.0', method:'x.y', ...} (generic /rpc 만)

한계(문서화):
  - 문자열 리터럴만 검출한다. 변수로 조립한 동적 메서드명은 스킵.
  - FE 가 REST 경로(EP.*)로 소비하는 계약(404/오라우팅형)은 이 게이트로 못 잡는다 —
    그것은 Stage 2(선언적 라우트 테이블/openapi/E2E, 미구현)의 몫이다.

check_rbac_policies.py 의 ROUTE_RE 와 동일한 등록 추출 규칙을 재사용한다.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from rpc_extract import (
    ROUTE_RE, SPECIAL_RE, CLI_RE, TUI_RE, FE_RE_A, FE_RE_B,
    strip_comments, read, is_source_js,
)

REPO_ROOT = Path(__file__).resolve().parent.parent
DISPATCHER_C = REPO_ROOT / "src" / "api" / "dispatcher.c"
CLI_C = REPO_ROOT / "src" / "cli" / "purecvisorctl.c"
TUI_C = REPO_ROOT / "src" / "tui" / "purecvisortui.c"
UI_DIR = REPO_ROOT / "ui"


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
    add(TUI_RE.findall(strip_comments(read(TUI_C))), "tui")
    # 생성 산출물(*.bundle.js, bundle.js, sw.js)은 제외 — 소스(app.js + modules)만
    # 스캔해 stale 번들 중복/노이즈를 배제한다.
    js_files = [p for p in (list(UI_DIR.glob("*.js"))
                            + list((UI_DIR / "modules").glob("*.js")))
                if is_source_js(p)]
    for js in js_files:
        txt = strip_comments(read(js))
        methods = set(FE_RE_A.findall(txt)) | set(FE_RE_B.findall(txt))
        add((m for m in methods if "." in m), f"fe:{js.name}")

    missing = sorted(m for m in consumers if m not in registered)

    print(f"[check-rpc-consumers] 등록 route {len(registered)} / "
          f"소비 메서드 {len(consumers)}")
    if not missing:
        print("[PASS] 모든 소비 메서드가 등록 route 에 존재 (소비 ⊆ 등록)")
        return 0

    print(f"[FAIL] 미등록 소비 메서드 {len(missing)}건 "
          f"(런타임 -32601 method not found):", file=sys.stderr)
    for m in missing:
        print(f"  - {m}  (소비: {', '.join(sorted(consumers[m]))})", file=sys.stderr)
    print("  * 한계: 리터럴만 검출(동적 메서드명 스킵), FE REST 경로 소비는 "
          "Stage 2(미구현).", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
