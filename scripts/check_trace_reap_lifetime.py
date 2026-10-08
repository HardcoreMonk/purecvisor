#!/usr/bin/env python3










from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOKENS = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', re.S)


def code(text: str) -> str:

    return TOKENS.sub(lambda m: "" if m[0].startswith(("/*", "//")) else m[0], text)


def body(text: str, name: str) -> str:

    match = re.search(rf'\b{re.escape(name)}\s*\([^;{{}}]*\)\s*\{{', text, re.S)
    require(match is not None, f"missing function {name}")
    start = match.end()
    depth = 1
    for token in re.finditer(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]', text[start:]):
        if token[0] == "{":
            depth += 1
        elif token[0] == "}":
            depth -= 1
            if depth == 0:
                return text[start:start + token.start()]
    raise ValueError(f"unclosed function {name}")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def check(root: Path = ROOT) -> None:
    trace = code((root / "src/modules/daemons/pcv_trace.c").read_text())
    rpc = code((root / "src/api/dispatcher.c").read_text())
    request = body(trace, "_trace_request_stop")
    stop = body(trace, "pcv_trace_stop")
    backstop = body(trace, "_trace_backstop_cb")
    wait = body(trace, "_trace_wait_done")
    final = body(trace, "_trace_finalize")
    status = body(trace, "pcv_trace_status")
    for name, text in (("request", request), ("stop", stop), ("backstop", backstop)):
        require(not re.search(r'\b(?:_trace_finalize|_trace_summarize|pcv_trace_release|'
                              r'g_subprocess_get_\w+|g_remove|g_unlink)\s*\(', text),
                f"{name} must retain completion resources until wait")
    require("_trace_request_stop(" in stop and "_trace_request_stop(" in backstop,
            "stop and backstop must use the request boundary")
    require(not re.search(r'\bg_subprocess_wait(?:_check)?\s*\(', trace),
            "production Trace must not block the main loop waiting for its child")
    require(re.search(r'gboolean\s+reaped\s*=\s*g_subprocess_wait_finish\(', wait),
            "wait_finish result must be checked")
    require(re.search(r'if\s*\(\s*reaped\s*&&\s*g_trace\s*&&\s*'
                      r'g_trace->proc\s*==\s*proc\s*&&\s*'
                      r'g_strcmp0\(g_trace->trace_id,\s*id\)\s*==\s*0', wait),
            "completion requires successful wait and matching process/id")
    require(re.search(r'g_trace->reaped\s*=\s*TRUE\s*;\s*_trace_finalize\(', wait),
            "only the confirmed wait path may authorize finalization")
    require(re.search(r'if\s*\([^)]*!g_trace->reaped[^)]*\)\s*return;', final),
            "finalization must reject unconfirmed wait")
    require(len(re.findall(r'\b_trace_finalize\s*\(', trace)) == 2,
            "wait must be the only caller of finalization")
    require('"stop_requested"' in status, "running status must expose stop_requested")
    stop_rpc = body(rpc, "_handle_debug_trace_stop")
    for field in ("stopped", "stop_requested"):
        require(re.search(r'json_object_set_boolean_member\(res,\s*"' + field + r'",\s*TRUE\)', stop_rpc),
                f"Trace stop RPC must preserve request ACK field {field}")


def main() -> int:
    try:
        check()
    except (OSError, ValueError) as exc:
        print(f"FAIL: Trace reap lifetime: {exc}", file=sys.stderr)
        return 1
    print("PASS: Trace wait-confirmed finalization and RPC request ACK wiring (ADR-0064)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
