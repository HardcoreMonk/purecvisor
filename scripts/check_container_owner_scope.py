
"""check_container_owner_scope.py — 컨테이너 operator owner-scope 게이트 (B1 / A01).

근거: 보안 평가 A01 — 컨테이너 owner-scope가 vm.*만 커버해 operator 교차테넌트 IDOR.

불변식(세 파일) — 전부 충족해야 PASS:

  (a) 강제(enforcement) — src/api/dispatcher.c
      - 게이트 함수 정의 실재: _container_method_requires_owner_scope,
        _lookup_container_owner, _container_owner_matches_caller,
        _container_owner_scoped_method_allowed.
      - owner-scope 세트: _container_method_requires_owner_scope 본문에
        "container.start", "container.stop", "container.clone" 세 메서드가 모두 존재.
      - 디스패치 배선: _container_owner_scoped_method_allowed 와
        _container_method_requires_owner_scope 가 정의 이외의 호출부에서도
        참조(각 식별자 2회 이상). 배선이 빠지면(게이트 정의만 있고 미호출) RED.

  (b) 스탬프(record) — src/modules/dispatcher/handler_container.c
      - container.create 성공 경로가 pcv_lxc_stamp_owner 를 호출(소유자 미기록 시
        전 컨테이너가 owner 부재 → operator 전면 차단, 기능 무력화).

  (c) 저장소(substrate) — src/modules/lxc/lxc_owner.c
      - pcv_lxc_stamp_owner / pcv_lxc_read_owner 정의 실재.
      - purecvisor.owner 파일 경로 리터럴 존재(저장소 규칙 drift 차단).

반사실: 세트에서 메서드 제거 / 게이트 함수 삭제 / 배선(operator 분기 호출) 제거 /
        create 스탬프 제거 / 저장소 함수·파일명 제거 — 어느 하나라도 RED.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DISPATCHER_REL = "src/api/dispatcher.c"
HANDLER_REL = "src/modules/dispatcher/handler_container.c"
OWNER_REL = "src/modules/lxc/lxc_owner.c"

OWNER_SCOPE_METHODS = ("container.start", "container.stop", "container.clone")
GATE_FNS = (
    "_container_method_requires_owner_scope",
    "_lookup_container_owner",
    "_container_owner_matches_caller",
    "_container_owner_scoped_method_allowed",
)

def strip_code(text: str) -> str:
    """주석·문자열·문자 리터럴 내용을 공백/개행으로 치환(오프셋 1:1 유지).
    브레이스 매칭·식별자 카운트용 — 문자열/주석 속 { } ; 토큰이 구조를 흐리지 않는다."""
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
    """fn '정의' 본문({...})의 RAW 슬라이스 반환(없으면 None).
    경계는 strip_code(code)에서 잡고(문자열/주석 속 중괄호 회피), 실제 내용은 오프셋이
    1:1 대응하는 raw text에서 잘라 문자열 리터럴("container.start" 등)을 보존한다.
    전방 선언(name(...);)·호출부는 건너뛰고 name(...){ 형태만 정의로 인정한다."""
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

def check_dispatcher(rel: str, text: str):
    code = strip_code(text)
    reasons = []

    for fn in GATE_FNS:
        if extract_fn_body(text, code, fn) is None:
            reasons.append(f"게이트 함수 {fn} 정의 없음")

    set_body = extract_fn_body(text, code, "_container_method_requires_owner_scope")
    if set_body is None:
        reasons.append("_container_method_requires_owner_scope 본문 부재 — owner-scope 세트 확인 불가")
    else:
        for meth in OWNER_SCOPE_METHODS:
            if f'"{meth}"' not in set_body:
                reasons.append(f"owner-scope 세트에 {meth} 누락 — operator 교차테넌트 재노출")

    for fn in ("_container_owner_scoped_method_allowed",
               "_container_method_requires_owner_scope"):
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
    disp = Path(argv[0]) if len(argv) >= 1 else ROOT / DISPATCHER_REL
    handler = Path(argv[1]) if len(argv) >= 2 else ROOT / HANDLER_REL
    owner = Path(argv[2]) if len(argv) >= 3 else ROOT / OWNER_REL

    disp_ok, disp_r = check_dispatcher(DISPATCHER_REL, disp.read_text(errors="replace"))
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
    print("[PASS] container.start/stop/clone owner-scope 세트 + 게이트 배선 + create 스탬프 + 저장소 충족")
    return 0

if __name__ == "__main__":
    sys.exit(main())
