#!/usr/bin/env python3
                          
                                                                          
                                                                                      
                                                                                                                            
 
                      
                                                                                                                                   
                                            

                                                        
                                        
                                 

                                                                           
                                  
                                           
                                          
                                                
   

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TARGET_REL = "src/api/rest_server.c"
TARGET = ROOT / TARGET_REL
MAIN_REL = "src/main.c"
MAIN_TARGET = ROOT / MAIN_REL


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
                                         
    out: list[str] = []
    continuation = False
    for line in text.splitlines(keepends=True):
        directive = continuation or line.lstrip().startswith("#")
        if directive:
            out.append("".join("\n" if ch == "\n" else " " for ch in line))
            continuation = line.rstrip("\r\n").rstrip().endswith("\\")
        else:
            out.append(line)
            continuation = False
    return "".join(out)


def _has_preprocessor(text: str | None) -> bool:
    return text is not None and any(
        line.lstrip().startswith("#") for line in text.splitlines())


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


def _balanced_segment(code: str, opening: int,
                      open_char: str, close_char: str) -> tuple[str, int] | None:
                                                 
    depth = 0
    quote: str | None = None
    i = opening
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
        elif ch == open_char:
            depth += 1
        elif ch == close_char:
            depth -= 1
            if depth == 0:
                return code[opening + 1:i], i + 1
        i += 1
    return None


def _if_blocks(code: str | None) -> list[tuple[str, str]]:
                                                         
    if code is None:
        return []
    blocks: list[tuple[str, str]] = []
    for match in re.finditer(r"\bif\s*\(", code):
        condition = _balanced_segment(code, match.end() - 1, "(", ")")
        if condition is None:
            continue
        condition_text, after_condition = condition
        body_open = after_condition
        while body_open < len(code) and code[body_open].isspace():
            body_open += 1
        if body_open >= len(code) or code[body_open] != "{":
            continue
        body = _balanced_segment(code, body_open, "{", "}")
        if body is not None:
            blocks.append((condition_text, body[0]))
    return blocks


def _leading_if(statement: str) -> tuple[str, str] | None:
                                                        
    match = re.match(r"^\s*if\s*\(", statement)
    if match is None:
        return None
    condition = _balanced_segment(statement, match.end() - 1, "(", ")")
    if condition is None:
        return None
    condition_text, body_start = condition
    while body_start < len(statement) and statement[body_start].isspace():
        body_start += 1
    if body_start < len(statement) and statement[body_start] == "{":
        body = _balanced_segment(statement, body_start, "{", "}")
        return None if body is None else (condition_text, body[0])
    return condition_text, statement[body_start:]


def _strip_outer_parentheses(expression: str) -> str:
    expression = expression.strip()
    while expression.startswith("("):
        balanced = _balanced_segment(expression, 0, "(", ")")
        if balanced is None or balanced[1] != len(expression):
            break
        expression = balanced[0].strip()
    return expression


def _top_level_or_operands(condition: str) -> list[str]:
                                                         
    operands: list[str] = []
    depth = 0
    start = 0
    i = 0
    while i < len(condition):
        if condition[i] == "(":
            depth += 1
        elif condition[i] == ")":
            depth -= 1
        elif depth == 0 and condition.startswith("||", i):
            operands.append(condition[start:i])
            start = i + 2
            i += 1
        i += 1
    operands.append(condition[start:])
    return [_strip_outer_parentheses(part) for part in operands]


def _top_level_statements(body: str) -> list[str]:
                                      

                                                      
                                                
                                                        
       
    statements: list[str] = []
    start = 0
    brace_depth = paren_depth = 0
    quote: str | None = None
    i = 0
    while i < len(body):
        ch = body[i]
        if quote is not None:
            if ch == "\\":
                i += 2
                continue
            if ch == quote:
                quote = None
        elif ch in ("'", '"'):
            quote = ch
        elif ch == "(":
            paren_depth += 1
        elif ch == ")":
            paren_depth -= 1
        elif ch == "{":
            brace_depth += 1
        elif ch == "}":
            brace_depth -= 1
            if brace_depth == 0 and paren_depth == 0:
                candidate = body[start:i + 1].strip()
                if re.match(r"^(?:if|for|while|switch)\b", candidate):
                    statements.append(candidate)
                    start = i + 1
        elif ch == ";" and brace_depth == 0 and paren_depth == 0:
            candidate = body[start:i + 1].strip()
            if candidate:
                statements.append(candidate)
            start = i + 1
        i += 1
    tail = body[start:].strip()
    if tail:
        statements.append(tail)
    return statements


def _is_transport_fail_closed_if(condition: str, body: str) -> bool:
    condition = _strip_outer_parentheses(condition)
    operands = _top_level_or_operands(condition)
    normalized = {re.sub(r"\s+", "", operand) for operand in operands}
    expected = {
        "!pcv_config_get_https_enabled(&https_enabled,&error)",
        "!pcv_config_validate_transport(&error)",
    }
    if len(operands) != 2 or normalized != expected:
        return False
    statements = _top_level_statements(body)
    normalized_statements = [re.sub(r"\s+", "", item)
                             for item in statements]
    required = [
        "pcv_config_shutdown();",
        "pcv_log_shutdown();",
        "returnEXIT_FAILURE;",
    ]
    try:
        positions = [normalized_statements.index(item) for item in required]
    except ValueError:
        return False
    returns = [index for index, item in enumerate(normalized_statements)
               if item.startswith("return")]
    return positions == sorted(positions) and returns == [positions[-1]]


def function_body(code: str, name: str) -> str | None:
                                          
    match = re.search(
        rf"\b{re.escape(name)}\s*\([^;]*?\)\s*\{{", code, re.S)
    return None if match is None else _balanced_body(code, match.end() - 1)


def region(body: str | None, start: str, end: str) -> str | None:
    if body is None:
        return None
    begin = body.find(start)
    finish = body.find(end, begin + len(start)) if begin >= 0 else -1
    return None if begin < 0 or finish < 0 else body[begin:finish]


def _calls(text: str | None, name: str) -> list[str]:
    if text is None:
        return []
    return re.findall(
        rf"\b{re.escape(name)}\s*\([^;]*?\)\s*;", text, re.S)


def _reject_directives(reasons: list[str], scope: str,
                       raw: str | None) -> None:
    if _has_preprocessor(raw):
        reasons.append(
            f"{scope}: 보호 함수 내부 전처리 지시문 금지 "
            "(#if 0/macro decoy 우회 방지)")


def _require(reasons: list[str], scope: str, text: str | None,
             pattern: str, description: str) -> None:
    if text is None or re.search(pattern, text, re.S) is None:
        reasons.append(f"{scope}: {description}")


def scan_text(rest_text: str, main_text: str) -> list[str]:
                                                       
    rest_raw = strip_comments(rest_text)
    main_raw = strip_comments(main_text)
    rest = mask_preprocessor(rest_raw)
    main = mask_preprocessor(main_raw)
    reasons: list[str] = []

    transport_mutations = re.findall(
        r"(?:\+\+|--)?\s*self->transport(?:\s*\.[A-Za-z_][A-Za-z0-9_]*)?"
        r"\s*(?:\+\+|--|[+\-*/%&|^]?=(?!=))\s*[^;]*;",
        rest,
    )
    normalized_mutations = [
        re.sub(r"\s+", "", mutation) for mutation in transport_mutations]
    destination_mutations = re.findall(
        r"\b(?:memcpy|memmove|memset|g_clear_pointer|g_clear_object|"
        r"g_set_object)\s*\(\s*&?\s*self->transport\b[^;]*;",
        rest,
        re.S,
    )
    if normalized_mutations != ["self->transport=transport;"] or \
            destination_mutations:
        reasons.append(
            "REST transport snapshot: constructor의 단일 whole-struct 저장 외 "
            "self->transport 변이 발견")

    main_body = function_body(main, "main")
    main_body_raw = function_body(main_raw, "main")
    transport_setup = region(
        main_body, "gboolean https_enabled", "pcv_log_load_module_levels")
    transport_setup_raw = region(
        main_body_raw, "gboolean https_enabled", "pcv_log_load_module_levels")
    _reject_directives(reasons, "main transport wiring", transport_setup_raw)
    plan_calls = _calls(transport_setup, "pcv_rest_transport_plan")
    server_calls = _calls(main_body, "pcv_rest_server_new")
    transport_fail_closed = [
        block for block in _if_blocks(transport_setup)
        if _is_transport_fail_closed_if(*block)
    ]
    if len(transport_fail_closed) != 1:
        reasons.append(
            "main transport wiring: HTTPS getter와 transport validation만 "
            "최상위 ||로 묶은 fail-closed if가 정확히 하나가 아님")
    if len(plan_calls) != 1:
        reasons.append(
            f"main transport wiring: plan callsite 수 {len(plan_calls)} != 1")
    _require(
        reasons, "main transport wiring", transport_setup,
        r"PcvRestTransportPlan\s+rest_transport\s*=\s*"
        r"pcv_rest_transport_plan\s*\(\s*"
        r"pcv_rest_tls_mode_from_config\s*\(\s*https_enabled\s*\)\s*,\s*"
        r"pcv_config_get_string\s*\(\s*\"server\"\s*,\s*"
        r"\"bind_plaintext\"\s*,\s*\"loopback\"\s*\)\s*\)\s*;",
        "검증 결과와 bind_plaintext가 단일 rest_transport 계획을 만들지 않음")
    if transport_setup is None:
        reasons.append("main transport wiring: 전송 설정 보호 region 추출 실패")
    elif transport_fail_closed:
        fail_if_at = transport_setup.find(
            "pcv_config_get_https_enabled")
        plan_at = transport_setup.find("pcv_rest_transport_plan")
        if not (0 <= fail_if_at < plan_at):
            reasons.append(
                "main transport wiring: fail-closed if가 plan보다 먼저 실행되지 않음")
    if len(server_calls) != 1 or re.search(
            r"pcv_rest_server_new\s*\(\s*dispatcher\s*,\s*0\s*,\s*"
            r"rest_transport\s*\)\s*;", server_calls[0] if server_calls else "",
            re.S) is None:
        reasons.append(
            "main transport wiring: 동일 rest_transport가 유일한 server "
            "constructor에 전달되지 않음")

    plan = function_body(rest, "pcv_rest_transport_plan")
    plan_raw = function_body(rest_raw, "pcv_rest_transport_plan")
    _reject_directives(reasons, "transport plan", plan_raw)
    plan_statements = _top_level_statements(plan or "")
    normalized_plan_statements = [
        re.sub(r"\s+", "", statement) for statement in plan_statements]
    internal_definitions = [
        (index, statement)
        for index, statement in enumerate(normalized_plan_statements)
        if re.match(r"^(?:(?:const)?gboolean)?internal=", statement)
    ]
    bind_definitions = [
        (index, statement)
        for index, statement in enumerate(normalized_plan_statements)
        if re.match(r"^(?:constgchar\*)?bind_mode=", statement)
    ]
    expected_internal = "gbooleaninternal=mode==PCV_REST_TLS_INTERNAL;"
    expected_bind = (
        "constgchar*bind_mode="
        'internal&&g_strcmp0(configured_plaintext_bind,"all")==0'
        '?"all":"loopback";'
    )
    if len(internal_definitions) != 1 or \
            internal_definitions[0][1] != expected_internal:
        reasons.append(
            "transport plan: internal mode가 정확한 top-level 단일 정의가 아님")
    if len(bind_definitions) != 1 or bind_definitions[0][1] != expected_bind:
        reasons.append(
            "transport plan: bind_mode가 internal+configured bind에서 "
            "파생된 top-level 단일 정의가 아님")
    plan_returns = [
        (index, statement)
        for index, statement in enumerate(plan_statements)
        if re.match(r"^return\b", statement)
    ]
    returned_plan = plan_returns[0][1] if len(plan_returns) == 1 else None
    if len(plan_returns) == 1 and len(internal_definitions) == 1 and \
            len(bind_definitions) == 1 and not (
                internal_definitions[0][0] < bind_definitions[0][0] <
                plan_returns[0][0]):
        reasons.append(
            "transport plan: internal → bind_mode → return 순서가 아님")
    _require(
        reasons, "transport plan return", returned_plan,
        r"^return\s*\(\s*PcvRestTransportPlan\s*\)\s*\{"
        r"(?=[^{}]*\.mode\s*=\s*mode\s*,)"
        r"(?=[^{}]*\.initialize_tls\s*=\s*internal\s*,)"
        r"(?=[^{}]*\.load_certificate\s*=\s*internal\s*,)"
        r"(?=[^{}]*\.create_tls_context\s*=\s*internal\s*,)"
        r"(?=[^{}]*\.listen_https\s*=\s*internal\s*,)"
        r"(?=[^{}]*\.plaintext_bind_mode\s*=\s*bind_mode\s*,)"
        r"(?=[^{}]*\.plaintext_host\s*=\s*g_strcmp0\s*\(\s*"
        r"bind_mode\s*,\s*\"all\"\s*\)\s*==\s*0\s*\?\s*"
        r"\"0\.0\.0\.0\"\s*:\s*\"127\.0\.0\.1\"\s*,)"
        r"[^{}]*\}\s*;$",
        "실제 top-level 반환 plan이 검증된 mode/bind 데이터 흐름을 사용하지 않음")

    orchestration_body = function_body(rest, "pcv_rest_transport_start")
    orchestration_raw = function_body(rest_raw, "pcv_rest_transport_start")
    _reject_directives(reasons, "transport orchestration", orchestration_raw)
    orchestration_statements = _top_level_statements(orchestration_body or "")
    plaintext_assignments = [
        (index, re.sub(r"\s+", "", statement))
        for index, statement in enumerate(orchestration_statements)
        if "ops->listen_plaintext" in statement
    ]
    expected_plaintext = (
        "outcome->plaintext_listening="
        "ops->listen_plaintext(context,transport->plaintext_host,error);"
    )
    if len(plaintext_assignments) != 1 or \
            plaintext_assignments[0][1] != expected_plaintext:
        reasons.append(
            "transport orchestration: plaintext 결과가 transport host를 쓰는 "
            "top-level 단일 assignment가 아님")
    top_level_ifs = [
        parsed for statement in orchestration_statements
        if (parsed := _leading_if(statement)) is not None
    ]
    fail_closed_plaintext = [
        (condition, body) for condition, body in top_level_ifs
        if re.sub(r"\s+", "", condition) == "!outcome->plaintext_listening"
        and re.sub(r"\s+", "", body) == "returnFALSE;"
    ]
    if len(fail_closed_plaintext) != 1:
        reasons.append(
            "transport orchestration: plaintext 실패가 top-level if에서 "
            "즉시 FALSE를 반환하지 않음")
    if len(plaintext_assignments) == 1:
        assignment_index = plaintext_assignments[0][0]
        adjacent = (
            assignment_index + 1 < len(orchestration_statements)
            and (_leading_if(
                orchestration_statements[assignment_index + 1])
                 in fail_closed_plaintext)
        )
        if not adjacent:
            reasons.append(
                "transport orchestration: plaintext assignment 직후 "
                "fail-closed if/return FALSE가 오지 않음")
    https_statement_indexes = [
        index for index, statement in enumerate(orchestration_statements)
        if "ops->listen_https" in statement
    ]
    if len(plaintext_assignments) == 1 and https_statement_indexes and \
            plaintext_assignments[0][0] >= min(https_statement_indexes):
        reasons.append(
            "transport orchestration: plaintext listener가 HTTPS callback보다 늦음")

    constructor = function_body(rest, "pcv_rest_server_new")
    constructor_raw = function_body(rest_raw, "pcv_rest_server_new")
    _reject_directives(reasons, "REST constructor", constructor_raw)
    constructor_statements = _top_level_statements(constructor or "")
    assignments = [
        re.sub(r"\s+", "", statement)
        for statement in constructor_statements
        if re.match(r"^self->transport\s*=", statement)
    ]
    if assignments != ["self->transport=transport;"]:
        reasons.append(
            "REST constructor: 전달 plan을 self->transport에 정확히 한 번 "
            "불변 스냅샷으로 저장하지 않음")
    else:
        normalized_constructor = [
            re.sub(r"\s+", "", statement)
            for statement in constructor_statements
        ]
        assignment_at = normalized_constructor.index("self->transport=transport;")
        returns = [
            index for index, statement in enumerate(normalized_constructor)
            if statement.startswith("return")
        ]
        if returns != [len(normalized_constructor) - 1] or \
                assignment_at >= returns[0]:
            reasons.append(
                "REST constructor: transport 저장 뒤 유일한 top-level return이 아님")

    start = function_body(rest, "pcv_rest_server_start")
    start_raw = function_body(rest_raw, "pcv_rest_server_start")
    _reject_directives(reasons, "REST start", start_raw)
    ops_initializers = [] if start is None else re.findall(
        r"\bconst\s+PcvRestTransportOps\s+transport_ops\s*=\s*"
        r"\{[^{}]*\}\s*;", start, re.S)
    if len(ops_initializers) != 1:
        reasons.append(
            f"REST start: transport_ops initializer 수 "
            f"{len(ops_initializers)} != 1")
    else:
        mapping = ops_initializers[0]
        plaintext_mappings = re.findall(
            r"\.listen_plaintext\s*=\s*([A-Za-z_][A-Za-z0-9_]*)\s*,",
            mapping,
        )
        if plaintext_mappings != ["_transport_listen_plaintext"]:
            reasons.append(
                "REST start: listen_plaintext가 production scoped listener에 "
                "정확히 한 번 매핑되지 않음")
    orchestration = _calls(start, "pcv_rest_transport_start")
    if len(orchestration) != 1 or re.search(
            r"pcv_rest_transport_start\s*\(\s*&self->transport\s*,\s*"
            r"&transport_ops\s*,",
            orchestration[0] if orchestration else "", re.S) is None:
        reasons.append(
            "REST start: 저장 plan과 검증된 transport_ops를 유일한 "
            "orchestration 호출에 전달하지 않음")

    listener = function_body(rest, "_transport_listen_plaintext")
    listener_raw = function_body(rest_raw, "_transport_listen_plaintext")
    _reject_directives(reasons, "plaintext listener", listener_raw)
    singular = _calls(listener, "soup_server_listen")
    listen_all = _calls(listener, "soup_server_listen_all")
    if len(singular) != 1:
        reasons.append(
            f"plaintext listener: scoped soup_server_listen callsite 수 "
            f"{len(singular)} != 1")
    if len(listen_all) != 1:
        reasons.append(
            f"plaintext listener: explicit internal/all callsite 수 "
            f"{len(listen_all)} != 1")
    _require(
        reasons, "plaintext listener", listener,
        r"if\s*\(\s*g_strcmp0\s*\(\s*endpoint\s*,\s*\"0\.0\.0\.0\"\s*\)"
        r"\s*==\s*0\s*\)\s*\{[^{}]*soup_server_listen_all",
        "listen_all이 검증된 internal/all endpoint 분기에 한정되지 않음")
    _require(
        reasons, "plaintext listener", listener,
        r"g_inet_address_new_loopback\s*\(\s*AF_INET\s*\)"
        r"[^;]*;[^{}]*g_inet_socket_address_new\s*\(\s*lo\s*,"
        r"[^;]*;[^{}]*soup_server_listen\s*\(\s*ctx->server->soup\s*,"
        r"\s*addr\s*,",
        "external loopback 경로가 생성한 주소로 단수형 listen하지 않음")

    return reasons


def main() -> int:
    rest = TARGET.read_text(errors="replace")
    main_source = MAIN_TARGET.read_text(errors="replace")
    reasons = scan_text(rest, main_source)
    print("[check-transport-bind] 검증→계획→listener 구조 계약 "
          f"{'예' if not reasons else '아니오'}")
    if reasons:
        print(f"[FAIL] 평문 전송 불변식 위반 {len(reasons)}건:", file=sys.stderr)
        for reason in reasons:
            print(f"  - {reason}", file=sys.stderr)
        return 1
    print("[PASS] 외부 TLS 평문은 검증된 loopback plan과 단일 scoped listener 사용")
    return 0


if __name__ == "__main__":
    sys.exit(main())
