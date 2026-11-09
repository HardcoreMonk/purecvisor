#!/usr/bin/env python3
                          
                                                                                
                                                                                                                  
                                                                                                              
 
                      
                                                                                      
                                                                                      

                                                                        
                                                                     
                                                                 
                                                       

    
                                                                                            
                                                                     
                                                                   
                                                                           
                                                                            
                                                                         

        
                                                         
                                                                             
                                                                           
                                                       

   
                                                                                
                                                                
                                                                            
                                                                                                                             
   
import argparse
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_fe_rpc_params as fe              
import check_help_counts as counts              
import check_rbac_policies as rbac              
import extract_rest_routes as routes_tool              
import rpc_extract              

ROOT = Path(__file__).resolve().parent.parent
HELP_JS = ROOT / "ui" / "modules" / "help.js"

ADMIN = 2

                                                                  
MIN_ROLE_ROWS = 300
MIN_ENDPOINTS = 200
MIN_BODY_EXAMPLES = 50

                                                         
SAFE_VALUE = re.compile(r"pcv-?ex|^\.+$|^<uuid>$|^192\.0\.2\.|^0000:fe:1f\.7$")
IDENTITY_KEYS = frozenset({
    "name", "username", "vm", "vm_name", "pf", "bridge", "bridge_name", "switch", "router", "tenant",
    "dest", "new_name", "clone_name", "target_node", "ssh_user", "snap_name", "snapshot_name",
    "host_path", "container_path", "ova_path", "iso_path", "zvol_path", "disks", "pci_addr", "target_ip",
    "attachment_id", "subnet_id", "subnet_name",
})

                                                     
                                          
ROUTE_ALIASES = {
    ("GET", "/vms/{}/metrics"): "vm.metrics",                                                
    ("DELETE", "/vms/{}/snapshot/{}"): "vm.snapshot.delete",                        
    ("DELETE", "/vms/{}/nics/{}"): "device.nic.detach",                            
    ("DELETE", "/containers/{}"): "container.destroy",                                             
    ("GET", "/containers/{}/metrics"): "container.metrics",                     
    ("DELETE", "/containers/{}/snapshots/{}"): "container.snapshot.delete",
    ("DELETE", "/containers/{}/nics/{}"): "container.nic.detach",
    ("DELETE", "/containers/{}/volumes/{}"): "container.volume.detach",
    ("GET", "/alerts"): "alert.history",                                            
    ("POST", "/alerts/{}/ack"): "alert.ack",
    ("POST", "/jobs/{}/cancel"): "jobs.cancel",
    ("GET", "/vnc/{}"): "get_vnc_info",
    ("POST", "/auth/apikeys/{}/revoke"): "auth.apikey.revoke",
    ("POST", "/auth/totp/recovery/regenerate"): "auth.totp.recovery.regenerate",
    ("POST", "/backup/history"): "backup.history",                                                
}

                                                  
NATIVE_LITERALS = {
    ("GET", "/health"): 'REST_API_PREFIX "/health"',
    ("GET", "/health/recent-errors"): 'REST_API_PREFIX "/health/recent-errors"',
    ("GET", "/version"): 'REST_API_PREFIX "/version"',
    ("GET", "/update-check"): 'REST_API_PREFIX "/update-check"',
    ("GET", "/metrics"): 'REST_API_PREFIX "/metrics"',
    ("POST", "/auth/token"): 'REST_API_PREFIX "/auth/token"',
    ("POST", "/auth/refresh"): 'REST_API_PREFIX "/auth/refresh"',
    ("POST", "/auth/logout"): 'REST_API_PREFIX "/auth/logout"',
    ("POST", "/auth/logout/all"): 'REST_API_PREFIX "/auth/logout/all"',
    ("POST", "/auth/password"): 'REST_API_PREFIX "/auth/password"',
    ("POST", "/auth/register"): 'REST_API_PREFIX "/auth/register"',
    ("POST", "/auth/totp/verify"): 'REST_API_PREFIX "/auth/totp/verify"',
    ("POST", "/auth/totp/enroll"): 'REST_API_PREFIX "/auth/totp/enroll"',
    ("GET", "/auth/whoami"): 'g_strcmp0(name, "whoami")',
    ("POST", "/rpc"): 'g_strcmp0(resource, "rpc")',
}

                                                
                                               
REST_BODY_RENAMES = {
    ("POST", "/containers/{}/exec"): {"command": "cmd"},
}

ROW_RE = re.compile(r"^\s*\['([^']+)',\s*(\d+),")
ENDPOINT_RE = re.compile(r"\{\s*m:\s*'([A-Z]+)',\s*p:\s*'([^']+)'(.*)$")
BODY_RE = re.compile(r"\bbody:\s*'((?:[^'\\]|\\.)*)'")
RPC_HELPER_RE = re.compile(r"^\s*rpc\('([a-z0-9_.]+)'\s*,")
NATIVE_BODY_RE = re.compile(r'\(\s*body\s*,\s*"([a-z0-9_]+)"')


def norm_path(path):
                                      
    bare = path.split("?", 1)[0].rstrip("/")
    return "/" + "/".join("{}" if s.startswith("{") else s for s in bare.strip("/").split("/"))


def route_candidates(method, path):
                                                                                 

                                                                 
                                                     
    bare = norm_path(path)
    segs = bare.strip("/").split("/")
    out = [(method, bare)]
    if len(segs) >= 3 and segs[1] != "{}":
        out.append((method, "/" + "/".join([segs[0], "{}"] + segs[2:])))
    return out


def parse_role_rows(lines):
                                  
    out = []
    for line in counts._array_lines(lines, "var NS = ["):
        m = ROW_RE.match(line)
        if m:
            out.append((m.group(1), int(m.group(2))))
    return out


def parse_policy(text):
                                                                    
    block = rbac.POLICY_BLOCK.search(text)
    if not block:
        return None
    table = {}
    for method, value in rbac.POLICY_ENTRY.findall(block.group(1)):
        table[method] = int(value) if value.isdigit() else rbac.ROLE_VALUES[value]
    return table


def parse_endpoints(lines):
                                                                                      
    start = next(i for i, l in enumerate(lines) if l.strip().startswith("var groups = ["))
    out = []
    for i, line in enumerate(counts._array_lines(lines, "var groups = [")):
        lineno = start + i + 1
        rm = RPC_HELPER_RE.match(line)
        if rm:
            out.append((lineno, "RPC", rm.group(1), None))
            continue
        em = ENDPOINT_RE.search(line)
        if em:
            bm = BODY_RE.search(em.group(3))
            out.append((lineno, em.group(1), em.group(2), bm.group(1) if bm else None))
    return out


def check_roles(rows, policy):
                                                                  
    bad = []
    for method, role in rows:
        expected = policy.get(method, ADMIN)
        if role != expected:
            bad.append(f"{method}: help={role} 정책표={expected}"
                       + ("" if method in policy else " (표에 없어 ADMIN)"))
    return bad


def route_table():
                                               
    table = {}
    for method, url, rpc_name, _ in routes_tool.extract(ROOT):
        table.setdefault((method, norm_path(url)), set()).add(rpc_name)
    return table


def resolve_route(method, path, table, rest_text, chain_rpcs):
                                                       
    key = (method, norm_path(path))
    for candidate in route_candidates(method, path):
        if candidate in table:
            return "chain", table[candidate]
    if key in ROUTE_ALIASES:
        rpc_name = ROUTE_ALIASES[key]
        return ("alias", {rpc_name}) if rpc_name in chain_rpcs else None
    if key in NATIVE_LITERALS:
        return ("native", set()) if NATIVE_LITERALS[key] in rest_text else None
    return None


def bad_identity_values(obj):
                                        
    bad = []
    for key, value in obj.items():
        if key not in IDENTITY_KEYS:
            continue
        values = value if isinstance(value, list) else [value]
        for item in values:
            if isinstance(item, str) and not SAFE_VALUE.search(item):
                bad.append((key, item))
    return bad


def check_keys(where, keys, rpcs, kind, renames, read_fn, required_fn, injected, native_body_keys):
                                
    bad = []
    if kind == "native":
        extra = sorted(keys - native_body_keys)
        if extra:
            bad.append(f"{where}: REST 네이티브 분기가 읽지 않는 키 {extra}")
        return bad
                                                           
    dropped = sorted(keys & set(renames.values()))
    if dropped:
        bad.append(f"{where}: REST 분기가 읽지 않는 키 {dropped} (본문 키는 {sorted(renames)})")
    mapped = {renames.get(k, k) for k in keys}
    union, opaque = set(), []
    for rpc_name in rpcs:
        read = read_fn(rpc_name)
        if read is None:
            opaque.append(rpc_name)
        else:
            union |= read
    if opaque:
        bad.append(f"{where}: 핸들러 키를 정적으로 읽을 수 없다 {sorted(opaque)} — 대조 불가 예시를 두지 않는다")
        return bad
    extra = sorted(mapped - union)
    if extra:
        bad.append(f"{where}: 핸들러({sorted(rpcs)})가 읽지 않는 키 {extra}")
    if len(rpcs) == 1 and keys:
        required = required_fn(next(iter(rpcs))) or set()
        lacking = sorted(required - mapped - injected)
        if lacking:
            bad.append(f"{where}: 핸들러 필수 키가 빠졌다 {lacking}")
    return bad


def check_endpoints(endpoints, table, rest_text, read_fn, required_fn, injected, routes_registered):
                            
    bad = []
    chain_rpcs = {r for rs in table.values() for r in rs}
    native_body_keys = set(NATIVE_BODY_RE.findall(rest_text))
    for lineno, method, path, body in endpoints:
        where = f"help.js:{lineno} {method} {path}"
        if method == "RPC":
            if path not in routes_registered:
                bad.append(f"help.js:{lineno} rpc({path}): 등록되지 않은 RPC")
            continue
        resolved = resolve_route(method, path, table, rest_text, chain_rpcs)
        if resolved is None:
            bad.append(f"{where}: rest 계열에 없는 경로 — 실행하면 404다")
            continue
        kind, rpcs = resolved
        renames = REST_BODY_RENAMES.get((method, norm_path(path)), {})
        for key in renames:
            if key not in native_body_keys:
                bad.append(f"{where}: REST_BODY_RENAMES 의 '{key}' 를 rest 계열이 본문에서 읽지 않는다")
        query = path.split("?", 1)[1] if "?" in path else ""
        if query:
            qkv = dict(p.split("=", 1) for p in query.split("&"))
            bad += [f"{where}: 쿼리 값 {k}={v} 이 실재할 법한 이름이다" for k, v in bad_identity_values(qkv)]
            bad += check_keys(where, set(qkv), rpcs, kind, renames, read_fn, lambda _: set(), injected,
                              native_body_keys)
        if body is None:
            continue
        if method == "GET":
            bad.append(f"{where}: GET 예시에 본문이 있다 — swTry 가 보내지 않으므로 보이는 예시와 요청이 다르다")
            continue
        try:
            obj = json.loads(body.replace("\\'", "'"))
        except json.JSONDecodeError as exc:
            bad.append(f"{where}: 본문이 JSON 이 아니다 ({exc})")
            continue
        if not isinstance(obj, dict):
            bad.append(f"{where}: 본문이 객체가 아니다")
            continue
        bad += [f"{where}: 예시 값 {k}={v} 이 실재할 법한 이름이다(자원이 있으면 실제로 바뀐다)"
                for k, v in bad_identity_values(obj)]
        if path == "/rpc":
            continue
        bad += check_keys(where, set(obj), rpcs, kind, renames, read_fn, required_fn, injected, native_body_keys)
    return bad


def run(help_js, policy_path):
    lines = Path(help_js).read_text(encoding="utf-8", errors="replace").split("\n")
    policy_text = Path(policy_path).read_text(encoding="utf-8", errors="replace")
    policy = parse_policy(policy_text)
    if policy is None:
        print("[FAIL] g_method_policies 를 찾지 못했다 — 정책표 서식이 바뀌었다")
        return 1
    rows = parse_role_rows(lines)
    endpoints = parse_endpoints(lines)
    n_rest = sum(1 for e in endpoints if e[1] != "RPC")
    n_body = sum(1 for e in endpoints if e[3] is not None)
    print(f"[check-help-contract] 역할 행 {len(rows)} / 정책표 {len(policy)} / 엔드포인트 {n_rest} / 본문 예시 {n_body}")
    if len(rows) < MIN_ROLE_ROWS or n_rest < MIN_ENDPOINTS or n_body < MIN_BODY_EXAMPLES:
        print(f"[FAIL] 파싱 수가 하한 미만이다(역할 {MIN_ROLE_ROWS}·엔드포인트 {MIN_ENDPOINTS}·본문 {MIN_BODY_EXAMPLES}) — "
              "help.js 서식이 바뀌어 대조가 공허해졌다")
        return 1
    bad_roles = check_roles(rows, policy)
    rest_text = rpc_extract.rest_family_text(ROOT)
    registered = counts.registered_routes()
    bad_eps = check_endpoints(endpoints, route_table(), rest_text, fe.be_read_keys, fe.be_required_keys,
                              fe.rest_injected_keys(), registered)
    rc = 0
    if bad_roles:
        print(f"\n[FAIL] 역할 표가 정책표와 다르다 {len(bad_roles)}건:")
        for item in bad_roles:
            print(f"  - {item}")
        print("\n시정: help.js NS[] 의 rbac 값을 src/api/dispatcher_policy.c 에 맞춘다.")
        rc = 1
    if bad_eps:
        print(f"\n[FAIL] REST 예시가 정본과 다르다 {len(bad_eps)}건:")
        for item in bad_eps:
            print(f"  - {item}")
        print("\n시정: help.js 예시를 핸들러 키·실제 경로로 고친다. 기준선은 두지 않는다.")
        rc = 1
    if rc == 0:
        print("[PASS] help.js 역할 표·REST 예시가 정책표·라우팅·핸들러 정본과 일치")
    return rc


def dump_examples(help_js):
                                                                                
    lines = Path(help_js).read_text(encoding="utf-8", errors="replace").split("\n")
    for _, method, path, body in parse_endpoints(lines):
        if method == "RPC" or body is None:
            continue
        print(json.dumps({"m": method, "path": re.sub(r"\{[^}]+\}", "test", path), "body": body}, ensure_ascii=False))
    return 0


def self_test():
    ok = True

    def expect(cond, msg):
        nonlocal ok
        if not cond:
            print(f"[SELF-TEST FAIL] {msg}")
            ok = False

                                                     
    expect(check_roles([("a.b", 0)], {"a.b": 0}) == [], "일치 행을 위반으로 셌다")
    expect(check_roles([("a.b", 0)], {"a.b": 2}), "낮게 적은 행을 놓쳤다")
    expect(check_roles([("a.b", 2)], {"a.b": 0}), "높게 적은 행을 놓쳤다")
    expect(check_roles([("a.b", 0)], {}), "표에 없는 메서드를 VIEWER 로 봤다")
    expect(check_roles([("a.b", 2)], {}) == [], "표에 없는 메서드를 ADMIN 으로 보지 않았다")
    expect(parse_policy('g_method_policies[] = {\n{ "a.b", 1 },\n{ "c.d", PCV_ROLE_ADMIN },\n{ NULL, 0 }\n};')
           == {"a.b": 1, "c.d": 2}, "정책표 파싱 실패")

                
    expect(norm_path("/vms/{name}/start") == "/vms/{}/start", "자리표시자 정규화 실패")
    expect(norm_path("/ovn/acl?switch=x") == "/ovn/acl", "쿼리 제거 실패")
    table = {("POST", "/vms/{}/import"): {"vm.import.ova"}, ("GET", "/vms/{}"): {"vm.metrics"}}
    expect(resolve_route("POST", "/vms/import", table, "", {"vm.metrics"}) is None,
           "POST /vms/import 를 경로로 인정했다")
    expect(resolve_route("POST", "/vms/import/import", table, "", set()) == ("chain", {"vm.import.ova"}),
           "실제 경로 /vms/import/import 를 찾지 못했다")
    expect(resolve_route("POST", "/vms/{name}/import", table, "", set()) == ("chain", {"vm.import.ova"}),
           "사슬 경로를 찾지 못했다")
    expect(resolve_route("GET", "/vms/{name}/metrics", {}, "", set()) is None,
           "대상 RPC 가 사슬에 없는 별칭을 인정했다")
    expect(resolve_route("POST", "/auth/token", {}, 'REST_API_PREFIX "/auth/token"', set()) == ("native", set()),
           "네이티브 리터럴을 찾지 못했다")
    expect(resolve_route("POST", "/auth/token", {}, "", set()) is None, "리터럴 없는 네이티브를 인정했다")

        
    expect(bad_identity_values({"name": "tank"}) == [("name", "tank")], "tank 를 놓쳤다")
    expect(bad_identity_values({"name": "pcv-example-pool"}) == [], "예시 이름을 위반으로 셌다")
    expect(bad_identity_values({"disks": ["/dev/sdb"]}) == [("disks", "/dev/sdb")], "배열 값을 보지 않았다")
    expect(bad_identity_values({"mode": "nat"}) == [], "대상이 아닌 키를 봤다")

          
    read = lambda _r: {"a", "b"}              
    req = lambda _r: {"a"}              
    args = (read, req, frozenset(), set())
    expect(check_keys("w", {"a"}, {"r"}, "chain", {}, *args) == [], "정상 본문을 위반으로 셌다")
    expect(check_keys("w", {"a", "z"}, {"r"}, "chain", {}, *args), "읽지 않는 키를 놓쳤다")
    expect(check_keys("w", {"b"}, {"r"}, "chain", {}, *args), "필수 키 누락을 놓쳤다")
    expect(check_keys("w", {"b"}, {"r"}, "chain", {}, read, req, frozenset({"a"}), set()) == [],
           "REST 주입 키를 누락으로 셌다")
    expect(check_keys("w", {"c"}, {"r"}, "chain", {"c": "a"}, *args) == [], "개명 경로를 판정하지 못했다")
    expect(check_keys("w", {"a"}, {"r"}, "chain", {"c": "a"}, *args), "개명 경로의 핸들러 쪽 이름을 통과시켰다")
    expect(check_keys("w", {"a"}, {"r"}, "chain", {}, lambda _r: None, req, frozenset(), set()),
           "대조 불가 핸들러를 통과시켰다")
    expect(check_keys("w", {"x"}, set(), "native", {}, read, req, frozenset(), {"y"}), "네이티브 미독 키를 놓쳤다")

    print("[SELF-TEST PASS] 판정 함수 반사실 통과" if ok else "[SELF-TEST FAILED]")
    return 0 if ok else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--help-js", default=str(HELP_JS))
    parser.add_argument("--policy", default=None)
    parser.add_argument("--dump-examples", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if args.dump_examples:
        return dump_examples(args.help_js)
    policy = args.policy or rpc_extract.dispatcher_policy_file(ROOT)
    return run(args.help_js, policy)


if __name__ == "__main__":
    sys.exit(main())
