#!/usr/bin/env python3
                          
                                                                  
                                                                                      
                                                                                                                                                                                          
 
                      
                                                                                                                           
                                                                                 

                                                                           
                                                                              
                                                            
                                                     
                                                    
                                                       
                                                 
                                                                                  

                                                                  
                                         

               
                                                                     
                                                                  
                                                        
                                                                   
                                                            
                                 

          
                                                         
                                                           

          
                                                                 
                                                              

                 
                                                         
                                                  
                                       

                  
                                                              
                                                      
                                                                         
                                                         

                                     
                                                  
                                       

                                   
                                                                       
                                                             
                                                 
                                                                            
                               
                                                          
                                                
                                                    
                                                    
                                                 

                                        
                                                                
                                                         
                                                          
                                   

                 
                                                              
                                            
                                                                

          
                                                 
                                                            
   
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BASELINE_FILE = ROOT / "scripts" / "rerror_guard_baseline.txt"

                                                                 
EXCLUDE_NAMES = {"bundle.js", "app.bundle.js"}

CALL_RE = re.compile(
    r"\bawait\s+(?:PCV\.api\.|window\.)?fetch(Get|Post|Put|Delete)\s*\("
)
VERB = {"Get": "GET", "Post": "POST", "Put": "PUT", "Delete": "DELETE"}
STATE_CHANGING = {"POST", "PUT", "DELETE"}

                                                             
                                                
                                            
 
                                                                       
                                                       
                                                          
                                                    
                                             
RPC_READ_SUFFIXES = {
    "list", "get", "status", "stats", "history", "info", "detail", "details",
    "show", "recent", "forecast", "usage", "summary", "count", "search",
    "query", "capabilities", "pending", "current", "top", "tree", "describe",
    "preview",
}

                                                                    
                                                             
                                                 
IDENTITY_UNWRAPPERS = ("unwrapData",)

                                                 
_REGEX_PREV = set("(,=:[!&|?{};+-*%~^<>")


def mask_js(text):
                                                    

                                                 
                                                                    
                                    
       
    out = list(text)
    i, n = 0, len(text)
    prev_sig = ""                                
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                out[i] = " "
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            while i < n and not (text[i] == "*" and i + 1 < n and text[i + 1] == "/"):
                if text[i] != "\n":
                    out[i] = " "
                i += 1
            for j in range(i, min(i + 2, n)):
                out[j] = " "
            i = min(i + 2, n)
            continue
        if c == "/" and (prev_sig == "" or prev_sig in _REGEX_PREV):
                                               
            j, in_class, closed = i + 1, False, False
            while j < n and text[j] != "\n":
                d = text[j]
                if d == "\\":
                    j += 2
                    continue
                if d == "[":
                    in_class = True
                elif d == "]":
                    in_class = False
                elif d == "/" and not in_class:
                    closed = True
                    j += 1
                    break
                j += 1
            if closed:
                for k in range(i, j):
                    out[k] = " "
                prev_sig = "/"
                i = j
                continue
        if c in "'\"`":
            quote = c
            j = i + 1
            while j < n:
                d = text[j]
                if d == "\\":
                    j += 2
                    continue
                if d == quote:
                    j += 1
                    break
                j += 1
            for k in range(i, min(j, n)):
                if text[k] != "\n":
                    out[k] = " "
            prev_sig = quote
            i = min(j, n)
            continue
        if not c.isspace():
            prev_sig = c
        i += 1
    return "".join(out)


def _match_paren(masked, open_idx):
                                                  
    depth = 0
    for i in range(open_idx, len(masked)):
        c = masked[i]
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return i
    return -1


def _block_end(masked, start):
                                                       
    depth = 0
    for i in range(start, len(masked)):
        c = masked[i]
        if c in "({[":
            depth += 1
        elif c in ")}]":
            depth -= 1
            if depth < 0:
                return i
    return len(masked)


                                        
_BIND_RE = re.compile(r"\b(?:const|let|var)\s+([A-Za-z_$][\w$]*)\s*=\s*$")
_DESTRUCT_RE = re.compile(r"\b(?:const|let|var)\s*\{([^{}]*)\}\s*=\s*$")
_ASSIGN_RE = re.compile(r"(?:^|[^\w$.])([A-Za-z_$][\w$]*)\s*=\s*$")
_RETURN_RE = re.compile(r"\breturn\s*$")


def classify_receiver(masked, call_idx):
                                                                                     

                                                                  
                                                  
                                                              
                                                               
                               
       
    look = masked[max(0, call_idx - 200):call_idx]
    m = _DESTRUCT_RE.search(look)
    if m:
        return "destructure", [x.strip().split(":")[0].strip() for x in m.group(1).split(",")]
    lo = 0
    for ch in ";{}":
        p = masked.rfind(ch, 0, call_idx)
        if p > lo:
            lo = p
    prefix = masked[lo:call_idx]
    m = _BIND_RE.search(prefix)
    if m:
        return "bind", m.group(1)
    if _RETURN_RE.search(prefix):
        return "return", None
    m = _ASSIGN_RE.search(prefix)
    if m:
        return "bind", m.group(1)
                                                                            
                                               
    loose = list(re.finditer(r"\b(?:const|let|var)\s+([A-Za-z_$][\w$]*)\s*=", prefix))
    if loose:
        return "bind", loose[-1].group(1)
    return "none", None


def has_guard(masked, var, start, end):
                                                             

                                                                   
                                             
                                                                              
                                                              
                                                       
       
    window = masked[start:end]
    names = [var]
    unwrap = "|".join(re.escape(u) for u in IDENTITY_UNWRAPPERS)
    for m in re.finditer(
            r"\b(?:const|let|var)\s+([A-Za-z_$][\w$]*)\s*=\s*"
            r"(?:(?:[A-Za-z_$][\w$.]*\.)?(?:%s)\s*\(\s*)?%s\s*[);,]"
            % (unwrap, re.escape(var)), window):
        names.append(m.group(1))
    for name in names:
        v = re.escape(name)
        if re.search(r"\b%s\s*(?:\?\.|\.)\s*error\b" % v, window):
            return True
        if re.search(r"\b(?:const|let|var)\s*\{[^{}]*\berror\b[^{}]*\}\s*=\s*%s\b" % v, window):
            return True
    return False


def _guard_window_end(masked, var, start):
                                                             
    end = _block_end(masked, start)
    v = re.escape(var)
    m = re.search(
        r"\b(?:const|let|var)\s+%s\s*=|\b%s\s*=\s*await\b" % (v, v), masked[start:end]
    )
    if m:
        end = start + m.start()
    return end


_EP_RE = re.compile(r"\bEP\.([A-Za-z_0-9]+)")
_METHOD_RE = re.compile(r"\bmethod\s*:\s*['\"]([^'\"]+)['\"]")
_PATH_RE = re.compile(r"['\"](/[^'\"]*)['\"]")


def target_key(arg_src, body_src):
                                                                

                                                        
                                                                     
                                                       
                        
       
    method = _METHOD_RE.search(body_src)
    method = method.group(1) if method else None
    m = _EP_RE.search(arg_src)
    if m:
        name = "EP." + m.group(1)
        if m.group(1) in ("RPC", "VM_RPC"):
            return (name + ":" + method if method else name), True, method
        return name, False, None
    m = _PATH_RE.search(arg_src)
    if m:
        path = m.group(1).split("?")[0]
        if path.rstrip("/").endswith("/rpc"):
            return (path + ":" + method if method else path), True, method
        return path, False, None
    return (re.sub(r"\s+", " ", arg_src.strip())[:48] or "(none)"), False, None


def classify_tier(verb, is_rpc, rpc_method):
                               

                                                            
                                               
                                           
       
    if verb not in STATE_CHANGING:
        return "B"
    if is_rpc:
        if rpc_method and rpc_method.rsplit(".", 1)[-1].lower() in RPC_READ_SUFFIXES:
            return "B"
        return "A"
    return "A"


def analyze_source(rel, src):
                              

                                                                                
                                       
       
    masked = mask_js(src)
    seen = {}
    out = []
    for m in CALL_RE.finditer(masked):
        verb = VERB[m.group(1)]
        open_idx = m.end() - 1
        close_idx = _match_paren(masked, open_idx)
        if close_idx < 0:
            close_idx = len(masked) - 1
        args_src = src[open_idx + 1:close_idx]
        depth, split = 0, len(args_src)
        for i, c in enumerate(masked[open_idx + 1:close_idx]):
            if c in "([{":
                depth += 1
            elif c in ")]}":
                depth -= 1
            elif c == "," and depth == 0:
                split = i
                break
        arg0, body = args_src[:split], args_src[split + 1:]

        kind, var = classify_receiver(masked, m.start())
        if kind == "return":
            continue                                                   
        if kind == "destructure":
            if any(n == "error" for n in var):
                continue
            guarded = False
        elif kind == "bind":
            end = _guard_window_end(masked, var, close_idx + 1)
            guarded = has_guard(masked, var, close_idx + 1, end)
        else:
            guarded = False                                          
        if guarded:
            continue

        tkey, is_rpc, rpc_method = target_key(arg0, body)
        base = "%s|%s|%s" % (rel, verb, tkey)
        seen[base] = seen.get(base, 0) + 1
        key = "%s#%d" % (base, seen[base])
        tier = classify_tier(verb, is_rpc, rpc_method)
        line = src.count("\n", 0, m.start()) + 1
        out.append((key, tier, verb, line, kind))
    return out


def scan():
                                                                     
    files = [ROOT / "ui" / "app.js"] + sorted((ROOT / "ui" / "modules").glob("*.js"))
    findings = []
    for path in files:
        if not path.is_file() or path.name in EXCLUDE_NAMES or path.name.endswith(".min.js"):
            continue
        rel = path.relative_to(ROOT).as_posix()
        src = path.read_text(encoding="utf-8", errors="replace")
        for key, tier, verb, line, kind in analyze_source(rel, src):
            findings.append((key, tier, verb, rel, line, kind))
    return findings


def load_baseline():
    if not BASELINE_FILE.exists():
        return set()
    out = set()
    for line in BASELINE_FILE.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            out.add(line)
    return out


def load_annotations():
                                                  

                                                        
                                                 
       
    ann = {}
    if not BASELINE_FILE.exists():
        return ann
    buf = []
    for line in BASELINE_FILE.read_text(encoding="utf-8").splitlines():
        s = line.strip()
        if s.startswith("#"):
            buf.append(line.rstrip())
        elif not s:
            buf = []
        else:
            if buf:
                ann[s] = buf
            buf = []
    return ann


def write_baseline(findings, allow_tier_a):
    tier_a = sorted(k for k, t, *_ in findings if t == "A")
    tier_b = sorted(k for k, t, *_ in findings if t == "B")
    ann = load_annotations()

    def block(keys):
        out = []
        for k in keys:
            out.extend(ann.get(k, []))
            out.append(k)
        return "\n".join(out)

    if tier_a and not allow_tier_a:
        print("[generate 거부] Tier A(상태 변경) 미검사 호출부 %d건은 기준선 등재 대상이 아니다."
              % len(tier_a))
        for k in tier_a:
            print("  - " + k)
        print("시정하거나, 부득이하면 --allow-tier-a 로 사유를 남겨 등재한다.")
        return 1
    head = (
        "# r.error 미검사 호출부 기준선 (2026-08-06, 설계 §8.2) — 단조감소 래칫.\n"
        "#\n"
        "# 형식: <파일>|<HTTP VERB>|<엔드포인트>#<파일 내 출현 순번>\n"
        "# 라인 번호를 키로 쓰지 않는다 — UI 파일은 편집이 잦아 무관한 편집마다 대량\n"
        "# 오탐이 난다. 현재 라인 번호는 게이트 출력에서 확인한다.\n"
        "#\n"
        "# 이 명단 밖의 신규 미검사 호출부만 FAIL. 시정하면 이 명단에서 제거한다\n"
        "# (추가는 부채 증가다). 생성: python3 scripts/check_rerror_guard.py --generate\n"
        "# 항목 바로 위 주석은 재생성해도 보존된다(빈 줄이 블록을 끊는다).\n"
        "\n"
    )
    sections = []
    if tier_a:
        sections.append(
            "# ═══ Tier A (상태 변경) ═══ 실패를 성공으로 오보할 수 있다. 원칙적으로 등재\n"
            "# 금지이며 --allow-tier-a 로 강제 등재됐다. 각 줄 위에 왜 지금 안 고치는지 남긴다.\n"
            "\n" + block(tier_a)
        )
    sections.append(
        "# ═══ Tier B (조회) ═══ 실패가 '데이터 없음'이라는 거짓 빈 상태로 보인다. 당장\n"
        "# 위험하진 않으나 장애를 숨기고 디버깅을 어렵게 한다. 접점을 고칠 때 함께 걷어낸다.\n"
        "\n" + block(tier_b)
    )
    BASELINE_FILE.write_text(head + "\n\n".join(sections) + "\n", encoding="utf-8")
    print("[generate] 기준선 %d건 기록 (Tier A %d / Tier B %d): %s"
          % (len(tier_a) + len(tier_b), len(tier_a), len(tier_b),
             BASELINE_FILE.relative_to(ROOT)))
    return 0


def run_check():
    findings = scan()
    baseline = load_baseline()
    by_key = {f[0]: f for f in findings}

    a_all = [f for f in findings if f[1] == "A"]
    b_all = [f for f in findings if f[1] == "B"]
    new = sorted((f for f in findings if f[0] not in baseline), key=lambda f: (f[1], f[3], f[4]))
    stale = sorted(b for b in baseline if b not in by_key)

    print("[check-rerror-guard] 미검사 호출부 Tier A(상태변경) %d건 / Tier B(조회) %d건, "
          "기준선 %d건 (유효 %d)"
          % (len(a_all), len(b_all), len(baseline), len(baseline) - len(stale)))
    for b in stale:
        print("[INFO] 기준선 항목이 더는 위반이 아님(prune 권장): %s" % b)

    if new:
        print("\n[FAIL] 기준선 밖 `r.error` 미검사 호출부 %d건:" % len(new))
        for key, tier, verb, rel, line, kind in new:
            note = {"bind": "반환을 변수에 받고 .error 미검사",
                    "destructure": "구조분해에 error 없음",
                    "none": "반환값을 받지 않음"}.get(kind, kind)
            print("  - [Tier %s] %s:%d  %s  (%s)" % (tier, rel, line, key, note))
        print("\n시정: `const r = await fetch…(); if (r.error) { …실패 표면화…; return; }`.\n"
              "      try/catch 는 가드가 아니다 — fetch* 는 HTTP 오류에서 throw 하지 않는다\n"
              "      (ui/modules/api.js `[에러 반환 패턴]`).\n"
              "      Tier A(상태 변경)는 기준선 등재 대상이 아니다 — 반드시 시정한다.")
        return 1
    print("[PASS] 기준선 밖 신규 `r.error` 미검사 호출부 없음")
    return 0


def self_test():
                                                                   

                                                
                                                          
       
    ok = True

    def check(cond, msg):
        nonlocal ok
        if not cond:
            print("[SELF-TEST FAIL] " + msg)
            ok = False

                      
    r = analyze_source("t.js", "async function f(){ const r = await fetchGet(EP.VM_LIST());\n"
                                "  if (r.error) { msg('x'); return; }\n  use(r); }\n")
    check(r == [], "가드(`if (r.error)`) 있는 호출을 위반으로 오판: %r" % (r,))

                                             
    r = analyze_source("t.js", "async function f(){ const r = await fetchGet(EP.VM_LIST());\n"
                                "  render(r.items); }\n")
    check(len(r) == 1 and r[0][0] == "t.js|GET|EP.VM_LIST#1" and r[0][1] == "B"
          and r[0][3] == 1 and r[0][4] == "bind",
          "미검사 GET 호출을 잡지 못했거나 키/라인/수신형태가 다름: %r" % (r,))

                                                                 
    r = analyze_source("t.js", "async function f(){ try { const r = await fetchGet(EP.A());\n"
                                "  render(r); } catch(e) { toast(e); } }\n")
    check(len(r) == 1 and r[0][1] == "B",
          "try/catch 를 가드로 오인(계약상 HTTP 오류는 throw 되지 않는다): %r" % (r,))

                      
    r = analyze_source("t.js", "async function f(){ await fetchPost(EP.VM_START(n), {}); }\n")
    check(len(r) == 1 and r[0][1] == "A" and r[0][2] == "POST" and r[0][4] == "none",
          "수신 변수 없는 POST 를 Tier A 위반으로 잡지 못함: %r" % (r,))
    for fn, verb in (("fetchPut", "PUT"), ("fetchDelete", "DELETE")):
        r = analyze_source("t.js", "async function f(){ var x = await %s(EP.A(), {}); use(x); }\n" % fn)
        check(len(r) == 1 and r[0][1] == "A" and r[0][2] == verb,
              "%s 를 Tier A/%s 로 분류하지 못함: %r" % (fn, verb, r))

                                     
    r = analyze_source("t.js", "async function f(){ const { error } = await fetchGet(EP.A()); }\n")
    check(r == [], "구조분해 `{ error }` 를 가드로 인정하지 않음: %r" % (r,))
    r = analyze_source("t.js", "async function f(){ const { items } = await fetchGet(EP.A()); }\n")
    check(len(r) == 1 and r[0][4] == "destructure",
          "error 없는 구조분해를 위반으로 잡지 못함: %r" % (r,))

                                                            
    r = analyze_source("t.js", "async function f(){ return await fetchGet(EP.A()); }\n")
    check(r == [], "반환 위임을 위반으로 오판(문서화된 계약과 불일치): %r" % (r,))

                       
    r = analyze_source("t.js", "async function f(){ const r = await fetchGet(EP.A());\n"
                                "  if (r?.error) return; }\n")
    check(r == [], "`r?.error` 가드를 인정하지 않음: %r" % (r,))
    r = analyze_source("t.js", "async function f(){ var r = await fetchGet(EP.A());\n"
                                "  var xs = (r && !r.error) ? r.items : []; }\n")
    check(r == [], "`r && !r.error` 가드를 인정하지 않음: %r" % (r,))

                                
    r = analyze_source("t.js", "async function f(){ const a = await fetchGet(EP.A());\n"
                                "  if (b.error) return; }\n")
    check(len(r) == 1, "다른 변수(b)의 .error 를 a 의 가드로 오인: %r" % (r,))

                                        
    r = analyze_source("t.js", "async function f(){ if (x) { const r = await fetchGet(EP.A()); }\n"
                                "  if (r.error) return; }\n")
    check(len(r) == 1, "블록 밖의 .error 를 가드로 오인: %r" % (r,))

                                              
    r = analyze_source("t.js", "async function f(){ var r = await fetchGet(EP.A());\n"
                                "  var r = await fetchGet(EP.B());\n"
                                "  if (r.error) return; }\n")
    check(len(r) == 1 and r[0][0] == "t.js|GET|EP.A#1",
          "재바인딩 이후의 가드를 앞 호출이 빌림: %r" % (r,))

                                            
    r = analyze_source("t.js", "// const r = await fetchGet(EP.A());\n"
                                "var s = 'await fetchPost(EP.B())';\n"
                                "/* await fetchPut(EP.C()); */\n")
    check(r == [], "주석·문자열 속 호출을 실제 호출로 오인: %r" % (r,))

                                             
    masked = mask_js("var fn = p.replace(/^.*\\//, ''); var r = 1;\n")
    check("var r = 1" in masked, "정규식 리터럴 뒤 코드가 마스킹으로 소실됨: %r" % (masked,))
    r = analyze_source("t.js", "async function f(){ var fn = p.replace(/^.*\\//, '');\n"
                                "  await fetchDelete(EP.A()); }\n")
    check(len(r) == 1 and r[0][2] == "DELETE",
          "정규식 리터럴 뒤의 호출을 놓침: %r" % (r,))

                                              
    r = analyze_source("t.js", "async function f(){ await fetchGet(EP.A());\n"
                                "  await fetchGet(EP.A()); }\n")
    check([x[0] for x in r] == ["t.js|GET|EP.A#1", "t.js|GET|EP.A#2"],
          "동일 엔드포인트 반복 호출의 순번 부여 실패: %r" % (r,))

                                            
    r = analyze_source("t.js", "async function f(){ await fetchPost(EP.RPC(), "
                                "{ jsonrpc:'2.0', method:'vm.list', params:{}, id:'1' }); }\n")
    check(len(r) == 1 and r[0][0] == "t.js|POST|EP.RPC:vm.list#1",
          "EP.RPC 본문 method 로 키를 가르지 못함: %r" % (r,))

                                           
    r = analyze_source("t.js", "async function f(){ const r = await PCV.api.fetchGet(EP.A()); use(r); }\n")
    check(len(r) == 1 and r[0][2] == "GET", "PCV.api.fetchGet 호출을 놓침: %r" % (r,))

                                                               
    r = analyze_source("t.js", "async function f(){ var json = A.fetchPost\n"
                                "    ? await A.fetchPost(EP.RPC(), body)\n"
                                "    : await fetchPost(EP.RPC(), body);\n"
                                "  if (json && json.error) throw new Error('x'); }\n")
    check(r == [], "삼항 뒤 대입 변수의 가드를 인정하지 않음: %r" % (r,))

                                                     
    r = analyze_source("t.js", "async function f(){ var r = await fetchPost(EP.RPC(), b);\n"
                                "  var d = unwrapData(r);\n"
                                "  if (d && d.error) throw new Error('x'); }\n")
    check(r == [], "unwrapData 별칭 뒤의 .error 가드를 인정하지 않음: %r" % (r,))
                                                 
    r = analyze_source("t.js", "async function f(){ var r = await fetchPost(EP.RPC(), b);\n"
                                "  var d = unwrapList(r);\n"
                                "  if (d.error) return; }\n")
    check(len(r) == 1, "unwrapList 를 error 보존 언랩으로 오인: %r" % (r,))

                                                      
    r = analyze_source("t.js", "async function f(){ await fetchPost(EP.RPC(), "
                                "{ method:'gpu.list', params:{} }); }\n")
    check(len(r) == 1 and r[0][1] == "B",
          "조회성 RPC(gpu.list)를 POST 라는 이유로 Tier A 로 오분류: %r" % (r,))
    r = analyze_source("t.js", "async function f(){ await fetchPost(EP.RPC(), "
                                "{ method:'vm.create', params:{} }); }\n")
    check(len(r) == 1 and r[0][1] == "A",
          "변경성 RPC(vm.create)를 Tier A 로 잡지 못함: %r" % (r,))
                                  
    r = analyze_source("t.js", "async function f(){ await fetchPost(EP.RPC(), "
                                "{ method: m, params:{} }); }\n")
    check(len(r) == 1 and r[0][1] == "A" and r[0][0] == "t.js|POST|EP.RPC#1",
          "메서드 미상 RPC 를 보수적으로 Tier A 로 두지 않음: %r" % (r,))
                                                               
    r = analyze_source("t.js", "async function f(){ await fetchPost(EP.CTR_EXEC(n), "
                                "{ command:'hostname' }); }\n")
    check(len(r) == 1 and r[0][1] == "A",
          "REST POST 를 Tier A 로 잡지 못함: %r" % (r,))
                                                    
    r = analyze_source("t.js", "async function f(){ r = await fetchPost(API_BASE + '/rpc', "
                                "{ method:'alert.dlq.list', params:{} }); use(r); }\n")
    check(len(r) == 1 and r[0][1] == "B" and r[0][0] == "t.js|POST|/rpc:alert.dlq.list#1",
          "경로 리터럴 RPC 의 메서드 기반 tier/키 산출 실패: %r" % (r,))

    print("[SELF-TEST PASS] 판정 경로가 가드 유무·Tier·키를 정확히 식별"
          if ok else "[SELF-TEST FAILED]")
    return 0 if ok else 1


def main():
    argv = sys.argv[1:]
    if "--self-test" in argv:
        return self_test()
    if "--generate" in argv:
        return write_baseline(scan(), "--allow-tier-a" in argv)
    return run_check()


if __name__ == "__main__":
    sys.exit(main())
