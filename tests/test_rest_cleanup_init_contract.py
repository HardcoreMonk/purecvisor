#!/usr/bin/env python3
                                                                                                  
                                                                       
                                                                                       
                                                                           
                                                                                          
                                                                                                           
                                                                                        
                                                                                                      
                                                                     
                                                                                                 
                                                                           
                                                                                              
                                                                     
                                                                           
                                              
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import rpc_extract              
import split_target                                                     

_T = split_target.load("rest_server")
CTX_TYPE = _T.context_type or "PcvRestRequest"
CTX_VAR = _T.context_var or "rq"
                                                                 
FREE_CALL = re.compile(r"\b(?:g_free|g_strfreev|json_object_unref|pcv_trace_context_free)\s*\(\s*"
                       r"((?:" + re.escape(CTX_VAR) + r"\s*->\s*)?)([A-Za-z_]\w*)\s*\)")
                                                       
EXPECTED_MIN = {"segs", "body", "subject", "rpc", "req_id", "trace_ctx"}                               
                                                    
DECL = re.compile(r"^    (?:const\s+)?[A-Za-z_]\w*\s*(?:\*\s*)*[A-Za-z_]\w*\s*(?:=|,|;)")
                                                                       
FIRST_EXIT = re.compile(r"\bgoto\s+cleanup(?:_no_free)?\s*;|\bPCV_REST_RUN\s*\(|\breturn\s+PCV_REST_STEP_(?:CLEANUP|RETURN)\s*;")
STORAGE = re.compile(r"\b" + re.escape(CTX_TYPE) + r"\s+" + re.escape(CTX_VAR) + r"_storage\s*=\s*\{\s*0?\s*\}\s*;")
STORAGE_ANY = re.compile(r"\b" + re.escape(CTX_TYPE) + r"\s+" + re.escape(CTX_VAR) + r"_storage\b")
CTX_PTR = re.compile(r"\b" + re.escape(CTX_TYPE) + r"\s*\*\s*" + re.escape(CTX_VAR) + r"\s*=\s*&\s*"
                     + re.escape(CTX_VAR) + r"_storage\s*;")
ALLOC = re.compile(r"(?:\b" + re.escape(CTX_VAR) + r"\s*->\s*)?\breq_id\s*=\s*pcv_generate_request_id\s*\(")
BYPASS = re.compile(r"\breturn\s*;|\breturn\s+PCV_REST_STEP_RETURN\s*;")


def strip_c(text: str) -> str:
                                                          
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
        elif text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            out.append(c + " " * (min(j, n) - i - 1) + (c if j < n else ""))
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def function_scope_decls(body: str, initialized: set[str] | None = None) -> dict[str, int]:
                                                                                    
    found: dict[str, int] = {}
    offset = 0
    for line in body.splitlines(keepends=True):
        if DECL.match(line) and not line.lstrip().startswith(("return", "goto")):
            stmt = line.strip().rstrip(";")
            head = re.match(r"(?:const\s+)?[A-Za-z_]\w*", stmt)
            rest = stmt[head.end():] if head else ""
            depth, part, parts = 0, [], []
            for ch in rest:                                  
                depth += ch in "([{"
                depth -= ch in ")]}"
                if ch == "," and depth == 0:
                    parts.append("".join(part))
                    part = []
                else:
                    part.append(ch)
            parts.append("".join(part))
            for declarator in parts:
                m = re.match(r"\s*(?:\*\s*)*([A-Za-z_]\w*)\s*(=)?", declarator)
                if m and m.group(1) not in found:
                    found[m.group(1)] = offset
                    if initialized is not None and m.group(2):
                        initialized.add(m.group(1))
        offset += len(line)
    return found


def check(text: str, functions=None) -> dict[str, list[str]]:
                                                                            
    out: dict[str, list[str]] = {"labels": [], "frees": [], "init": [], "bypass": []}
    code = strip_c(text)
    spans = rpc_extract.rest_request_spans(code, functions)
    if spans is None:
        out["labels"].append("요청 범위(_on_request 또는 구간 함수 정의)를 찾지 못함")
        return out
    scope = "".join(code[a:b] for a, b in spans)

    def loc(pos: int) -> str:
        return rpc_extract.rest_text_location(text, rpc_extract.rest_scope_origin(spans, pos))

    clean = re.search(r"^cleanup:", scope, re.M)
    if not clean or not re.search(r"^cleanup_no_free:", scope, re.M):
        out["labels"].append("_on_request에 cleanup:/cleanup_no_free: 레이블이 없다")
        return out
    frees = FREE_CALL.findall(scope[clean.start():])
    freed = {name for _, name in frees}
    rq_freed = {name for prefix, name in frees if prefix}
    missing = EXPECTED_MIN - freed
    if missing:
        out["frees"].append(f"cleanup 해제 목록 파싱 결과에 {sorted(missing)} 없음 — 계약 파서 점검")

    first = FIRST_EXIT.search(scope, 0, clean.start())
    if first is None:
        out["init"].append("_on_request에 cleanup으로 가는 출구(goto cleanup·PCV_REST_RUN)가 없다")
    else:
        initialized: set[str] = set()
        decls = function_scope_decls(scope[:clean.start()], initialized)
        late, uninit, absent = {}, {}, []
        for name in sorted(freed - rq_freed):
            if name not in decls:
                absent.append(name)
            else:
                if decls[name] > first.start():
                    late[name] = loc(decls[name])
                if name not in initialized:
                    uninit[name] = loc(decls[name])
        if absent:
            out["init"].append(f"{absent}: 함수 지역 선언을 찾지 못함")
        if late:
            out["init"].append(f"cleanup이 해제하는 포인터가 첫 출구({loc(first.start())}) 뒤에서야 선언된다"
                               f"(선언: {late}) — 그 출구가 초기화를 건너뛰어 쓰레기 포인터를 해제한다")
                                                                    
        if uninit:
            out["init"].append(f"cleanup이 해제하는 포인터가 선언에서 초기화되지 않는다(선언: {uninit})"
                               " — `= nullptr`로 선언할 것")
        if rq_freed:
            storage = STORAGE.search(scope, 0, first.start())
            ptr = CTX_PTR.search(scope, 0, first.start())
            if storage is None:
                any_decl = STORAGE_ANY.search(scope, 0, clean.start())
                why = ("0 초기화식(`= {0}`)이 없다" if any_decl and any_decl.start() < first.start()
                       else f"첫 출구({loc(first.start())})보다 앞에 없다")
                out["init"].append(f"cleanup이 {CTX_VAR}->{sorted(rq_freed)}를 해제하는데 "
                                   f"`{CTX_TYPE} {CTX_VAR}_storage = {{0}};`가 {why} — 첫 출구가 미정 값을 해제한다")
            if ptr is None:
                out["init"].append(f"`{CTX_TYPE} *{CTX_VAR} = &{CTX_VAR}_storage;`가 첫 출구보다 앞에 없다")

    alloc = ALLOC.search(scope, 0, clean.start())
    if alloc is None:
        out["bypass"].append("req_id 할당 지점을 요청 범위(_on_request + 구간 함수)에서 찾지 못함")
    else:
                                                           
        hits = [loc(m.start()) for m in BYPASS.finditer(scope, alloc.start(), clean.start())]
        if hits:
            out["bypass"].append(f"req_id 할당({loc(alloc.start())}) 뒤 cleanup:을 건너뛰는 출구"
                                 f"(return; 또는 return PCV_REST_STEP_RETURN;)가 {len(hits)}개 있다(위치: {hits})"
                                 " — req_id·trace_ctx가 새고 TLS 요청 ID가 남는다. goto cleanup / PCV_REST_STEP_CLEANUP으로 빠질 것")
    return out


class RestCleanupInitContract(unittest.TestCase):
                                   

    @classmethod
    def setUpClass(cls):
        cls.result = check(rpc_extract.rest_family_text(ROOT))

    def test_cleanup_labels_present(self):
        self.assertFalse(self.result["labels"], self.result["labels"])

    def test_cleanup_frees_expected_pointers(self):
        self.assertFalse(self.result["frees"], self.result["frees"])

    def test_freed_pointers_initialized_before_first_exit(self):
        self.assertFalse(self.result["init"], self.result["init"])

    def test_no_cleanup_bypass_after_request_id_allocated(self):
        self.assertFalse(self.result["bypass"], self.result["bypass"])


                                                                          
_SIG = ("static void\n_on_request(SoupServer *server, SoupServerMessage *msg, const gchar *path,\n"
        "            GHashTable *query, gpointer user_data)\n{\n")

PLAIN = _SIG + """\
    const gchar *method = soup_server_message_get_method(msg);
    gchar *req_id = nullptr;
    PcvTraceContext *trace_ctx = nullptr;
    JsonObject *body = nullptr;
    gchar **segs = nullptr, *subject = nullptr;
    gchar *rpc = nullptr, *vm_delete_name = nullptr;
    if (rate_limited(msg)) {
        send_429(msg);
        return;
    }
    req_id = pcv_generate_request_id();
    if (is_options(method)) {
        goto cleanup;
    }
    subject = authenticate(msg);
    if (!subject) goto cleanup;
    rpc = pcv_rest_build_rpc("vm.list", NULL);
    goto cleanup_no_free;

cleanup:
    g_strfreev(segs);
    if (body) json_object_unref(body);
    g_free(subject);
    g_free(rpc);
    g_free(vm_delete_name);
    g_free(req_id);
    pcv_trace_context_free(trace_ctx);
    return;

cleanup_no_free:
    g_strfreev(segs);
    g_free(req_id);
}
"""

SPLIT = _SIG + """\
    const gchar *method = soup_server_message_get_method(msg);
    PcvRestRequest rq_storage = {0};   /* 골격이 0 초기화 */
    PcvRestRequest *rq = &rq_storage;
    rq->msg = msg;
    rq->req_id = nullptr;
    JsonObject *body = nullptr;
    gchar **segs = nullptr, *subject = nullptr;

    PCV_REST_RUN(pcv_rest_prelude(rq));
    PCV_REST_RUN(pcv_rest_auth_token(rq));
    subject = authenticate(msg);
    rq->subject = subject;
    if (!subject) goto cleanup;
    PcvRestStep st = pcv_rest_map_compute(rq);
    if (st == PCV_REST_STEP_CLEANUP) goto cleanup;
    goto cleanup_no_free;

cleanup:
    g_strfreev(rq->segs);
    if (rq->body) json_object_unref(rq->body);
    g_free(rq->subject);
    g_free(rq->rpc);
    g_free(rq->vm_delete_name);
    g_free(rq->req_id);
    pcv_trace_context_free(rq->trace_ctx);
    return;

cleanup_no_free:
    g_strfreev(rq->segs);
    g_free(rq->req_id);
}

/* ==== src/api/rest_prelude.c ==== */
PcvRestStep
pcv_rest_prelude(PcvRestRequest *rq)
{
    SoupServerMessage *msg = rq->msg;
    if (rate_limited(msg)) {
        send_429(msg);
        return PCV_REST_STEP_RETURN;
    }
    rq->req_id = pcv_generate_request_id();
    if (is_options(rq->method)) {
        return PCV_REST_STEP_CLEANUP;
    }
    return PCV_REST_STEP_NEXT;
}

/* ==== src/api/rest_auth_endpoints.c ==== */
PcvRestStep
pcv_rest_auth_token(PcvRestRequest *rq)
{
    if (g_strcmp0(rq->path, "/api/v1/auth/token") == 0) {
        send_token(rq->msg);
        return PCV_REST_STEP_CLEANUP;
    }
    return PCV_REST_STEP_NEXT;
}

/* ==== src/api/rest_map_compute.c ==== */
PcvRestStep
pcv_rest_map_compute(PcvRestRequest *rq)
{
    rq->rpc = pcv_rest_build_rpc("vm.list", NULL);
    return PCV_REST_STEP_NEXT;
}
"""


def _once(text: str, old: str, new: str) -> str:
    assert text.count(old) == 1, f"합성 텍스트 앵커가 하나가 아님: {old!r}"
    return text.replace(old, new, 1)


class RestCleanupInitCounterfactual(unittest.TestCase):
    def assertRed(self, text: str, key: str, needle: str):
        got = check(text)
        self.assertTrue(got[key] and any(needle in r for r in got[key]), got)

    def assertGreen(self, text: str):
        got = check(text)
        self.assertFalse(any(got.values()), got)

                              
    def test_plain_hotfix_shape_passes(self):
        self.assertGreen(PLAIN)

    def test_plain_decl_after_first_goto_is_red(self):
        moved = _once(PLAIN, "    gchar *rpc = nullptr, *vm_delete_name = nullptr;\n", "")
        moved = _once(moved, "    subject = authenticate(msg);\n",
                      "    gchar *rpc = nullptr, *vm_delete_name = nullptr;\n    subject = authenticate(msg);\n")
        self.assertRed(moved, "init", "뒤에서야 선언")

    def test_plain_decl_without_initializer_is_red(self):
        self.assertRed(_once(PLAIN, "gchar **segs = nullptr,", "gchar **segs,"), "init", "초기화되지 않는다")

    def test_plain_bare_return_after_req_id_is_red(self):
        self.assertRed(_once(PLAIN, "    if (!subject) goto cleanup;\n", "    if (!subject) return;\n"),
                       "bypass", "return;")

            
    def test_split_shape_passes(self):
        self.assertGreen(SPLIT)

    def test_split_storage_without_zero_init_is_red(self):
        self.assertRed(_once(SPLIT, "PcvRestRequest rq_storage = {0};", "PcvRestRequest rq_storage;"),
                       "init", "0 초기화식")

    def test_split_storage_declared_after_first_run_is_red(self):
        moved = _once(SPLIT, "    PcvRestRequest rq_storage = {0};   /* 골격이 0 초기화 */\n", "")
        moved = _once(moved, "    PCV_REST_RUN(pcv_rest_auth_token(rq));\n",
                      "    PCV_REST_RUN(pcv_rest_auth_token(rq));\n    PcvRestRequest rq_storage = {0};\n")
        self.assertRed(moved, "init", "첫 출구")

    def test_split_step_return_after_alloc_in_block_is_red(self):
        self.assertRed(_once(SPLIT, "        send_token(rq->msg);\n        return PCV_REST_STEP_CLEANUP;",
                             "        send_token(rq->msg);\n        return PCV_REST_STEP_RETURN;"),
                       "bypass", "PCV_REST_STEP_RETURN")

    def test_split_chain_return_line_in_skeleton_is_red(self):
                                                                                         
        self.assertRed(_once(SPLIT, "    if (st == PCV_REST_STEP_CLEANUP) goto cleanup;\n",
                             "    if (st == PCV_REST_STEP_CLEANUP) goto cleanup;\n"
                             "    if (st == PCV_REST_STEP_RETURN) return;\n"),
                       "bypass", "return;")

    def test_split_missing_block_definition_is_red(self):
                                                             
        self.assertRed(_once(SPLIT, "pcv_rest_prelude(PcvRestRequest *rq)\n{",
                             "pcv_rest_prelude_gone(PcvRestRequest *rq)\n{"),
                       "labels", "구간 함수 정의")

    def test_split_cleanup_parser_sees_rq_fields(self):
                                                                         
        self.assertRed(_once(SPLIT, "    g_free(rq->rpc);\n", ""), "frees", "rpc")


if __name__ == "__main__":
    unittest.main()
