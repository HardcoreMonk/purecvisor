#!/usr/bin/env python3
                          
                                                                  
                                                                                      
                                                                                                                            
 
                      
                                                                                                                           
                                                                      

                                                                      

                                          

                                                                 
                                                     
                                                             
                                                 

                                                                  
                                                
                                             
                                     
                                                                        
                                                  
                                                
                                         

                                                             
                                        
   
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TARGET_REL = "src/api/uds_server.c"
TARGET = ROOT / TARGET_REL

                                                    
ACCEPT_FNS = ["on_incoming_connection", "_uring_accept_cb"]
                                        
PEERCRED_MARKERS = ["_uds_peer_is_root", "SO_PEERCRED"]

UMASK_SAFE_RE = re.compile(r'umask\s*\(\s*0117\s*\)')
UMASK_BAD_RE = re.compile(r'umask\s*\(\s*0111\s*\)')
NONROOT_REJECT_RE = re.compile(r'\.uid\s*!=\s*0')
GETSOCKOPT_RE = re.compile(r'\bgetsockopt\s*\(')
PEERCRED_TOKEN = "SO_PEERCRED"
RESIDUAL_0666_RE = re.compile(r'0666')


def strip_code(text: str) -> str:
                                                     
                                                         
    out = []
    i, n = 0, len(text)
    in_block = in_line = False
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
            quote = ch
            out.append(' ')
            i += 1
            while i < n:
                c2 = text[i]
                if c2 == '\\' and i + 1 < n:
                    out.append(' \n' if text[i + 1] == '\n' else '  ')
                    i += 2
                    continue
                if c2 == quote:
                    out.append(' ')
                    i += 1
                    break
                out.append('\n' if c2 == '\n' else ' ')
                i += 1
            continue
        out.append(ch)
        i += 1
    return ''.join(out)


def _match_delims(s: str, open_pos: int, opench: str, closech: str) -> int:
                                                                
    depth = 0
    for i in range(open_pos, len(s)):
        c = s[i]
        if c == opench:
            depth += 1
        elif c == closech:
            depth -= 1
            if depth == 0:
                return i
    return -1


def _match_brace(s: str, open_pos: int) -> int:
    return _match_delims(s, open_pos, '{', '}')


def _extract_fn_body(code: str, fn: str):
                                                        
                                                           
    for m in re.finditer(r'\b' + re.escape(fn) + r'\s*\(', code):
        paren_open = m.end() - 1                      
        paren_close = _match_delims(code, paren_open, '(', ')')
        if paren_close < 0:
            continue
        j = paren_close + 1
        while j < len(code) and code[j] in ' \t\r\n':
            j += 1
        if j < len(code) and code[j] == '{':                           
            close = _match_brace(code, j)
            if close < 0:
                return None
            return code[j:close + 1]
                                          
    return None


def check_socket_perms(text: str, code: str):
                                                          
    reasons = []
    safe = len(UMASK_SAFE_RE.findall(code))
    bad = len(UMASK_BAD_RE.findall(code))
    if bad > 0:
        reasons.append(f"umask(0111) {bad}건 잔존 — 소켓 0666(rw-rw-rw-)로 회귀")
    if safe < 2:
        reasons.append(f"umask(0117) {safe}건(<2) — 양 bind 사이트가 0660으로 생성해야 함")
    if RESIDUAL_0666_RE.search(text):
        reasons.append("0666 리터럴 잔존(코드/주석) — 소켓 권한 드리프트")
    return (not reasons), reasons


def check_peercred(code: str):
                                                          
    reasons = []
    if PEERCRED_TOKEN not in code:
        reasons.append("SO_PEERCRED 토큰 없음 — 피어 신원 게이트 미구현")
    if not GETSOCKOPT_RE.search(code):
        reasons.append("getsockopt 호출 없음 — 피어 자격 취득 경로 부재")
    if not NONROOT_REJECT_RE.search(code):
        reasons.append("비-root 거부 비교(.uid != 0) 없음 — root-only 강제 부재")
    for fn in ACCEPT_FNS:
        body = _extract_fn_body(code, fn)
        if body is None:
            reasons.append(f"{fn} 함수 본문을 찾지 못함")
            continue
        if not any(mk in body for mk in PEERCRED_MARKERS):
            reasons.append(f"{fn} 본문에 peercred 게이트 마커 없음 — 이 accept 경로가 비-root 우회")
    return (not reasons), reasons


def scan_text(text: str):
    code = strip_code(text)
    perms_ok, perms_reasons = check_socket_perms(text, code)
    peer_ok, peer_reasons = check_peercred(code)
    return perms_ok, perms_reasons, peer_ok, peer_reasons


def main(argv=None) -> int:
    argv = list(sys.argv[1:]) if argv is None else list(argv)
    target = Path(argv[0]) if argv else TARGET
    rel = argv[0] if argv else TARGET_REL
    text = target.read_text(errors="replace")

    perms_ok, perms_reasons, peer_ok, peer_reasons = scan_text(text)

    print(f"[check-uds-authz] 소켓 0660(umask 0117) {'예' if perms_ok else '아니오'} / "
          f"SO_PEERCRED root-only 게이트 {'예' if peer_ok else '아니오'}")

    fails = []
    if not perms_ok:
        fails += [f"소켓 권한: {r}" for r in perms_reasons]
    if not peer_ok:
        fails += [f"접근 게이트: {r}" for r in peer_reasons]

    if fails:
        print(f"[FAIL] UDS root-only 접근 불변식 위반 {len(fails)}건 ({rel}):", file=sys.stderr)
        for f in fails:
            print(f"  - {f}", file=sys.stderr)
        return 1
    print(f"[PASS] UDS 소켓 0660 + SO_PEERCRED root-only 게이트(양 accept 경로) 충족 ({rel})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
