#!/usr/bin/env python3
























from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_SOURCE = ROOT / "src" / "modules" / "dispatcher" / "handler_snapshot.c"


class ExtractError(Exception):
    pass


def strip_comments(text: str) -> str:

    out: list[str] = []
    i, n = 0, len(text)
    state = "code"
    quote = ""
    while i < n:
        ch = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if state == "code":
            if ch == "/" and nxt == "/":
                state = "line"
                out.append("  ")
                i += 2
                continue
            if ch == "/" and nxt == "*":
                state = "block"
                out.append("  ")
                i += 2
                continue
            if ch in ('"', "'"):
                state = "literal"
                quote = ch
            out.append(ch)
            i += 1
        elif state == "literal":
            out.append(ch)
            if ch == "\\" and i + 1 < n:
                out.append(text[i + 1])
                i += 2
                continue
            if ch == quote or ch == "\n":
                state = "code"
            i += 1
        elif state == "line":
            if ch == "\n":
                state = "code"
                out.append("\n")
            else:
                out.append(" ")
            i += 1
        else:
            if ch == "*" and nxt == "/":
                state = "code"
                out.append("  ")
                i += 2
                continue
            out.append("\n" if ch == "\n" else " ")
            i += 1
    return "".join(out)


def function_body(code: str, name: str) -> str:

    match = re.search(rf"\b{re.escape(name)}\s*\([^;{{]*\)\s*\{{", code)
    if not match:
        raise ExtractError(f"missing definition of {name}()")
    depth = 0
    start = match.end() - 1
    for pos in range(start, len(code)):
        if code[pos] == "{":
            depth += 1
        elif code[pos] == "}":
            depth -= 1
            if depth == 0:
                return code[start + 1:pos]
    raise ExtractError(f"unbalanced braces in {name}()")


def block_after(code: str, anchor: str) -> str | None:

    idx = code.find(anchor)
    if idx < 0:
        return None
    brace = code.find("{", idx)
    if brace < 0:
        return None
    depth = 0
    for pos in range(brace, len(code)):
        if code[pos] == "{":
            depth += 1
        elif code[pos] == "}":
            depth -= 1
            if depth == 0:
                return code[brace + 1:pos]
    return None


def _first(body: str, token: str) -> int:
    idx = body.find(token)
    return idx if idx >= 0 else len(body) + 1


def check_text(text: str) -> list[str]:

    code = strip_comments(text)
    fails: list[str] = []

    worker = function_body(code, "_rollback_worker")
    exists_at = worker.find("_zfs_snapshot_exists(")
    power_at = min(_first(worker, "virDomainShutdown("), _first(worker, "virDomainDestroy("))
    if exists_at < 0:
        fails.append("① _rollback_worker 가 _zfs_snapshot_exists() 로 스냅샷 존재를 확인하지 않는다")
    elif exists_at > power_at:
        fails.append("① _rollback_worker 가 스냅샷 존재 확인 전에 VM 을 종료/destroy 한다")
    elif "g_task_return_new_error(" not in worker[exists_at:power_at]:
        fails.append("① 스냅샷 미존재 시 VM 제어 전에 실패 반환(g_task_return_new_error)이 없다")

    failure_block = block_after(worker, "if (!zfs_ok)")
    if failure_block is None or "virDomainCreate(" not in failure_block:
        fails.append("② ZFS 롤백 실패 블록이 끈 VM 을 재기동(virDomainCreate)하지 않는다")

    fallback = function_body(code, "_libvirt_snapshot_rollback")
    lookup_at = fallback.find("virDomainSnapshotLookupByName(")
    shutdown_at = _first(fallback, "virDomainShutdown(")
    if lookup_at < 0 or lookup_at > shutdown_at:
        fails.append("③ libvirt 폴백이 스냅샷 조회 전에 VM 을 종료한다")

    error_responses = fallback.count("pure_rpc_build_error_response(")
    fail_audits = len(re.findall(r'pcv_audit_log\([^;]*"vm\.snapshot\.rollback"[^;]*"fail"', fallback))
    if fail_audits < error_responses:
        fails.append(f"④ libvirt 폴백 오류 응답 {error_responses}건 중 fail audit 은 {fail_audits}건뿐이다")
    return fails


def main(argv: list[str]) -> int:
    source = Path(argv[1]) if len(argv) > 1 else DEFAULT_SOURCE
    try:
        text = source.read_text(encoding="utf-8")
    except OSError as exc:
        print(f"ERROR: failed to read {source}: {exc}", file=sys.stderr)
        return 2
    try:
        fails = check_text(text)
    except ExtractError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2
    if fails:
        print("[FAIL] snapshot rollback 선확인 계약 위반:", file=sys.stderr)
        for reason in fails:
            print(f"  - {reason}", file=sys.stderr)
        return 1
    print("[PASS] snapshot rollback: 존재 확인→전원 제어 순서·실패 재기동·폴백 audit 계약 유지")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
