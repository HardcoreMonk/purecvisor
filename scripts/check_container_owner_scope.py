#!/usr/bin/env python3
                          
                                                                           
                                                                                      
                                                                                                                            
 
                      
                                                                                                                                    
                                                                             

                                                                

                         

                                            
                                                             
                                                                 
                                               
                                                                  
                                                                            
                                                         
                                                              
                                                     

                                                              
                                                                  
                                                   

                                                  
                                                       
                                                       

                                                      
                                                    
   
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rpc_extract

ROOT = Path(__file__).resolve().parent.parent
DISPATCHER_REL = "src/api/dispatcher.c"
HANDLER_REL = "src/modules/dispatcher/handler_container.c"
OWNER_REL = "src/modules/lxc/lxc_owner.c"

OWNER_SCOPE_METHODS = (
    "container.start", "container.stop", "container.clone", "container.logs",
    "container.memory.stats", "container.health.check",
)
GATE_FNS = (
    "_container_method_requires_owner_scope",
    "_lookup_container_owner",
    "_container_owner_matches_caller",
    "_container_owner_scoped_method_allowed",
)


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


def extract_fn_body(text: str, code: str, fn: str):
                                                
                                                           
                                                              
                                                           
    for m in re.finditer(r'\b' + re.escape(fn) + r'\s*\(', code):
        paren_open = m.end() - 1
        paren_close = _match_delims(code, paren_open, '(', ')')
        if paren_close < 0:
            continue
        j = paren_close + 1
        while j < len(code) and code[j] in ' \t\r\n':
            j += 1
        if j < len(code) and code[j] == '{':
            close = _match_delims(code, j, '{', '}')
            if close < 0:
                return None
            return text[j:close + 1]
    return None


def _resolve_fn(text: str, code: str, fn: str) -> str:

    if extract_fn_body(text, code, fn) is not None:
        return fn
    return rpc_extract.split_renames(ROOT).get(fn, fn)

def check_dispatcher(rel: str, text: str):
    code = strip_code(text)
    reasons = []

    for fn in GATE_FNS:
        fn = _resolve_fn(text, code, fn)
        if extract_fn_body(text, code, fn) is None:
            reasons.append(f"게이트 함수 {fn} 정의 없음")

    set_fn = _resolve_fn(text, code, "_container_method_requires_owner_scope")
    set_body = extract_fn_body(text, code, set_fn)
    if set_body is None:
        reasons.append(f"{set_fn} 본문 부재 — owner-scope 세트 확인 불가")
    else:
        for meth in OWNER_SCOPE_METHODS:
            if f'"{meth}"' not in set_body:
                reasons.append(f"owner-scope 세트에 {meth} 누락 — operator 교차테넌트 재노출")

                                             
    for fn in (_resolve_fn(text, code, "_container_owner_scoped_method_allowed"),
               set_fn):
        cnt = len(re.findall(r'\b' + re.escape(fn) + r'\b', code))
        if cnt < 2:
            reasons.append(f"{fn} 디스패치 배선 없음(참조 {cnt}회<2) — 게이트 미호출(우회)")

    return (not reasons), reasons


def check_stamp(rel: str, text: str):
    code = strip_code(text)
    reasons = []
    if not re.search(r'\bpcv_lxc_stamp_owner\s*\(', code):
        reasons.append("container.create 경로에 pcv_lxc_stamp_owner 호출 없음 — 소유자 미기록(operator 전면 차단)")
    return (not reasons), reasons


def check_substrate(rel: str, text: str):
    code = strip_code(text)
    reasons = []
    for fn in ("pcv_lxc_stamp_owner", "pcv_lxc_read_owner"):
                                   
        if extract_fn_body(text, code, fn) is None:
            reasons.append(f"저장소 함수 {fn} 정의 없음")
    if "purecvisor.owner" not in text:
        reasons.append("purecvisor.owner 파일 경로 리터럴 없음 — 저장소 규칙 drift")
    return (not reasons), reasons


def main(argv=None) -> int:
    argv = list(sys.argv[1:]) if argv is None else list(argv)
    handler = Path(argv[1]) if len(argv) >= 2 else ROOT / HANDLER_REL
    owner = Path(argv[2]) if len(argv) >= 3 else ROOT / OWNER_REL

    disp_text = (Path(argv[0]).read_text(errors="replace") if len(argv) >= 1
                 else rpc_extract.dispatcher_family_text(ROOT))
    disp_ok, disp_r = check_dispatcher(DISPATCHER_REL, disp_text)
    stamp_ok, stamp_r = check_stamp(HANDLER_REL, handler.read_text(errors="replace"))
    sub_ok, sub_r = check_substrate(OWNER_REL, owner.read_text(errors="replace"))

    print(f"[check-container-owner-scope] 강제 {'예' if disp_ok else '아니오'} / "
          f"스탬프 {'예' if stamp_ok else '아니오'} / 저장소 {'예' if sub_ok else '아니오'}")

    fails = []
    fails += [f"강제(dispatcher): {r}" for r in disp_r]
    fails += [f"스탬프(handler): {r}" for r in stamp_r]
    fails += [f"저장소(lxc_owner): {r}" for r in sub_r]

    if fails:
        print(f"[FAIL] 컨테이너 owner-scope 불변식 위반 {len(fails)}건:", file=sys.stderr)
        for f in fails:
            print(f"  - {f}", file=sys.stderr)
        return 1
    print("[PASS] container.start/stop/clone/logs/memory.stats/health.check owner-scope 세트 + 게이트 배선 + create 스탬프 + 저장소 충족")
    return 0


if __name__ == "__main__":
    sys.exit(main())
