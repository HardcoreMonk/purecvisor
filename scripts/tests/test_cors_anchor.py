#!/usr/bin/env python3
"""check_cors_anchor.py self-test (Wave A / A05·V3·V13).

① 현행 트리에서 게이트 PASS(exit 0).
② 반사실: CORS 블록을 예전 substring 휴리스틱으로 되돌리면 게이트 RED.
   - 로직 단위: scan_text가 예전 블록 스니펫에서 금지 패턴을 잡는다.
   - 실파일 단위: 실제 rest_server.c의 _cors_origin_allowed 호출을
     strstr(origin, "://192.168.") 로 되돌리면 게이트가 exit 1.
③ 정당한 scheme 구분자 strstr(origin, "://")는 오탐하지 않는다.
"""
import os
import sys
import tempfile
import subprocess
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from check_cors_anchor import scan_text, TARGET  # noqa: E402

GATE = Path(__file__).resolve().parent.parent / "check_cors_anchor.py"

# 시정 전(예전) CORS 블록 — 이 스니펫은 반드시 위반으로 잡혀야 한다.
OLD_BLOCK = '''
        if (!origin || origin[0] == '\\0') {
            allow = TRUE;
        } else if (strstr(origin, "://localhost") ||
                   strstr(origin, "://127.0.0.1") ||
                   strstr(origin, "://192.168.") ||
                   strstr(origin, "://10.")) {
            allow = TRUE;
        } else {
            const gchar *host = soup_message_headers_get_one(reqh, "Host");
            if (host && strstr(origin, host)) allow = TRUE;
        }
'''

# 시정 후(현행) 스니펫 — 금지 패턴 없음 + 헬퍼 사용.
NEW_BLOCK = '''
static gboolean _cors_origin_allowed(const gchar *origin, const gchar *host_hdr) {
    const gchar *sep = strstr(origin, "://");
    if (!sep) return FALSE;
    return g_strcmp0(origin, host_hdr) == 0;
}
if (origin && _cors_origin_allowed(origin, host_hdr)) { emit(); }
'''


def test_old_block_flagged():
    """② 반사실(로직): 예전 substring 휴리스틱 → 금지 패턴 검출(≥1)."""
    fails, _ = scan_text(OLD_BLOCK)
    assert len(fails) >= 1, "예전 substring 블록이 게이트에 잡히지 않음"
    joined = " ".join(desc for _, desc, _ in fails)
    assert "substring" in joined


def test_new_block_clean():
    """③ 현행 스니펫: 금지 패턴 없음 + 헬퍼 사용(strstr(origin, \"://\")는 허용)."""
    fails, has_helper = scan_text(NEW_BLOCK)
    assert fails == [], f"현행 스니펫이 오탐: {fails}"
    assert has_helper


def test_scheme_sep_not_flagged():
    """정당한 scheme 구분자 탐색만으로는 위반이 아니다."""
    fails, _ = scan_text('const gchar *sep = strstr(origin, "://");')
    assert fails == []


def test_host_substring_flagged():
    """origin ⊇ host 반사 substring 비교는 위반."""
    fails, _ = scan_text('if (host && strstr(origin, host)) allow = TRUE;')
    assert len(fails) == 1


def test_gate_passes_on_current_tree():
    """① 현행 트리에서 게이트 PASS(exit 0)."""
    r = subprocess.run([sys.executable, str(GATE)], capture_output=True, text=True)
    assert r.returncode == 0, f"{r.stdout}\n{r.stderr}"


def test_reverted_source_fails():
    """② 반사실(temp 사본): 실제 rest_server.c의 _cors_origin_allowed 호출을
    substring 휴리스틱으로 되돌린 사본에서 게이트가 exit 1.
    추적 소스는 절대 건드리지 않는다(경로 오버라이드로 temp 사본만 검사)."""
    orig = TARGET.read_text()
    needle = "_cors_origin_allowed(origin, host_hdr)"
    assert needle in orig, "CORS 호출 사이트를 찾지 못함 — 소스 포맷 변경?"
    reverted = orig.replace(needle, 'strstr(origin, "://192.168.")', 1)
    with tempfile.NamedTemporaryFile("w", suffix=".c", delete=False) as f:
        f.write(reverted)
        tmp = f.name
    try:
        r = subprocess.run([sys.executable, str(GATE), tmp], capture_output=True, text=True)
        assert r.returncode == 1, f"되돌린 사본에서 게이트가 RED가 아님:\n{r.stdout}\n{r.stderr}"
    finally:
        os.unlink(tmp)


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
    print(f"[test_cors_anchor] {len(tests) - failed}/{len(tests)} passed")
    sys.exit(1 if failed else 0)
