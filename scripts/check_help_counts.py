#!/usr/bin/env python3
                          
                                                               
                                                                                      
                                                                                                                            
 
                      
                                                                                                                        
                                                              

                                                                       
                                              
                                              
                                              

                           

                                                       

                                                                    
                                                                         
                                                                  
                                                  
                                                                 
                                                        

                                 

                                                               
                                                             
                                                                
                                            

                                                            

                                                                 
                                                
                                                              
                                                                   
                                                   
                                                      

    
                                                  
                                                             
                                                           
                                                             

                                                          
                                                              
                                       
   
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DISPATCHER = ROOT / "src/api/dispatcher.c"
APP_JS = ROOT / "ui/app.js"
HELP_JS = ROOT / "ui/modules/help.js"

                                           
                                                                           
LEGACY_ALIASES = {"get_vnc_info"}

                                       
                                                      
SPECIAL_METHODS = {"vm.create"}

ROUTE_RE = re.compile(r'g_hash_table_insert\(g_rpc_routes,\s*"([^"]+)"')
CATALOG_ROW_RE = re.compile(r"^\s*\['([^']+)'")
SWAGGER_EP_RE = re.compile(r"\{\s*m:\s*'([A-Z]+)',\s*p:\s*'([^']+)'")


def _array_lines(lines, opener):
                                      

                                                   
                                       
    start = next(i for i, l in enumerate(lines) if l.strip().startswith(opener))
    end = next(i for i in range(start, len(lines)) if lines[i].rstrip() == "  ];")
    return lines[start:end]


def registered_routes(text=None) -> set:
                                              
    if text is None:
        text = DISPATCHER.read_text(errors="replace")
    return set(ROUTE_RE.findall(text))


def catalog_methods(lines=None) -> list:
                                                    
    if lines is None:
        lines = HELP_JS.read_text(errors="replace").split("\n")
    body = _array_lines(lines, "var NS = [")
    return [m.group(1) for m in (CATALOG_ROW_RE.match(l) for l in body) if m]


def swagger_endpoints(lines=None) -> list:
                                                          
    if lines is None:
        lines = HELP_JS.read_text(errors="replace").split("\n")
    body = _array_lines(lines, "var groups = [")
    return SWAGGER_EP_RE.findall("\n".join(body))


def config_counts(text=None) -> dict:
                                                        
    if text is None:
        text = APP_JS.read_text(errors="replace")
    out = {}
    for key in ("RPC_COUNT", "REST_COUNT"):
        m = re.search(r"\b" + key + r":\s*(\d+)", text)
        out[key] = int(m.group(1)) if m else None
    return out


def fallback_literals(text=None) -> dict:
                                                          

                                                  
                                  
    if text is None:
        text = HELP_JS.read_text(errors="replace")
    return {
        "RPC_COUNT": sorted({int(v) for v in re.findall(r"RPC_COUNT\s*(?:\||:)\|?\s*(\d+)", text)}
                            | {int(v) for v in re.findall(r"RPC_COUNT\s*:\s*(\d+)", text)}),
        "REST_COUNT": sorted({int(v) for v in re.findall(r"REST_COUNT\s*(?:\||:)\|?\s*(\d+)", text)}
                             | {int(v) for v in re.findall(r"REST_COUNT\s*:\s*(\d+)", text)}),
    }


def evaluate() -> tuple:
                                                      
    routes = registered_routes()
    cat = catalog_methods()
    sw = swagger_endpoints()
    cfg = config_counts()
    fb = fallback_literals()

    want_rpc = len(routes - LEGACY_ALIASES)
    want_rest = len(set(sw))

    fails = []
    if cfg["RPC_COUNT"] != want_rpc:
        fails.append(
            f"RPC_COUNT={cfg['RPC_COUNT']} != 고유 라우트 {len(routes)} − 레거시 별칭 "
            f"{len(LEGACY_ALIASES)} = {want_rpc} (ui/app.js 갱신 필요)")
    if cfg["REST_COUNT"] != want_rest:
        fails.append(
            f"REST_COUNT={cfg['REST_COUNT']} != swagger 리터럴 고유 (method,path) "
            f"{want_rest} (ui/app.js 갱신 필요)")

    dup = sorted({m for m in cat if cat.count(m) > 1})
    if dup:
        fails.append(f"NS 카탈로그 중복 행 {len(dup)}건: {', '.join(dup)}")
    want_cat = routes | SPECIAL_METHODS
    miss = sorted(want_cat - set(cat))
    ghost = sorted(set(cat) - want_cat)
    if miss:
        fails.append(f"NS 카탈로그 누락 {len(miss)}건(등록됐지만 도움말에 없음): "
                     f"{', '.join(miss)}")
    if ghost:
        fails.append(f"NS 카탈로그 유령 {len(ghost)}건(도움말에 있으나 미등록·비특수): "
                     f"{', '.join(ghost)}")

    for key, want in (("RPC_COUNT", want_rpc), ("REST_COUNT", want_rest)):
        bad = [v for v in fb[key] if v != want]
        if bad:
            fails.append(f"help.js {key} 폴백 리터럴이 정본({want})과 불일치: {bad}")

    report = (f"[check-help-counts] 라우트 {len(routes)} / 카탈로그 {len(cat)} / "
              f"swagger {len(set(sw))} / RPC_COUNT {cfg['RPC_COUNT']}(기대 {want_rpc}) / "
              f"REST_COUNT {cfg['REST_COUNT']}(기대 {want_rest})")
    return fails, report


def self_test() -> int:
                                                       
    ok = True

    routes = {"a.list", "b.get", "get_vnc_info"}
    if len(routes - LEGACY_ALIASES) != 2:
        print("self-test: 레거시 별칭 제외 실패", file=sys.stderr); ok = False

    lines = ["  var NS = [", "    ['a.list', 0, '-', '-', 'x', 'x'],",
             "    ['vm.create', 1, '-', '-', 'x', 'x'],", "  ];"]
    if catalog_methods(lines) != ["a.list", "vm.create"]:
        print("self-test: 카탈로그 추출 실패", file=sys.stderr); ok = False

    lines = ["  var groups = [", "      { m: 'GET', p: '/health', d: 'x' },",
             "      { m: 'POST', p: '/vms', d: 'x' },", "  ];"]
    if swagger_endpoints(lines) != [("GET", "/health"), ("POST", "/vms")]:
        print("self-test: swagger 추출 실패", file=sys.stderr); ok = False

    if config_counts("  RPC_COUNT: 7,\n  REST_COUNT: 9,") != {"RPC_COUNT": 7, "REST_COUNT": 9}:
        print("self-test: config 추출 실패", file=sys.stderr); ok = False

    if fallback_literals("cfg.RPC_COUNT || 7, x.REST_COUNT : 9")["RPC_COUNT"] != [7]:
        print("self-test: 폴백 리터럴 추출 실패", file=sys.stderr); ok = False

    print(f"check_help_counts self-test: {'OK' if ok else 'FAILED'}")
    return 0 if ok else 1


def main(argv=None) -> int:
    argv = list(sys.argv[1:]) if argv is None else list(argv)
    if "--self-test" in argv:
        return self_test()

    fails, report = evaluate()
    print(report)
    if fails:
        print(f"[FAIL] help 카운트 정본 불변식 위반 {len(fails)}건:", file=sys.stderr)
        for f in fails:
            print(f"  - {f}", file=sys.stderr)
        return 1
    print("[PASS] RPC_COUNT/REST_COUNT 정본 + NS 카탈로그 전수 일치")
    return 0


if __name__ == "__main__":
    sys.exit(main())
