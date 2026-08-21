#!/usr/bin/env python3
                          
                                                             
                                                                                      
                                                                                                                            
 
                      
                                                                                                                      
                                       

                                                
                                              
   

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TARGET_REL = "src/api/rest_server.c"
TARGET = ROOT / TARGET_REL
WS_TARGET_REL = "src/api/ws_server.c"
WS_TARGET = ROOT / WS_TARGET_REL
FORBIDDEN = "soup_server_message_get_remote_address"
ACCESSOR = "pcv_rest_client_identity_get"


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


def region(body: str, start: str, end: str) -> str | None:
    begin = body.find(start)
    if begin < 0:
        return None
    finish = body.find(end, begin + len(start))
    if finish < 0:
        return None
    return body[begin:finish]


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


def scan_text(text: str, ws_text: str | None = None) -> list[str]:
                                              
    commentless = strip_comments(text)
    code = mask_preprocessor(commentless)
    reasons: list[str] = []
    if FORBIDDEN in code:
        reasons.append(
            f"전역: {FORBIDDEN} 직접 호출 발견 — identity module만 허용")

    address = function_body(code, "pcv_rest_auth_audit_address")
    address_raw = function_body(commentless, "pcv_rest_auth_audit_address")
    _reject_directives(reasons, "identity seam", address_raw)
    _require(reasons, "identity seam", address,
             r"pcv_rest_client_identity_get\s*\(\s*message\s*\)",
             "typed SoupServerMessage accessor 미사용")

    rate_seam = function_body(code, "pcv_rest_rate_limit_key_for_message")
    rate_seam_raw = function_body(
        commentless, "pcv_rest_rate_limit_key_for_message")
    _reject_directives(reasons, "rate-limit seam", rate_seam_raw)
    _require(reasons, "rate-limit seam", rate_seam,
             r"pcv_rest_auth_audit_address\s*\(\s*message\s*\)",
             "공통 audit 주소에서 bucket key를 만들지 않음")

    send_json = function_body(code, "_send_json")
    send_json_raw = function_body(commentless, "_send_json")
    _reject_directives(reasons, "HSTS/external HTTPS", send_json_raw)
    _require(reasons, "HSTS/external HTTPS", send_json,
             r"pcv_rest_client_identity_get\s*\(\s*msg\s*\)",
             "요청 캐시 identity 미사용")
    _require(reasons, "HSTS/external HTTPS", send_json,
             r"pcv_rest_client_identity_is_external_https\s*\(\s*identity\s*\)",
             "신뢰 프록시 external HTTPS 판정 미사용")

    authenticate = function_body(code, "_authenticate")
    authenticate_raw = function_body(commentless, "_authenticate")
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

    request = function_body(code, "_on_request")
    request_raw = function_body(commentless, "_on_request")
    _require(reasons, "request security identity", request,
             r"request_client_ip\s*=\s*pcv_rest_auth_audit_address\s*\(\s*msg\s*\)",
             "보안 소비자 공통 주소 snapshot 미생성")

    rate = region(request or "", "static GMutex  rate_mu",
                  "static GMutex      _user_rl_mu")
    rate_raw = region(request_raw or "", "static GMutex  rate_mu",
                      "static GMutex      _user_rl_mu")
    _reject_directives(reasons, "rate-limit callsite", rate_raw)
    _require(reasons, "rate-limit callsite", rate,
             r"pcv_rest_rate_limit_key_for_message\s*"
             r"\(\s*msg\s*,\s*path\s*,\s*method\s*\)",
             "production rate bucket이 message identity seam을 우회")

    mutation = region(request or "",
                      'if (g_strcmp0(method, "POST") == 0',
                      "req_id = pcv_generate_request_id")
    mutation_raw = region(request_raw or "",
                          'if (g_strcmp0(method, "POST") == 0',
                          "req_id = pcv_generate_request_id")
    _reject_directives(reasons, "mutation/access audit", mutation_raw)
    _require(reasons, "mutation/access audit", mutation,
             r"remote_str\s*=\s*request_client_ip",
             "mutation/access 로그 주소가 request identity와 다름")
    _require_all_calls(
        reasons, "mutation/access audit", mutation, "PCV_LOG_INFO", 2,
        r",\s*remote_str(?:\s*,|\s*\))",
        "mutation/access 로그가 파생 주소를 기록하지 않음")

    token = region(request or "",
                   'if (g_strcmp0(path, REST_API_PREFIX "/auth/token")',
                   'REST_API_PREFIX "/auth/refresh"')
    token_raw = region(request_raw or "",
                       'if (g_strcmp0(path, REST_API_PREFIX "/auth/token")',
                       'REST_API_PREFIX "/auth/refresh"')
    _reject_directives(reasons, "brute-force/security IP", token_raw)
    for call in (
        "pcv_rbac_get_ip_remaining_lockout",
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

    password = region(
        request or "",
        'if (g_strcmp0(path, REST_API_PREFIX "/auth/password")',
        'if (g_strcmp0(path, REST_API_PREFIX "/auth/register")')
                                                             
                                                                     
                                                                    
                                                                             
                                                                 
                                                                       
                                                           
    register = region(
        request or "",
        'if (g_strcmp0(path, REST_API_PREFIX "/auth/register")',
        'if (g_strcmp0(path, REST_API_PREFIX "/auth/totp/verify")')
    password_raw = region(
        request_raw or "",
        'if (g_strcmp0(path, REST_API_PREFIX "/auth/password")',
        'if (g_strcmp0(path, REST_API_PREFIX "/auth/register")')
    register_raw = region(
        request_raw or "",
        'if (g_strcmp0(path, REST_API_PREFIX "/auth/register")',
        'if (g_strcmp0(path, REST_API_PREFIX "/auth/totp/verify")')
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

                                                                                
                                                                    
                                                                        
                                                                 
    totp = region(
        request or "",
        'if (g_strcmp0(path, REST_API_PREFIX "/auth/totp/verify")',
        "gint key_role")
    totp_raw = region(
        request_raw or "",
        'if (g_strcmp0(path, REST_API_PREFIX "/auth/totp/verify")',
        "gint key_role")
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
    return reasons


def main(argv: list[str] | None = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    target = Path(args[0]) if args else TARGET
    label = args[0] if args else TARGET_REL
    reasons = scan_text(target.read_text(errors="replace"))
    if reasons:
        print(f"[FAIL] 프록시 클라이언트 신원 경계 위반 ({label}):",
              file=sys.stderr)
        for reason in reasons:
            print(f"  - {reason}", file=sys.stderr)
        return 1
    print(f"[PASS] REST/WS 보안 소비자 callsite별 공통 요청 신원 배선 "
          f"({label}, {WS_TARGET_REL})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
