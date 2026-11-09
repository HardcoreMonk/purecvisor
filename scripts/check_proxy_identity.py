#!/usr/bin/env python3
                          
                                                             
                                                                                      
                                                                                                                            
 
                      
                                                                                                                      
                                       

                                                
                                              
   

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rpc_extract

ROOT = Path(__file__).resolve().parent.parent
TARGET_REL = "src/api/rest_server.c"
TARGET = ROOT / TARGET_REL
WS_TARGET_REL = "src/api/ws_server.c"
WS_TARGET = ROOT / WS_TARGET_REL
FORBIDDEN = "soup_server_message_get_remote_address"
ACCESSOR = "pcv_rest_client_identity_get"
IDENTITY_REL = "src/api/rest_client_identity.c"
IDENTITY_TARGET = ROOT / IDENTITY_REL

COLLECTOR_PATHS = ("/health/recent-errors", "/internal/vms", "/internal/cluster/vms",
                   "/internal/telemetry", "/metrics")

PUBLIC_PATHS = frozenset({
    "/health/recent-errors", "/health", "/version", "/update-check",
    "/internal/vms", "/internal/cluster/vms", "/internal/telemetry", "/metrics",
    "/auth/token", "/auth/refresh", "/auth/logout", "/auth/logout/all", "/auth/password", "/auth/register",
    "/auth/totp/verify", "/auth/totp/enroll",
})
GATE_CALL_RE = r"\s*if\s*\(\s*!\s*pcv_rest_collector_gate\s*\(\s*msg\s*\)\s*\)\s*return\s+PCV_REST_STEP_CLEANUP\s*;"
AUTH_GATE_RE = r"\bpcv_rest_authenticate\s*\(\s*msg\s*,\s*&\s*key_role\s*(?:,\s*&\s*subject_role\s*)?\)"


def strip_comments(text: str) -> str:
                                         
    out: list[str] = []
    i = 0
    in_block = in_line = False
    quote: str | None = None
    while i < len(text):
        ch = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""
        if in_line:
            out.append("\n" if ch == "\n" else " ")
            in_line = ch != "\n"
            i += 1
            continue
        if in_block:
            if ch == "*" and nxt == "/":
                out.extend((" ", " "))
                i += 2
                in_block = False
            else:
                out.append("\n" if ch == "\n" else " ")
                i += 1
            continue
        if quote is not None:
            out.append(ch)
            if ch == "\\" and nxt:
                out.append(nxt)
                i += 2
                continue
            if ch == quote:
                quote = None
            i += 1
            continue
        if ch == "/" and nxt == "/":
            out.extend((" ", " "))
            i += 2
            in_line = True
            continue
        if ch == "/" and nxt == "*":
            out.extend((" ", " "))
            i += 2
            in_block = True
            continue
        if ch in ("'", '"'):
            quote = ch
        out.append(ch)
        i += 1
    return "".join(out)


def mask_preprocessor(text: str) -> str:
                                                       
    lines = text.splitlines(keepends=True)
    out: list[str] = []
    continuation = False
    for line in lines:
        directive = continuation or line.lstrip().startswith("#")
        if directive:
            out.append("".join("\n" if ch == "\n" else " " for ch in line))
            continuation = line.rstrip("\r\n").rstrip().endswith("\\")
        else:
            out.append(line)
            continuation = False
    return "".join(out)


def _has_preprocessor(text: str | None) -> bool:
    if text is None:
        return False
    return any(line.lstrip().startswith("#") for line in text.splitlines())


def _balanced_body(code: str, open_brace: int) -> str | None:
    depth = 0
    quote: str | None = None
    i = open_brace
    while i < len(code):
        ch = code[i]
        if quote is not None:
            if ch == "\\":
                i += 2
                continue
            if ch == quote:
                quote = None
        elif ch in ("'", '"'):
            quote = ch
        elif ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return code[open_brace + 1:i]
        i += 1
    return None


def function_body(code: str, name: str) -> str | None:
                                          
    match = re.search(rf"\b{re.escape(name)}\s*\([^;]*?\)\s*\{{", code, re.S)
    if match is None:
        return None
    return _balanced_body(code, match.end() - 1)


def rest_function_body(code: str, name: str) -> str | None:

    body = rpc_extract.function_body_in(code, rpc_extract.rest_fn_name_in(code, name))
    return None if body is None else body[1:-1]

def region(body: str, start: str, end: str) -> str | None:
    begin = body.find(start)
    if begin < 0:
        return None
    finish = body.find(end, begin + len(start))
    if finish < 0:
        return None
    return body[begin:finish]

def region_re(body: str | None, start: str, end: str) -> str | None:

    if body is None:
        return None
    begin = re.search(start, body)
    if begin is None:
        return None
    finish = re.compile(end).search(body, begin.end())
    if finish is None:
        return None
    return body[begin.start():finish.start()]

def _path_if(path: str) -> str:

    return (r'if\s*\(\s*g_strcmp0\s*\(\s*path\s*,\s*REST_API_PREFIX\s*"'
            + re.escape(path) + r'"\s*\)')

SNAPSHOT_RE = r"\brequest_client_ip\s*=\s*pcv_rest_auth_audit_address\s*\(\s*msg\s*\)"
REQ_ID_ALLOC_RE = r"(?:\brq\s*->\s*)?\breq_id\s*=\s*pcv_generate_request_id\b"
RATE_START_RE = r"\bstatic\s+GMutex\s+rate_mu\b"
RATE_END_RE = r"\bstatic\s+GMutex\s+_user_rl_mu\b"
MUTATION_START_RE = r'if\s*\(\s*g_strcmp0\s*\(\s*method\s*,\s*"POST"\s*\)\s*==\s*0'
KEY_ROLE_DECL_RE = r"\bgint\s+key_role\b"
RQ_IP_READ_RE = r"(?<![=!<>])=\s*rq\s*->\s*request_client_ip\b"
RQ_IP_COPY_RE = r"\brq\s*->\s*request_client_ip\s*=\s*request_client_ip\s*;"


def _require(reasons: list[str], scope: str, text: str | None,
             pattern: str, description: str) -> None:
    if text is None or re.search(pattern, text, re.S) is None:
        reasons.append(f"{scope}: {description}")


def _require_all_calls(reasons: list[str], scope: str, text: str | None,
                       call_name: str, expected: int, argument_pattern: str,
                       description: str) -> None:
    calls = [] if text is None else re.findall(
        rf"\b{re.escape(call_name)}\s*\([^;]*?\)\s*;", text, re.S)
    if len(calls) != expected:
        reasons.append(
            f"{scope}: {call_name} callsite 수 {len(calls)} != {expected}")
        return
    for index, call in enumerate(calls, 1):
        if re.search(argument_pattern, call, re.S) is None:
            reasons.append(f"{scope} #{index}: {description}")


def _reject_directives(reasons: list[str], scope: str,
                       text: str | None) -> None:
    if _has_preprocessor(text):
        reasons.append(
            f"{scope}: 보호 region 내부 전처리 지시문 금지 "
            "(비활성/macro decoy 우회 방지)")


def scan_collectors(code: str, request: str | None) -> list[str]:

    reasons: list[str] = []
    for path in COLLECTOR_PATHS:
        match = re.search(_path_if(path), code)
        if match is None:
            reasons.append(f"collector gate: {path} 처리부 없음")
            continue
        brace = code.find("{", match.end())
        body = _balanced_body(code, brace) if brace >= 0 else None
        if body is None or re.match(GATE_CALL_RE, body) is None:
            reasons.append(f"collector gate: {path} 처리부 첫 문장이 pcv_rest_collector_gate가 아님(본문 작성 전 판정 누락)")
    gate = rest_function_body(code, "pcv_rest_collector_gate")
    _require(reasons, "collector gate", gate, r"\bpcv_rest_client_identity_is_direct_local\s*\(", "직접 로컬 판정 미사용")
    _require(reasons, "collector gate", gate, r"\bpcv_rest_collector_needs_auth\s*\(", "모드 판정 미사용")
    _require(reasons, "collector gate", gate, r"\bpcv_rest_authenticate\s*\(\s*msg\s*,\s*NULL\s*(?:,\s*NULL\s*)?\)", "토큰 검증 미사용")
    if request is not None:
        cut = re.search(AUTH_GATE_RE, request)
        if cut is None:
            reasons.append("public path allowlist: 인증 게이트 호출을 찾지 못함")
        else:
            seen = set(re.findall(r'REST_API_PREFIX\s*"([^"]+)"', request[:cut.start()]))
            for extra in sorted(seen - PUBLIC_PATHS):
                reasons.append(f"public path allowlist: 인증 게이트 위 새 무인증 경로 {extra}")
    return reasons

def scan_identity(identity_text: str) -> list[str]:

    reasons: list[str] = []
    code = mask_preprocessor(strip_comments(identity_text))
    if re.search(r"\bis_trusted_loopback\b", code):
        reasons.append("proxy trust: 127/8 리터럴 신뢰 함수(is_trusted_loopback) 잔존")
    new_identity = function_body(code, "identity_new_for_message")
    _require(reasons, "proxy trust", new_identity, r"\bpcv_rest_proxy_trusted\s*\(",
             "신뢰 판정이 pcv_rest_proxy_trusted를 거치지 않음")
    _require(reasons, "proxy trust", new_identity, r"\bpcv_rest_resolve_client_ip\s*\(\s*trusted\s*,",
             "client IP 해석이 공통 trusted 판정을 쓰지 않음")
    _require(reasons, "proxy trust", new_identity, r"\bpcv_rest_resolve_external_https\s*\(\s*trusted\s*,",
             "HSTS 판정이 공통 trusted 판정을 쓰지 않음")
    _require(reasons, "direct local", new_identity, r"\bpcv_rest_is_direct_local\s*\(", "직접 로컬 판정 미사용")
    direct = function_body(code, "pcv_rest_is_direct_local") or ""
    total = len(re.findall(r"\bis_loopback_peer\s*\(", code))
    defined = 1 if re.search(r"\bis_loopback_peer\s*\([^;{]*\)\s*\{", code) else 0
    if total - defined - len(re.findall(r"\bis_loopback_peer\s*\(", direct)) != 0:
        reasons.append("proxy trust: 루프백 리터럴 판정(is_loopback_peer)이 직접 로컬 판정 밖에서 쓰임")
    for fn in ("pcv_rest_resolve_client_ip", "pcv_rest_resolve_external_https", "pcv_rest_proxy_trusted"):
        body = function_body(code, fn)
        if body is not None and re.search(r"g_inet_address_to_bytes|\[\s*0\s*\]\s*==\s*127", body):
            reasons.append(f"proxy trust: {fn}가 127/8 바이트 리터럴로 신뢰를 판정")
    return reasons

def scan_text(text: str, ws_text: str | None = None, identity_text: str | None = None) -> list[str]:
                                              
    commentless = strip_comments(text)
    code = mask_preprocessor(commentless)
    reasons: list[str] = []
    if FORBIDDEN in code:
        reasons.append(
            f"전역: {FORBIDDEN} 직접 호출 발견 — identity module만 허용")

    address = rest_function_body(code, "pcv_rest_auth_audit_address")
    address_raw = rest_function_body(commentless, "pcv_rest_auth_audit_address")
    _reject_directives(reasons, "identity seam", address_raw)
    _require(reasons, "identity seam", address,
             r"pcv_rest_client_identity_get\s*\(\s*message\s*\)",
             "typed SoupServerMessage accessor 미사용")

    rate_seam = rest_function_body(code, "pcv_rest_rate_limit_key_for_message")
    rate_seam_raw = rest_function_body(
        commentless, "pcv_rest_rate_limit_key_for_message")
    _reject_directives(reasons, "rate-limit seam", rate_seam_raw)
    _require(reasons, "rate-limit seam", rate_seam,
             r"pcv_rest_auth_audit_address\s*\(\s*message\s*\)",
             "공통 audit 주소에서 bucket key를 만들지 않음")

    send_json = rest_function_body(code, "_send_json")
    send_json_raw = rest_function_body(commentless, "_send_json")
    _reject_directives(reasons, "HSTS/external HTTPS", send_json_raw)
    _require(reasons, "HSTS/external HTTPS", send_json,
             r"pcv_rest_client_identity_get\s*\(\s*msg\s*\)",
             "요청 캐시 identity 미사용")
    _require(reasons, "HSTS/external HTTPS", send_json,
             r"pcv_rest_client_identity_is_external_https\s*\(\s*identity\s*\)",
             "신뢰 프록시 external HTTPS 판정 미사용")

    authenticate = rest_function_body(code, "_authenticate")
    authenticate_raw = rest_function_body(commentless, "_authenticate")
    api_key = region(authenticate or "", "if (api_key && *api_key)",
                     "const gchar *auth")
    no_header = region(authenticate or "", "if (!auth || !*auth)",
                       "GError *err")
    invalid_token = region(authenticate or "", "if (!subject)",
                           "return subject")
    api_key_raw = region(authenticate_raw or "", "if (api_key && *api_key)",
                         "const gchar *auth")
    no_header_raw = region(authenticate_raw or "", "if (!auth || !*auth)",
                           "GError *err")
    invalid_token_raw = region(authenticate_raw or "", "if (!subject)",
                               "return subject")
    for label, auth_region in (
        ("auth invalid API key audit", api_key),
        ("auth missing header audit", no_header),
        ("auth invalid JWT audit", invalid_token),
    ):
        _require(reasons, label, auth_region,
                 r"rip\s*=\s*pcv_rest_auth_audit_address\s*\(\s*msg\s*\)",
                 "callsite 주소가 공통 request identity에서 파생되지 않음")
        _require_all_calls(
            reasons, label, auth_region, "pcv_audit_log", 1,
            r"\"auth\.failed\"[^;]*,\s*rip\s*\)\s*;",
            "audit 호출이 파생 주소 rip을 기록하지 않음")
    for label, auth_region in (
        ("auth invalid API key audit", api_key_raw),
        ("auth missing header audit", no_header_raw),
        ("auth invalid JWT audit", invalid_token_raw),
    ):
        _reject_directives(reasons, label, auth_region)

    request = rpc_extract.rest_request_scope(code)
    request_raw = rpc_extract.rest_request_scope(commentless)
    _require(reasons, "request security identity", request,
             SNAPSHOT_RE, "보안 소비자 공통 주소 snapshot 미생성")

    ip_read = None if request is None else re.search(RQ_IP_READ_RE, request)
    if ip_read is not None:
        snap = re.search(SNAPSHOT_RE, request)
        copy = re.search(RQ_IP_COPY_RE, request)
        if snap is None or copy is None or not (snap.end() <= copy.start() < ip_read.start()):
            reasons.append("request security identity: 구간 함수가 읽는 rq->request_client_ip에 "
                           "공통 주소 snapshot이 먼저 복사되지 않음(감사 주소 NULL)")

    rate = region_re(request, RATE_START_RE, RATE_END_RE)
    rate_raw = region_re(request_raw, RATE_START_RE, RATE_END_RE)
    _reject_directives(reasons, "rate-limit callsite", rate_raw)
    _require(reasons, "rate-limit callsite", rate,
             r"pcv_rest_rate_limit_key_for_message\s*"
             r"\(\s*msg\s*,\s*path\s*,\s*method\s*\)",
             "production rate bucket이 message identity seam을 우회")

    mutation = region_re(request, MUTATION_START_RE, REQ_ID_ALLOC_RE)
    mutation_raw = region_re(request_raw, MUTATION_START_RE, REQ_ID_ALLOC_RE)
    _reject_directives(reasons, "mutation/access audit", mutation_raw)
    _require(reasons, "mutation/access audit", mutation,
             r"remote_str\s*=\s*request_client_ip",
             "mutation/access 로그 주소가 request identity와 다름")
    _require_all_calls(
        reasons, "mutation/access audit", mutation, "PCV_LOG_INFO", 2,
        r",\s*remote_str(?:\s*,|\s*\))",
        "mutation/access 로그가 파생 주소를 기록하지 않음")

    token_end = r'REST_API_PREFIX\s*"/auth/refresh"'
    token = region_re(request, _path_if("/auth/token"), token_end)
    token_raw = region_re(request_raw, _path_if("/auth/token"), token_end)
    _reject_directives(reasons, "brute-force/security IP", token_raw)

    login_run = rest_function_body(code, "pcv_rest_login_run")
    login_run_raw = rest_function_body(commentless, "pcv_rest_login_run")
    _reject_directives(reasons, "brute-force/security IP", login_run_raw)
    _require(reasons, "brute-force/security IP", token,
             r"pcv_rest_login_start\s*\([^;]*,\s*client_ip\s*\)\s*;",
             "로그인 창구가 공통 client_ip를 전용 풀에 넘기지 않음")
    _require(reasons, "brute-force/security IP", login_run,
             r"\bclient_ip\s*=\s*in\s*->\s*client_ip\s*;",
             "로그인 본체가 넘겨받은 client_ip를 쓰지 않음")

    for scope_text in (token, login_run):
        _require(reasons, "brute-force/security IP", scope_text,
                 r"pcv_rbac_get_ip_remaining_lockout\s*\(\s*client_ip\s*\)",
                 "pcv_rbac_get_ip_remaining_lockout가 공통 client_ip을 사용하지 않음")
    if token is not None:
        token = token + "\n" + (login_run or "")
    for call in (
        "pcv_rbac_ip_record_auth_failure",
        "pcv_rbac_ip_record_auth_success",
    ):
        _require(reasons, "brute-force/security IP", token,
                 rf"{call}\s*\(\s*client_ip\s*\)",
                 f"{call}가 공통 client_ip을 사용하지 않음")
    for action in ("auth.bootstrap.fallback", "auth.failed"):
        matching = None if token is None else "\n".join(
            call for call in re.findall(
                r"\bpcv_audit_log\s*\([^;]*?\)\s*;", token, re.S)
            if f'"{action}"' in call)
        _require_all_calls(
            reasons, f"login audit {action}", matching,
            "pcv_audit_log", 1,
            rf"\"{re.escape(action)}\"[^;]*,\s*client_ip\s*\)\s*;",
            "로그인 audit가 공통 client_ip을 기록하지 않음")

    password = region_re(request, _path_if("/auth/password"), _path_if("/auth/register"))
                                                             
                                                                     
                                                                    
                                                                             
                                                                 
                                                                       
                                                           
    register = region_re(request, _path_if("/auth/register"), _path_if("/auth/totp/verify"))
    password_raw = region_re(request_raw, _path_if("/auth/password"), _path_if("/auth/register"))
    register_raw = region_re(request_raw, _path_if("/auth/register"), _path_if("/auth/totp/verify"))
    _reject_directives(reasons, "password audit", password_raw)
    _reject_directives(reasons, "register audit", register_raw)
    for label, auth_region, action in (
        ("password audit", password, "auth.password.change"),
        ("register audit", register, "auth.register"),
    ):
        _require(reasons, label, auth_region,
                 r"rip\s*=\s*request_client_ip",
                 "callsite 주소가 request identity snapshot과 다름")
        _require_all_calls(
            reasons, label, auth_region, "pcv_audit_log", 2,
            rf"\"{re.escape(action)}\"[^;]*,\s*rip\s*\)\s*;",
            "audit가 파생 주소 rip을 기록하지 않음")

                                                                                
                                                                    
                                                                        
                                                                 
    totp = region_re(request, _path_if("/auth/totp/verify"), KEY_ROLE_DECL_RE)
    totp_raw = region_re(request_raw, _path_if("/auth/totp/verify"), KEY_ROLE_DECL_RE)
    _reject_directives(reasons, "totp audit", totp_raw)
    _require(reasons, "totp audit", totp,
             r"client_ip\s*=\s*request_client_ip",
             "totp callsite 주소가 request identity snapshot에서 파생되지 않음")
    _require_all_calls(
        reasons, "totp audit", totp, "pcv_audit_log", 3,
        r",\s*client_ip\s*\)\s*;",
        "totp audit가 공통 request client_ip을 기록하지 않음")
    if ws_text is None:
        ws_text = WS_TARGET.read_text()
    ws_commentless = strip_comments(ws_text)
    ws_code = mask_preprocessor(ws_commentless)
    ws_connected = function_body(ws_code, "_on_ws_connected")
    ws_connected_raw = function_body(ws_commentless, "_on_ws_connected")
    _reject_directives(reasons, "WebSocket per-IP consumer",
                       ws_connected_raw)
    _require(
        reasons, "WebSocket per-IP consumer", ws_connected,
        r"identity\s*=\s*pcv_rest_client_identity_get\s*\(\s*msg\s*\)",
        "요청 단위 cached proxy identity 미사용")
    _require(
        reasons, "WebSocket per-IP consumer", ws_connected,
        r"client_ip\s*=\s*g_strdup\s*\(\s*"
        r"pcv_rest_client_identity_get_client_ip\s*\(\s*identity\s*\)\s*\)",
        "per-IP key가 cached identity의 canonical IP에서 파생되지 않음")
    _require(
        reasons, "WebSocket per-IP consumer", ws_connected,
        r"pcv_client_identity_admission_try\s*\(\s*G\.ip_counts\s*,\s*client_ip\s*,"
        r"\s*ws_max_per_ip\s*\)",
        "실제 admission 소비자가 canonical client_ip을 우회")
    if FORBIDDEN in ws_code:
        reasons.append(
            f"WebSocket per-IP consumer: {FORBIDDEN} 직접 호출 발견")
    reasons.extend(scan_collectors(code, request))
    if identity_text is None:
        identity_text = IDENTITY_TARGET.read_text()
    reasons.extend(scan_identity(identity_text))
    return reasons


def main(argv: list[str] | None = None) -> int:

    args = list(sys.argv[1:] if argv is None else argv)
    if args:
        label = args[0]
        text = Path(args[0]).read_text(errors="replace")
    else:
        label = "rest 계열: " + ", ".join(
            p.relative_to(ROOT).as_posix() for p in rpc_extract.rest_family(ROOT))
        text = rpc_extract.rest_family_text(ROOT)
    reasons = scan_text(text)
    if reasons:
        print(f"[FAIL] 프록시 클라이언트 신원 경계 위반 ({label}):",
              file=sys.stderr)
        for reason in reasons:
            print(f"  - {reason}", file=sys.stderr)
        return 1
    print(f"[PASS] REST/WS 보안 소비자 callsite별 공통 요청 신원 배선 "
          f"({label}, {WS_TARGET_REL}, {IDENTITY_REL})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
