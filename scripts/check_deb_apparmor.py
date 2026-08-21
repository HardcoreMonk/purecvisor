#!/usr/bin/env python3
                          
                                                                 
                                                                                      
                                                                                                                            
 
                      
                                                                                                                          
                                                                               

                                                                   
                                                                
                                                                   
                                                                          
                                           

                                                                   
                                                        

    
                                                                          
                                                                                 
                                                     
                                                                                
                                                                             
                                                            
                               
                                                                       
                                                         
                                                                          
                                                                  
                                                                          
                                                        
                        

                                                                     
                                                         
                                                                    

                                                                      
                                                   
   
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD_REL = "packaging/deb/build-deb.sh"
HELPER_REL = "packaging/apparmor/pcv-apparmor"

APROF = "/etc/apparmor.d/usr.local.bin.purecvisorsd"
DISLINK = "/etc/apparmor.d/disable/usr.local.bin.purecvisorsd"
FCLINK = "/etc/apparmor.d/force-complain/usr.local.bin.purecvisorsd"


                                                            
def strip_comments(text: str) -> str:
                                              

                                                             
                                                         
                                          
       
    out = []
    in_s = None                        
    prev = "\n"                                        
    i, n = 0, len(text)
    in_comment = False
    while i < n:
        ch = text[i]
        if in_comment:
            out.append("\n" if ch == "\n" else " ")
            if ch == "\n":
                in_comment = False
            i += 1
            continue
        if in_s:
            out.append(ch)
            if ch == "\\" and in_s == '"' and i + 1 < n:
                out.append(text[i + 1])
                i += 2
                continue
            if ch == in_s:
                in_s = None
            prev = ch
            i += 1
            continue
        if ch in ("'", '"'):
            in_s = ch
            out.append(ch)
            prev = ch
            i += 1
            continue
        if ch == "#" and (prev in ("\n", " ", "\t", ";", "(", "&", "|")):
            in_comment = True
            out.append(" ")
            i += 1
            continue
        out.append(ch)
        prev = ch
        i += 1
    return "".join(out)


def extract_generated(build_text: str, name: str):
                                                              

                                                           
                                                             
                               

                   
                               
                                                                   
                                                                       
                             
       
    target = r"\"\$STAGE/DEBIAN/" + re.escape(name) + r"\""
    start = re.compile(r"^cat\s*>>?\s*" + target + r"\s*<<\s*(['\"]?)([A-Za-z0-9_]+)\1\s*$", re.M)
    bodies, spans, problems = [], [], []
    for m in start.finditer(build_text):
        quote, delim = m.group(1), m.group(2)
        if not quote:
            line = build_text[: m.start()].count("\n") + 1
            problems.append(f"DEBIAN/{name}: unquoted heredoc `<<{delim}`(line {line}) — "
                            "빌드 시 변수 확장이 일어나 검사 텍스트가 산출물과 달라진다")
        tail = build_text[m.end():]
        rest = tail.lstrip("\n")
        base = m.end() + (len(tail) - len(rest))
        end = re.search(r"^" + re.escape(delim) + r"\s*$", rest, re.M)
        bodies.append(rest[: end.start()] if end else rest)
        spans.append((m.start(), base + (end.end() if end else len(rest))))
    if not bodies:
        problems.append(f"DEBIAN/{name} 을 만드는 heredoc 을 찾지 못함 — build-deb.sh 구조 변경?")
    for m in re.finditer(r">>?\s*" + target, build_text):
        if not any(s <= m.start() < e for s, e in spans):
            line = build_text[: m.start()].count("\n") + 1
            problems.append(f"DEBIAN/{name}: heredoc 이 아닌 기록 경로(line {line}) — "
                            "게이트가 보지 못하는 내용이 산출물에 들어갈 수 있다")
    return "\n".join(bodies), problems


_ASSIGN = re.compile(r"^\s*([A-Za-z_][A-Za-z0-9_]*)=(\S*)\s*$")
_VARREF = re.compile(r"\$\{([A-Za-z_][A-Za-z0-9_]*)\}|\$([A-Za-z_][A-Za-z0-9_]*)")


def _unquote(tok: str) -> str:
    if len(tok) >= 2 and tok[0] == tok[-1] and tok[0] in ("'", '"'):
        return tok[1:-1]
    return tok


def _expand(tok: str, env: dict) -> str:
    tok = _unquote(tok)
    return _VARREF.sub(lambda m: env.get(m.group(1) or m.group(2), ""), tok)


def commands(script: str, env: dict = None):
                                                    

                                                                   
                                                     
                                           
                                                     
                                 
       
    env = {} if env is None else env
    for raw in strip_comments(script).splitlines():
        line = raw.strip()
        if not line:
            continue
        a = _ASSIGN.match(line)
        if a:
            env[a.group(1)] = _expand(a.group(2), env)
            continue
                                         
        for part in re.split(r"(?<!\\);|&&|\|\|", line):
            toks = [_expand(t, env) for t in part.split()]
            toks = [t for t in toks if t]
            if toks:
                yield toks, raw


def _cmd_is(toks, name: str) -> bool:
                                                        
    head = [t for t in toks if t not in ("if", "then", "else", "elif", "do", "!")]
    if not head:
        return False
    base = head[0].rsplit("/", 1)[-1]
    return base == name


                                                             
                                                         
_LONG_REMOVE = {"--remove"}
_LONG_LOAD = {"--add", "--replace", "--reload"}


def apparmor_action(flags) -> str:
                                                                 

                                                                    
                                                                         
                                                              
                                                                  
                                                  
       
    has_remove = has_load = False
    for f in flags:
        if f.startswith("--"):
            name = f.split("=", 1)[0]
            if name in _LONG_REMOVE:
                has_remove = True
            elif name in _LONG_LOAD:
                has_load = True
        elif f.startswith("-"):
            if "R" in f[1:]:
                has_remove = True
            if any(c in f[1:] for c in "ar"):
                has_load = True
    return "remove" if (has_remove and not has_load) else "load"


                                                            
def check_postinst(postinst: str):
                                     
    bad = []
    if not postinst.strip():
        return ["postinst heredoc 을 찾지 못함 — build-deb.sh 구조 변경?"]

    loads, unlinks = [], []
    has_disable_link = has_unload = creates_fc = removes_fc = False
    for toks, raw in commands(postinst):
        if _cmd_is(toks, "apparmor_parser"):
            flags = [t for t in toks if t.startswith("-")]
            if apparmor_action(flags) == "load":
                loads.append(raw.strip())
            elif APROF in toks:
                has_unload = True
        elif _cmd_is(toks, "aa-enforce") or _cmd_is(toks, "aa-complain"):
            loads.append(raw.strip())
        elif _cmd_is(toks, "pcv-apparmor") and ("enforce" in toks or "complain" in toks):
            loads.append(raw.strip())
        elif _cmd_is(toks, "ln"):
            if DISLINK in toks:
                has_disable_link = True
            if FCLINK in toks:
                creates_fc = True
        elif _cmd_is(toks, "rm") or _cmd_is(toks, "unlink"):
            if DISLINK in toks:
                unlinks.append(raw.strip())
            if FCLINK in toks:
                removes_fc = True

    for cmd in loads:
        bad.append(f"[불변식1] postinst 가 프로필을 로드(부착)한다: {cmd}")
    if not has_disable_link:
        bad.append(f"[불변식2] postinst 가 disable 심링크({DISLINK})를 만들지 않는다 "
                   "— 프로필이 enforce-default 라 부팅 시 자동 로드된다")
    for cmd in unlinks:
                                                      
        bad.append(f"[불변식2] postinst 가 disable 심링크({DISLINK})를 제거한다: {cmd}")
    if not has_unload:
        bad.append(f"[불변식3] postinst 에 `apparmor_parser -R {APROF}` 해제가 없다 "
                   "— 1.x enforce 노드 업그레이드 시 confine 된 채 남는다")
    if creates_fc:
        bad.append(f"[불변식4] postinst 가 force-complain 심링크({FCLINK})를 만든다 "
                   "— 구 스킴(=complain 부착) 복귀")
    if not removes_fc:
        bad.append(f"[불변식4] postinst 가 구 스킴 잔재 force-complain 심링크({FCLINK})를 제거하지 않는다")
    return bad


def purge_block(postrm: str) -> str:
                                                                             

                                                                    
                                                     
       
    code = strip_comments(postrm)
    m = re.search(r'if\s+\[\s*"\$1"\s*=\s*"?purge"?\s*\]\s*;\s*then', code)
    if m:
        lines = code[m.end():].splitlines()
        depth, out = 1, []
        for ln in lines:
            s = ln.strip()
            if re.match(r"^if\b", s) or s.endswith("; then") and re.match(r"^if\b", s):
                depth += 1
            elif s == "fi":
                depth -= 1
                if depth == 0:
                    break
            out.append(ln)
        return "\n".join(out)
    m = re.search(r"^\s*(?:[A-Za-z0-9_|*]*\|)?purge(?:\|[A-Za-z0-9_|*]*)?\)\s*$", code, re.M)
    if m:
        rest = code[m.end():]
        end = re.search(r"^\s*;;\s*$", rest, re.M)
        return rest[: end.start()] if end else rest
    return ""


def check_postrm(postrm: str):
    bad = []
    if not postrm.strip():
        return ["postrm heredoc 을 찾지 못함 — build-deb.sh 구조 변경?"]
    block = purge_block(postrm)
    if not block.strip():
        return ["[불변식5] postrm 에서 purge 전용 구간을 찾지 못함 — 구조 변경?"]
    if not any(_cmd_is(t, "rm") and DISLINK in t for t, _ in commands(block)):
        bad.append(f"[불변식5] postrm 의 purge 구간이 disable 심링크({DISLINK})를 정리하지 않는다 "
                   "(purge 밖에 있으면 업그레이드 중 링크가 사라져 부팅 자동 로드가 되살아난다)")
    return bad


_FUNC = re.compile(r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s*\(\)\s*\{", re.M)


def _func_bodies(text: str) -> dict:
                                            
    out = {}
    for m in _FUNC.finditer(text):
        depth, i, n = 0, m.end() - 1, len(text)
        while i < n:
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        out[m.group(1)] = text[m.end():i]
    return out


def _case_branch(text: str, label: str) -> str:
                                             
    m = re.search(r"^\s*" + re.escape(label) + r"\)\s*$", text, re.M)
    if not m:
        return ""
    rest = text[m.end():]
    end = re.search(r"^\s*;;\s*$", rest, re.M)
    return rest[: end.start()] if end else rest


def check_helper(helper: str):
    bad = []
    if not helper.strip():
        return ["pcv-apparmor 헬퍼를 찾지 못함"]
    code = strip_comments(helper)
                                                             
                                                                
                                                         
    warn_funcs = {n for n, b in _func_bodies(code).items()
                  if "ADR-0028" in b and ">&2" in b}
                                                             
    env: dict = {}
    for _ in commands(helper, env):
        pass
    for label in ("enforce", "complain"):
        body = _case_branch(code, label)
        if not body:
            bad.append(f"[불변식6] pcv-apparmor 에 '{label}' 분기가 없다 — 헬퍼 구조 변경?")
            continue
        warned = "ADR-0028" in body or any(fn in body for fn in warn_funcs)
        if not warned:
            bad.append(f"[불변식6] pcv-apparmor '{label}' 분기에 ADR-0028 경고가 없다 "
                       "(직접 문자열 또는 경고 함수 호출)")
        if not any(_cmd_is(t, "rm") and DISLINK in t for t, _ in commands(body, dict(env))):
            bad.append(f"[불변식6] pcv-apparmor '{label}' 분기가 부착 전 disable 심링크"
                       f"({DISLINK})를 제거하지 않는다 — 재부팅 시 상태가 어긋난다")
    return bad


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="2.0 deb AppArmor 미부착 게이트 (ADR-0028)")
    ap.add_argument("--build-script", default=str(ROOT / BUILD_REL))
    ap.add_argument("--helper", default=str(ROOT / HELPER_REL))
    args = ap.parse_args(argv)

    build_text = Path(args.build_script).read_text(errors="replace")
    helper_text = Path(args.helper).read_text(errors="replace")

    postinst, p1 = extract_generated(build_text, "postinst")
    postrm, p2 = extract_generated(build_text, "postrm")

    bad = ([f"[구조] {p}" for p in p1 + p2]
           + check_postinst(postinst) + check_postrm(postrm) + check_helper(helper_text))

    print(f"[check-deb-apparmor] postinst {len(postinst.splitlines())}줄 / "
          f"postrm {len(postrm.splitlines())}줄 / 헬퍼 {len(helper_text.splitlines())}줄 검사")
    if bad:
        for b in bad:
            print(f"[FAIL] {b}", file=sys.stderr)
        print(f"[FAIL] ADR-0028 위반 {len(bad)}건 — 2.0 deb 는 AppArmor 프로필을 "
              "데몬에 부착하지 않는다(docs/adr/0028-*.md 결정 2)", file=sys.stderr)
        return 1
    print("[PASS] 2.0 deb: 프로필 미로드 + disable 심링크 + 로드분 해제 + 헬퍼 ADR-0028 경고")
    return 0


if __name__ == "__main__":
    sys.exit(main())
