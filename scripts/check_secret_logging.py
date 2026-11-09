#!/usr/bin/env python3
                          
                                                                
                                                                                      
                                                                                                                            
 
                      
                                                                                                                         
                                                                       

                                                                          

                               
                                                             
                                                               
                                                              
                      

     
                                        
                                                                 

                                                        
   
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rpc_extract

ROOT = Path(__file__).resolve().parent.parent
TARGET_REL = "src/api/rest_server.c"
TARGET = ROOT / TARGET_REL

HELPER = "_body_has_secret"
                                                        
IS_AUTH_ASSIGN_RE = re.compile(r'\bis_auth\s*=(?P<rhs>.*?);', re.S)

HELPER_CALL_RE = re.compile(r'\b(?:pcv_rest)?_body_has_secret\s*\(')


def strip_comments(text: str) -> str:
                                                     
    out = []
    i, n = 0, len(text)
    in_block = in_line = False
    in_str = None
    while i < n:
        ch = text[i]
        if in_line:
            out.append('\n' if ch == '\n' else ' ')
            if ch == '\n':
                in_line = False
            i += 1
            continue
        if in_block:
            if ch == '*' and i + 1 < n and text[i + 1] == '/':
                out.append('  ')
                i += 2
                in_block = False
            else:
                out.append('\n' if ch == '\n' else ' ')
                i += 1
            continue
        if in_str:
            out.append(ch)
            if ch == '\\' and i + 1 < n:
                out.append(text[i + 1])
                i += 2
                continue
            if ch == in_str:
                in_str = None
            i += 1
            continue
        if ch == '/' and i + 1 < n and text[i + 1] == '*':
            in_block = True
            out.append('  ')
            i += 2
            continue
        if ch == '/' and i + 1 < n and text[i + 1] == '/':
            in_line = True
            out.append('  ')
            i += 2
            continue
        if ch == '"' or ch == "'":
            in_str = ch
            out.append(ch)
            i += 1
            continue
        out.append(ch)
        i += 1
    return ''.join(out)


def scan_text(text: str):
                                     
                                    
                                                                 
    code = strip_comments(text)
    defined = rpc_extract.function_body_in(code, rpc_extract.rest_fn_name_in(code, HELPER)) is not None
    used = False
    for m in IS_AUTH_ASSIGN_RE.finditer(code):
        if HELPER_CALL_RE.search(m.group('rhs')):
            used = True
            break
    return defined, used


WEBHOOK_LOG_TARGETS = ("src/modules/daemons/alert_engine.c", "src/modules/daemons/alert_dlq.c")
LOG_CALL_RE = re.compile(r'\b(?:PCV_LOG_[A-Z]+|g_warning|g_message|g_info|g_debug|g_critical|g_printerr|fprintf)\s*\(')

WEBHOOK_URL_ARG_RE = re.compile(r'\b(?:url|target_url|webhook_url|webhook_crit_url|vm_wh)\b(?!\s*\[)')
STRING_RE = re.compile(r'"(?:[^"\\]|\\.)*"')
SECRET_KEYS_RE = re.compile(r'secret_keys\[\]\s*=\s*\{(?P<body>.*?)\}', re.S)

def webhook_log_violations(text: str):

    return log_arg_violations(text, WEBHOOK_URL_ARG_RE)

def body_mask_has_webhook(text: str) -> bool:

    m = SECRET_KEYS_RE.search(strip_comments(text))
    return bool(m and '"webhook"' in m.group('body'))

REST_RAW_ARG_RE = re.compile(r'\b(?:rpc|rpc_with_context|body|body_str)\b')

def log_arg_violations(text: str, arg_re):

    code = strip_comments(text)
    out = []
    for m in LOG_CALL_RE.finditer(code):
        depth, i = 1, m.end()
        while i < len(code) and depth:
            depth += {'(': 1, ')': -1}.get(code[i], 0)
            i += 1
        args = STRING_RE.sub('""', code[m.end():i - 1])
        if arg_re.search(args):
            out.append((code.count('\n', 0, m.start()) + 1, code[m.start():i].splitlines()[0].strip()))
    return out

def rest_raw_log_violations(text: str):

    return log_arg_violations(text, REST_RAW_ARG_RE)

def check_rest_raw_log_contract(root: Path = ROOT):

    fails = []
    for path in rpc_extract.rest_family(root):
        for line, call in rest_raw_log_violations(path.read_text(errors="replace")):
            fails.append(f"{path.relative_to(root).as_posix()}:{line}: 로그 인자에 요청 원문 변수 — {call}")
    return fails

def check_webhook_contract(root: Path = ROOT):

    fails = []
    for rel in WEBHOOK_LOG_TARGETS:
        for line, call in webhook_log_violations((root / rel).read_text(errors="replace")):
            fails.append(f"{rel}:{line}: 로그 인자에 웹훅 URL 원문 변수 — {call}")
    if not body_mask_has_webhook((root / "src/api/rest_request.c").read_text(errors="replace")):
        fails.append('src/api/rest_request.c: 본문 마스킹 키에 "webhook" 없음')
    return fails

def main(argv=None) -> int:
                                                          
                                                 
    argv = list(sys.argv[1:]) if argv is None else list(argv)
    if argv:
        rel = argv[0]
        text = Path(argv[0]).read_text(errors="replace")
    else:
        rel = "rest 계열(" + ", ".join(
            p.relative_to(ROOT).as_posix() for p in rpc_extract.rest_family(ROOT)) + ")"
        text = rpc_extract.rest_family_text(ROOT)
    defined, used = scan_text(text)

    print(f"[check-secret-logging] {HELPER} 정의 {'예' if defined else '아니오'} / "
          f"is_auth 산정 사용 {'예' if used else '아니오'}")

    fails = []
    if not defined:
        fails.append(f"{HELPER} 헬퍼 정의가 없음 (본문 기반 마스킹 판정 제거됨)")
    if not used:
        fails.append(f"is_auth 산정에서 {HELPER}(...) 미사용 — "
                     "/rpc 경유 평문 자격증명이 저널에 기록됨")

    if fails:
        print(f"[FAIL] 감사 로그 자격증명 마스킹 계약 위반 {len(fails)}건:", file=sys.stderr)
        for f in fails:
            print(f"  - {rel}: {f}", file=sys.stderr)
        return 1
    print(f"[PASS] {HELPER} 정의 + is_auth 산정에 결합 — 본문 기반 자격증명 마스킹 유지")
    if not argv:
        webhook_fails = check_webhook_contract()
        if webhook_fails:
            print(f"[FAIL] 웹훅 URL 로그 계약 위반 {len(webhook_fails)}건:", file=sys.stderr)
            for f in webhook_fails:
                print(f"  - {f}", file=sys.stderr)
            return 1
        print("[PASS] 웹훅 URL 원문 로그 없음 + 요청 본문 마스킹 키 유지")
        raw_fails = check_rest_raw_log_contract()
        if raw_fails:
            print(f"[FAIL] REST 요청 원문 로그 계약 위반 {len(raw_fails)}건:", file=sys.stderr)
            for f in raw_fails:
                print(f"  - {f}", file=sys.stderr)
            return 1
        print("[PASS] REST 계열 로그 인자에 요청 원문 없음")
    return 0


if __name__ == "__main__":
    sys.exit(main())
