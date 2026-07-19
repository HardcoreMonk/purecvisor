
"""ADR-0018 audit 배치 게이트(check_audit_placement.py) self-test.

핵심 회귀 방지: WS completion regex 가 워커 스레드 마샬링 변형
`pcv_ws_broadcast_job_complete_mt`(A2-2 libsoup 어피니티, 커밋 09d66ae)를
인식해야 한다. `_mt` 를 놓치면 backup.restore/replicate/export_s3·vm.clone·
vm.export.ova·vm.import.ova·vm.resize_disk·vm.disk.live_resize 처럼 `_mt`로만
완료를 브로드캐스트하는 fire-and-forget 메서드가 위음성(거짓 "누락")으로 잡혀
게이트가 거짓 실패한다.

실행: python3 -m pytest scripts/tests/test_audit_placement.py -q
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import check_audit_placement as gate

def test_ws_regex_matches_plain_variant():
    """메인컨텍스트 직결 pcv_ws_broadcast_job_complete 는 여전히 매칭."""
    s = 'pcv_ws_broadcast_job_complete(job_id, "vm.start", "completed", NULL);'
    m = gate.WS_COMPLETE_RE.search(s)
    assert m is not None
    assert m.group(1) == "vm.start"

def test_ws_regex_matches_mt_variant():
    """워커 마샬링 변형 _mt 도 매칭해야 한다 (regex 정밀화의 핵심).

    반사실: regex 에서 `(?:_mt)?` 를 제거하면 이 assert 가 RED 가 된다
    (`_mt(` 의 `_` 가 `\\s*\\(` 와 불일치 → search 실패).
    """
    for s in (
        'pcv_ws_broadcast_job_complete_mt(job_id, "backup.restore", "failed", err);',
        'pcv_ws_broadcast_job_complete_mt(job_id, "vm.clone", "completed", NULL);',
        'pcv_ws_broadcast_job_complete_mt(ctx->job_id, "vm.export.ova",\n'
        '    "completed", NULL);',
    ):
        m = gate.WS_COMPLETE_RE.search(s)
        assert m is not None, f"_mt 변형 미매칭: {s!r}"
    assert gate.WS_COMPLETE_RE.search(
        'pcv_ws_broadcast_job_complete_mt(job_id, "backup.restore", "failed", err);'
    ).group(1) == "backup.restore"

def test_ws_regex_does_not_overmatch_unrelated():
    """유사하지만 무관한 이름은 잡지 않는다 (과매칭 방지)."""
    s = 'pcv_ws_broadcast("anomaly", payload);'
    assert gate.WS_COMPLETE_RE.search(s) is None

def test_audit_regex_matches_log_and_rpc():
    """audit regex 는 두 함수명 변형을 인식하고 첫 문자열 리터럴을 잡는다.

    lazy `[^)]*?` 이므로 group(1)=괄호 뒤 첫 문자열: worker-result 관례
    `pcv_audit_log(NULL, "method", ...)` 는 method 를, rpc 변형은 첫 인자를 잡는다.
    """
    m_worker = gate.AUDIT_CALL_RE.search(
        'pcv_audit_log(NULL, "backup.restore", vm, d, 0, 0, "x");')
    assert m_worker is not None and m_worker.group(1) == "backup.restore"

    m_rpc = gate.AUDIT_CALL_RE.search('pcv_audit_log_rpc("vm.clone", params);')
    assert m_rpc is not None and m_rpc.group(1) == "vm.clone"

def test_mt_only_methods_recognized_in_tree():
    """실 소스 스캔: `_mt`로만 완료를 쏘는 required 메서드가 WS set 에 포함."""
    ws = gate.collect_methods(gate.WS_COMPLETE_RE)
    for m in ("backup.restore", "backup.replicate", "vm.clone",
              "vm.export.ova", "vm.import.ova"):
        assert m in ws, f"{m} 이 WS completion set 에 없음 (regex 위음성 회귀)"

def test_gate_passes_on_current_tree():
    """현행 트리에서 게이트가 0(PASS)로 통과해야 한다 (거짓 실패 회귀 방지)."""
    assert gate.main() == 0

if __name__ == "__main__":
    sys.exit(__import__("pytest").main([__file__, "-q"]))
