#!/usr/bin/env python3
"""RPC param-key 계약 게이트 (Stage 2). 설계: docs/superpowers/specs/2026-07-11-...

3방 대조: ① registry.required == 핸들러 가드키(drift, 항상 FAIL)
          ② registry.required ⊆ 소비 전송키(missing, FAIL − baseline)
          ③ 소비 전송키 ⊆ known(ignored, WARN)
"""
import json, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from rpc_extract import (read, strip_comments, is_source_js,
    CLI_RE, TUI_RE, extract_method_to_fn, extract_fn_body,
    extract_handler_required, extract_consumer_sent)

ROOT = Path(__file__).resolve().parent.parent
CONTRACTS = ROOT / "contracts"
DISPATCHER = ROOT / "src" / "api" / "dispatcher.c"
HANDLER_GLOBS = ["src/modules/dispatcher/handler_*.c", "src/modules/network/network_manager.c",
                 "src/api/dispatcher.c"]
CLI_C = ROOT / "src" / "cli" / "purecvisorctl.c"
TUI_C = ROOT / "src" / "tui" / "purecvisortui.c"


def _norm_required(required):
    """registry.required 원소를 str 또는 frozenset(alias)로 정규화."""
    out = set()
    for r in required:
        out.add(frozenset(r) if isinstance(r, list) else r)
    return out


def _satisfied(req_item, sent: set) -> bool:
    if isinstance(req_item, frozenset):
        return bool(req_item & sent)   # alias-group: any-of
    return req_item in sent


def diff_method(method, spec, handler_req: set, sent_by_consumer: dict) -> dict:
    reg_req = _norm_required(spec.get("required", []))
    known = set()
    for r in reg_req:
        known |= (set(r) if isinstance(r, frozenset) else {r})
    known |= set(spec.get("optional", []))
    res = {"drift": None, "missing": set(), "ignored": set()}
    # ① drift: registry.required 집합 != 핸들러 가드키 집합 (keys_via 예외는 호출부에서 스킵)
    if handler_req is not None and handler_req != reg_req:
        res["drift"] = (reg_req, handler_req)
    # ② missing / ③ ignored
    for consumer, sent in sent_by_consumer.items():
        # str/alias 통합 표현
        miss = frozenset(x for r in reg_req if not _satisfied(r, sent)
                         for x in ([r] if isinstance(r, str) else ["|".join(sorted(r))]))
        if miss:
            res["missing"].add((consumer, miss))
        for k in sent - known:
            res["ignored"].add((consumer, k))
    return res


def _load_baseline():
    b = json.loads((CONTRACTS / "rpc_param_baseline.json").read_text())
    return {(e["method"], e["consumer"], frozenset(e["missing_required"]))
            for e in b["known_consumer_mismatches"]}


def main() -> int:
    registry = json.loads((CONTRACTS / "rpc_params.json").read_text())
    baseline = _load_baseline()
    disp = read(DISPATCHER)
    m2fn = extract_method_to_fn(disp)
    handler_srcs = {}
    for g in HANDLER_GLOBS:
        for p in ROOT.glob(g):
            handler_srcs[p] = read(p)

    def handler_required(method):
        fn = m2fn.get(method)
        if not fn:
            return None
        for src in handler_srcs.values():
            body = extract_fn_body(src, fn)
            if body:
                return extract_handler_required(body)
        return None

    cli_sent = extract_consumer_sent(read(CLI_C), CLI_RE)
    tui_sent = extract_consumer_sent(read(TUI_C), TUI_RE)

    fails, warns = [], []
    for method, spec in registry.items():
        if "keys_via" in spec:
            hreq = None  # 헬퍼 위임 — drift 검사 스킵(어노테이션 예외)
        elif method not in m2fn:
            # 레지스트리가 참조하나 dispatcher에 등록/핸들러가 없음 — 죽은 참조(spec §5 row1)
            fails.append(f"DEAD-METHOD {method}: 레지스트리 참조하나 dispatcher 등록/핸들러 없음")
            hreq = None
        else:
            hreq = handler_required(method)
        sent = {}
        if method in cli_sent: sent["cli"] = cli_sent[method]
        if method in tui_sent: sent["tui"] = tui_sent[method]
        r = diff_method(method, spec, hreq, sent)
        if r["drift"]:
            fails.append(f"DRIFT {method}: registry.required={sorted(map(str,r['drift'][0]))} "
                         f"!= 핸들러={sorted(map(str,r['drift'][1]))}")
        for consumer, miss in r["missing"]:
            if (method, consumer, miss) not in baseline:
                fails.append(f"MISSING {method}[{consumer}]: 필수 {sorted(miss)} 미전송 (-32602)")
        for consumer, k in r["ignored"]:
            warns.append(f"IGNORED {method}[{consumer}]: '{k}' 핸들러 미read (거짓성공 가능)")

    print(f"[check-rpc-param-contract] registry {len(registry)} / "
          f"baseline {len(baseline)} / FAIL {len(fails)} / WARN {len(warns)}")
    for w in warns:
        print(f"  WARN: {w}")
    if fails:
        print(f"[FAIL] param-key 계약 위반 {len(fails)}건 (baseline 외 신규):", file=sys.stderr)
        for f in fails:
            print(f"  - {f}", file=sys.stderr)
        return 1
    print("[PASS] 신규 param-key 계약 위반 없음")
    return 0


if __name__ == "__main__":
    sys.exit(main())
