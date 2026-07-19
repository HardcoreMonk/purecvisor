
"""dead-export 게이트 자기검증(인수) 테스트.

Task 1 단위테스트(test_dead_exports.py)와 별개로, 게이트가 현 트리에서
(1) SEC-1급(선언·정의만 있고 배선 0인 안전 함수) dead export를 탐지하고,
(2) 실사용 함수를 오탐하지 않으며,
(3) baseline 착지 상태에서 PASS함을 증명한다.

pytest 없이 `python3 scripts/tests/test_dead_exports_acceptance.py`로 자체 실행,
실패 시 비-0 종료.
"""
import sys
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "scripts"))
from check_dead_exports import find_dead

def test_sec1_class_detected_synthetic():
    """SEC-1급(배선 누락 안전 함수)의 dead export 탐지를 합성 입력으로 증명.

    실제 SEC-1 함수(pcv_rbac_session_is_revoked)는 이미 시정(삭제)되어 재현
    불가 → 동일 '클래스'를 in-memory로 재현: 헤더에 선언되고 .c에 정의만 있는
    pcv_* 안전 함수는 dead로 탐지되고, 배선(호출)된 동류 함수는 탐지되지 않는다.
    실소스는 건드리지 않는다(핸드오프 승인 편차: 원안 test_sec1_reproduced 대체).
    """
    header = "gboolean pcv_session_is_revoked(const gchar *sid);\n"

    c_wired = (
        "gboolean pcv_session_is_revoked(const gchar *sid){ return check(sid); }\n"
        "void enforce(const gchar *sid){ if (pcv_session_is_revoked(sid)) deny(); }\n"
    )
    assert "pcv_session_is_revoked" not in find_dead([header], [c_wired]), \
        "배선된 안전 함수를 dead로 오탐"

    c_unwired = (
        "gboolean pcv_session_is_revoked(const gchar *sid){ return check(sid); }\n"
        "void enforce(const gchar *sid){ /* revoke 검사 누락 */ allow(); }\n"
    )
    assert "pcv_session_is_revoked" in find_dead([header], [c_unwired]), \
        "SEC-1급 dead export 미탐지"

def _real_tree():
    headers = [p.read_text(errors="replace") for p in
               list((ROOT / "src").rglob("*.h")) + list((ROOT / "include").rglob("*.h"))]
    c_raw = [p.read_text(errors="replace") for p in (ROOT / "src").rglob("*.c")]
    return headers, c_raw

def test_handlers_not_false_flagged():
    """확실히 사용되는 pcv_ 함수(직접 호출/등록)는 dead로 오탐되지 않음."""
    headers, c_raw = _real_tree()
    dead = find_dead(headers, c_raw)

    assert "pcv_rbac_check_permission" not in dead, "실사용 함수 오탐(pcv_rbac_check_permission)"

    assert "pcv_rest_server_start" not in dead, "실사용 함수 오탐(pcv_rest_server_start)"

def test_gate_passes_at_baseline():
    """baseline 착지 상태에서 게이트가 신규 dead 0으로 PASS(exit 0)."""
    r = subprocess.run([sys.executable, str(ROOT / "scripts" / "check_dead_exports.py")],
                       capture_output=True, text=True)
    assert r.returncode == 0, f"게이트 미착지(exit={r.returncode}):\n{r.stdout}\n{r.stderr}"

if __name__ == "__main__":
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_") and callable(v)]
    failed = 0
    for t in tests:
        try:
            t()
            print(f"OK   {t.__name__}")
        except AssertionError as e:
            failed += 1
            print(f"FAIL {t.__name__}: {e}")
    print(f"[test_dead_exports_acceptance] {len(tests) - failed}/{len(tests)} passed")
    sys.exit(1 if failed else 0)
