#!/usr/bin/env python3
                          
                                                                             
                                                                                      
                                                                                                                            
 
                      
                                                                                                                                      
                                                

                                                                   
                                                                 
                                                                    
   

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CHAIN_TARGET = ROOT / "src/modules/audit/pcv_audit_chain.c"
LIFECYCLE_TARGET = ROOT / "src/modules/audit/pcv_audit.c"
MAIN_TARGET = ROOT / "src/main.c"
REST_TARGET = ROOT / "src/api/rest_server.c"
TELEMETRY_TARGET = ROOT / "src/modules/daemons/ebpf_telemetry.c"


def analyze(chain: str, lifecycle: str, main: str, rest: str,
            telemetry: str) -> dict[str, bool]:
                                                                 
    return {
        "begin_immediate": "BEGIN IMMEDIATE" in chain,
        "db_head_read": bool(re.search(
            r"SELECT\s+rec_hash\s+FROM\s+audit_log\s+WHERE\s+chain_epoch=\?",
            chain, re.IGNORECASE | re.DOTALL)),
        "chain_insert": bool(re.search(
            r"INSERT\s+INTO\s+audit_log\s*\([^)]*prev_hash[^)]*rec_hash"
            r"[^)]*chain_epoch",
            chain.replace('"\n        "', ""), re.IGNORECASE | re.DOTALL)),
        "unique_predecessor": "CREATE UNIQUE INDEX IF NOT EXISTS idx_audit_epoch_prev" in chain,
        "production_hash": "pcv_audit_chain_record_hash(" in chain,
        "production_verify": bool(re.search(
            r"gboolean\s+pcv_audit_chain_verify\s*\(", chain)),
        "epoch_metadata": "audit_chain_epoch" in chain and "predecessor_status" in chain,
        "legacy_not_rewritten": "chain_epoch IS NULL" in chain,
        "retention_checkpoint": (
            "audit_chain_checkpoint" in chain
            and "pcv_audit_chain_retention(" in chain
        ),
        "rollback_paths": chain.count("_rollback(db);") >= 8,
        "lifecycle_append": "pcv_audit_chain_append(" in lifecycle,
        "lifecycle_prepare": "pcv_audit_chain_prepare(" in lifecycle,
        "sidecar_lock": all(token in lifecycle for token in (
            'g_strconcat(db_path, ".lock"', "flock(G.lock_fd, LOCK_EX | LOCK_NB)"
        )),
        "main_fail_closed": all(token in main for token in (
            "if (!pcv_audit_init(", "audit subsystem failed closed", "return 1;"
        )),
        "health_wiring": all(token in rest for token in (
            '"audit_chain"', "pcv_audit_get_chain_health()",
            "audit.current_ok", "audit.historical_break"
        )),
        "metric_wiring": all(token in telemetry for token in (
            '"purecvisor_audit_chain_ok"',
            '"purecvisor_audit_chain_historical_break"',
            "pcv_audit_get_chain_health()"
        )),
    }


MESSAGES = {
    "begin_immediate": "BEGIN IMMEDIATE 부재 — head 조회와 append 원자성 상실",
    "db_head_read": "active epoch DB head 조회 부재 — process-local stale head 위험",
    "chain_insert": "prev_hash/rec_hash/chain_epoch INSERT 계약 누락",
    "unique_predecessor": "epoch predecessor 부분 UNIQUE 인덱스 누락",
    "production_hash": "production 공유 hash 함수 부재",
    "production_verify": "production epoch 검증 함수 부재",
    "epoch_metadata": "epoch predecessor 상태 metadata 계약 누락",
    "legacy_not_rewritten": "legacy chain_epoch NULL 보존 경로 누락",
    "retention_checkpoint": "retention checkpoint 계약 누락",
    "rollback_paths": "append/migration/retention rollback 경로 수축",
    "lifecycle_append": "audit worker가 production atomic append를 호출하지 않음",
    "lifecycle_prepare": "audit init이 production schema/verify 준비를 호출하지 않음",
    "sidecar_lock": "<db>.lock 비차단 단일 데몬 잠금 계약 누락",
    "main_fail_closed": "audit init 실패 시 listener 전 main 기동 중단 계약 누락",
    "health_wiring": "/health checks.audit_chain current/historical 배선 누락",
    "metric_wiring": "audit chain current/historical Prometheus gauge 배선 누락",
}


def failures(signals: dict[str, bool]) -> list[str]:
    return [message for key, message in MESSAGES.items() if not signals.get(key)]


def main(argv: list[str]) -> int:
                                                         
    targets = [CHAIN_TARGET, LIFECYCLE_TARGET, MAIN_TARGET,
               REST_TARGET, TELEMETRY_TARGET]
    for index, value in enumerate(argv[1:]):
        if index >= len(targets):
            print("ERROR: override 대상은 최대 5개", file=sys.stderr)
            return 2
        targets[index] = Path(value)
    missing = [str(path) for path in targets if not path.exists()]
    if missing:
        print(f"ERROR: 대상 파일 미존재: {', '.join(missing)}", file=sys.stderr)
        return 2

    signals = analyze(*(path.read_text(errors="replace") for path in targets))
    failed = failures(signals)
    passed = sum(signals.values())
    print(f"[check-audit-hashchain] ADR-0034 계약 {passed}/{len(signals)}")
    if failed:
        print("\033[31m[FAIL]\033[0m 감사 해시체인 계약 위반:", file=sys.stderr)
        for message in failed:
            print(f"  - {message}", file=sys.stderr)
        return 1
    print("\033[32m[PASS]\033[0m 원자 append·epoch·retention·fail-closed 계약 확인")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
