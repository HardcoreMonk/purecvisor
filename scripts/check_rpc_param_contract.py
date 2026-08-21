#!/usr/bin/env python3
                          
                                                                                  
                                                                                      
                                                                                                                            
 
                      
                                                                                                                                           
                                                                

                                                                 
                                               

      
                                                              
                                                                                               
                                                                                                    

                                                                               
                                                  
                                                        
                            
                                                                            

          
                                                                         
                                                                             
                                                                       
                                                                          
                                                                           
                                                          
                                                       

          
                                                 
                                                            
                                       

                 
                                             
                                                                  

                
                                                   
                                                          
                                                                      
                                                               
                                              
                                                          
                                                           
                       
                                           
                                                          
                                             

   
                                                                                     
                                                                         
   
import json, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from rpc_extract import (read, strip_comments, is_source_js,
    CLI_RE, extract_method_to_fn, extract_fn_body,
    extract_handler_required, extract_consumer_sent)

ROOT = Path(__file__).resolve().parent.parent
CONTRACTS = ROOT / "contracts"
DISPATCHER = ROOT / "src" / "api" / "dispatcher.c"
HANDLER_GLOBS = ["src/modules/dispatcher/handler_*.c", "src/modules/network/network_manager.c",
                 "src/api/dispatcher.c"]
CLI_C = ROOT / "src" / "cli" / "purecvisorctl.c"
IGNORED_BASELINE = ROOT / "scripts" / "rpc_param_ignored_baseline.txt"


class _Marker:
                                                                
    def __init__(self, name): self.name = name
    def __repr__(self): return self.name


SKIP_DRIFT = _Marker("SKIP_DRIFT")                                          
DEAD_METHOD = _Marker("DEAD_METHOD")                                    


def _norm_required(required):
                                                             
    out = set()
    for r in required:
        out.add(frozenset(r) if isinstance(r, list) else r)
    return out


def _satisfied(req_item, sent: set) -> bool:
    if isinstance(req_item, frozenset):
        return bool(req_item & sent)                        
    return req_item in sent


def diff_method(method, spec, handler_req: set, sent_by_consumer: dict) -> dict:
    reg_req = _norm_required(spec.get("required", []))
    known = set()
    for r in reg_req:
        known |= (set(r) if isinstance(r, frozenset) else {r})
    known |= set(spec.get("optional", []))
    res = {"drift": None, "missing": set(), "ignored": set()}
                                                                         
    if handler_req is not None and handler_req != reg_req:
        res["drift"] = (reg_req, handler_req)
                           
    for consumer, sent in sent_by_consumer.items():
                         
        miss = frozenset(x for r in reg_req if not _satisfied(r, sent)
                         for x in ([r] if isinstance(r, str) else ["|".join(sorted(r))]))
        if miss:
            res["missing"].add((consumer, miss))
        for k in sent - known:
            res["ignored"].add((consumer, k))
    return res


def analyze(registry, resolve_handler, resolve_sent):
                                                       

                                                                                   
                                                 

                                                                    
                                                    
                                                
                   
       
    hard, missing, ignored = [], set(), set()
    for method, spec in registry.items():
        h = resolve_handler(method)
        if h is DEAD_METHOD:
            hard.append(f"DEAD-METHOD {method}: 레지스트리 참조하나 dispatcher 등록/핸들러 없음")
            hreq = None
        elif h is SKIP_DRIFT:
            hreq = None
        else:
            hreq = h
        r = diff_method(method, spec, hreq, resolve_sent(method))
        if r["drift"]:
            hard.append(f"DRIFT {method}: registry.required={sorted(map(str, r['drift'][0]))} "
                        f"!= 핸들러={sorted(map(str, r['drift'][1]))}")
        for consumer, miss in r["missing"]:
            missing.add((method, consumer, miss))
        for consumer, k in r["ignored"]:
            ignored.add((method, consumer, k))
    return hard, missing, ignored


def judge(hard, missing, ignored, base_missing, base_ignored):
                                                        

                                                                   
                                                               
       
    fails = list(hard)
    for method, consumer, miss in sorted(missing, key=lambda t: (t[0], t[1], sorted(t[2]))):
        if (method, consumer, miss) not in base_missing:
            fails.append(f"MISSING {method}[{consumer}]: 필수 {sorted(miss)} 미전송 (-32602)")
    for method, consumer, key in sorted(ignored):
        if (method, consumer, key) not in base_ignored:
            fails.append(f"IGNORED {method}[{consumer}]: '{key}' 핸들러 미read (거짓성공 가능)")
    stale = sorted(set(base_ignored) - ignored)
    return fails, stale


def parse_ignored_baseline(text):
                                                             
    out = {}
    for lineno, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        entry, sep, reason = line.partition("#")
        parts = [p.strip() for p in entry.strip().split("|")]
        if len(parts) != 3 or not all(parts):
            raise ValueError(f"{IGNORED_BASELINE.name}:{lineno} 형식 오류 "
                             f"(<method>|<consumer>|<key> 여야 한다): {raw!r}")
        out[(parts[0], parts[1], parts[2])] = reason.strip() if sep else ""
    return out


def load_ignored_baseline():
    if not IGNORED_BASELINE.exists():
        return {}
    return parse_ignored_baseline(IGNORED_BASELINE.read_text(encoding="utf-8"))


def _load_baseline():
    b = json.loads((CONTRACTS / "rpc_param_baseline.json").read_text())
    return {(e["method"], e["consumer"], frozenset(e["missing_required"]))
            for e in b["known_consumer_mismatches"]}


def main() -> int:
    registry = json.loads((CONTRACTS / "rpc_params.json").read_text())
    baseline = _load_baseline()
    ign_base = load_ignored_baseline()
    disp = read(DISPATCHER)
    m2fn = extract_method_to_fn(disp)
    handler_srcs = {}
    for g in HANDLER_GLOBS:
        for p in ROOT.glob(g):
            handler_srcs[p] = read(p)

    def handler_required(method):
        fn = m2fn.get(method)
        if not fn:
            return None
        for src in handler_srcs.values():
            body = extract_fn_body(src, fn)
            if body:
                return extract_handler_required(body)
        return None

    def resolve_handler(method):
        if "keys_via" in registry[method]:
            return SKIP_DRIFT                                                    
        if method not in m2fn:
            return DEAD_METHOD                                         
        return handler_required(method)

    cli_sent = extract_consumer_sent(read(CLI_C), CLI_RE)

    def resolve_sent(method):
        return {"cli": cli_sent[method]} if method in cli_sent else {}

    hard, missing, ignored = analyze(registry, resolve_handler, resolve_sent)
    fails, stale = judge(hard, missing, ignored, baseline, ign_base)

                                                
    for key in sorted(k for k, v in ign_base.items() if not v):
        fails.append(f"BASELINE-NO-REASON {key[0]}[{key[1]}]: '{key[2]}' "
                     f"— {IGNORED_BASELINE.name} 항목에 사유(# ...) 가 없다")

    print(f"[check-rpc-param-contract] registry {len(registry)} / "
          f"missing-baseline {len(baseline)} / ignored-baseline {len(ign_base)} / "
          f"ignored 관측 {len(ignored)} / FAIL {len(fails)}")
    for method, consumer, key in stale:
        print(f"[INFO] ignored baseline 항목이 해소됨(제거 권장): {method}[{consumer}] '{key}'")
    if fails:
        print(f"[FAIL] param-key 계약 위반 {len(fails)}건 (baseline 외 신규):", file=sys.stderr)
        for f in fails:
            print(f"  - {f}", file=sys.stderr)
        print(f"\n시정: 소비자를 핸들러 계약에 맞춰라. 지금 못 고칠 사유가 있으면 "
              f"{IGNORED_BASELINE.name} 에 사유와 함께 등재(총량은 줄어들기만 해야 한다).",
              file=sys.stderr)
        return 1
    print("[PASS] 신규 param-key 계약 위반 없음")
    return 0


def self_test() -> int:
                                                            

                                                      
                              
       
    ok = True

    def check(cond, msg):
        nonlocal ok
        if not cond:
            print(f"[SELF-TEST FAIL] {msg}")
            ok = False

    def run(registry, handlers, sent, base_missing=frozenset(), base_ignored=frozenset()):
        hard, missing, ignored = analyze(
            registry,
            lambda m: handlers.get(m),
            lambda m: sent.get(m, {}))
        fails, stale = judge(hard, missing, ignored, base_missing, base_ignored)
        return fails, stale, ignored

                                                        
                                                           
    fails, _, ignored = run({"t.ign": {"required": ["a"], "optional": []}},
                            {"t.ign": {"a"}},
                            {"t.ign": {"cli": {"a", "zz"}}})
    check(ignored == {("t.ign", "cli", "zz")}, f"ignored 관측이 어긋난다: {ignored}")
    check([f for f in fails if f.startswith("IGNORED")]
          == ["IGNORED t.ign[cli]: 'zz' 핸들러 미read (거짓성공 가능)"],
          f"IGNORED 가 FAIL 로 승격되지 않았다: {fails}")

                                                  
    fails, _, _ = run({"t.ign": {"required": ["a"], "optional": []}},
                      {"t.ign": {"a"}},
                      {"t.ign": {"cli": {"a", "zz"}}},
                      base_ignored={("t.ign", "cli", "zz")})
    check(not fails, f"baseline 등재 항목이 FAIL 로 샜다: {fails}")

                                                          
                                               
    fails, _, _ = run({"t.ign": {"required": ["a"], "optional": []}},
                      {"t.ign": {"a"}},
                      {"t.ign": {"cli": {"a", "zz"}}},
                      base_ignored={("t.ign", "ui", "zz"), ("t.ign", "cli", "other")})
    check(any(f.startswith("IGNORED") for f in fails),
          f"다른 소비자/키의 baseline 이 면제로 오작동했다: {fails}")

                                                   
    fails, _, ignored = run({"t.opt": {"required": ["a"], "optional": ["b"]}},
                            {"t.opt": {"a"}},
                            {"t.opt": {"cli": {"a", "b"}}})
    check(not fails and not ignored, f"optional 키를 IGNORED 로 오판: {fails} / {ignored}")

                                                           
    fails, stale, _ = run({"t.ign": {"required": ["a"], "optional": []}},
                          {"t.ign": {"a"}},
                          {"t.ign": {"cli": {"a"}}},
                          base_ignored={("t.ign", "cli", "zz")})
    check(not fails and stale == [("t.ign", "cli", "zz")],
          f"해소된 baseline 항목 감지 실패: {fails} / {stale}")

                                                 
                                       
    fails, _, _ = run({"t.drift": {"required": ["a"], "optional": []}},
                      {"t.drift": {"b"}},
                      {},
                      base_ignored={("t.drift", "cli", "a")})
    check(any(f.startswith("DRIFT") for f in fails), f"DRIFT 판정이 약화됐다: {fails}")

                                                        
                                                             
    fails, _, _ = run({"t.miss": {"required": ["a"], "optional": []}},
                      {"t.miss": {"a"}},
                      {"t.miss": {"cli": {"zz"}}},
                      base_ignored={("t.miss", "cli", "zz")})
    check([f for f in fails if f.startswith("MISSING")]
          == ["MISSING t.miss[cli]: 필수 ['a'] 미전송 (-32602)"],
          f"MISSING 판정이 ignored baseline 에 의해 약화됐다: {fails}")
    fails, _, _ = run({"t.miss": {"required": ["a"], "optional": []}},
                      {"t.miss": {"a"}},
                      {"t.miss": {"cli": {"zz"}}},
                      base_missing={("t.miss", "cli", frozenset({"a"}))},
                      base_ignored={("t.miss", "cli", "zz")})
    check(not fails, f"두 baseline 을 모두 만족하는데 FAIL 했다: {fails}")

                                                                     
                        
    fails, _, ignored = run({"t.alias": {"required": [["x", "y"]], "optional": []}},
                            {"t.alias": SKIP_DRIFT},
                            {"t.alias": {"cli": {"y"}}})
    check(not fails and not ignored, f"alias-group 판정이 어긋난다: {fails} / {ignored}")

                                                                  
    fails, _, _ = run({"t.dead": {"required": ["a"], "optional": []}},
                      {"t.dead": DEAD_METHOD},
                      {"t.dead": {"cli": {"a", "zz"}}},
                      base_ignored={("t.dead", "cli", "zz")})
    check(any(f.startswith("DEAD-METHOD") for f in fails), f"DEAD-METHOD 판정 소실: {fails}")

                                              
    parsed = parse_ignored_baseline(
        "# 주석\n\n a.b | cli | k   # [실제결함] 사유\nc.d|cli|k2\n")
    check(parsed == {("a.b", "cli", "k"): "[실제결함] 사유", ("c.d", "cli", "k2"): ""},
          f"baseline 파싱 결과가 어긋난다: {parsed}")
    try:
        parse_ignored_baseline("a.b|cli\n")
        check(False, "형식 오류(필드 2개) 를 파서가 통과시켰다")
    except ValueError:
        pass

                                                     
    real = load_ignored_baseline()
    check(real and all(v for v in real.values()),
          f"실제 baseline 에 사유 없는 항목: {[k for k, v in real.items() if not v]}")

    print("[SELF-TEST PASS] 게이트 판정(승격·래칫·해소감지·기존판정 불변) 정확"
          if ok else "[SELF-TEST FAILED]")
    return 0 if ok else 1


if __name__ == "__main__":
    if "--self-test" in sys.argv:
        sys.exit(self_test())
    sys.exit(main())
