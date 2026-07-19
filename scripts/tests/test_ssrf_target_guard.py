
"""check_ssrf_target_guard.py self-test (Wave B Item 5-a / A10·V4).

① 현행 트리에서 게이트 PASS(exit 0) + 3 사이트 호출 + 헬퍼 정의 인식.
② 로직: file_calls_helper 가 호출 유무를 정확히 판정(주석/문자열 오탐 없음).
③ 반사실(temp 사본): alert_engine.c 에서 pcv_url_target_allowed 호출을 제거한 사본을
   단일 파일 모드로 검사하면 게이트 exit 1. 추적 소스는 절대 건드리지 않는다.
"""
import os
import sys
import tempfile
import subprocess
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from check_ssrf_target_guard import (
    file_calls_helper, helper_defined, find_missing_calls, ROOT, TARGET_RELS, HELPER,
)

GATE = Path(__file__).resolve().parent.parent / "check_ssrf_target_guard.py"
ALERT = ROOT / "src" / "modules" / "daemons" / "alert_engine.c"

def _write_tmp(text: str) -> str:
    f = tempfile.NamedTemporaryFile("w", suffix=".c", delete=False)
    f.write(text)
    f.close()
    return f.name

def test_current_tree_pass():
    """① 게이트 PASS(exit 0)."""
    r = subprocess.run([sys.executable, str(GATE)], capture_output=True, text=True)
    assert r.returncode == 0, f"{r.stdout}\n{r.stderr}"

def test_three_sites_call_helper():
    """① 정본 3 사이트 모두 헬퍼 호출."""
    assert find_missing_calls([ROOT / rel for rel in TARGET_RELS]) == []

def test_helper_defined():
    """① 헬퍼 정의 존재(src/utils/)."""
    assert helper_defined()

def test_call_detected():
    """② 실제 호출은 검출된다."""
    tmp = _write_tmp('void f(void){ if (!pcv_url_target_allowed(url, &e)) return; }\n')
    try:
        assert file_calls_helper(Path(tmp))
    finally:
        os.unlink(tmp)

def test_comment_string_not_counted():
    """② 주석/문자열 속 토큰은 호출로 세지 않는다."""
    tmp = _write_tmp('/* pcv_url_target_allowed(x) */\n'
                     'const char *s = "pcv_url_target_allowed(y)";\n')
    try:
        assert not file_calls_helper(Path(tmp))
    finally:
        os.unlink(tmp)

def test_reverted_source_fails():
    """③ 반사실: alert_engine.c 에서 pcv_url_target_allowed 호출 라인을 제거한 사본을
    단일 파일 모드로 검사하면 게이트 exit 1."""
    orig = ALERT.read_text()
    assert HELPER in orig, "alert_engine.c 에서 헬퍼 호출을 찾지 못함 — 소스 변경?"

    reverted = orig.replace("pcv_url_target_allowed", "_ssrf_removed_call")
    tmp = _write_tmp(reverted)
    try:
        r = subprocess.run([sys.executable, str(GATE), tmp],
                           capture_output=True, text=True)
        assert r.returncode == 1, f"되돌린 사본에서 게이트가 RED가 아님:\n{r.stdout}\n{r.stderr}"
    finally:
        os.unlink(tmp)

if __name__ == "__main__":
    tests = [v for k, v in sorted(globals().items())
             if k.startswith("test_") and callable(v)]
    failed = 0
    for t in tests:
        try:
            t()
            print(f"OK   {t.__name__}")
        except AssertionError as e:
            failed += 1
            print(f"FAIL {t.__name__}: {e}")
    print(f"[test_ssrf_target_guard] {len(tests) - failed}/{len(tests)} passed")
    sys.exit(1 if failed else 0)
