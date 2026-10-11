#!/usr/bin/env python3





















from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WS_SERVER_C = ROOT / "src" / "api" / "ws_server.c"

FUNC_HEAD_RE = re.compile(
    r'^(?:static\s+)?(?:[A-Za-z_][\w\s\*]*?\s+\**)([A-Za-z_]\w*)\s*\(([^;{}]*)\)\s*\{',
    re.MULTILINE)
SEND_RE = re.compile(r'\bsoup_websocket_connection_send_text\s*\(')
CLIENT_ITER_RE = re.compile(r'g_ptr_array_index\s*\(\s*G\.clients\s*,')
GUARD_RE = re.compile(
    r'if\s*\(\s*!\s*_ws_conn_is_authed\s*\(\s*conn\s*\)\s*\)\s*'
    r'(?:\{[^{}]*?\bcontinue\s*;[^{}]*\}|[^;{}]*?\bcontinue\s*;)',
    re.DOTALL)


def strip_comments(text: str) -> str:

    def blank(match: re.Match) -> str:
        return re.sub(r'[^\n]', ' ', match.group(0))
    text = re.sub(r'/\*.*?\*/', blank, text, flags=re.DOTALL)
    return re.sub(r'//[^\n]*', blank, text)


def function_bodies(text: str) -> dict[str, tuple[int, str]]:

    bodies: dict[str, tuple[int, str]] = {}
    for match in FUNC_HEAD_RE.finditer(text):
        name = match.group(1)
        if name in {"if", "for", "while", "switch", "return", "sizeof"}:
            continue
        start = match.end() - 1
        depth = 0
        for pos in range(start, len(text)):
            if text[pos] == '{':
                depth += 1
            elif text[pos] == '}':
                depth -= 1
                if depth == 0:
                    bodies[name] = (start, text[start:pos + 1])
                    break
    return bodies


def check(text: str) -> list[str]:
    code = strip_comments(text)
    bodies = function_bodies(code)
    failures: list[str] = []

    fanouts = [name for name, (_, body) in bodies.items()
               if CLIENT_ITER_RE.search(body) and SEND_RE.search(body)]
    if "pcv_ws_broadcast" not in fanouts:
        failures.append("R2: pcv_ws_broadcast fan-out 루프를 찾지 못함 — 구조 변경 시 게이트를 갱신하라")

    for name in fanouts:
        offset, body = bodies[name]
        iter_at = CLIENT_ITER_RE.search(body).start()
        send = SEND_RE.search(body, iter_at)
        if not send:
            continue
        guard = GUARD_RE.search(body, iter_at, send.start())
        if not guard:
            line = code.count('\n', 0, offset + send.start()) + 1
            failures.append(
                f"R1: {name}() line {line}: G.clients fan-out send_text 앞에 "
                f"`if (!_ws_conn_is_authed(conn)) continue;` 가드가 없음")

    helper = bodies.get("_ws_conn_is_authed")
    if not helper:
        failures.append("R3: _ws_conn_is_authed() 정의가 없음")
    else:
        _, body = helper
        if '"pcv-ws-authed"' not in body or not re.search(r'==\s*1\b', body):
            failures.append('R3: _ws_conn_is_authed()가 "pcv-ws-authed" == 1 비교로 판정하지 않음')
    return failures


def main(argv: list[str] | None = None) -> int:
    args = sys.argv[1:] if argv is None else argv
    path = Path(args[0]) if args else WS_SERVER_C
    try:
        text = path.read_text(errors="replace")
    except OSError as exc:
        print(f"ERROR: {path} 읽기 실패: {exc}", file=sys.stderr)
        return 2
    failures = check(text)
    if failures:
        print("[FAIL] WebSocket fan-out 미인증 연결 차단 계약 위반 (ADR-0079):")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print("[PASS] WebSocket fan-out은 인증된 연결(pcv-ws-authed == 1)에만 전송")
    return 0


if __name__ == "__main__":
    sys.exit(main())
