#!/usr/bin/env python3
"""check_zpool_suspend_recover.py self-test.

① 현행 트리에서 게이트 PASS(exit 0).
② 반사실(로직 단위): 가공 스니펫에서 각 불변식 위반이 검출된다.
③ 반사실(실파일 temp 사본): 실제 소스의 다음 회귀를 되돌리면 게이트 exit 1.
   - SUSPENDED→4.0 을 →0.0 으로 되돌림 (원 버그)
   - 디바이스-읽기 가드 _zfs_vdev_readable(...) 제거
   - 서킷브레이커 pcv_zfs_recover_guard_allow(...) 제거
   - ebpf 매핑 pcv_zfs_pool_state_metric_val() 을 인라인 상수로 되돌림
   추적 소스는 절대 건드리지 않는다(temp 사본만 --zfs/--ebpf 로 검사).
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from check_zpool_suspend_recover import (  # noqa: E402
    check_mapping, check_ebpf_wiring, check_recover_guards,
    ROOT, ZFS_REL, EBPF_REL,
)

GATE = Path(__file__).resolve().parent.parent / "check_zpool_suspend_recover.py"
ZFS = ROOT / ZFS_REL
EBPF = ROOT / EBPF_REL


# ── ② 로직 단위 반사실 ────────────────────────────────────────
MAP_OK = 'gdouble f(const gchar *s){ if (g_strcmp0(s, "SUSPENDED") == 0) return 4.0; return 0.0; }'
MAP_BUG = 'gdouble f(const gchar *s){ if (g_strcmp0(s, "SUSPENDED") == 0) return 0.0; return 0.0; }'
MAP_MISSING = 'gdouble f(const gchar *s){ return 0.0; }'

# 매핑 함수명은 실제 이름이어야 extract_func_body 가 잡는다.
def _wrap_map(body_inner):
    return ("gdouble pcv_zfs_pool_state_metric_val(const gchar *state){ "
            + body_inner + " }")


def test_mapping_ok():
    ok, _ = check_mapping(_wrap_map('if (g_strcmp0(state, "SUSPENDED") == 0) return 4.0; return 0.0;'))
    assert ok


def test_mapping_zero_bug_flagged():
    ok, msg = check_mapping(_wrap_map('if (g_strcmp0(state, "SUSPENDED") == 0) return 0.0; return 0.0;'))
    assert not ok and "SUSPENDED" in msg


def test_mapping_missing_flagged():
    ok, _ = check_mapping(_wrap_map('return 0.0;'))
    assert not ok


def _wrap_recover(inner):
    return ("PcvZfsRecoverResult pcv_zfs_pool_recover_suspended(const gchar *p, ZfsRecoverGuard *g){ "
            + inner + " }")


RECOVER_OK = _wrap_recover(
    'if (!have_dev || !_zfs_vdev_readable(dev)) return PCV_ZFS_RECOVER_DEV_UNREADABLE; '
    'if (g && !pcv_zfs_recover_guard_allow(g, now, w, 3)) return PCV_ZFS_RECOVER_CB_TRIPPED; '
    'const gchar *argv[] = {"zpool", "clear", p, NULL}; return PCV_ZFS_RECOVER_CLEARED;'
)


def test_recover_ok():
    ok, _ = check_recover_guards(RECOVER_OK)
    assert ok


def test_recover_no_device_guard_flagged():
    inner = _wrap_recover(
        'if (g && !pcv_zfs_recover_guard_allow(g, now, w, 3)) return PCV_ZFS_RECOVER_CB_TRIPPED; '
        'const gchar *argv[] = {"zpool", "clear", p, NULL}; return PCV_ZFS_RECOVER_CLEARED;'
    )
    ok, msg = check_recover_guards(inner)
    assert not ok and "_zfs_vdev_readable" in msg


def test_recover_no_circuit_breaker_flagged():
    inner = _wrap_recover(
        'if (!have_dev || !_zfs_vdev_readable(dev)) return PCV_ZFS_RECOVER_DEV_UNREADABLE; '
        'const gchar *argv[] = {"zpool", "clear", p, NULL}; return PCV_ZFS_RECOVER_CLEARED;'
    )
    ok, msg = check_recover_guards(inner)
    assert not ok and "guard_allow" in msg


def test_recover_clear_before_guard_flagged():
    inner = _wrap_recover(
        'const gchar *argv[] = {"zpool", "clear", p, NULL}; '
        'if (!have_dev || !_zfs_vdev_readable(dev)) return PCV_ZFS_RECOVER_DEV_UNREADABLE; '
        'if (g && !pcv_zfs_recover_guard_allow(g, now, w, 3)) return PCV_ZFS_RECOVER_CB_TRIPPED; '
        'return PCV_ZFS_RECOVER_CLEARED;'
    )
    ok, _ = check_recover_guards(inner)
    assert not ok


def test_ebpf_wiring_missing_recover_flagged():
    ok, _ = check_ebpf_wiring('x = pcv_zfs_pool_state_metric_val(zh.state); /* no recover */')
    assert not ok


# ── ① 현행 트리 PASS ──────────────────────────────────────────
def test_gate_passes_on_current_tree():
    r = subprocess.run([sys.executable, str(GATE)], capture_output=True, text=True)
    assert r.returncode == 0, f"{r.stdout}\n{r.stderr}"


# ── ③ 실파일 temp 사본 반사실 ─────────────────────────────────
def _run_gate(zfs_path, ebpf_path):
    return subprocess.run(
        [sys.executable, str(GATE), "--zfs", str(zfs_path), "--ebpf", str(ebpf_path)],
        capture_output=True, text=True,
    )


def _temp_with(src: Path, needle: str, repl: str) -> str:
    orig = src.read_text()
    assert needle in orig, f"needle 미발견(소스 포맷 변경?): {needle}"
    with tempfile.NamedTemporaryFile("w", suffix=".c", delete=False) as f:
        f.write(orig.replace(needle, repl, 1))
        return f.name


def test_reverted_mapping_fails():
    tmp = _temp_with(ZFS,
                     'if (g_strcmp0(state, "SUSPENDED") == 0) return 4.0;',
                     'if (g_strcmp0(state, "SUSPENDED") == 0) return 0.0;')
    try:
        r = _run_gate(tmp, EBPF)
        assert r.returncode == 1, f"매핑 0 회귀에서 RED 아님:\n{r.stdout}\n{r.stderr}"
    finally:
        os.unlink(tmp)


def test_removed_device_guard_fails():
    tmp = _temp_with(ZFS, '!have_dev || !_zfs_vdev_readable(dev)', '!have_dev')
    try:
        r = _run_gate(tmp, EBPF)
        assert r.returncode == 1, f"디바이스 가드 제거에서 RED 아님:\n{r.stdout}\n{r.stderr}"
    finally:
        os.unlink(tmp)


def test_removed_circuit_breaker_fails():
    tmp = _temp_with(ZFS,
                     'guard && !pcv_zfs_recover_guard_allow(guard, now_us, window_us, max_attempts)',
                     '0')
    try:
        r = _run_gate(tmp, EBPF)
        assert r.returncode == 1, f"서킷브레이커 제거에서 RED 아님:\n{r.stdout}\n{r.stderr}"
    finally:
        os.unlink(tmp)


def test_reverted_ebpf_inline_fails():
    tmp = _temp_with(EBPF, 'pcv_zfs_pool_state_metric_val(zh.state)', '0.0')
    try:
        r = _run_gate(ZFS, tmp)
        assert r.returncode == 1, f"ebpf 인라인 회귀에서 RED 아님:\n{r.stdout}\n{r.stderr}"
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
    print(f"[test_zpool_suspend_recover] {len(tests) - failed}/{len(tests)} passed")
    sys.exit(1 if failed else 0)
