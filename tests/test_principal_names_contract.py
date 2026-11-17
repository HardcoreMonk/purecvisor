#!/usr/bin/env python3
                          
                                                                                                        
                                                                       
                                                                                                       
                                                                                         
                                                                                                                  
                                                                
                                                                                                                     
                                                                               
                                                                                                  
                                                                                                      
                                                                                        
                                                                                                  
                                                           
 
                      
                                                                            
                     
from __future__ import annotations

import functools
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts" / "split"))
import c_blocks              

SRC = ROOT / "src"
NAMES = "src/modules/auth/pcv_rbac_names.c"
RBAC = "src/modules/auth/pcv_rbac.c"
HANDLER = "src/modules/dispatcher/handler_auth.c"
IDENTITY = "src/modules/auth/pcv_rbac_identity.c"
SIGNUP = "src/api/rest_auth_endpoints.c"
INSERT_RE = re.compile(r"INSERT\s+(?:OR\s+\w+\s+)?INTO\s+(users|api_keys)\b")
INSERT_FNS = {"pcv_rbac_names_insert_user_locked", "pcv_rbac_names_insert_apikey_locked"}
TOMBSTONE = "deleted_user_names"
REFUSAL_LOG = "pcv_rbac_names_log_bootstrap_refusal("


def src_sources() -> dict[str, str]:
                                             
    return {p.relative_to(ROOT).as_posix(): p.read_text() for p in sorted(SRC.rglob("*.[ch]"))}


@functools.lru_cache(maxsize=None)
def fn_spans(src: str) -> tuple[tuple[str, int, int], ...]:
                                                    
    return tuple((b.name, b.body_open, b.body_close) for b in c_blocks.scan(src)[0])


@functools.lru_cache(maxsize=None)
def kinds_of(src: str) -> str:
                                   
    return c_blocks.classify(src)


def masked_of(src: str) -> str:
                                   
    return c_blocks.masked(src, kinds_of(src))


def enclosing(src: str, pos: int) -> str | None:
                                           
    for name, start, end in fn_spans(src):
        if start <= pos <= end:
            return name
    return None


def body(src: str, name: str) -> str:
                           
    for fn, start, end in fn_spans(src):
        if fn == name:
            return src[start:end + 1]
    raise AssertionError(f"함수 없음: {name}")


def masked_body(src: str, name: str) -> str:
                           
    m = masked_of(src)
    for fn, start, end in fn_spans(src):
        if fn == name:
            return m[start:end + 1]
    raise AssertionError(f"함수 없음: {name}")


def insert_problems(sources: dict[str, str]) -> list[str]:
                                                         
    out = []
    seen: dict[str, int] = {}
    for path, text in sources.items():
        if "INTO" not in text:
            continue
        kinds = kinds_of(text)
        for mt in INSERT_RE.finditer(text):
            if kinds[mt.start()] == "m":
                continue
            fn = enclosing(text, mt.start())
            if path != NAMES or fn not in INSERT_FNS:
                out.append(f"{path}:{fn}: {mt.group(0)}")
            else:
                seen[fn] = seen.get(fn, 0) + 1
    for fn in sorted(INSERT_FNS):
        if seen.get(fn, 0) != 1:
            out.append(f"{NAMES}:{fn}: INSERT 출현 {seen.get(fn, 0)}번(1번이어야 한다)")
    legacy = [(p, enclosing(t, mt.start())) for p, t in sources.items()
              for mt in re.finditer(r"INSERT\s+INTO\s+api_keys_new\b", t) if kinds_of(t)[mt.start()] != "m"]
    if legacy != [(RBAC, "_migrate_api_keys_schema")]:
        out.append(f"옛 키 스키마 이관 INSERT 자리가 바뀌었다: {legacy}")
    return out


def caller_problems(sources: dict[str, str]) -> list[str]:
                                
    out = []
    want = {
        "pcv_rbac_user_create": {(RBAC, "_ensure_admin_user"), (SIGNUP, "pcv_rest_auth_register")},
        "pcv_rbac_user_create_ex": {(HANDLER, "handle_auth_user_create"), (NAMES, "pcv_rbac_user_create")},
        "pcv_rbac_apikey_create": {(HANDLER, "handle_apikey_create")},
    }
    for callee, expected in want.items():
        got = set()
        pat = re.compile(r"\b" + callee + r"\s*\(")
        for path, text in sources.items():
            if callee not in text:
                continue
            m = masked_of(text)
            for mt in pat.finditer(m):
                fn = enclosing(text, mt.start())
                if fn is None:             
                    continue
                got.add((path, fn))
        if got != expected:
            out.append(f"{callee} 호출처 {sorted(got)} != {sorted(expected)}")
    rbac = sources[RBAC]
    for fn, judge in (("pcv_rbac_user_create_ex", "pcv_rbac_names_insert_user_locked("),
                      ("pcv_rbac_apikey_create", "pcv_rbac_names_insert_apikey_locked(")):
        if judge not in masked_body(rbac, fn):
            out.append(f"{RBAC}:{fn}: {judge} 를 부르지 않는다")
    return out


def in_string(text: str, needle: str) -> bool:
                                                          
    if needle not in text:
        return False
    kinds = kinds_of(text)
    return any(kinds[mt.start()] == "s" for mt in re.finditer(re.escape(needle), text))


def tombstone_problems(sources: dict[str, str]) -> list[str]:
                                          
    return [p for p, t in sources.items() if p != NAMES and in_string(t, TOMBSTONE)] + \
        ([] if in_string(sources.get(NAMES, ""), TOMBSTONE) else [f"{NAMES}: {TOMBSTONE} 없음"])


def delete_problems(text: str) -> list[str]:
                                                                      
    b = masked_body(text, "pcv_rbac_user_delete")
    out = []
    n = b.count("pcv_rbac_names_tombstone_locked(")
    if n != 1:
        out.append(f"pcv_rbac_user_delete: pcv_rbac_names_tombstone_locked( 출현 {n}번(1번이어야 한다)")
    else:
        cond = re.search(r"if \(rc != SQLITE_DONE \|\| !pcv_rbac_names_tombstone_locked\(db, username\)\s*\|\|\s*sqlite3_exec\(db,", b)
        if not cond:
            out.append("pcv_rbac_user_delete: 묘비 호출이 COMMIT 앞 실패 조건(롤백 갈래) 안에 없다")
    if in_string(text, TOMBSTONE):
        out.append("pcv_rbac_identity.c: 묘비 SQL 문자열이 있다")
    return out


DNA = "auth.deleted_name.add"
DNA_HANDLER = "handle_auth_deleted_name_add"


def deleted_name_add_problems(files: dict[str, str]) -> list[str]:
                                                              
    import json
    out = []
    routes = re.findall(r'g_hash_table_insert\(g_rpc_routes,\s*"' + re.escape(DNA) + r'",\s*\(gpointer\)(\w+)\)',
                        files["src/api/dispatcher.c"])
    if routes != [DNA_HANDLER]:
        out.append(f"dispatcher.c 라우트 {routes} (기대 [{DNA_HANDLER}])")
    pol = re.findall(r'\{\s*"' + re.escape(DNA) + r'",\s*(\d+)\s*\}', files["src/api/dispatcher_policy.c"])
    if pol != ["2"]:
        out.append(f"dispatcher_policy.c 정책 {pol} (기대 ['2'])")
    if DNA in files["src/modules/dispatcher/rpc_offload.c"]:
        out.append("rpc_offload.c 표에 있다(오프로드 없음이어야 한다)")
    for path, text in files.items():
        if path.startswith("src/api/rest_map_") and DNA in text:
            out.append(f"{path}: REST 전용 매핑이 있다")
    reg = json.loads(files["contracts/rpc_params.json"]).get(DNA)
    if reg != {"required": ["username"], "optional": []}:
        out.append(f"rpc_params.json 항목 {reg}")
    if not re.search(r'method="' + re.escape(DNA) + r'",\s*min_role=2,\s*owner_scope=False', files["scripts/check_rbac_policies.py"]):
        out.append("check_rbac_policies.py POLICY_CONTRACTS 항목(min_role=2) 없음")
    hb = masked_body(files["src/modules/dispatcher/handler_auth_names.c"], DNA_HANDLER)
    if "pcv_audit_log(" in hb:
        out.append("핸들러가 직접 감사한다(자동 감사와 겹친다)")
    return out


def dna_files() -> dict[str, str]:
                      
    paths = ["src/api/dispatcher.c", "src/api/dispatcher_policy.c", "src/modules/dispatcher/rpc_offload.c",
             "contracts/rpc_params.json", "scripts/check_rbac_policies.py", "src/modules/dispatcher/handler_auth_names.c"]
    out = {p: (ROOT / p).read_text() for p in paths}
    for q in sorted((ROOT / "src/api").glob("rest_map_*.c")):
        out[q.relative_to(ROOT).as_posix()] = q.read_text()
    return out


def recovery_text_problems(text: str) -> list[str]:
                                           
    kinds = kinds_of(text)
    lit = "".join(ch for ch, k in zip(text, kinds) if k == "s")
    out = []
    if "--reuse-deleted-name" not in lit:
        out.append("rest_login.c: 복구 안내에 --reuse-deleted-name 이 없다")
                                                                         
                                                  
    if "restart the daemon to re-seed" in lit:
        out.append("rest_login.c: 재시작 다시 시딩 안내(restart the daemon to re-seed)가 남아 있다")
    return out


def _send_calls(branch: str) -> list[str]:
                                                     
    out, i = [], 0
    m = masked_of(branch)
    while True:
        i = branch.find("pcv_rest_send_error(", i)
        if i < 0:
            return out
        open_paren = i + len("pcv_rest_send_error")
        depth, j = 0, open_paren
        while j < len(m):
            if m[j] == "(":
                depth += 1
            elif m[j] == ")":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        out.append(branch[open_paren:j + 1])
        i = j


def signup_problems(text: str) -> list[str]:
                                                                                            
                                                                                           
    b = body(text, "pcv_rest_auth_register")
    call = b.find("pcv_rbac_user_create(")
    if call < 0:
        return ["셀프 가입이 pcv_rbac_user_create 를 부르지 않는다"]
    fail = b.find("if (!ok) {", call)
    if fail < 0:
        return ["셀프 가입 생성 실패 갈래(if (!ok) {)가 없다"]
    end = c_blocks._match_brace(masked_of(b), b.index("{", fail))
    branch = b[fail:end + 1]
    out = []
    dom = branch.find("pcv_principal_signup_name_conflict(err)")
    first_send = branch.find("pcv_rest_send_error(")
    if dom < 0:
        out.append("실패 갈래가 pcv_principal_signup_name_conflict(err) 판정을 쓰지 않는다")
    if dom >= 0 and first_send >= 0 and not dom < first_send:
        out.append("이름 충돌 판정이 첫 응답 줄보다 뒤다")
    sends = [re.sub(r"\s+", " ", s) for s in _send_calls(branch)]
    if sorted(sends) != sorted(['(msg, 409, "CONFLICT", "username is reserved")',
                                '(msg, 500, "SERVER_ERROR", "registration failed")']):
        out.append(f"실패 갈래 응답이 409·500 고정 문구 둘이 아니다: {sends}")
    if any("err" in s for s in sends):
        out.append("응답 인자에 RBAC 오류(err)가 들어간다")
    if "PCV_LOG_WARN(" not in branch:
        out.append("오류 원문을 남기는 WARN이 없다")
    if "pcv_principal_names.h" not in text:
        out.append("pcv_principal_names.h include 없음")
    return out


def bootstrap_problems(text: str) -> list[str]:
                        
    b = masked_body(text, "_ensure_admin_user")
    out = []
    if b.count(REFUSAL_LOG) != 1:
        out.append(f"_ensure_admin_user: {REFUSAL_LOG} 출현 {b.count(REFUSAL_LOG)}번(1번이어야 한다)")
    if "PCV_LOG_ERROR(" in b:
        out.append("_ensure_admin_user: PCV_LOG_ERROR( 가 남아 있다")
    return out


WHOAMI_TU = "src/api/rest_map_platform.c"
_WHOAMI_GUARD = re.compile(r"pcv_principal_tenant_lookup_applies\(key_role\)\s*\?\s*pcv_rbac_get_tenant\(")


def whoami_problems(text: str) -> list[str]:
                                                                                                                
    start = text.find('g_strcmp0(name, "whoami")')
    if start < 0:
        return ["whoami 분기 없음"]
    end = text.find("} else if", start)
    block = text[start:end if end > 0 else len(text)]
    calls = block.count("pcv_rbac_get_tenant(")
    guarded = len(_WHOAMI_GUARD.findall(block))
    if calls == 0:
        return ["whoami 분기에 테넌트 조회가 없다(응답 tenant 칸)"]
    if calls != guarded:
        return [f"whoami 테넌트 조회 {calls}곳 중 키 판정 참 갈래 밖 {calls - guarded}곳"]
    return []


GRPC_TU = "src/api/grpc_server.c"
RPC_METHOD_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,126}")                                              


def grpc_order_problems(text: str) -> list[str]:
                                                                                       
    b = body(text, "_handle_client")
    i_map = b.find("for (int i = 0; MAP[i].grpc; i++)")
    i_chk = b.find("if (!pcv_rpc_method_name_is_valid(rpc_method))")
    i_inj = b.find("_grpc_inject_caller_identity(")
    i_call = b.find("_rpc_call(")
    if min(i_map, i_chk, i_inj, i_call) < 0:
        return ["MAP 탐색·문법 검사·주입·디스패처 호출 중 빠진 것이 있다"]
    probs = []
    if not (i_map < i_chk < i_inj < i_call):
        probs.append("문법 검사가 MAP 탐색 뒤·주입/디스패처 앞이 아니다")
    blk = b[i_chk:b.find("\n    }\n", i_chk)]
    if "return;" not in blk or "g_free(payload)" not in blk or blk.find("g_free(payload)") > blk.find("return;"):
        probs.append("거짓 갈래가 g_free(payload) 뒤 return 하지 않는다")
    if '"{\\"error\\":\\"invalid method\\"}"' not in blk:
        probs.append("거짓 갈래가 invalid method 오류 프레임을 보내지 않는다")
    return probs


def grpc_grammar_names() -> dict[str, list[str]]:
                                                   
    pol = re.findall(r'\{\s*"([^"]+)"\s*,\s*[0-9]\s*\}', (ROOT / "src/api/dispatcher_policy.c").read_text())
    rt = re.findall(r'g_hash_table_insert\(g_rpc_routes,\s*"([^"]+)"', (ROOT / "src/api/dispatcher.c").read_text())
    mp = re.findall(r'\{"/purecvisor[^"]*",\s*"([^"]+)"\}', (ROOT / GRPC_TU).read_text())
    return {"정책표": pol, "라우트": rt, "gRPC MAP": mp}


RA7_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,62}")
_LIT_ACTOR = re.compile(r'pcv_audit_log\(\s*"([^"]*)"', re.S)
_FALLBACK_ACTOR = re.compile(r'\b(actor|caller|caller_sub|sub|user|username|admin_user|owner)\b\s*\)?\s*:\s*"([^"]+)"')
_ACTOR_MACRO = re.compile(r'#define\s+(PCV_AUDIT_ACTOR_\w+|PCV_\w*_CALLER_SUBJECT)\s+"([^"]*)"')


def reserved_names(sources: dict[str, str]) -> set[str]:
                                                                                               
    text = sources["src/modules/auth/pcv_principal_names.c"]
    blk = text[text.index("k_reserved[] = {"):]
    blk = blk[:blk.index("};")]
    defs = dict(re.findall(r'#define\s+(\w+)\s+"([^"]*)"', "\n".join(sources.values())))
    out = set()
    for tok in re.findall(r'"([^"]*)"|\b([A-Z][A-Z0-9_]+)\b', blk):
        lit, mac = tok
        if lit:
            out.add(lit)
        elif mac in defs:
            out.add(defs[mac])
    return out


def actor_literal_problems(sources: dict[str, str]) -> list[str]:
                                                                                  
    res = reserved_names(sources)
    probs = []
    for path, text in sources.items():
        for rx, g in ((_LIT_ACTOR, 1), (_FALLBACK_ACTOR, 2)):
            for m in rx.finditer(text):
                v = m.group(g)
                if v not in res and RA7_RE.fullmatch(v):
                    probs.append(f"{path}:{text.count(chr(10), 0, m.start()) + 1} 행위자 '{v}' 가 예약 목록 밖이고 사용자 이름 규칙 안")
    return probs


def actor_macro_problems(sources: dict[str, str]) -> list[str]:
    res = reserved_names(sources)
    return [f"{p}: {m.group(1)}='{m.group(2)}' 예약 목록 밖" for p, t in sources.items()
            for m in _ACTOR_MACRO.finditer(t) if m.group(2) not in res]


ADMIN_FALLBACKS = (("src/modules/dispatcher/handler_push.c", "(sub && *sub) ? sub :"),
                   ("src/modules/network/vpc/vpc_manager.c", "actor ? actor :"))


def admin_fallback_problems(sources: dict[str, str]) -> list[str]:
    probs = []
    for path, needle in ADMIN_FALLBACKS:
        lines = [l for l in sources[path].splitlines() if needle in l]
        if not lines:
            probs.append(f"{path}: 폴백 줄 없음")
        probs += [f"{path}: '{l.strip()}' 에 \"admin\"" for l in lines if '"admin"' in l]
    return probs


class PrincipalNamesContract(unittest.TestCase):
    sources = src_sources()

    def test_single_insert_sites(self):
        self.assertEqual(insert_problems(self.sources), [])

    def test_single_insert_sites_counterfactual(self):
        bad = dict(self.sources)
        bad[HANDLER] = bad[HANDLER] + '\nstatic void x(void) { q("INSERT INTO users (username) VALUES (?)"); }\n'
        self.assertTrue(any(HANDLER in p for p in insert_problems(bad)))
        bad = dict(self.sources)
        bad[RBAC] = bad[RBAC] + '\nstatic void y(void) { q("INSERT OR IGNORE INTO api_keys (key_hash) VALUES (?)"); }\n'
        self.assertTrue(any(RBAC in p for p in insert_problems(bad)))

    def test_user_create_callers(self):
        self.assertEqual(caller_problems(self.sources), [])

    def test_user_create_callers_counterfactual(self):
        bad = dict(self.sources)
        bad[HANDLER] = bad[HANDLER] + "\nstatic void z(void) { pcv_rbac_user_create(\"a\", \"b\", 0, NULL, NULL); }\n"
        self.assertTrue(caller_problems(bad))
        bad = dict(self.sources)
        bad[RBAC] = bad[RBAC].replace("pcv_rbac_names_insert_apikey_locked(", "pcv_rbac_names_skip(", 1)
        self.assertTrue(any("pcv_rbac_apikey_create" in p for p in caller_problems(bad)))

    def test_tombstone_sql_single_tu(self):
        self.assertEqual(tombstone_problems(self.sources), [])
        self.assertEqual(delete_problems(self.sources[IDENTITY]), [])
        gone = self.sources[IDENTITY].replace("!pcv_rbac_names_tombstone_locked(db, username)", "FALSE", 1)
        self.assertTrue(delete_problems(gone))
        bad = dict(self.sources)
        bad["src/modules/auth/pcv_rbac_identity.c"] += '\n/* x */ static const char *s = "DELETE FROM deleted_user_names";\n'
        self.assertTrue(tombstone_problems(bad))

    def test_deleted_name_add_registered(self):
        files = dna_files()
        self.assertEqual(deleted_name_add_problems(files), [])
        bad = dict(files)
        bad["src/api/dispatcher_policy.c"] = re.sub(r'(\{\s*"auth\.deleted_name\.add",\s*)2(\s*\})', r'\g<1>1\2',
                                                    files["src/api/dispatcher_policy.c"], count=1)
        self.assertNotEqual(bad["src/api/dispatcher_policy.c"], files["src/api/dispatcher_policy.c"])
        self.assertTrue(any("정책" in p for p in deleted_name_add_problems(bad)))
        bad = dict(files)
        bad["src/api/rest_map_platform.c"] = files["src/api/rest_map_platform.c"] + '\n/* x */ static const char *m = "auth.deleted_name.add";\n'
        self.assertTrue(any("REST" in p for p in deleted_name_add_problems(bad)))

    def test_bootstrap_recovery_text(self):
        text = (ROOT / "src/api/rest_login.c").read_text()
        self.assertEqual(recovery_text_problems(text), [])
        self.assertTrue(recovery_text_problems(text.replace("--reuse-deleted-name", "--x", 1)))
        self.assertTrue(recovery_text_problems(
            'PCV_LOG_WARN(D, "--reuse-deleted-name ... or restart the daemon to re-seed the bootstrap admin");'))

    def test_signup_maps_principal_errors(self):
        self.assertEqual(signup_problems(self.sources[SIGNUP]), [])

    def test_signup_counterfactual(self):
        text = self.sources[SIGNUP]
        self.assertTrue(signup_problems(text.replace("pcv_principal_signup_name_conflict(err)", "FALSE", 1)))
        self.assertTrue(signup_problems(text.replace('"registration failed")', 'err ? err->message : "registration failed")', 1)))

    def test_bootstrap_uses_refusal_log(self):
        self.assertEqual(bootstrap_problems(self.sources[RBAC]), [])
        bad = self.sources[RBAC].replace("        " + REFUSAL_LOG + "admin_user, err);",
                                         '        PCV_LOG_ERROR(RBAC_LOG_DOM, "x %s", err ? err->message : "?");', 1)
        self.assertTrue(bootstrap_problems(bad))


    def test_whoami_skips_tenant_lookup_for_keys(self):
        self.assertEqual(whoami_problems(self.sources[WHOAMI_TU]), [])

    def test_whoami_counterfactual(self):
        text = self.sources[WHOAMI_TU]
        bad = text.replace("pcv_principal_tenant_lookup_applies(key_role) ? pcv_rbac_get_tenant(subject) : NULL",
                           "pcv_rbac_get_tenant(subject)", 1)
        self.assertNotEqual(bad, text)
        self.assertTrue(whoami_problems(bad))


    def test_grpc_checks_method_before_dispatch(self):
        self.assertEqual(grpc_order_problems(self.sources[GRPC_TU]), [])

    def test_grpc_order_counterfactual(self):
        text = self.sources[GRPC_TU]
        bad = text.replace("strlen(bad));\n        g_free(payload);\n        return;\n    }\n", "strlen(bad));\n        return;\n    }\n", 1)
        self.assertNotEqual(bad, text)
        self.assertTrue(grpc_order_problems(bad))

    def test_policy_names_pass_grpc_grammar(self):
        names = grpc_grammar_names()
        for kind, lst in names.items():
            self.assertTrue(lst, kind + " 이름을 못 읽음")
            self.assertEqual([n for n in lst if not RPC_METHOD_RE.fullmatch(n)], [], kind)
        c = (ROOT / "src/modules/auth/pcv_principal_names.c").read_text()
        self.assertIn("127", c, "C 문법 길이 상한(127자)이 파이썬 사본과 같아야 한다")


    def test_actor_literals_reserved_or_invalid(self):
        self.assertEqual(actor_literal_problems(self.sources), [])
        bad = dict(self.sources)
        bad["src/main.c"] = bad["src/main.c"] + '\nstatic void x(void) { pcv_audit_log("robot", "m", "t", "ok", 0, 0, "local"); }\n'
        self.assertTrue(actor_literal_problems(bad), "새 리터럴 행위자를 잡아야 한다(L9)")

    def test_actor_macros_reserved(self):
        self.assertEqual(actor_macro_problems(self.sources), [])
        bad = dict(self.sources)
        bad["src/modules/audit/pcv_audit.h"] = bad["src/modules/audit/pcv_audit.h"] + '\n#define PCV_AUDIT_ACTOR_ROBOT "robot"\n'
        self.assertTrue(actor_macro_problems(bad), "새 PCV_AUDIT_ACTOR_* 매크로를 잡아야 한다(L9)")

    def test_admin_fallbacks_replaced(self):
        self.assertEqual(admin_fallback_problems(self.sources), [])


if __name__ == "__main__":
    unittest.main()
