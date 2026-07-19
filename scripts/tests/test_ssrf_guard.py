#!/usr/bin/env python3
"""check_ssrf_guard.py self-test (Wave A / A10·V4).

① 현행 트리에서 게이트 PASS(exit 0) + 대상 2곳 인식.
② 반사실: 아웃바운드 사이트에서 SOUP_MESSAGE_NO_REDIRECT를 제거하면 게이트 RED.
   - 로직 단위: find_unguarded_in_text가 무가드 사이트를 잡는다.
   - 실파일 단위: alert_engine.c에서 플래그를 제거하면 게이트 exit 1.
③ 주석/문자열 속 토큰은 오탐/누락하지 않는다.
"""
import os
import sys
import tempfile
import subprocess
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from check_ssrf_guard import find_unguarded_in_text, scan_tree, ROOT, WINDOW  # noqa: E402

GATE = Path(__file__).resolve().parent.parent / "check_ssrf_guard.py"
ALERT = ROOT / "src" / "modules" / "daemons" / "alert_engine.c"

GUARDED = '''
    SoupMessage *msg = soup_message_new("POST", url);
    if (!msg) return FALSE;
    soup_message_set_flags(msg, SOUP_MESSAGE_NO_REDIRECT);
'''

UNGUARDED = '''
    SoupMessage *msg = soup_message_new("POST", url);
    if (!msg) return FALSE;
    GBytes *b = g_bytes_new(payload, strlen(payload));
'''


def test_guarded_ok():
    """가드된 사이트는 무가드로 잡히지 않는다."""
    assert find_unguarded_in_text("fake.c", GUARDED) == []


def test_unguarded_flagged():
    """② 반사실(로직): NO_REDIRECT 없는 사이트 → 무가드로 검출."""
    hits = find_unguarded_in_text("fake.c", UNGUARDED)
    assert len(hits) == 1 and hits[0][0] == "fake.c"


def test_comment_and_string_ignored():
    """③ 주석/문자열 속 토큰은 세지 않는다(오탐 방지)."""
    txt = ('/* soup_message_new("POST", x) in comment */\n'
           'const char *s = "soup_message_new(fake)";\n')
    assert find_unguarded_in_text("fake.c", txt) == []


def test_flag_out_of_window():
    """WINDOW 밖의 NO_REDIRECT는 가드로 인정되지 않는다."""
    body = 'SoupMessage *msg = soup_message_new("POST", url);\n'
    body += '\n' * (WINDOW + 2)
    body += 'soup_message_set_flags(msg, SOUP_MESSAGE_NO_REDIRECT);\n'
    hits = find_unguarded_in_text("fake.c", body)
    assert len(hits) == 1


def test_current_tree_three_sites_guarded():
    """① 현행 트리: 아웃바운드 3곳(webhook POST 2 + update-check GitHub GET 1), 무가드 0곳."""
    total, unguarded = scan_tree()
    assert total == 3, f"아웃바운드 사이트 수 예상 3, 실제 {total}"
    assert unguarded == [], f"무가드 사이트 존재: {unguarded}"


def test_gate_passes_on_current_tree():
    """① 게이트 PASS(exit 0)."""
    r = subprocess.run([sys.executable, str(GATE)], capture_output=True, text=True)
    assert r.returncode == 0, f"{r.stdout}\n{r.stderr}"


def test_reverted_source_fails():
    """② 반사실(temp 사본): alert_engine.c에서 NO_REDIRECT 라인을 제거한 사본을
    단일 파일 모드로 검사하면 게이트가 exit 1. 추적 소스는 절대 건드리지 않는다."""
    orig = ALERT.read_text()
    needle = "    soup_message_set_flags(msg, SOUP_MESSAGE_NO_REDIRECT);\n"
    assert needle in orig, "alert_engine.c의 NO_REDIRECT 라인을 찾지 못함 — 소스 포맷 변경?"
    reverted = orig.replace(needle, "", 1)
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
    print(f"[test_ssrf_guard] {len(tests) - failed}/{len(tests)} passed")
    sys.exit(1 if failed else 0)
