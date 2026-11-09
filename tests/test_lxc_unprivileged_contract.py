#!/usr/bin/env python3
                                                                               
                                                                                     
                                                                                 
                                                                                    
                                                                      
import re
import unittest
from pathlib import Path


def _replace_nth(text, old, new, nth):
    start = -1
    for _ in range(nth):
        start = text.find(old, start + 1)
        if start < 0:
            return text
    return text[:start] + new + text[start + len(old):]

SRC = Path(__file__).resolve().parents[1] / "src/modules"


def code_only(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def body(rel: str, anchor: str) -> str:
    text = (SRC / rel).read_text()
    start = text.index(anchor)
    return code_only(text[start:text.index("\n}\n", start)])


                                                                             
                                                
DEF_RE = re.compile(r"^(?:[A-Za-z_][\w \t*]*[ \t*])?([A-Za-z_]\w*)\([^;{}]*?\)\s*\{[ \t]*\n", re.M)


def functions(code: str):
                                                                
    for m in DEF_RE.finditer(code):
        yield m.group(1), code[m.start():code.index("\n}\n", m.end())]


class LxcUnprivilegedContract(unittest.TestCase):
    def test_create_handler_checks_before_lock_and_accept(self):
        b = body("dispatcher/handler_container.c", "handle_container_create(JsonObject *params")
        at = b.find("pcv_container_create_mode_check(")
        self.assertGreater(at, 0)
                                                                                                
                                                                                   
        alias = b.find("pcv_container_create_bridge_alias(")
        read = b.find('json_object_has_member(params, "network_bridge")')
        valid = b.find("pcv_validate_bridge_name(")
        self.assertTrue(0 < alias < read < valid < at, (alias, read, valid, at))
        for later in ("pcv_vm_lock_acquire(", '"accepted"', "pcv_lxc_create_async_full("):
            self.assertLess(at, b.find(later), later)
        self.assertNotIn('json_object_get_boolean_member(params, "rootless")', b)
        self.assertIn("mode.rootless", b)

    def test_mode_check_order_form_role_host(self):
        b = body("dispatcher/handler_container_mode.c", "pcv_container_create_mode_check(JsonObject *params")
        form = b.find('pcv_rpc_params_get_bool(params, "rootless"')
        role = b.find("pcv_disp_dispatcher_caller_role(")
        host = b.find("pcv_lxc_userns_host_check(")
        self.assertTrue(0 < form < role < host, (form, role, host))

    def test_driver_has_no_silent_privileged_path(self):
        raw = (SRC / "lxc/lxc_driver.c").read_text()
        code = code_only(raw)
        self.assertNotIn("continuing as privileged", raw)
        self.assertNotIn("_rootless_apply_config", code)
        self.assertNotIn('set_config_item(c, "lxc.idmap"', code)
        create = body("lxc/lxc_driver.c", "_lxc_create_thread(GTask")
        self.assertIn('"-f", userns_conf', create)
        settle = create.find("pcv_lxc_userns_settle_dirs(")
        scan = create.find("pcv_lxc_userns_settle_rootfs(")
        self.assertTrue(0 < settle < scan, (settle, scan))              
                                                    
        self.assertRegex(create, r"!pcv_lxc_userns_settle_rootfs\([^;]*\) \|\|\s*"
                                 r"!pcv_lxc_userns_check_top\(PCV_LXC_PATH, d->name, &error\)\)\) \{\s*_create_rollback\(")
        self.assertLess(create.find("pcv_lxc_userns_check_top("), create.find("lxc_container_new("))
        self.assertIn("_lxc_handle_privileged(c)", create)
                                                                      
        self.assertRegex(create, r"\} else if \(userns\) \{[^}]*?if \(_lxc_handle_privileged\(c\)\) \{\s*stage = \"idmap check\";")
        self.assertRegex(create, r"\} else if \(c->get_config_item\(c, \"lxc\.net\.0\.link\", link, sizeof\(link\)\) <= 0 \|\|"
                                 r" g_strcmp0\(link, bridge\) != 0\) \{\s*stage = \"config check\";")

    def test_create_userns_branch_skips_liblxc_config_writes(self):
                                                                                                     
                                                                              
        create = body("lxc/lxc_driver.c", "_lxc_create_thread(GTask")
        start = create.index("else if (userns) {")
        branch = create[start:create.index("} else {", start)]
        for call in ("save_config(", "set_config_item(", "clear_config_item("):
            self.assertNotIn(call, branch)
        self.assertEqual(create.count("save_config("), 1)
        self.assertIn("!userns && !c->save_config(c, NULL)", create)
        res = create.find("pcv_lxc_userns_resource_lines(bridge, memory_mb, vcpu)")
        self.assertTrue(0 < res < create.find('"-f", userns_conf'), res)                            
        self.assertIn("us.start, us.count, res, &error", create)

    def test_no_privileged_file_writes_in_daemon_thread(self):
                                                                                
        banned = re.compile(r"\b(?:l?chown|fchownat|fchown|setxattr|lsetxattr|fsetxattr)\s*\(")
        for rel in ("lxc/lxc_userns.c", "lxc/lxc_driver.c"):
            self.assertEqual(banned.findall(code_only((SRC / rel).read_text())), [], rel)
        userns = code_only((SRC / "lxc/lxc_userns.c").read_text())
        self.assertIn('"chown", "-R", "-h"', userns)
        self.assertIn('"lxc-usernsexec", "-m"', userns)
                                                                               
        self.assertEqual(userns.count("PCV_CHILD_CAP_STORAGE"), 1)
        self.assertEqual(userns.count("PCV_CHILD_CAP_RUNTIME"), 1)
        self.assertEqual(len(re.findall(r"\b_child_storage\(argv,", userns)), 3)                                     
        self.assertEqual(len(re.findall(r"\b_child_runtime\(argv,", userns)), 1)                    

    def test_clone_checks_privileged_source_before_accept(self):
        b = body("dispatcher/handler_container_net.c", "void handle_container_clone(JsonObject *params")
        at = b.find("pcv_lxc_container_privileged(src)")
        self.assertGreater(at, 0)
        self.assertLess(at, b.find('"accepted"'))
        self.assertIn("PCV_ROLE_ADMIN", b[:at + 200])

    def test_clone_worker_settles_userns_target(self):
                                                                                                             
                                                                                           
        b = body("lxc/lxc_driver.c", "_clone_worker(GTask *task")
        seq = [b.find("_lock_container_op(ctx->source)"), b.find("_lock_container_op(ctx->target)"),
               b.find("const gboolean src_unpriv = !pcv_lxc_container_privileged(ctx->source);"), b.find("pcv_lxc_clone_run(")]
        self.assertTrue(0 < seq[0] < seq[1] < seq[2] < seq[3], seq)
        self.assertRegex(b, r"pcv_lxc_clone_run\(PCV_LXC_PATH, PCV_LXC_ZFS_BASE, ctx->source, ctx->target, ctx->subject, "
                            r"src_unpriv, &\w+\)")
                                                                                                     
        w = body("lxc/lxc_clone.c", "pcv_lxc_clone_run(const gchar *lxc_path")
        self.assertRegex(w, r"if \(!stage && src_unpriv && !ops->settle\(lxc_path, dst, &err\)\)\s*stage = \"directory setup\";")
                                                                                
        for rel in ("lxc/lxc_driver.c", "lxc/lxc_clone.c"):
            self.assertNotIn('"lxc-copy"', code_only((SRC / rel).read_text()), rel)
        real = code_only((SRC / "lxc/lxc_clone.c").read_text())
        self.assertRegex(real, r"\.settle\s*=\s*_clone_real_settle,")
        settle = dict(functions(real)).get("_clone_real_settle", "")
        self.assertRegex(settle, r"pcv_lxc_userns_settle_dirs\(lxc_path, dst, error\) &&\s*pcv_lxc_userns_check_top\(lxc_path, dst, error\)")

    def test_clone_locks_and_renew_order(self):
                                                                                                                  
                                                                                                  
                                                                                                    
                                                                                                               
                                                                     
                                                                                              
                                                                                            
                                                                                              
                                                                                                     
                                                 
        net = (SRC / "dispatcher/handler_container_net.c").read_text()
        drv = (SRC / "lxc/lxc_driver.c").read_text()
        self.assertEqual(self._clone_violations(net, drv), [])

        def move_line(text, needle, before):
                                                                
            lines = text.split("\n")
            i = next(k for k, ln in enumerate(lines) if needle in ln)
            moved = lines.pop(i)
            j = next(k for k, ln in enumerate(lines) if before in ln)
            lines.insert(j, moved)
            return "\n".join(lines)

        h0 = net.index("void handle_container_clone(JsonObject *params")
        h1 = net.index("\n}\n", h0)
        gate = net.index("pcv_container_clone_volume_gate(", h0)

        def in_gate_branch(old, new):
            return net[:gate] + net[gate:h1].replace(old, new, 1) + net[h1:]

        def in_fn(text, head, old, new):
                                                          
            f0 = text.index(head)
            f1 = text.index("\n}\n", f0)
            return text[:f0] + text[f0:f1].replace(old, new, 1) + text[f1:]

        hook = "_ctr_clone_release_locks(gpointer data)"
        cb = "_on_container_clone_done(GObject *src"
        ret = "_clone_task_return(GTask *task, CloneCtx *ctx, GError *err)"
        net_mutants = {                           
            "원본 잠금 op 바꿈": net.replace("pcv_vm_lock_acquire(src, VM_OP_SNAPSHOT", "pcv_vm_lock_acquire(src, VM_OP_TUNING", 1),
            "대상 잠금 op 바꿈": net.replace("pcv_vm_lock_acquire(dst, VM_OP_CREATING", "pcv_vm_lock_acquire(dst, VM_OP_TUNING", 1),
            "대상 잠금을 원본보다 먼저": move_line(net, "pcv_vm_lock_acquire(dst, VM_OP_CREATING", "pcv_vm_lock_acquire(src, VM_OP_SNAPSHOT"),
            "잠금을 privileged 확인 앞으로": move_line(net, "pcv_vm_lock_acquire(src, VM_OP_SNAPSHOT", "pcv_lxc_container_privileged(src)"),
            "관문을 잠금 앞으로": move_line(net, "pcv_container_clone_volume_gate(", "pcv_vm_lock_acquire(src, VM_OP_SNAPSHOT"),
            "대상 잠금 실패에 원본 해제 없음": net.replace("        pcv_vm_lock_release(src_lock);\n", "", 1),
            "대상 잠금 실패가 −32603": net.replace("PURE_RPC_ERR_BUSY,\n            lock_err ? lock_err : \"Target",
                                             "PURE_RPC_ERR_INTERNAL_ERROR,\n            lock_err ? lock_err : \"Target", 1),
            "관문 거절에 원본 해제 없음": in_gate_branch("        pcv_vm_lock_release(src_lock);\n", ""),
            "관문 거절에 대상 해제 없음": in_gate_branch("        pcv_vm_lock_release(dst_lock);\n", ""),
            "관문 거절이 −32002": in_gate_branch("PURE_RPC_ERR_FORBIDDEN, gate_msg", "PURE_RPC_ERR_CONFLICT, gate_msg"),
            "브리지 관문 빼기": net.replace(" &&\n                             pcv_container_clone_bridge_gate(caller_role, config_text, "
                                         "_clone_bridge_allowed, &res_caller, NULL, &gate_msg);", ";", 1),
            "브리지 판정이 늘 허용": net.replace("pcv_res_check(data, PCV_RES_BRIDGE, bridge, NULL, NULL) == PCV_RES_ALLOW", "TRUE", 1),
            "브리지 판정이 다른 종류": net.replace("pcv_res_check(data, PCV_RES_BRIDGE,", "pcv_res_check(data, PCV_RES_ISO,", 1),
            "브리지 거절 감사 없음": net.replace('    pcv_res_audit_denied("container.clone", "network_bridge", bridge);\n', "", 1),

            "mutex 준비를 갱신 스레드 뒤로": move_line(net, "g_mutex_init(&ctx->lock_renew_mu);", "pcv_lxc_clone_async(src, dst"),
            "cond 준비 없음": net.replace("    g_cond_init(&ctx->lock_renew_cond);\n", "", 1),
            "갱신 스레드를 응답 뒤로": move_line(net, "_ctr_clone_lock_renew_worker, ctx", "pcv_lxc_clone_async(src, dst"),
            "워커를 응답 앞으로": move_line(net, "pcv_lxc_clone_async(src, dst", "json_object_set_string_member(res, \"status\", \"accepted\")"),
            "volume_entries -1도 응답": net.replace("if (volume_entries >= 0)", "if (volume_entries >= -1)", 1),
            "subject 출처 바뀜": net.replace("\"_pcv_caller_sub\", NULL),\n                        _ctr_clone_release_locks",
                                          "\"_pcv_caller_name\", NULL),\n                        _ctr_clone_release_locks", 1),
            "해제 훅을 넘기지 않음": net.replace("_ctr_clone_release_locks, ctx, NULL, _on_container_clone_done",
                                           "NULL, NULL, NULL, _on_container_clone_done", 1),
            "훅: 해제 뒤 join": move_line(move_line(net, "pcv_vm_lock_release(ctx->source_lock);", "if (ctx->lock_renew_thread) {"),
                                       "pcv_vm_lock_release(ctx->target_lock);", "if (ctx->lock_renew_thread) {"),
            "훅: 스레드 없으면 해제 안 함": in_fn(net, hook, "        ctx->lock_renew_thread = NULL;\n    }\n",
                                         "        ctx->lock_renew_thread = NULL;\n"),
            "훅: 대상 해제 없음": net.replace("    pcv_vm_lock_release(ctx->target_lock);\n", "", 1),
            "훅: 정지 플래그 없음": in_fn(net, hook, "ctx->lock_renew_stop = TRUE;", ""),
            "훅: 신호 없음": in_fn(net, hook, "g_cond_signal(&ctx->lock_renew_cond);", ""),
            "훅: 신호를 join 뒤로": move_line(net, "g_cond_signal(&ctx->lock_renew_cond);", "ctx->lock_renew_thread = NULL;"),
            "훅: ctx까지 해제": in_fn(net, hook, "    ctx->target_lock = NULL;", "    ctx->target_lock = NULL;\n    _container_clone_audit_ctx_free(ctx);"),
            "콜백: 메인 루프에서 join": in_fn(net, cb, "    _container_clone_audit_ctx_free(ctx);",
                                        "    g_thread_join(ctx->lock_renew_thread);\n    _container_clone_audit_ctx_free(ctx);"),
            "콜백: 메인 루프에서 해제": in_fn(net, cb, "    _container_clone_audit_ctx_free(ctx);",
                                        "    pcv_vm_lock_release(ctx->source_lock);\n    _container_clone_audit_ctx_free(ctx);"),
            "콜백: 해제 워커 되살림": in_fn(net, cb, "    _container_clone_audit_ctx_free(ctx);",
                                       "    GTask *unlock = pcv_drain_task_new(NULL, NULL, NULL, NULL);\n    _container_clone_audit_ctx_free(ctx);"),
            "콜백: 감사 앞 ctx 해제": move_line(net, "    _container_clone_audit_ctx_free(ctx);", "pcv_audit_log(pcv_drain_task_actor(res)"),
            "갱신: 250 ms 폴링으로 되돌림": net.replace(
                "signaled = g_cond_wait_until(&ctx->lock_renew_cond, &ctx->lock_renew_mu, deadline);",
                "{ g_mutex_unlock(&ctx->lock_renew_mu); g_usleep(250 * 1000); g_mutex_lock(&ctx->lock_renew_mu); }", 1),
            "갱신이 대상 잠금을 빠뜨림": net.replace("(void)pcv_vm_lock_renew(ctx->target_lock);", "", 1),
            "갱신: mutex를 쥔 채 갱신": in_fn(net, "_ctr_clone_lock_renew_worker(gpointer data)",
                                         "        g_mutex_unlock(&ctx->lock_renew_mu);", ""),
            "콜백이 훅을 직접 부름": in_fn(net, cb, "    _container_clone_audit_ctx_free(ctx);",
                                       "    _ctr_clone_release_locks(ctx);\n    _container_clone_audit_ctx_free(ctx);"),
            "갱신 주기 1초": net.replace("#define CTR_CLONE_LOCK_RENEW_INTERVAL_US   (60 * G_USEC_PER_SEC)",
                                     "#define CTR_CLONE_LOCK_RENEW_INTERVAL_US   (1 * G_USEC_PER_SEC)", 1),
        }
        drv_mutants = {
            "껍데기: 결과 뒤 훅": in_fn(in_fn(drv, ret, "    if (release) release(ctx->release_data);", ""), ret,
                                     "        g_task_return_boolean(task, TRUE);\n",
                                     "        g_task_return_boolean(task, TRUE);\n    if (release) release(ctx->release_data);\n"),
            "껍데기: 한 번 보장 없음": in_fn(drv, ret, "    ctx->release_cb = NULL;", ""),
            "껍데기: 원본 BUSY를 직접 반환": in_fn(drv, "_clone_worker(GTask *task",
                                            "_clone_task_return(task, ctx, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_BUSY,\n"
                                            "                                                          \"source",
                                            "g_task_return_error(task, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_BUSY, \"source"),
            "껍데기: 성공을 직접 반환": in_fn(drv, "_clone_worker(GTask *task", "    _clone_task_return(task, ctx, ok ? NULL : err);",
                                         "    if (ok) g_task_return_boolean(task, TRUE); else _clone_task_return(task, ctx, err);"),
            "껍데기: 인자 오류를 호출 스레드에서": in_fn(drv, "pcv_lxc_clone_async(const gchar *source", "    CloneCtx *ctx = g_new0(CloneCtx, 1);",
                                               "    if (!source) { g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, \"x\"); "
                                               "g_object_unref(task); return; }\n    CloneCtx *ctx = g_new0(CloneCtx, 1);"),
            "껍데기: 접수 함수가 훅을 부름": in_fn(drv, "pcv_lxc_clone_async(const gchar *source", "    ctx->release_data = release_data;",
                                            "    ctx->release_data = release_data;\n    if (release_cb) release_cb(release_data);"),
            "껍데기: 소각기가 훅을 부름": in_fn(drv, "static void _clone_ctx_free(gpointer p) {", "    CloneCtx *ctx = p;",
                                          "    CloneCtx *ctx = p;\n    if (ctx->release_cb) ctx->release_cb(ctx->release_data);"),
            "껍데기: 훅을 담지 않음": drv.replace("    ctx->release_cb = release_cb;\n", "", 1),
            "껍데기: 인자 오류 확인 없음": in_fn(drv, "_clone_worker(GTask *task", "if (!ctx->source || !ctx->target || !*ctx->source || !*ctx->target) {",
                                            "if (0) {"),
        }
        for what, mutated in net_mutants.items():
            with self.subTest(mutant=what):
                self.assertNotEqual(mutated, net, (what, "mutation did not apply"))
                self.assertNotEqual(self._clone_violations(mutated, drv), [], what)
        for what, mutated in drv_mutants.items():
            with self.subTest(mutant=what):
                self.assertNotEqual(mutated, drv, (what, "mutation did not apply"))
                self.assertNotEqual(self._clone_violations(net, mutated), [], what)

    @staticmethod
    def _seq_ok(seq):
        return 0 < seq[0] and all(a < b for a, b in zip(seq, seq[1:]))

    @classmethod
    def _clone_violations(cls, net: str, drv: str):
        fns = dict(functions(code_only(net)))
        out = []
        h = fns.get("handle_container_clone", "")
        acc = h.find('"accepted"')
        hseq = [h.find("pcv_lxc_container_privileged(src)"), h.find("pcv_vm_lock_acquire(src, VM_OP_SNAPSHOT, &lock_err)"),
                h.find("pcv_vm_lock_acquire(dst, VM_OP_CREATING, &lock_err)"),
                h.find('g_build_filename(PCV_LXC_PATH, src, "config", NULL)'), h.find("g_file_get_contents(config_path"),
                h.find("const gint caller_role = pcv_disp_dispatcher_caller_role(params, connection);"),
                h.find("pcv_container_clone_volume_gate(caller_role, config_text, &volume_entries, &gate_msg)"),
                h.find("pcv_container_clone_bridge_gate(caller_role, config_text, _clone_bridge_allowed, &res_caller, NULL, &gate_msg)"),
                h.find("ctx->target_lock = dst_lock;"), h.find("g_mutex_init(&ctx->lock_renew_mu);"),
                h.find("g_cond_init(&ctx->lock_renew_cond);"), h.find("_ctr_clone_lock_renew_worker, ctx"), acc,
                h.find("pure_uds_server_send_response(", acc) if acc > 0 else -1, h.find("pcv_lxc_clone_async(src, dst")]
        if not cls._seq_ok(hseq):
            out.append(f"handler: privileged < SNAPSHOT < CREATING < config read < caller role < volume gate < bridge gate < ctx locks "
                       f"< mutex < cond "
                       f"< renew thread < accepted < send < clone_async {hseq}")
        for what, pat in (
                ("SNAPSHOT busy", r"if \(!src_lock\) \{[^{}]*PURE_RPC_ERR_BUSY,[^{}]*\breturn;\s*\}"),
                ("CREATING busy releases source",
                 r"if \(!dst_lock\) \{[^{}]*pcv_vm_lock_release\(src_lock\);[^{}]*PURE_RPC_ERR_BUSY,[^{}]*\breturn;\s*\}"),
                ("gate reject releases both, −32006",
                 r"if \(!gate_ok\) \{[^{}]*pcv_vm_lock_release\(src_lock\);[^{}]*pcv_vm_lock_release\(dst_lock\);[^{}]*"
                 r"PURE_RPC_ERR_FORBIDDEN, gate_msg[^{}]*\breturn;\s*\}"),
                ("volume_entries only when read", r"if \(volume_entries >= 0\)\s*"
                                                  r"json_object_set_int_member\(res, \"volume_entries\", volume_entries\);"),
                ("renew thread", r"ctx->lock_renew_thread = g_thread_try_new\(\"[\w-]+\", _ctr_clone_lock_renew_worker, ctx, NULL\);"),
                ("worker subject and release hook",
                 r"pcv_lxc_clone_async\(src, dst, json_object_get_string_member_with_default\(params, "
                 r"\"_pcv_caller_sub\", NULL\),\s*_ctr_clone_release_locks, ctx, NULL, _on_container_clone_done, ctx\);")):
            if not re.search(pat, h):
                out.append(f"handler: {what}")
        if h.count("pcv_vm_lock_acquire(") != 2:
            out.append("handler: two lock acquires")
                                                                                                        
        if not re.search(r"const gboolean gate_ok = pcv_container_clone_volume_gate\([^;]*\) &&\s*"
                         r"pcv_container_clone_bridge_gate\(caller_role, config_text, _clone_bridge_allowed, &res_caller, NULL, &gate_msg\);", h):
            out.append("handler: bridge gate joins the volume gate result")
        if not re.search(r"pcv_res_caller_init\(&res_caller, params, connection\);[\s\S]*const gboolean gate_ok[\s\S]*"
                         r"pcv_res_caller_clear\(&res_caller\);", h):
            out.append("handler: bridge gate caller init/clear")
        bc = fns.get("_clone_bridge_allowed", "")
        if not re.search(r"pcv_res_check\(data, PCV_RES_BRIDGE, bridge, NULL, NULL\) == PCV_RES_ALLOW", bc) or \
                'pcv_res_audit_denied("container.clone", "network_bridge", bridge)' not in bc:
            out.append("bridge check: pcv_res_check(PCV_RES_BRIDGE) + denied audit")
                                                                                             
        rk = fns.get("_ctr_clone_release_locks", "")
        rseq = [rk.find("g_mutex_lock(&ctx->lock_renew_mu);"), rk.find("ctx->lock_renew_stop = TRUE;"),
                rk.find("g_cond_signal(&ctx->lock_renew_cond);"), rk.find("g_mutex_unlock(&ctx->lock_renew_mu);"),
                rk.find("if (ctx->lock_renew_thread) {"), rk.find("g_thread_join(ctx->lock_renew_thread);"),
                rk.find("pcv_vm_lock_release(ctx->source_lock);"), rk.find("pcv_vm_lock_release(ctx->target_lock);")]
        if not cls._seq_ok(rseq):
            out.append(f"release hook: lock < stop < signal < unlock < join < release source < release target {rseq}")
        if not re.search(r"if \(ctx->lock_renew_thread\) \{[^{}]*g_thread_join\(ctx->lock_renew_thread\);[^{}]*\}\s*"
                         r"pcv_vm_lock_release\(ctx->source_lock\);\s*ctx->source_lock = NULL;\s*"
                         r"pcv_vm_lock_release\(ctx->target_lock\);\s*ctx->target_lock = NULL;", rk):
            out.append("release hook: releases outside the join branch (no renew thread = release only)")
        if rk.count("g_thread_join(") != 1 or rk.count("pcv_vm_lock_release(") != 2 or "_container_clone_audit_ctx_free(" in rk:
            out.append("release hook: one join, two releases, ctx stays for the completion callback")
                                                                               
        cb = fns.get("_on_container_clone_done", "")
        cseq = [cb.find("pcv_lxc_clone_finish(res, &error)"), cb.find("pcv_audit_log("), cb.find("pcv_ws_broadcast_job_complete("),
                cb.find("_container_clone_audit_ctx_free(ctx);")]
        if not cls._seq_ok(cseq):
            out.append(f"callback: finish < audit < ws < ctx free {cseq}")
        for banned in ("g_thread_join(", "pcv_vm_lock_release(", "lock_renew", "g_task_run_in_thread(", "pcv_drain_task_new("):
            if banned in cb:
                out.append(f"callback: {banned} on the main loop")
        if "_ctr_clone_unlock_worker" in code_only(net):
            out.append("unlock worker still present (released after the completion event)")
                                                                                   
        w = fns.get("_ctr_clone_lock_renew_worker", "")
        wseq = [w.find("g_mutex_lock(&ctx->lock_renew_mu);"), w.find("while (!ctx->lock_renew_stop) {"),
                w.find("const gint64 deadline = g_get_monotonic_time() + CTR_CLONE_LOCK_RENEW_INTERVAL_US;"),
                w.find("signaled = g_cond_wait_until(&ctx->lock_renew_cond, &ctx->lock_renew_mu, deadline);"),
                w.find("if (ctx->lock_renew_stop) break;"), w.find("g_mutex_unlock(&ctx->lock_renew_mu);"),
                w.find("(void)pcv_vm_lock_renew(ctx->source_lock);"), w.find("(void)pcv_vm_lock_renew(ctx->target_lock);")]
        if not cls._seq_ok(wseq):
            out.append(f"renew worker: lock < loop < deadline < cond wait < stop check < unlock < renew source < renew target {wseq}")
        if "g_usleep(" in w or w.count("g_mutex_unlock(&ctx->lock_renew_mu);") != 2:
            out.append("renew worker: waits on the cond (no polling sleep), renews outside the mutex")
        if not re.search(r"#define CTR_CLONE_LOCK_RENEW_INTERVAL_US\s+\(60 \* G_USEC_PER_SEC\)", net):
            out.append("renew worker: 60 s interval (VM clone CMP-7)")
                                                                                               
        dfns = dict(functions(code_only(drv)))
        tr = dfns.get("_clone_task_return", "")
        tseq = [tr.find("PcvLxcCloneReleaseFunc release = ctx->release_cb;"), tr.find("ctx->release_cb = NULL;"),
                tr.find("if (release) release(ctx->release_data);"), tr.find("g_task_return_error(task, err);"),
                tr.find("g_task_return_boolean(task, TRUE);")]
        if not cls._seq_ok(tseq):
            out.append(f"driver: take hook < clear < call hook < return error < return ok {tseq}")
        cw = dfns.get("_clone_worker", "")
        if "g_task_return" in cw or cw.count("_clone_task_return(task, ctx, ") != 4:
            out.append("driver: _clone_worker returns only through _clone_task_return (args, source busy, target busy, result)")
        if not re.search(r"if \(!ctx->source \|\| !ctx->target \|\| !\*ctx->source \|\| !\*ctx->target\) \{\s*"
                         r"_clone_task_return\(task, ctx, g_error_new_literal\(G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,", cw):
            out.append("driver: argument check in the worker returns through the hook")
        ca = dfns.get("pcv_lxc_clone_async", "")
        if ("g_task_return" in ca or "ctx->release_cb = release_cb;" not in ca or "ctx->release_data = release_data;" not in ca
                or "g_task_run_in_thread(task, _clone_worker);" not in ca):
            out.append("driver: pcv_lxc_clone_async stores the hook and always runs the worker")
        if "pcv_lxc_clone_async(source, target, NULL, NULL, NULL, NULL, NULL, NULL);" not in dfns.get("pcv_lxc_clone", ""):
            out.append("driver: pcv_lxc_clone passes no hook")
                                                                                              
                                                                        
        if code_only(net).count("_ctr_clone_release_locks") != 2:
            out.append("handler: release hook referenced only by its definition and the clone_async argument")
        dcode = code_only(drv)
        if re.search(r"\brelease_cb\s*\(", dcode) or dcode.count("release(ctx->release_data)") != 1:
            out.append("driver: release hook called only in _clone_task_return")
        return out

    def test_volume_routes_offloaded(self):
                                                                                                 
        off = code_only((SRC / "dispatcher/rpc_offload.c").read_text())
        for m in ("container.volume.attach", "container.volume.detach"):
            self.assertRegex(off, r'\{ "%s", PCV_OFFLOAD_LANE_CTR_MUTATE \}' % re.escape(m))
        self.assertRegex(off, r"\[PCV_OFFLOAD_LANE_CTR_MUTATE\]\s*=\s*\{ 1, 8, TRUE \}")
        self.assertRegex(off, r'\{ "container.volume.list", PCV_OFFLOAD_LANE_HOST_READ \}')
        hdr = code_only((SRC / "dispatcher/rpc_offload.h").read_text())
        self.assertRegex(hdr, r"PCV_OFFLOAD_LANE_CTR_MUTATE,\s*PCV_OFFLOAD_LANE_COUNT")

    def test_volume_attach_readonly_default_true(self):
        b = body("dispatcher/handler_container_volume.c", "handle_container_volume_attach(JsonObject *params")
        self.assertIn('pcv_rpc_params_get_bool(params, "readonly", TRUE, &readonly)', b)
        self.assertNotIn('json_object_get_boolean_member(params, "readonly")', b)

    def test_running_volume_attach_ro_from_start(self):
                                                                                                   
                                                                                       
                                                                                                             
                                                                                      
        v = body("dispatcher/handler_container_volume.c", "handle_container_volume_attach(JsonObject *params")
        call = re.search(r"pcv_lxc_volume_attach\(PCV_LXC_PATH, name, src_fd, canon, container_path, readonly, running, "
                         r"&run_err\)", v)
        self.assertIsNotNone(call)
        self.assertRegex(v, r'gboolean running = state && g_strcmp0\(state, "RUNNING"\) == 0;')
        ready, state = v.find("pcv_container_config_ready("), v.find("pcv_lxc_get_state(name)")
        self.assertTrue(0 < ready < state < call.start(), (ready, state, call.start()))                                 
        self.assertNotIn("pcv_lxc_volume_mount_running(", v)
        self.assertNotIn("pcv_lxc_volume_config_save(", v)
        at = body("lxc/lxc_volume_attach.c", "pcv_lxc_volume_attach(const gchar *lxc_path, const gchar *name, int src_fd")
        mnt = at.find("pcv_lxc_volume_mount_running(lxc_path, name, src_fd, container_path, readonly, &new_id, error)")
        self.assertTrue(0 < mnt < at.find("pcv_lxc_volume_config_save("), mnt)                                       
                                                                                  
        dc = code_only((SRC / "lxc/lxc_driver.c").read_text())
        self.assertNotIn('g_strdup_printf("%s/%s/rootfs%s"', dc)                               
        tu = code_only((SRC / "lxc/lxc_volume_attach.c").read_text())
        self.assertNotIn('g_strdup_printf("%s/%s/rootfs%s"', tu)
        run = body("lxc/lxc_volume_attach.c", "pcv_lxc_volume_mount_running(const gchar *lxc_path, const gchar *name, int src_fd")
        self.assertNotIn('"mount"', run)                              
        self.assertNotIn('"umount"', run)
        self.assertNotIn('"mkdir"', run)
                                                                                                      
                                                           
        opened = run.find("ops->open_dir(")
        target = run.find("_attach_target_open(ops, rootfd, container_path, S_ISDIR(sst.st_mode), error)")
        attach = run.find("ops->mount_clone(src_fd, target_fd, readonly ? PCV_LXC_MOUNT_RO : 0, &id, error)")
        check = run.find("if (ok && readonly && !_volume_ro_inside(lxc_path, name, container_path)) {")
        self.assertTrue(0 < opened < target < attach < check, (opened, target, attach, check))
        self.assertIn("pcv_lxc_volume_unmount_id(lxc_path, name, id, &e)", run[check:])
        self.assertNotIn("umount2(", run)
        self.assertIn('"read-only bind mount could not be enforced inside the container"', run)
        tgt = body("lxc/lxc_volume_attach.c", "_attach_target_open(const PcvLxcMountOps *ops, int rootfd")
        walk, leaf = tgt.find("ops->walk(rootfd, parent, TRUE, error)"), tgt.find("ops->open_leaf(pfd, slash, &e)")
        self.assertTrue(0 < walk < leaf, (walk, leaf))                                                                       
        self.assertNotIn("pcv_lxc_rootfs_walk_dir(", tgt)                                                  
        self.assertIn("O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC", tgt)                             
        self.assertIn("PCV_LXC_VOLUME_TYPE_MISMATCH_MSG", tgt)
        self.assertRegex(tgt, r"g_error_matches\(e, G_IO_ERROR, G_IO_ERROR_NOT_REGULAR_FILE\)")                                 
        userns = code_only((SRC / "lxc/lxc_userns.c").read_text()) + code_only((SRC / "lxc/lxc_userns.h").read_text())
        self.assertNotIn("pcv_lxc_userns_mount_move", userns)                                                     
                                                                                                                       
                                                                                                              
        d = body("dispatcher/handler_container_volume.c", "handle_container_volume_detach(JsonObject *params")
        lk = body("dispatcher/handler_container_volume.c", "_volume_lock_or_reply(const gchar *name")
        self.assertIn("pcv_vm_lock_acquire(name, VM_OP_TUNING, &lock_err)", lk)                               
        order = [d.find(x) for x in ("_volume_lock_or_reply(name, rpc_id, server, conn)", "pcv_container_config_ready(",
                                     "pcv_lxc_volume_detach_run(", "pcv_lxc_volume_config_drop(")]
        self.assertTrue(0 < order[0] < order[1] < order[2] < order[3], order)
        self.assertRegex(d, r"pcv_lxc_volume_detach_run\(PCV_LXC_PATH, name, container_path, &drop_line, &was_running, &err\)")
        self.assertRegex(d, r"drop_line && !pcv_lxc_volume_config_drop\(PCV_LXC_PATH, name, container_path, was_running, &err\)")
        for gone in ("g_file_set_contents", "pcv_lxc_get_state(", "umount", 'g_strdup_printf("%s/%s/rootfs%s"', "g_mutex_locker_new("):
            self.assertNotIn(gone, d, gone)
        self.assertNotIn("pcv_lxc_volume_umount_running", code_only((SRC / "lxc/lxc_volume_mount.c").read_text()))
        insp = body("lxc/lxc_volume_attach.c", "_volume_ro_inside(const gchar *lxc_path, const gchar *name")
        self.assertIn('"/proc/%d/mountinfo"', insp)
        self.assertRegex(insp, r"gboolean ro = pid > 0 && g_file_get_contents\(path, &text, NULL, NULL\) &&\s*"
                               r"pcv_lxc_mountinfo_is_readonly\(text, container_path, &found\);")
        self.assertRegex(insp, r"\breturn ro;\s*$")

    def test_running_attach_occupied_conflict(self):
                                                                                                      
                                                                            
                                                                                          
        at = body("lxc/lxc_volume_attach.c", "_attach_precheck(const PcvLxcMountOps *ops")
        self.assertRegex(at, r"_attach_refuse_duplicate\(set, mi, src_fd, canon, error\) && _attach_refuse_cover\(set, container_path, error\) &&\s*"
                             r"\(running \? _attach_refuse_occupied\(ops, lxc_path, name, set, mi, container_path, error\)\s*"
                             r": _attach_refuse_nest_missing\(ops, set, container_path, src_fd, error\)\)")
        main = body("lxc/lxc_volume_attach.c", "pcv_lxc_volume_attach(const gchar *lxc_path, const gchar *name, int src_fd")
        pre = main.find("_attach_precheck(pcv_lxc_volume_mount_ops(), lxc_path, name, src_fd, canon, container_path, running, error)")
        self.assertTrue(0 < pre < main.find("pcv_lxc_volume_mount_running("), pre)
        v = body("dispatcher/handler_container_volume.c", "handle_container_volume_attach(JsonObject *params")
        self.assertIn("const gboolean occupied = g_error_matches(run_err, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_CONFLICT);", v)
        self.assertRegex(v, r"dup \? PURE_RPC_ERR_INVALID_PARAMS : occupied \? PURE_RPC_ERR_CONFLICT : PURE_RPC_ERR_INTERNAL_ERROR")

    def test_volume_attach_source_walk(self):
                                                                                                          
                                                                            
        v = body("dispatcher/handler_container_volume.c", "handle_container_volume_attach(JsonObject *params")
        self.assertEqual(v.count("realpath("), 0)
        walk = v.find("pcv_lxc_volume_source_open(host_path, &canon, &run_err)")
        self.assertGreater(walk, 0)
        self.assertLess(v.find("container_path[0] != '/'"), walk)                                                  
        self.assertLess(v.find("!pcv_container_config_token_safe(host_path, FALSE)"), walk)
        for later in ("pcv_container_config_ready(", "pcv_lxc_volume_attach("):
            self.assertLess(walk, v.find(later), later)
        at = body("lxc/lxc_volume_attach.c", "pcv_lxc_volume_attach(const gchar *lxc_path, const gchar *name, int src_fd")
        self.assertRegex(at, r'g_strdup_printf\("%s %s none bind%s 0 0", canon, dest, readonly \? ",ro" : ""\)')          
        self.assertRegex(v, r"if \(src_fd < 0\) \{\s*_send_volume_mount_error\(rpc_id, server, conn, run_err, "
                            r"PURE_RPC_ERR_INVALID_PARAMS,\s*\"host_path does not exist or is invalid\"\);\s*return;\s*\}")
        self.assertEqual(v.count("close(src_fd);"), 3)                                                                    
        h = body("dispatcher/handler_container_volume.c", "_send_volume_mount_error(const gchar *rpc_id")
        self.assertIn('"host_path check needs openat2 (kernel 5.6+)"', h)
        self.assertIn('"stop the container to attach a volume on this kernel"', h)
        g = body("lxc/lxc_volume_guard.c", "pcv_lxc_volume_source_open(const gchar *host_path")
        self.assertIn("RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS", g)
        self.assertIn('open("/", O_PATH | O_DIRECTORY | O_CLOEXEC)', g)
        self.assertNotIn("realpath(", g)
        self.assertNotIn("O_NOFOLLOW", g)                                                                            
        tree = body("lxc/lxc_userns.c", "pcv_lxc_userns_mount_clone(int src_fd")                                        
        self.assertIn('open_tree(src_fd, ""', tree)
        self.assertIn("AT_EMPTY_PATH", tree)
        self.assertNotIn("open_tree(AT_FDCWD", tree)
                                                  
        self.assertLessEqual(len((SRC / "dispatcher/handler_container.c").read_text().splitlines()), 2556)
        self.assertLessEqual(len((SRC / "lxc/lxc_driver.c").read_text().splitlines()), 3198)

    def test_volume_attach_rejects_backslash(self):
                                                                                                           
                                                                                       
                                                                             
        tok = body("dispatcher/handler_container_mode.c", "pcv_container_config_token_safe(const gchar *value, gboolean allow_space)")
        self.assertRegex(tok, r"if \(!allow_space && \(\*p == ' ' \|\| \*p == '\\\\'\)\)\s*return FALSE;")
        v = body("dispatcher/handler_container_volume.c", "handle_container_volume_attach(JsonObject *params")
        self.assertRegex(v, r"if \(!pcv_container_config_token_safe\(container_path, FALSE\) \|\|\s*!pcv_container_config_token_safe\(host_path, FALSE\)\) \{"
                            r"[^{}]*\"host_path/container_path must not contain whitespace, control characters or backslashes\"\);"
                            r"[^{}]*\breturn;\s*\}")
        self.assertLess(v.find("!pcv_container_config_token_safe(host_path, FALSE)"), v.find("pcv_lxc_volume_source_open("))         
        env = body("dispatcher/handler_container.c", "handle_container_env_set(JsonObject *params")
        self.assertIn("!pcv_container_config_token_safe(value, TRUE)", env)

    def test_start_paths_guarded(self):
                                                                                       
                                                                         
                                                                                                           
        s = body("dispatcher/handler_container.c", "handle_container_start(JsonObject *params")
        sub = s.find("pcv_container_start_submit(name, NULL, &why)")
        self.assertTrue(0 < s.find("pcv_validate_vm_name(name)") < sub, sub)                     
        self.assertLess(sub, s.find("pure_rpc_build_success_response("))                            
                                                                                                  
        self.assertRegex(s, r"if \(r == PCV_CTR_START_DENIED\) \{\s*"
                            r"g_autofree gchar \*e = pure_rpc_build_error_response\(rpc_id, PURE_RPC_ERR_CONFLICT, why\);\s*"
                            r"pure_uds_server_send_response\(server, conn, e\);\s*"
                            r'pcv_audit_log\(pcv_rpc_completion_actor\(\), "container\.start", name, "denied", PURE_RPC_ERR_CONFLICT, 0, "local"\);\s*'
                            r"return;\s*\}")
        self.assertRegex(s, r"if \(r == PCV_CTR_START_BUSY\) \{\s*"
                            r"g_autofree gchar \*e = pure_rpc_build_error_response\(rpc_id, PURE_RPC_ERR_BUSY, why\);\s*"
                            r"pure_uds_server_send_response\(server, conn, e\);\s*return;\s*\}")
        g = body("dispatcher/handler_container_mode.c", "pcv_container_volume_start_check(const gchar *name")
        self.assertRegex(g, r"if \(pcv_lxc_volume_guard_check\(PCV_LXC_PATH, name, &bad\)\) \{\s*return TRUE;\s*\}")                                   
        self.assertIn("*msg_out = pcv_lxc_volume_guard_message(bad);", g)
        for gone in ("pure_rpc_build_error_response(", "pure_uds_server_send_response(", "pcv_audit_log("):                 
            self.assertNotIn(gone, g, gone)
        r = body("dispatcher/handler_container_mode.c", "_restart_check(const gchar *name, gboolean before_start)")
        self.assertRegex(r, r"if \(before_start \? pcv_lxc_volume_guard_check_start\(PCV_LXC_PATH, name, &bad\) : "
                            r"pcv_lxc_volume_guard_check\(PCV_LXC_PATH, name, &bad\)\)\s*return TRUE;")                
        self.assertIn("return _restart_check(name, FALSE);",
                      body("dispatcher/handler_container_mode.c", "pcv_container_volume_restart_allowed(const gchar *name)"))
                                                                                            
                                                                                                     
        a = body("dispatcher/handler_container_mode.c", "pcv_container_volume_restart_after_stop(const gchar *name, gint stop_rc)")
        self.assertRegex(a, r'g_autofree gchar \*state = pcv_lxc_get_state\(name\);\s*'
                            r'if \(\(stop_rc == 0 \|\| stop_rc == 2\) && g_strcmp0\(state, "STOPPED"\) == 0\)\s*'
                            r'return _restart_check\(name, TRUE\);')
        self.assertRegex(a, re.compile(r'PCV_LOG_WARN\(.*?\);\s*pcv_audit_log\(PCV_AUDIT_ACTOR_SYSTEM, "container\.start", name, '
                                       r'"denied", PURE_RPC_ERR_CONFLICT, 0, "local"\);\s*return FALSE;\s*$', re.S))
        self.assertIn('pcv_audit_log(PCV_AUDIT_ACTOR_SYSTEM, "container.start", name, "denied", PURE_RPC_ERR_CONFLICT,', r)
        self.assertIn("PCV_LOG_WARN(", r)
                                                                                   
        w = body("lxc/lxc_driver.c", "_lxc_start_thread(GTask")
        chk = w.find("pcv_lxc_volume_guard_check_start(")
        self.assertTrue(0 < w.find('"zfs", "mount"') < chk < w.find("pcv_lxc_start_sync(name, &error)"), chk)
        self.assertRegex(w, r"if \(!pcv_lxc_volume_guard_check_start\(PCV_LXC_PATH, name, &bad\)\) \{\s*"
                            r"bad_msg = pcv_lxc_volume_guard_message\(bad\);\s*"
                            r"g_task_return_new_error\(task, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, \"%s\", bad_msg\);\s*"
                            r"return;\s*\}")
                                                                                                 
                                                                                               
                                                                                              
                                                    
        hp = "dispatcher/handler_container_health.c"
        h = body(hp, "_health_restart(const PcvHealthOps *ops, const gchar *name)")
        calls = ("ops->lock(", "ops->restart_allowed(", "ops->stop(", "ops->renew(", "ops->after_stop(", "ops->start(")
        for c in calls:
            self.assertEqual(h.count(c), 1, c)
        pos = [h.find(c) for c in calls]
        self.assertTrue(0 < pos[0] < pos[1] < pos[2] < pos[3] < pos[4] < pos[5] < h.rfind("ops->unlock("), pos)
        self.assertRegex(h, r"if \(!lock\)\s*return HEALTH_RESTART_BUSY;")                                           
        self.assertRegex(h, r"if \(!ops->restart_allowed\(name\)\) \{\s*ops->unlock\(lock\);\s*return HEALTH_RESTART_DENIED;\s*\}")
        self.assertRegex(h, r"gint stop_rc = ops->stop\(name\);")                                                 
                                                                                               
        self.assertRegex(h, r"if \(ops->after_stop\(name, stop_rc\)\) \{\s*if \(ops->is_terminating\(\)\) \{[^{}]*\} else \{"
                            r"[^{}]*ops->start\(name\);[^{}]*\}\s*\}")
                                                                                                    
                                                 
        hc = code_only((SRC / hp).read_text())
        table = re.search(r"static const PcvHealthOps G_health_real_ops = \{(.*?)\};", hc, re.S)
        self.assertIsNotNone(table)
        table = table.group(1)
        self.assertRegex(table, r"\.restart_allowed\s*=\s*pcv_container_volume_restart_allowed,")
        self.assertRegex(table, r"\.after_stop\s*=\s*pcv_container_volume_restart_after_stop,")
        stop_fn = re.search(r"\.stop\s*=\s*(\w+),", table).group(1)
        start_fn = re.search(r"\.start\s*=\s*(\w+),", table).group(1)
        hstop = body(hp, f"{stop_fn}(const gchar *name)")
        hstart = body(hp, f"{start_fn}(const gchar *name)")
        self.assertIn('"lxc-stop"', hstop)
        self.assertEqual(hstart.count("pcv_lxc_start_sync("), 1)
                                                         
        rs = body("lxc/lxc_driver.c", "pcv_lxc_restore(const gchar *name, const gchar *checkpoint_dir)")
        rchk = rs.find("pcv_lxc_volume_guard_check_start(")
        self.assertTrue(0 < rs.find("if (!name || !checkpoint_dir) return FALSE;") < rchk < rs.find('"lxc-checkpoint"'), rchk)
        self.assertRegex(rs, r"if \(!pcv_lxc_volume_guard_check_start\(PCV_LXC_PATH, name, &bad\)\) \{\s*PCV_LOG_WARN\([^;]*;\s*"
                             r"return FALSE;\s*\}")
                                                                                            
                                                                                                        
                                                                                          
        sr = body("lxc/lxc_rollback.c", "_snap_rollback_thread(GTask")
        self.assertRegex(sr, r"else if \(!fail && !ops->forget\(PCV_LXC_PATH, d->name\)\)[^\n]*\n\s*"
                             r'fail = g_strdup\("snapshot rolled back, but the volume fingerprint could not be cleared: "\s*'
                             r'"stop the container and start it with container\.start"\);')
        self.assertRegex(sr, r'if \(fail\)\s*g_task_return_new_error\(task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", fail\);\s*'
                             r"else\s+g_task_return_boolean\(task, TRUE\);")
        self.assertRegex(code_only((SRC / "lxc/lxc_rollback.c").read_text()),
                         r"static const PcvLxcRollbackOps G_rb_real_ops = \{[^}]*\.forget\s*= pcv_lxc_volume_guard_forget,\s*\};")
                                                                                                        
                                                                                 
                                                                                                           
                                                                                          
                                                                            
        root = SRC.parent
        codes = {p.relative_to(root).as_posix(): code_only(p.read_text()) for p in sorted(root.rglob("*.[ch]"))}
        starts = {k: v.count('"lxc-start"') for k, v in codes.items() if '"lxc-start"' in v}
        self.assertEqual(starts, {"utils/pcv_spawn.c": 1}, starts)
        self.assertEqual(w.count('"lxc-start"'), 0)
        self.assertEqual(w.count("pcv_lxc_start_sync("), 1)
        self.assertEqual(hstart.count('"lxc-start"'), 0)
        syncs = {k: v.count("pcv_lxc_start_sync(") for k, v in codes.items() if k.endswith(".c") and "pcv_lxc_start_sync(" in v}
        self.assertEqual(syncs, {"modules/lxc/lxc_driver.c": 2,                                                    
                                 "modules/dispatcher/handler_container_health.c": 1}, syncs)
        ss = body("lxc/lxc_driver.c", "pcv_lxc_start_sync(const gchar *name, GError **error)")
        unit_calls = {k: v.count("pcv_lxc_unit_start(") for k, v in codes.items()
                      if "pcv_lxc_unit_start(" in v and not k.startswith("modules/lxc/lxc_unit.")}
        self.assertEqual(unit_calls, {"modules/lxc/lxc_driver.c": 1}, unit_calls)
        self.assertEqual(ss.count("pcv_lxc_unit_start("), 1)
        self.assertTrue(0 < ss.find("is_running(") < ss.find("pcv_lxc_unit_start("), ss[:200])                             
        scope = {k: v.count('"systemctl"') for k, v in codes.items()
                 if (k.startswith("modules/lxc/") or k.startswith("modules/dispatcher/handler_container"))
                 and '"systemctl"' in v}
        self.assertEqual(set(scope), {"modules/lxc/lxc_unit.c"}, scope)
        argv_fn = body("lxc/lxc_unit.c", "_unit_argv(const gchar *verb")
        self.assertEqual(argv_fn.count('"systemctl"'), scope["modules/lxc/lxc_unit.c"])
        names = body("../utils/pcv_spawn.c", "pcv_spawn_capability_profile_for_argv(const gchar * const *argv)")
        self.assertRegex(names, r'static const gchar \* const runtime_names\[\] = \{[^}]*"lxc-start"')
        for k, v in codes.items():                                                                         
            v = v.replace("ops->start(", "", 1) if k == "modules/dispatcher/handler_container_health.c" else v
            self.assertIsNone(re.search(r"->\s*(?:start|restore)\s*\(", v), k)
        restores = [(k, m.group(0)) for k, v in codes.items()
                    for m in re.finditer(r'\{[^{};]*"lxc-checkpoint"[^{};]*\}', v) if '"-r"' in m.group(0)]
        self.assertEqual(len(restores), 1, restores)
        self.assertIn(restores[0][1], rs)
                                                  
        self.assertLessEqual(len((SRC / "dispatcher/handler_container.c").read_text().splitlines()), 2556)
        self.assertLessEqual(len((SRC / "lxc/lxc_driver.c").read_text().splitlines()), 3198)

    def test_start_submit_single_stage(self):
                                                                                                             
                                                                                                     
                                                                              
        s = body("dispatcher/handler_container.c", "handle_container_start(JsonObject *params")
        self.assertEqual(s.count("pcv_container_start_submit("), 1)
        for direct in ("pcv_vm_lock_acquire(", "pcv_lxc_start_async(", "pcv_container_volume_start_check("):
            self.assertNotIn(direct, s, direct)
        sb = body("dispatcher/handler_container.c", "pcv_container_start_submit(const gchar *name, const gchar *actor, gchar **msg_out)")
        chk, lock, start = (sb.find("pcv_container_volume_start_check("), sb.find("pcv_vm_lock_acquire(name, VM_OP_STARTING"),
                            sb.find("pcv_lxc_start_async("))
        self.assertTrue(0 < chk < lock < start, (chk, lock, start))
        self.assertIn("pcv_lxc_start_async(name, actor, NULL, _on_start_done, ctx);", sb)
        self.assertIn("ctx->lock = op_lock;", sb[lock:start])                                                             
        a = body("lxc/lxc_driver.c", "pcv_lxc_start_async(const gchar        *name")
        self.assertRegex(a, r"GTask \*task = pcv_drain_task_new\(NULL, cancellable, callback, user_data\);\s*"
                            r"if \(actor\)\s*pcv_drain_task_set_actor\(task, actor\);")

    def test_container_lock_conflict_busy_code(self):
                                                                                                     
                                                                                                    
        for anchor in ("handle_container_create(JsonObject *params",
                       "handle_container_destroy(JsonObject *params",
                       "handle_container_stop(JsonObject *params"):
            b = body("dispatcher/handler_container.c", anchor)
            self.assertRegex(b, r"PURE_RPC_ERR_BUSY,\s*lock_err \? lock_err : \"Container is busy\"", anchor)
            self.assertNotIn("PURE_RPC_ERR_INTERNAL_ERROR,\n                       lock_err", b, anchor)
                                                    
        s = body("dispatcher/handler_container.c", "handle_container_start(JsonObject *params")
        self.assertRegex(s, r"if \(r == PCV_CTR_START_BUSY\) \{\s*"
                            r"g_autofree gchar \*e = pure_rpc_build_error_response\(rpc_id, PURE_RPC_ERR_BUSY, why\);")
        rr = body("dispatcher/handler_container.c", "pcv_container_start_report_rejected(const gchar *name")
        self.assertIn("denied ? PURE_RPC_ERR_CONFLICT : PURE_RPC_ERR_BUSY", rr)
                                                                 
        w = body("dispatcher/handler_container_net.c", "_cnet_worker(GTask *task")
        self.assertIn("ctx->code = PURE_RPC_ERR_BUSY;", w)
        self.assertNotIn("ctx->code = PURE_RPC_ERR_INTERNAL_ERROR;", w)

    def test_rollback_handler_order(self):
                                                                                                                  
                                                                                                
                                                                                                 
                                                                                                                      
        hc = (SRC / "dispatcher/handler_container.c").read_text()
        self.assertEqual(self._rollback_violations(hc), [])
        release = "    g_clear_pointer(&ctx->lock, pcv_vm_lock_release);"
        c0 = hc.index("\n_on_snap_rollback_done(GObject")
        c1 = hc.index("\n}\n", c0)
        cb = hc[c0:c1]
        mutants = {                           
            "잠금 없이 워커": hc.replace("pcv_vm_lock_acquire(name, VM_OP_SNAPSHOT", "pcv_vm_lock_acquire(name, VM_OP_TUNING", 1),
            "잠금 실패가 −32603": hc.replace("PURE_RPC_ERR_BUSY, lock_err", "PURE_RPC_ERR_INTERNAL_ERROR, lock_err", 1),
            "잠금을 쥔 채 재시작": hc[:c0] + cb.replace(release + "\n", "", 1).replace("    _ctx_free(ctx);", release + "\n    _ctx_free(ctx);", 1)
                              + hc[c1:],
            "행위자 누락": hc.replace("pcv_container_start_submit(ctx->name, pcv_drain_task_actor(res), &why)",
                                 "pcv_container_start_submit(ctx->name, NULL, &why)", 1),
            "거절 보고 없음": hc.replace("pcv_container_start_report_rejected(ctx->name, r, why, pcv_drain_task_actor(res));", "(void)r;", 1),
            "재시작 꼬리 없음": hc.replace(" (container restart requested; see container.start result)", "", 1),
            "거절 감사가 늘 fail": hc.replace('denied ? "denied" : "fail"', '"fail"', 1),
        }
        for what, mutated in mutants.items():
            self.assertNotEqual(self._rollback_violations(mutated), [], what)

    @staticmethod
    def _rollback_violations(hc: str):
        raw_fns = dict(functions(hc))
        fns = dict(functions(code_only(hc)))
        out = []
        h = fns.get("handle_container_snapshot_rollback", "")
        hseq = [h.find("pcv_validate_snap_name("), h.find("pcv_vm_lock_acquire(name, VM_OP_SNAPSHOT, &lock_err)"),
                h.find("PURE_RPC_ERR_BUSY, lock_err"), h.find("ctx->lock          = lock;"),
                h.find("pcv_lxc_snapshot_rollback_async(name, snap_name, NULL, _on_snap_rollback_done, ctx)")]
        if not (0 < hseq[0] and all(a < b for a, b in zip(hseq, hseq[1:]))):
            out.append(f"handler: validate < VM_OP_SNAPSHOT < BUSY reply < ctx->lock < rollback_async {hseq}")
        cb = fns.get("_on_snap_rollback_done", "")
        cseq = [cb.find("pcv_lxc_snapshot_rollback_finish(res, &restart, &error)"), cb.find("pcv_audit_log("), cb.find("_send_"),
                cb.find("g_clear_pointer(&ctx->lock, pcv_vm_lock_release)"), cb.find("if (restart)"),
                cb.find("pcv_container_start_submit(ctx->name, pcv_drain_task_actor(res), &why)"),
                cb.find("if (r != PCV_CTR_START_SUBMITTED)"),
                cb.find("pcv_container_start_report_rejected(ctx->name, r, why, pcv_drain_task_actor(res))"), cb.find("_ctx_free(ctx)")]
        if not (0 < cseq[0] and all(a < b for a, b in zip(cseq, cseq[1:]))):
            out.append(f"callback: finish < audit < send < release < if restart < submit(actor) < report_rejected < free {cseq}")
        if cb.count("pcv_container_start_submit(") != 1 or cb.count("g_clear_pointer(&ctx->lock") != 1:
            out.append("callback: one submit, one lock release")
        rcb = raw_fns.get("_on_snap_rollback_done", "")
        if not re.search(r'restart \? " \(container restart requested; see container\.start result\)" : ""', rcb):
            out.append("callback: failure reply ends with the restart tail when restart")
        rej = raw_fns.get("pcv_container_start_report_rejected", "")
        for need in ('pcv_audit_log(actor, "container.start", name, denied ? "denied" : "fail",',
                     "denied ? PURE_RPC_ERR_CONFLICT : PURE_RPC_ERR_BUSY",
                     'pcv_ws_broadcast_job_complete(job_id, "container.start", "failed", reason)',
                     'g_strdup_printf("container.start:%s", name)', "const gboolean denied = why == PCV_CTR_START_DENIED;"):
            if need not in rej:
                out.append(f"report_rejected: {need}")
        return out

    def test_volume_guard_hook_wiring(self):
                                                                                                    
                                                                            
        root = SRC.parent
        hdr = code_only((SRC / "lxc/lxc_volume_guard.h").read_text())
        self.assertIn('#define PCV_LXC_VOLUME_GUARD_HOOK "/usr/local/bin/pcvctl"', hdr)
        self.assertIn('#define PCV_LXC_VOLUME_GUARD_HOOK_KEY   "lxc.hook.pre-start"', hdr)
        self.assertIn('#define PCV_LXC_VOLUME_GUARD_HOOK_VALUE PCV_LXC_VOLUME_GUARD_HOOK " container volume-guard-hook"', hdr)
                                                    
        codes = {q.relative_to(root).as_posix(): code_only(q.read_text()) for q in sorted(root.rglob("*.[ch]"))}
        for lit in ('"/usr/local/bin/pcvctl', ' container volume-guard-hook"'):
            where = {k: v.count(lit) for k, v in codes.items() if lit in v}
            self.assertEqual(where, {"modules/lxc/lxc_volume_guard.h": 1}, (lit, where))
                                                                                      
        cli = (root / "cli/purecvisorctl.c").read_text()
        self.assertRegex(cli, r'\{"container","volume-guard-hook", cmd_container_volume_guard_hook, "[^"]+"\},')
        self.assertIn('#include "cli_container_guard.h"', cli)
        hook_cmd = body("../cli/cli_container_guard.c", "cmd_container_volume_guard_hook(int argc")
        self.assertRegex(hook_cmd, r'if \(pcv_lxc_volume_guard_hook_main\(g_getenv\("LXC_CONFIG_FILE"\), g_getenv\("LXC_NAME"\)\) == 0\)\s*'
                                   r'pcv_cli_command_mark_success\(\);\s*else\s*pcv_cli_command_mark_runtime_failure\(\);')
        self.assertNotIn("purectl_send_request(", hook_cmd)                                              
                                                                             
        vg = code_only((SRC / "lxc/lxc_volume_guard.c").read_text())
        self.assertRegex(body("lxc/lxc_volume_guard.c", "pcv_lxc_volume_guard_check(const gchar *lxc_path"),
                         r"gboolean ok = _guard_text\(text, bad_container_path_out\);")
                                                                                         
        self.assertIn("return pcv_lxc_volume_guard_hook_run_with_root(PCV_LXC_VOLSTAGE_ROOT, config_path, lxc_name);",
                      body("lxc/lxc_volume_guard.c", "pcv_lxc_volume_guard_hook_main(const gchar *config_path, const gchar *lxc_name)"))
        hm = body("lxc/lxc_volume_guard.c", "pcv_lxc_volume_guard_hook_run_with_root(const gchar *volstage_root")
        self.assertIn("gboolean ok = _guard_text(text, &bad);", hm)
        self.assertRegex(hm, r"if \(!config_path \|\| !\*config_path \|\| !g_file_get_contents\(config_path, &text, NULL, NULL\)\) \{"
                             r"[^{}]*\breturn 1;\s*\}")                                                                
        self.assertIn("return ok ? 0 : 1;", hm)
        printed = re.findall(r"g_printerr\(([^;]*)\);", hm)                                                    
        self.assertEqual(printed, ['"pcv volume guard: cannot read the container config (LXC_CONFIG_FILE)\\n"',
                                   '"pcv volume guard: cannot read the container name (LXC_NAME)\\n"',
                                   '"pcv volume guard: bind mount with a relative source is not allowed: %s\\n", relative_dest',
                                   '"pcv volume guard: volume source is outside the purecvisor staging area — start it through purecvisor\\n"',
                                   '"pcv volume guard: %s\\n", msg'], printed)
        self.assertIn("gchar *msg = pcv_lxc_volume_guard_message(bad);", hm)
        self.assertEqual(vg.count("pcv_lxc_volume_source_open(e->source"), 1)
        self.assertIn("pcv_lxc_volume_source_open(e->source", body("lxc/lxc_volume_guard.c", "_guard_text(const gchar *text"))
                                                                                                              
        order = [hm.find(x) for x in ("g_file_get_contents(config_path, &text", "_lxc_name_ok(lxc_name)", "g_str_has_prefix(e->source, prefix)",
                                      "_guard_text(text, &bad)")]
        self.assertTrue(0 < order[0] < order[1] < order[2] < order[3], order)
        self.assertIn('gchar *prefix = g_strconcat(volstage_root, "/", lxc_name, "/", NULL);', hm)
        self.assertIn("pcv_lxc_mount_entry_is_volume(e)", hm)
        self.assertNotIn("fingerprint(", vg.replace("could not be reset", ""))
        self.assertNotIn("_volgen_write", vg)
        self.assertNotIn("bind_fingerprint", hdr)
                                                                     
        self.assertIn('#define PCV_LXC_VOLUME_GUARD_VOLGEN "purecvisor.volgen"', hdr)
        where = {k: v.count('"purecvisor.volgen"') for k, v in codes.items() if '"purecvisor.volgen"' in v}
        self.assertEqual(where, {"modules/lxc/lxc_volume_guard.h": 1}, where)
        cs = body("lxc/lxc_volume_guard.c", "pcv_lxc_volume_guard_check_start(const gchar *lxc_path")
        self.assertTrue(0 < cs.find("_volgen_clear(lxc_path, name)")
                        < cs.find("pcv_lxc_volume_guard_check(lxc_path, name, bad_container_path_out)"), cs)
        self.assertRegex(cs, r"if \(lxc_path && name && !_volgen_clear\(lxc_path, name\)\)[^;]*\s*return _volgen_refuse\(name, bad_container_path_out\);")
        vc = body("lxc/lxc_volume_guard.c", "_volgen_clear(const gchar *lxc_path, const gchar *name)")
        self.assertIn("ok = unlink(path) == 0 || errno == ENOENT;", vc)
        self.assertIn("else if (!S_ISDIR(st.st_mode) || st.st_uid != geteuid())\n        ok = FALSE;", vc)
        self.assertIn('#define VOLGEN_RESET_FAILED "(volume guard fingerprint could not be reset)"', vg)
        gate = body("dispatcher/handler_container_mode.c", "pcv_container_volume_start_check(const gchar *name")
        self.assertNotIn("check_start", gate)
        stamps = {k: v.count("pcv_lxc_volume_guard_check_start(") for k, v in codes.items() if "pcv_lxc_volume_guard_check_start(" in v}
        self.assertEqual(stamps, {"modules/lxc/lxc_volume_guard.c": 1, "modules/lxc/lxc_volume_guard.h": 1,
                                  "modules/lxc/lxc_driver.c": 2, "modules/dispatcher/handler_container_mode.c": 1}, stamps)
                                                 
        mk = (root.parent / "Makefile").read_text()
        cli_srcs = mk[mk.index("\nCLI_SRCS = "):]
        cli_srcs = cli_srcs[:cli_srcs.index("\n\n")]
        self.assertIn("src/cli/cli_container_guard.c", cli_srcs)
        self.assertIn("src/modules/lxc/lxc_volume_guard.c", cli_srcs)
        self.assertNotIn("lxc_userns.c", cli_srcs)
        self.assertNotIn("pcv_lxc_userns_", vg)                                                                       
                                                                                             
        v = body("dispatcher/handler_container_volume.c", "handle_container_volume_attach(JsonObject *params")
        ready = v.find("pcv_container_volume_hook_ready(rpc_id, server, conn)")
        self.assertTrue(0 < v.find("pcv_lxc_volume_source_open(") < ready < v.find("pcv_container_config_ready(")
                        < v.find("pcv_lxc_volume_attach("), ready)
        self.assertIn("if (!pcv_container_volume_hook_ready(rpc_id, server, conn)) { close(src_fd); return; }", v)
        at = body("lxc/lxc_volume_attach.c", "pcv_lxc_volume_attach(const gchar *lxc_path, const gchar *name, int src_fd")
        self.assertIn("const gboolean saved = pcv_lxc_volume_config_save(lxc_path, name, entry, &e);", at)
        self.assertNotIn("fopen(", v)                                                                        
        self.assertNotIn("fopen(", at)
        hr = body("dispatcher/handler_container_mode.c", "pcv_container_volume_hook_ready(const gchar *rpc_id")
        self.assertIn("if (pcv_lxc_volume_guard_hook_installed(PCV_LXC_VOLUME_GUARD_HOOK))\n        return TRUE;", hr)
        self.assertIn('pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_CONFLICT,', hr)
        self.assertIn('"volume guard hook not installed: " PCV_LXC_VOLUME_GUARD_HOOK', hr)
        sv = body("lxc/lxc_volume_mount.c", "pcv_lxc_volume_config_save(const gchar *lxc_path")
        self.assertIn("pcv_lxc_userns_config_rewrite(lxc_path, name, _volume_config_rewrite, mount_value, error)", sv)
        rw = body("lxc/lxc_volume_mount.c", "_volume_config_rewrite(const gchar *text")
        self.assertIn('{ "lxc.mount.entry", data, TRUE }', rw)
        self.assertIn("pcv_lxc_config_with_guard_hook(with_entry ? with_entry : text)", rw)
                                                                         
        en = body("lxc/lxc_volume_mount.c", "pcv_lxc_volume_hook_ensure(const gchar *lxc_path, const gchar *name)")
        self.assertRegex(en, r"if \(!g_file_get_contents\(path, &text, NULL, NULL\) \|\| !\(hooked = pcv_lxc_config_with_guard_hook\(text\)\)\)"
                             r"\s*return;")
        inst = en.find("pcv_lxc_volume_guard_hook_installed(PCV_LXC_VOLUME_GUARD_HOOK)")
        self.assertTrue(0 < inst < en.find("PCV_LOG_WARN(") < en.find("pcv_lxc_volume_config_save(lxc_path, name, NULL, &err)"), inst)
        self.assertNotIn("return FALSE", en)
        self.assertNotIn("pure_rpc_build_error_response(", en)
                                                                  
        ed = body("lxc/lxc_userns.c", "pcv_lxc_userns_config_edit(const gchar *lxc_path")
        self.assertIn("pcv_lxc_userns_config_rewrite(lxc_path, name, _apply_edits_rewrite, &e, error)", ed)
        rr = body("lxc/lxc_userns.c", "pcv_lxc_userns_config_rewrite(const gchar *lxc_path")
        self.assertIn("!_owned_plain(&st, S_IFDIR)", rr)
        self.assertIn("G_FILE_SET_CONTENTS_CONSISTENT, st.st_mode & 0777", rr)
                                       
        self.assertLessEqual(len((SRC / "dispatcher/handler_container.c").read_text().splitlines()), 2556)
        self.assertLessEqual(len((SRC / "lxc/lxc_driver.c").read_text().splitlines()), 3198)
        self.assertLessEqual(len((root / "cli/purecvisorctl.c").read_text().splitlines()), 9149)

    def test_list_serializes_privileged(self):
                                                                                            
        b = body("dispatcher/handler_container.c",
                 "pcv_container_list_compute(JsonObject *params, PcvPollGuard *guard, GError **error)")
        self.assertIn("pcv_container_info_json(", b)
        self.assertRegex(body("lxc/lxc_driver.c", "pcv_lxc_list_guarded(PcvPollGuard *g, GError **error)"),
                         r"info->privileged\s*=\s*_lxc_handle_privileged\(c\);")

    def test_guard_branches_keep_their_shape(self):
                                                                                          
                                                               
        b = body("dispatcher/handler_container.c", "handle_container_create(JsonObject *params")
        self.assertRegex(b, r"if \(!pcv_container_create_mode_check\(params, conn, NULL, PCV_LXC_SUBUID_PATH, "
                            r"PCV_LXC_SUBGID_PATH, &mode\)\) \{[^{}]*\breturn;\s*\}")
        self.assertRegex(b, r"ctx->rootless\s*=\s*mode\.rootless;")
        self.assertRegex(b, r"pcv_lxc_create_async_full\([^;]*ctx->bridge,\s*mode\.rootless,")
        n = body("dispatcher/handler_container_net.c", "void handle_container_clone(JsonObject *params")
        self.assertRegex(n, r"if \(pcv_disp_dispatcher_caller_role\(params, connection\) < PCV_ROLE_ADMIN && "
                            r"pcv_lxc_container_privileged\(src\)\) \{[^{}]*\breturn;\s*\}")
        v = body("dispatcher/handler_container_volume.c", "handle_container_volume_attach(JsonObject *params")
        self.assertRegex(v, r"if \(!pcv_rpc_params_get_bool\(params, \"readonly\", TRUE, &readonly\)\) \{[^{}]*\breturn;\s*\}")
        d = body("lxc/lxc_driver.c", "pcv_lxc_container_privileged(const gchar *name)")
        self.assertIn("!c || !c->is_defined(c) || _lxc_handle_privileged(c)", d)                          

                                                                                              
    CONFIG_WRITERS = {"_lxc_create_thread", "_set_limits_config", "pcv_lxc_nic_attach", "pcv_lxc_nic_detach",
                      "pcv_lxc_volume_attach", "pcv_lxc_volume_config_drop",
                      "handle_container_env_set", "handle_container_env_delete", "pcv_lxc_volume_hook_ensure",
                      "_clone_real_rewrite_config"}

    def test_config_writers_hold_work_lock(self):
                                                                                                         
                                                                                          
                                                   
        direct = r"pcv_vm_lock_acquire\([^;]*?\bVM_OP_%s\b"
        volume = r"_volume_lock_or_reply\("
                                                                                                               
                                                                                                       
        table = {
            "_clone_real_rewrite_config": ("dispatcher/handler_container_net.c", "void handle_container_clone(JsonObject", "CREATING", direct, "pcv_lxc_clone_async("),
            "_lxc_create_thread": ("dispatcher/handler_container.c", "handle_container_create(JsonObject", "CREATING", direct, "pcv_lxc_create_async_full("),
            "_set_limits_config": ("dispatcher/handler_container_net.c", "void handle_container_set_limits(JsonObject", "TUNING", direct, "pcv_lxc_set_resource_limits("),
            "pcv_lxc_nic_attach": ("dispatcher/handler_container_net.c", "_cnet_worker(GTask *task", "TUNING", direct, "switch (ctx->kind)"),
            "pcv_lxc_nic_detach": ("dispatcher/handler_container_net.c", "_cnet_worker(GTask *task", "TUNING", direct, "switch (ctx->kind)"),
            "pcv_lxc_volume_attach": ("dispatcher/handler_container_volume.c", "handle_container_volume_attach(JsonObject", "TUNING", volume, "pcv_lxc_volume_attach("),
            "pcv_lxc_volume_config_drop": ("dispatcher/handler_container_volume.c", "handle_container_volume_detach(JsonObject", "TUNING", volume, "pcv_lxc_volume_config_drop("),
            "handle_container_env_set": ("dispatcher/handler_container.c", "handle_container_env_set(JsonObject", "TUNING", direct, "_ensure_container_config_ready("),
            "handle_container_env_delete": ("dispatcher/handler_container.c", "handle_container_env_delete(JsonObject", "TUNING", direct, "_ensure_container_config_ready("),
            "pcv_lxc_volume_hook_ensure": ("dispatcher/handler_container.c", "pcv_container_start_submit(const gchar *name, const gchar *actor", "STARTING", direct, "pcv_lxc_start_async("),
        }
        self.assertEqual(set(table), self.CONFIG_WRITERS)
        for writer, (rel, head, op, acquire, call) in table.items():
            with self.subTest(writer=writer):
                b = body(rel, head)
                m = re.search(acquire % op if "%s" in acquire else acquire, b)
                self.assertIsNotNone(m, (writer, "no work lock in entry function"))
                at = b.find(call)
                self.assertGreater(at, 0, (writer, call))
                self.assertLess(m.start(), at, (writer, "work lock after the write path"))
        helper = body("dispatcher/handler_container_volume.c", "_volume_lock_or_reply(const gchar *name")
        self.assertIn("pcv_vm_lock_acquire(name, VM_OP_TUNING, &lock_err)", helper)
                                                                                         
        for rel, head in (("dispatcher/handler_container.c", "handle_container_env_set(JsonObject"),
                          ("dispatcher/handler_container.c", "handle_container_env_delete(JsonObject"),
                          ("dispatcher/handler_container_net.c", "void handle_container_set_limits(JsonObject")):
            b = body(rel, head)
            valid = b.find("pcv_validate_vm_name(")
            lock = b.find("pcv_vm_lock_acquire(")
            self.assertTrue(0 < valid < lock, (head, valid, lock))
            self.assertRegex(b[lock:], r"PURE_RPC_ERR_BUSY")                                      
            self.assertIn("pcv_vm_lock_release", b[lock:], head)

    def test_set_limits_validates_name(self):
                                                                                                                           
                                                                                          
                            
        b = body("dispatcher/handler_container_net.c", "void handle_container_set_limits(JsonObject")
        valid = b.find("pcv_validate_vm_name(ctr_name)")
        self.assertGreater(valid, 0)
        self.assertGreater(valid, b.find("Missing required parameter: name"))                        
        self.assertLess(valid, b.find("pcv_vm_lock_acquire("))
        self.assertLess(valid, b.find("pcv_lxc_"))
        self.assertRegex(b[valid:valid + 400], r"PURE_RPC_ERR_INVALID_PARAMS,\s*\"Invalid container name\"")

    def test_no_seccomp_config_writer(self):
                                                                                                                             
                                                                                        
        for path in sorted(SRC.parent.rglob("*.[ch]")):
            self.assertNotIn("pcv_lxc_set_seccomp_profile", code_only(path.read_text()), path.name)
        self.assertNotIn("pcv_lxc_set_seccomp_profile", self.CONFIG_WRITERS)
        self.assertNotIn("lxc.seccomp.profile =", code_only((SRC / "lxc/lxc_driver.c").read_text()))

    def test_config_writes_hold_config_lock(self):
                                                                                                                 
                                                                                                 
                                                                                              
        writes = re.compile(r"save_config\(|pcv_lxc_userns_config_edit\(|pcv_lxc_userns_config_rewrite\(|"
                            r"pcv_lxc_volume_config_save\(|g_file_set_contents\(config_path")
        reads = re.compile(r"g_file_get_contents\(|lxc_container_new\(|\b_lxc_get\(")                                       
        last_writes = re.compile(writes.pattern.replace("g_file_set_contents\\(config_path", "g_file_set_contents\\("))
        lock, unlock = "g_mutex_locker_new(pcv_lxc_config_mutex())", "g_clear_pointer(&cfg_lock, g_mutex_locker_free)"
        helpers = {"pcv_lxc_userns_config_rewrite", "pcv_lxc_userns_config_edit", "pcv_lxc_volume_config_save"}
        files = sorted((SRC / "lxc").glob("*.c")) + sorted((SRC / "dispatcher").glob("handler_container*.c"))
        sites, helper_bodies = {}, {}
        for path in files:
            for name, b in functions(code_only(path.read_text())):
                if name in helpers:
                    helper_bodies[name] = b
                elif writes.search(b):
                    sites[name] = b
        self.assertEqual(set(sites), self.CONFIG_WRITERS)
        for name, b in sites.items():
            at = b.find(lock)
            self.assertGreater(at, 0, name)
            self.assertEqual(b.count(lock), 1, name)
            first_write = writes.search(b).start()
            first_read = reads.search(b)
            ensure_m = re.search(r"(?:_ensure_|pcv_)container_config_ready\(", b)                                                 
            ensure = ensure_m.start() if ensure_m else -1
            if ensure >= 0:
                self.assertLess(ensure, at, name)                                                                  
            if first_read:
                self.assertTrue(at < first_read.start() < first_write, (name, at, first_read.start(), first_write))
            self.assertLess(at, first_write, name)
                                                                                          
            last_write = max(m.start() for m in last_writes.finditer(b))
            for u in re.finditer(re.escape(unlock), b):
                self.assertGreater(u.start(), last_write, (name, "unlock before last write"))
            if name.startswith("handle_container_"):                                              
                                                                                  
                                                                         
                unlocks = [u.start() for u in re.finditer(re.escape(unlock), b)]
                self.assertTrue(unlocks, name)
                for send in re.finditer(r"pure_uds_server_send_response\(|_send_volume_mount_error\(", b[at:]):
                    s_at = at + send.start()
                    ok = False
                    for u in unlocks:
                        if u >= s_at:
                            continue
                        depth, same_path = 0, True
                        for ch in b[u:s_at]:
                            depth += (ch == "{") - (ch == "}")
                            if depth < 0:
                                same_path = False
                                break
                        ok = ok or same_path
                    self.assertTrue(ok, (name, "response sent while lock may be held", s_at))
        create = sites["_lxc_create_thread"]                                                                
        free = create.find(unlock)
        self.assertGreater(free, create.rfind("save_config("))
        for m in re.finditer(r"_create_rollback\(", create):
            self.assertTrue(m.start() < create.find(lock) or m.start() > free, m.start())
        for name, live in (("pcv_lxc_nic_attach", "pcv_lxc_ns_nic_wire("), ("pcv_lxc_nic_detach", "pcv_lxc_ns_nic_remove(")):
            b = sites[name]                                                                                      
            free = b.find("g_mutex_locker_free")
            last_write = max(m.start() for m in writes.finditer(b))
            self.assertTrue(b.find(lock) < last_write < free < b.find("c->is_running(c)") < b.find(live), name)
        self.assertEqual(set(helper_bodies), helpers)
        for name, b in helper_bodies.items():
            self.assertNotIn("pcv_lxc_config_mutex", b, name)                                              
        defs, uses, lockers = [], 0, 0
        for q in sorted(SRC.rglob("*.c")):
            code = code_only(q.read_text())
            defs += [q.relative_to(SRC).as_posix() for n, _ in functions(code) if n == "pcv_lxc_config_mutex"]
            uses += code.count("pcv_lxc_config_mutex()")
            lockers += code.count(lock)
        self.assertEqual(defs, ["lxc/lxc_userns.c"])
        self.assertEqual(uses, lockers)                                                                             
        self.assertIn("pcv_validate_mac(", body("dispatcher/handler_container_net.c", "void handle_container_nic_attach(JsonObject"))

    def test_daemon_config_reports_new_default(self):
        b = body("dispatcher/handler_daemon.c", "void handle_daemon_config_get(JsonObject *params")
        self.assertIn('pcv_config_get_string("container", "rootless", PCV_LXC_ROOTLESS_DEFAULT)', b)

    def test_cli_surface(self):
        cli = (SRC.parents[0] / "cli/purecvisorctl.c").read_text()
        create = cli[cli.index("void cmd_container_create("):]
        create = create[:create.index("\n}\n")]
        self.assertIn('"--privileged"', create)
        self.assertIn('json_object_set_boolean_member(params,"rootless",     FALSE)', create)
        listing = cli[cli.index("void cmd_container_list("):]
        listing = listing[:listing.index("\n}\n")]
        self.assertIn('"PRIV"', listing)
        self.assertEqual(listing.count("_ctr_priv_label(obj)"), 2)                          
        attach = cli[cli.index("static void cmd_container_volume_attach("):]
        attach = attach[:attach.index("\n}\n")]
        self.assertIn('"--rw"', attach)
        self.assertIn('json_object_set_boolean_member(p, "readonly", FALSE)', attach)

    def test_residue_touchpoints(self):
                                                                                            
                                                                                                                   
        text = (SRC / "lxc/lxc_driver.c").read_text()
        vm = (SRC / "lxc/lxc_volume_mount.c").read_text()
        self.assertEqual(self._touchpoint_violations(text, vm), [])
        call = "pcv_lxc_rootfs_residue_clear("
        s0 = text.index("_lxc_stop_thread(GTask"); s1 = text.index("\n}\n", s0)
        mutants = {                                     
            "정지 접점 삭제": (text[:s0] + text[s0:s1].replace(call, "(void)", 1) + text[s1:], vm),
        }
        d0 = text.index("_lxc_destroy_thread(GTask"); d1 = text.index("\n}\n", d0)
        mutants["삭제 접점 삭제"] = (text[:d0] + text[d0:d1].replace(call, "(void)", 1) + text[d1:], vm)
        t0 = vm.index("\npcv_lxc_volume_start_prepare(")
        t1 = vm.index("\n}\n", t0)
        mutants["시작 접점 삭제"] = (text, vm[:t0] + vm[t0:t1].replace(call, "(void)", 1) + vm[t1:])
        mutants["시작 staging 진입 삭제"] = (text.replace("if (!pcv_lxc_volume_start_prepare(lxc_path, name, error))",
                                                       "if (FALSE)", 1), vm)
        for what, (mutated, mvm) in mutants.items():
            self.assertNotEqual(self._touchpoint_violations(mutated, mvm), [], what)

    @staticmethod
    def _touchpoint_violations(text: str, vm: str):
        code = code_only(text)
        fns = dict(functions(code))
        call = "pcv_lxc_rootfs_residue_clear("
        out = []
        stop = fns.get("_lxc_stop_thread", "")
                                                                                                             
        last_ok = stop.rfind("_lxc_halt(c, d->name, d->force ? 0 : PCV_LXC_STOP_GRACE_SEC, &error)")
        halt = fns.get("_lxc_halt", "")
        if not 0 < halt.find("h->shutdown(h, (int)grace_sec)") < halt.find("h->stop(h)"):
            out.append("stop: _lxc_halt tries shutdown(grace) before stop")
        clear, unlock = stop.rfind(call), stop.rfind("_unlock_container_op(")
        branch = stop.rfind("if (!ok)")
        if stop.count(call) != 1 or not 0 <= last_ok < clear < unlock:
            out.append("stop: order ok-assign < residue_clear < unlock")
        if not (0 <= branch < clear and "} else {" in stop[branch:clear]):
            out.append("stop: residue_clear must sit in the success (else) branch")
        start = fns.get("pcv_lxc_start_sync", "")
        prep = "pcv_lxc_volume_start_prepare("
        if start.count(prep) != 1 or not 0 < start.find("is_running(") < start.find(prep) < start.find("pcv_lxc_unit_start("):
            out.append("start: is_running < pcv_lxc_volume_start_prepare < pcv_lxc_unit_start")
        sp = dict(functions(code_only(vm))).get("pcv_lxc_volume_start_prepare", "")
        if sp.count(call) != 1 or not 0 < sp.find(call) < sp.find("->source_open("):
            out.append("start: residue_clear once in pcv_lxc_volume_start_prepare, before ->source_open")
        destroy = fns.get("_lxc_destroy_thread", "")
        i_stop, i_probe, i_call, i_des = destroy.find("c->stop(c)"), destroy.find("is_running(c)", destroy.find("c->stop(c)")), \
            destroy.find(call), destroy.find('"lxc-destroy"')
        if destroy.count(call) != 1 or not 0 <= i_stop < i_probe < i_call < i_des:
            out.append("destroy: stop < is_running recheck < residue_clear < lxc-destroy")
        if not re.search(r"if \(still_running\)[^}]*\}\s*else\s*\{[^}]*" + re.escape(call), destroy):
            out.append("destroy: residue_clear only when not still running")
        return out

                                                                                                         
    @staticmethod
    def _src_codes():
        root = SRC.parent
        return {p.relative_to(root).as_posix(): p.read_text() for p in sorted(root.rglob("*.[ch]"))}

    @staticmethod
    def _callers(codes, call):
                                                                              
        out = set()
        for k, v in codes.items():
            if not k.endswith(".c") or call not in v:
                continue
            code = code_only(v)
            for m in DEF_RE.finditer(code):                                                          
                end = code.find("\n}\n", m.end())
                if end >= 0 and call in code[m.end() - 2:end]:
                    out.add(m.group(1))
        return out

    def test_runtime_config_single_writer(self):
                                                                                            
                                                                   
        codes = self._src_codes()
        self.assertEqual(self._single_writer_violations(codes), [])
        vm, drv, hc = "modules/lxc/lxc_volume_mount.c", "modules/lxc/lxc_driver.c", "modules/dispatcher/handler_container.c"
        rm_head = "\nvoid\npcv_lxc_runtime_config_remove(const gchar *name)\n{\n"
        self.assertIn(rm_head, codes[vm])
        mutants = {                          
            "다른 함수가 런타임 config를 쓴다": {vm: codes[vm].replace(rm_head, rm_head + '    (void)_runtime_config_write(name, "", NULL);\n', 1)},
            "attach 경로가 staging 진입을 부른다": {hc: codes[hc] + '\nstatic void\n_x(void)\n{\n    (void)pcv_lxc_volume_start_prepare("/x", "c", NULL);\n}\n'},
            "자리 만들기를 다른 함수에서": {vm: codes[vm] + '\nstatic int\n_y(const PcvLxcMountOps *ops)\n{\n    return ops->prepare_slot(0, "k", TRUE, NULL);\n}\n'},
            "런타임 루트 상수를 다른 TU에서": {drv: codes[drv] + "\nstatic const char *_z = PCV_LXC_RUNTIME_CONFIG_DIR;\n"},
        }
        for what, patch in mutants.items():
            self.assertNotEqual(self._single_writer_violations({**codes, **patch}), [], what)

    def _single_writer_violations(self, codes):
        out = []
        for call, want in (("_runtime_config_write(", {"pcv_lxc_volume_start_prepare"}),
                           ("pcv_lxc_volume_start_prepare(", {"pcv_lxc_start_sync"}),
                           ("->prepare_slot(", {"pcv_lxc_volume_start_prepare"})):
            got = self._callers(codes, call)
            if got != want:
                out.append(f"callers of {call} = {sorted(got)}, want {sorted(want)}")
        homes = {k for k, v in codes.items() if "PCV_LXC_RUNTIME_CONFIG_DIR" in code_only(v)}
        if homes != {"modules/lxc/lxc_volume_mount.c", "modules/lxc/lxc_unit.h"}:
            out.append(f"PCV_LXC_RUNTIME_CONFIG_DIR used in {sorted(homes)}")
        return out

    def test_prepare_dir_no_daemon_chown(self):
                                                                                                     
                                                                                                              
        text = (SRC / "lxc/lxc_volume_mount.c").read_text()
        self.assertEqual(self._prepare_dir_violations(text), [])
        fchmod = "if (made && fchmod(fd, mode) < 0) {"
        check = "if (fstat(fd, &st) < 0 || !S_ISDIR(st.st_mode) || !_stage_owner_ok(&st, mode, gid)) {"
        spawn = "pcv_spawn_sync_profile(argv, PCV_CHILD_CAP_STORAGE, NULL, &err_out, &local)"
        for tok in (fchmod, check, spawn):
            self.assertIn(tok, text)
        mutants = {
            "fchown": text.replace(fchmod, "if (made && (fchown(fd, 0, (gid_t)gid) < 0 || fchmod(fd, mode) < 0)) {"),
            "fchownat": text.replace(fchmod, "if (made && (fchownat(parent_fd, name, 0, (gid_t)gid, 0) < 0 || fchmod(fd, mode) < 0)) {"),
            "chown(path)": text.replace("gboolean ran = path && (child ?", "gboolean ran = path && chown(path, 0, (gid_t)gid) == 0 && (child ?"),
            "STORAGE 자식 빼기": text.replace(spawn, "TRUE"),
            "fstat 확인 빼기": text.replace(check, "if (!S_ISDIR(0)) {"),
            "실제 표가 다른 본문": text.replace(".prepare_dir    = _real_prepare_dir,", ".prepare_dir    = _plain_dir,")
                                 + "\nstatic int\n_plain_dir(int a, const gchar *b, guint c, gint64 d, GError **e)\n{\n    return mkdirat(a, b, c);\n}\n",
        }
        for what, mutated in mutants.items():
            self.assertNotEqual(self._prepare_dir_violations(mutated), [], what)

    @staticmethod
    def _prepare_dir_violations(text: str):
        code = code_only(text)
        fns = dict(functions(code))
        table = re.search(r"static const PcvLxcMountOps G_vol_real_ops = \{(.*?)\};", code, re.S)
        if not table:
            return ["real ops table"]
        slots = dict(re.findall(r"\.(\w+)\s*=\s*(\w+)", table.group(1)))
        b = fns.get(slots.get("prepare_dir", ""), "")
        banned = re.compile(r"\b(?:l?chown|fchownat|fchown)\s*\(")
        out = []
        if banned.search(b):
            out.append("prepare_dir: daemon-side chown call")
        if banned.search(code):
            out.append("lxc_volume_mount.c: daemon-side chown call")
        for tok in ("pcv_spawn_sync_profile(", "PCV_CHILD_CAP_STORAGE", '"chown"', "fstat(", "_stage_owner_ok("):
            if tok not in b:
                out.append("prepare_dir: " + tok)
        return out

    def test_runtime_dir_removers(self):
                                                                                                       
                                              
        drv = (SRC / "lxc/lxc_driver.c").read_text()
        vm = (SRC / "lxc/lxc_volume_mount.c").read_text()
        self.assertEqual(self._remover_violations(drv, vm), [])
        call = "pcv_lxc_runtime_config_remove(d->name);"
        mutants = {
            "다른 함수가 지운다": (drv + "\nstatic void\n_w(void)\n{\n    pcv_lxc_runtime_config_remove(\"c\");\n}\n", vm),
            "정지 실패 분기에서 지운다": (drv.replace(call, "", 1).replace("        g_task_return_error(task, error);\n    } else {",
                                                            "        " + call + "\n        g_task_return_error(task, error);\n    } else {", 1), vm),
            "start_abort가 안 지운다": (drv, vm.replace("    g_clear_error(&e);\n    pcv_lxc_runtime_config_remove(name);\n}", "    g_clear_error(&e);\n}", 1)),
            "삭제가 lxc-destroy 뒤에 지운다": (drv.replace("    pcv_lxc_runtime_config_remove(name);", "    (void)0;", 1)
                                       .replace("    pcv_lxc_unit_reset_failed_best_effort(name);", "    pcv_lxc_runtime_config_remove(name);\n    pcv_lxc_unit_reset_failed_best_effort(name);", 1), vm),
        }
        for what, (md, mv) in mutants.items():
            self.assertNotEqual(self._remover_violations(md, mv), [], what)

    def _remover_violations(self, drv: str, vm: str):
        codes = {"modules/lxc/lxc_driver.c": drv, "modules/lxc/lxc_volume_mount.c": vm}
        codes.update({k: v for k, v in self._src_codes().items() if k not in codes})
        call = "pcv_lxc_runtime_config_remove("
        out = []
        got = self._callers(codes, call)
        want = {"_lxc_stop_thread", "_lxc_destroy_thread", "pcv_lxc_volume_start_abort"}
        if got != want:
            out.append(f"callers = {sorted(got)}, want {sorted(want)}")
        dfns = dict(functions(code_only(drv)))
        stop = dfns.get("_lxc_stop_thread", "")
        branch, clear, rm = stop.rfind("if (!ok)"), stop.rfind("pcv_lxc_rootfs_residue_clear("), stop.find(call)
        if stop.count(call) != 1 or not (0 <= branch < clear < rm and "} else {" in stop[branch:clear]):
            out.append("stop: success branch, after residue_clear")
        des = dfns.get("_lxc_destroy_thread", "")
        if des.count(call) != 1 or not 0 <= des.find("pcv_lxc_rootfs_residue_clear(") < des.find(call) < des.find('"lxc-destroy"'):
            out.append("destroy: residue_clear < runtime_config_remove < lxc-destroy")
        ab = dict(functions(code_only(vm))).get("pcv_lxc_volume_start_abort", "")
        if not 0 <= ab.find("pcv_lxc_rootfs_residue_clear(") < ab.find(call):
            out.append("start_abort: residue_clear < runtime_config_remove")
        return out

    def test_staging_mount_from_fd_and_slave(self):
                                                                                                                    
                                                                                                                  
                                              
        userns = (SRC / "lxc/lxc_userns.c").read_text()
        vm = (SRC / "lxc/lxc_volume_mount.c").read_text()
        self.assertEqual(self._staging_mount_violations(userns, vm), [])
        move = 'move_mount(tree, "", target_fd, "", MOVE_MOUNT_F_EMPTY_PATH | MOVE_MOUNT_T_EMPTY_PATH) < 0) {\n        _set_mount_errno(error, "move_mount");'
        self.assertIn(move, userns)
        mutants = {
            "open_tree 경로 문자열": (userns.replace('open_tree(src_fd, "", OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC | AT_EMPTY_PATH | rec)',
                                                 'open_tree(AT_FDCWD, "/x", OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC | rec)'), vm),
            "slave 빼기": (userns.replace("struct mount_attr slave = { .propagation = MS_SLAVE };",
                                         "struct mount_attr slave = { .propagation = MS_SHARED };"), vm),
            "slave를 붙이기 앞에": (userns.replace("    if (ok && move_mount(tree, \"\", target_fd, \"\", MOVE_MOUNT_F_EMPTY_PATH | MOVE_MOUNT_T_EMPTY_PATH) < 0) {\n        _set_mount_errno(error, \"move_mount\");",
                                               "    (void)MS_SLAVE;\n    if (ok && move_mount(tree, \"\", target_fd, \"\", MOVE_MOUNT_F_EMPTY_PATH | MOVE_MOUNT_T_EMPTY_PATH) < 0) {\n        _set_mount_errno(error, \"move_mount\");"), vm),
            "ro를 붙인 뒤에": (userns.replace("struct mount_attr ro = { .attr_set = MOUNT_ATTR_RDONLY };", "struct mount_attr ro = { 0 };")
                               .replace("struct mount_attr slave = { .propagation = MS_SLAVE };",
                                        "struct mount_attr slave = { .attr_set = MOUNT_ATTR_RDONLY, .propagation = MS_SLAVE };"), vm),
            "실패 분기 떼기 빼기": (userns.replace("            if (umount2(path, MNT_DETACH) < 0)\n                PCV_LOG_WARN(USERNS_LOG_DOM, \"volume mount: cannot remove",
                                                "            if (0)\n                PCV_LOG_WARN(USERNS_LOG_DOM, \"volume mount: cannot remove"), vm),
            "ID를 붙인 뒤에 읽기": (userns.replace("    if (ok && mnt_id_out) {", "    if (0) {").replace("    close(tree);\n    return ok;",
                                    "    if (ok && mnt_id_out && statx(tree, \"\", AT_EMPTY_PATH, STATX_MNT_ID, &(struct statx){0}) < 0)\n        ok = FALSE;\n    close(tree);\n    return ok;"), vm),
            "ID 못 읽어도 붙이기": (userns.replace("_set_mount_errno(error, \"statx(STATX_MNT_ID)\");\n            ok = FALSE;", "_set_mount_errno(error, \"statx(STATX_MNT_ID)\");"), vm),
            "문구에 staging": (userns.replace("_set_mount_errno(error, \"move_mount\");", "_set_mount_errno(error, \"move_mount volume staging\");"), vm),
            "실패 분기 private 빼기": (userns.replace("struct mount_attr priv = { .propagation = MS_PRIVATE };", "struct mount_attr priv = { 0 };"), vm),
            "vm에 문자열 mount(2)": (userns, vm + '\nstatic void\n_m(void)\n{\n    (void)mount("/a", "/b", NULL, MS_BIND, NULL);\n}\n'),
            "vm에 open_tree(AT_FDCWD": (userns, vm + '\nstatic int\n_o(void)\n{\n    return open_tree(AT_FDCWD, "/a", 0);\n}\n'),
            "실제 표 mount_clone 바꿈": (userns, vm.replace(".mount_clone    = pcv_lxc_userns_mount_clone,", ".mount_clone    = _other,")),
            "실제 표 source_open 바꿈": (userns, vm.replace(".source_open    = pcv_lxc_volume_source_open,", ".source_open    = _other,")),
        }
        for what, (mu, mv) in mutants.items():
            self.assertNotEqual(self._staging_mount_violations(mu, mv), [], what)

    @staticmethod
    def _staging_mount_violations(userns: str, vm: str):
        out = []
        raw = dict(functions(code_only(userns))).get("pcv_lxc_userns_mount_clone", "")
                                                                                                        
        c = re.sub(r'"(?:[^"\\\n]|\\.)*"', '""', raw)
        ot, ro, mv, sl = c.find('open_tree(src_fd, ""'), c.find("MOUNT_ATTR_RDONLY"), c.find("move_mount("), c.find("MS_SLAVE")
        if not 0 <= ot < mv < sl:
            out.append(f"clone: open_tree(src_fd) {ot} < move_mount {mv} < MS_SLAVE {sl}")
        if "OPEN_TREE_CLONE" not in c or "AT_EMPTY_PATH" not in c:
            out.append("clone: OPEN_TREE_CLONE|AT_EMPTY_PATH")
                                                                                                                 
        st = c.find('statx(tree, "", AT_EMPTY_PATH, STATX_MNT_ID')
        if not (0 <= ot < st < mv):
            out.append(f"clone: open_tree {ot} < statx(tree, STATX_MNT_ID) {st} < move_mount {mv}")
        fail = c[st:mv] if 0 <= st < mv else ""
        if fail.count("ok = FALSE;") < 2 or "if (ok && move_mount(" not in c:                                             
            out.append("clone: statx failure must skip move_mount")
        if "if (ok && mnt_id_out) {" not in (c[ot:mv] if 0 <= ot < mv else ""):
            out.append("clone: mount id read on the detached copy before move_mount")
        if "*mnt_id_out = stx.stx_mnt_id;" not in c:
            out.append("clone: mnt_id_out from the tree fd")
                                                                                                       
        lits = re.findall(r'"((?:[^"\\\n]|\\.)*)"', raw)
        if any("staging" in x for x in lits):
            out.append("clone: context-specific word 'staging' in a message")
        if not 0 <= ro < mv:
            out.append("clone: MOUNT_ATTR_RDONLY on the detached copy before move_mount")
        tail = c[sl:] if sl >= 0 else ""
        pv, um = tail.find("MS_PRIVATE"), tail.find("umount2(")
        if not (0 <= pv < um and "MNT_DETACH" in tail[um:]):
            out.append("clone: after move_mount, failure branch MS_PRIVATE < umount2(MNT_DETACH)")
        vcode = code_only(vm)
        if re.search(r"\bmount\s*\(", re.sub(r'"(?:[^"\\\n]|\\.)*"', '""', vcode)):                                 
            out.append("lxc_volume_mount.c: string-path mount(")
        if re.search(r"open_tree\s*\(\s*AT_FDCWD", vcode):
            out.append("lxc_volume_mount.c: open_tree(AT_FDCWD")
        table = re.search(r"static const PcvLxcMountOps G_vol_real_ops = \{(.*?)\};", vcode, re.S)
        slots = dict(re.findall(r"\.(\w+)\s*=\s*(\w+)", table.group(1))) if table else {}
        if slots.get("mount_clone") != "pcv_lxc_userns_mount_clone":
            out.append("real ops: mount_clone")
        if slots.get("source_open") != "pcv_lxc_volume_source_open":
            out.append("real ops: source_open")
        return out

    def test_mountinfo_tu_setns_safe(self):
                                                                                                        
        text = (SRC / "lxc/lxc_mountinfo.c").read_text()
        self.assertEqual(self._mountinfo_forbidden(text), [])
                                                         
        for bad in ("g_strdup(x);", "snprintf(b, 1, \"\");", "malloc(4);", "strtoull(s, 0, 10);", "free (p);", "sscanf(s, \"%d\", &i);"):
            self.assertNotEqual(self._mountinfo_forbidden(text + "\nstatic void f(void) { " + bad + " }\n"), [], bad)

    def test_residue_clear_private_before_umount(self):
                                                                                                      
                                                                                                       
                                                                                                        
                                                                               
        text = (SRC / "lxc/lxc_volume_mount.c").read_text()
        userns = (SRC / "lxc/lxc_userns.c").read_text()                                                 
        self.assertEqual(self._residue_violations(text, userns), [])
        swapped = (text.replace("->make_private(", "->@@(").replace("->umount_fd(", "->make_private(")
                   .replace("->@@(", "->umount_fd("))
        mutants = {"MS_PRIVATE→MS_SLAVE": (text.replace("MS_PRIVATE", "MS_SLAVE"), userns),
                                                                                    
                   "1단계 빼기": (text.replace("else if (_unmount_step(ops, pfd, plen, v, VOL_STEP_PRIVATE, what, name, res))",
                                           "else if (TRUE)", 1), userns),
                   "사슬 빼기": (text.replace("pcv_mountinfo_chain(lines, idx, m, depth, NULL);", "", 1), userns),
                   "맨 위 판정 빼기": (text.replace("if (depth[k] != 0 || _ids_has(done_top, v->mnt_id)", "if (_ids_has(done_top, v->mnt_id)", 1), userns),
                                                                                       
                   "겹침 shared 거절 빼기": (text.replace("            if (shared_below)",
                                                    "            if (FALSE)", 1), userns),
                   "겹침 맨 위 대신 사슬 맨 위": (text.replace("&lines[idx[pcv_mountinfo_stack_visible(lines, idx, m, k, &shared_below)]]",
                                                      "&lines[idx[k]]", 1), userns),
                   "조상 기준 후보 빼기": (text.replace("if (pcv_mountinfo_chain_has(lines, idx, m, k,", "if (TRUE || pcv_mountinfo_chain_has(lines, idx, m, k,", 1), userns),
                   "AT_RECURSIVE 빼기": (re.sub(r"\s*\|\s*AT_RECURSIVE", "", text), userns),
                   "MNT_DETACH 빼기": (text.replace("umount2(path, MNT_DETACH)", "umount2(path, 0)"), userns),
                   "umount 먼저": (swapped, userns),
                                                                                        
                   "마지막 성분 O_DIRECTORY": (text, userns.replace(".flags   = O_PATH | O_NOFOLLOW | O_CLOEXEC,",
                                                               ".flags   = O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC,", 1)),
                   "RESOLVE_NO_SYMLINKS 빼기": (text, userns.replace(".resolve = RESOLVE_IN_ROOT | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS,",
                                                                ".resolve = RESOLVE_IN_ROOT | RESOLVE_NO_MAGICLINKS,", 1)),
                   "디렉터리 전용 walk": (text.replace("ops->open_leaf(pfd, m->mount_point + plen, &e)",
                                                    "ops->walk(pfd, m->mount_point + plen, FALSE, &e)"), userns)}
        for what, (mutated, mu) in mutants.items():                                                        
            self.assertNotEqual((mutated, mu), (text, userns), what)
            self.assertNotEqual(self._residue_violations(mutated, mu), [], what)

    @staticmethod
    def _residue_violations(text: str, userns: str):
        code = code_only(text)
        fns = dict(functions(code))
        table = re.search(r"static const PcvLxcMountOps G_vol_real_ops = \{(.*?)\};", code, re.S)
        if not table:
            return ["real ops table"]
        slots = dict(re.findall(r"\.(\w+)\s*=\s*(\w+)", table.group(1)))
        out = []
        private = fns.get(slots.get("make_private", ""), "")
        for tok in ("mount_setattr(", "AT_EMPTY_PATH", "AT_RECURSIVE", "MS_PRIVATE"):
            if tok not in private:
                out.append("make_private: " + tok)
        umount = fns.get(slots.get("umount_fd", ""), "")
        for tok in ("umount2(", "MNT_DETACH"):
            if tok not in umount:
                out.append("umount_fd: " + tok)
                                                                                                
                                                             
        leaf = dict(functions(code_only(userns))).get(slots.get("open_leaf", ""), "")
        if not re.search(r"pcv_lxc_rootfs_walk_dir\([^;]*FALSE", leaf):
            out.append("open_leaf: parent walk create=FALSE")
        for tok in ("SYS_openat2", "O_PATH", "O_NOFOLLOW", "RESOLVE_IN_ROOT", "RESOLVE_NO_SYMLINKS",
                    "RESOLVE_NO_MAGICLINKS", "S_ISDIR(", "S_ISREG("):
            if tok not in leaf:
                out.append("open_leaf: " + tok)
        if "O_DIRECTORY" in leaf:
            out.append("open_leaf: O_DIRECTORY on the last component")
                                                                                                   
                                                       
        clear = fns.get("pcv_lxc_rootfs_residue_clear", "")
        loop = fns.get("_unmount_matching", "")
        call = re.search(r"_unmount_matching\(ops, pfd, prefix, (\w+),", clear)
        pick = fns.get(call.group(1), "") if call else ""
        if not call or "return 1;" not in pick or re.search(r"return\s+(0|-1)\s*;", pick):
            out.append("residue_clear: through _unmount_matching with an always-1 pick")
        for direct in ("->open_leaf(", "->mnt_id(", "->make_private(", "->umount_fd("):
            if direct in clear:
                out.append("residue_clear: direct " + direct)
        out += LxcUnprivilegedContract._two_phase_violations(fns)
        step = fns.get("_unmount_step", "")
        if "->walk(" in clear or "->walk(" in loop or "->walk(" in step:
            out.append("residue uses the directory-only walk")
        return out

    @staticmethod
    def _two_phase_violations(fns):
                                                                                                                       
                                                                                                                  
                                                      
        out = []
        step = fns.get("_unmount_step", "")
        loop = fns.get("_unmount_matching", "")
        pos = [step.find(x) for x in ("->open_leaf(", "->mnt_id(", "->make_private(", "->umount_fd(")]
        if not (all(p >= 0 for p in pos) and pos == sorted(pos)):                                             
            out.append(f"_unmount_step order open_leaf < mnt_id < make_private < umount_fd {pos}")
        if "now_id != m->mnt_id" not in step:
            out.append("_unmount_step: mount id check")
        if not re.search(r"step == VOL_STEP_PRIVATE && !ops->make_private\(fd, &e\)", step) or \
                not re.search(r"step == VOL_STEP_UMOUNT && !ops->umount_fd\(fd, &e\)", step):
            out.append("_unmount_step: VOL_STEP_PRIVATE → make_private, VOL_STEP_UMOUNT → umount_fd")
        chain = loop.find("pcv_mountinfo_chain(")
        ph1, ph2 = loop.find("VOL_STEP_PRIVATE"), loop.find("VOL_STEP_UMOUNT")
        if not 0 <= chain < ph1 < ph2 or loop.count("_unmount_step(") != 2:
            out.append(f"_unmount_matching: chain {chain} < VOL_STEP_PRIVATE {ph1} < VOL_STEP_UMOUNT {ph2} (two _unmount_step calls)")
        if not re.search(r"if \(depth\[k\] != 0 \|\|[^\n]*\n\s*continue;\n\s*if \(shared_below\)[^\n]*\n\s*_stack_refuse\([^\n]*\n"
                         r"\s*else if \(_unmount_step\([^\n]*VOL_STEP_PRIVATE", loop):
            out.append("_unmount_matching: phase 1 converts only chain tops (depth 0), shared stacks refused first")
                                                                               
        if "pcv_mountinfo_stack_visible(lines, idx, m, k, &shared_below)" not in loop:
            out.append("_unmount_matching: phase 1 targets the visible stack top")
        if "if (pcv_mountinfo_chain_has(lines, idx, m, k, (const guint64 *)(gconstpointer)done_top->data, done_top->len) &&" not in loop:
            out.append("_unmount_matching: candidates are under a converted mount")
        if "->open_leaf(" in loop or "->make_private(" in loop or "->umount_fd(" in loop:
            out.append("_unmount_matching: direct ops call (the slot is _unmount_step)")
        return out

    def test_hook_ensure_in_start_worker(self):
                                                                                             
        w = body("lxc/lxc_driver.c", "_lxc_start_thread(GTask")
        mnt, ens, chk = w.find('"zfs", "mount"'), w.find("pcv_lxc_volume_hook_ensure("), w.find("pcv_lxc_volume_guard_check_start(")
        self.assertTrue(0 < mnt < ens < chk, (mnt, ens, chk))
        g = body("dispatcher/handler_container_mode.c", "pcv_container_volume_start_check(const gchar *name")
        self.assertNotIn("hook_ensure", g)
        raw = (SRC / "lxc/lxc_volume_mount.c").read_text()
        self.assertIn("starts outside purecvisor are not blocked", raw)

    def test_verify_budget_in_file_loop(self):
                                                                                                                
                                                                                             
                                                                                     
                                
        text = (SRC / "lxc/lxc_volume_detach.c").read_text()
        header = (SRC / "lxc/lxc_volume_detach.h").read_text()
        self.assertEqual(self._budget_violations(text, header), [])
        mutants = {
            "스레드 반복의 예산 확인 빼기": text.replace(
                "        if (!(ok = _budget_ok(c, error))) {",
                "        if (!(ok = TRUE)) {", 1),
            "실제 read의 기한 빼기": _replace_nth(text,
                "        if (g_get_monotonic_time() >= deadline_us) {",
                "        if (0) {", 1),
            "실제 readdir의 기한 빼기": _replace_nth(text,
                "        if (g_get_monotonic_time() >= deadline_us) {",
                "        if (0) {", 2),
            "기한 대신 무한": text.replace("c->ops->read_text(path, c->deadline, &e)", "c->ops->read_text(path, G_MAXINT64, &e)"),
            "줄 훑기 예산 빼기": text.replace("&& !(ok = _budget_ok(c, error)))", "&& 0)"),
        }
        for what, mutated in mutants.items():                                                              
            self.assertNotEqual(mutated, text, what)
            self.assertNotEqual(self._budget_violations(mutated, header), [], what)
        self.assertNotEqual(self._budget_violations(text, header.replace("5000000", "50000000")), [], "예산 값")

    def test_detach_unmount_private_before_umount(self):
                                                                                                                           
                                                                                                            
                                                                                                                  
                                                                             
        text = (SRC / "lxc/lxc_volume_mount.c").read_text()
        self.assertEqual(self._private_violations(text), [])
        mutants = {
            "make_private 호출 빼기": re.sub(r"    else if \(step == VOL_STEP_PRIVATE && !ops->make_private\(fd, &e\)\)[^\n]*\n"
                                              r"        why = e \? e->message : \"cannot make the mount private\";\n", "", text, count=1),
            "두 단계 → 한 단계(1단계 빼기)": text.replace("else if (_unmount_step(ops, pfd, plen, v, VOL_STEP_PRIVATE, what, name, res))",
                                                    "else if (TRUE)", 1),
            "MS_PRIVATE → MS_SLAVE": text.replace("struct mount_attr attr = { .propagation = MS_PRIVATE };",
                                                  "struct mount_attr attr = { .propagation = MS_SLAVE };", 1),
            "AT_RECURSIVE 빼기": text.replace("mount_setattr(fd, \"\", AT_EMPTY_PATH | AT_RECURSIVE, &attr, sizeof attr)",
                                            "mount_setattr(fd, \"\", AT_EMPTY_PATH, &attr, sizeof attr)", 1),
            "staging이 공용 반복을 우회": text.replace("ok = _unmount_matching(ops, pfd, prefix, _pick_stage_slot,",
                                                  "ok = _unmount_direct(ops, pfd, prefix, _pick_stage_slot,", 1),
            "디렉터리만 걷기": text.replace("int fd = ops->open_leaf(pfd, m->mount_point + plen, &e);",
                                          "int fd = ops->walk(pfd, m->mount_point + plen, FALSE, &e);", 1),
            "잔여 정리가 따로 떼기(두 벌)": text.replace("    ok = _unmount_matching(ops, pfd, prefix, _pick_any,",
                                                  "    if (0) ops->umount_fd(pfd, NULL);\n    ok = _unmount_matching(ops, pfd, prefix, _pick_any,", 1),
        }
        for what, mutated in mutants.items():                                                              
            self.assertNotEqual(mutated, text, what)
            self.assertNotEqual(self._private_violations(mutated), [], what)

    @staticmethod
    def _private_violations(text: str):
        code = code_only(text)
        fns = dict(functions(code))
        out = []
        for pub in ("pcv_lxc_volume_unmount_attributed", "pcv_lxc_volume_staging_unmount", "pcv_lxc_rootfs_residue_clear"):
            b = fns.get(pub, "")
            if "_unmount_matching(" not in b:
                out.append(pub + ": not through _unmount_matching")
            for direct in ("->umount_fd(", "->make_private(", "umount2("):
                if direct in b:
                    out.append(pub + ": direct " + direct)
        for op in ("->umount_fd(", "->make_private(", "->open_leaf("):                                               
            if code.count(op) != 1:
                out.append(f"one unmount slot in the file: {op} x{code.count(op)}")
        out += LxcUnprivilegedContract._two_phase_violations(fns)                                     
        if "->walk(" in fns.get("_unmount_matching", "") + fns.get("_unmount_step", ""):
            out.append("_unmount_matching: directory-only walk (file volumes)")
        table = re.search(r"static const PcvLxcMountOps G_vol_real_ops = \{(.*?)\};", code, re.S)
        slots = dict(re.findall(r"\.(\w+)\s*=\s*(\w+)", table.group(1))) if table else {}
        priv = fns.get(slots.get("make_private", ""), "")
        for tok in ("mount_setattr(", "AT_RECURSIVE", "MS_PRIVATE"):
            if tok not in priv:
                out.append("real make_private: " + tok)
        if "MS_SLAVE" in priv or "MS_SHARED" in priv:
            out.append("real make_private: not private")
        return out

    def test_attach_revert_by_new_mount_id(self):
                                                                                                                               
                                                                                                    
                                                                                                                 
                                                                                                     
                                                      
        at = (SRC / "lxc/lxc_volume_attach.c").read_text()
        vm = (SRC / "lxc/lxc_volume_mount.c").read_text()
        self.assertEqual(self._attach_violations(at, vm), [])
        mutants = {
            "귀속 떼기로 되돌리기": (at.replace("pcv_lxc_volume_unmount_id(lxc_path, name, new_id, &re)",
                                          "pcv_lxc_volume_unmount_attributed(lxc_path, name, NULL, dest, &re)"), vm),
            "다시 열어 ID 읽기": (at.replace("readonly ? PCV_LXC_MOUNT_RO : 0, &id, error);",
                                        "readonly ? PCV_LXC_MOUNT_RO : 0, &id, error) && ops->mnt_id(target_fd, &id);"), vm),
            "clone ID 안 받기": (at.replace("readonly ? PCV_LXC_MOUNT_RO : 0, &id, error)", "readonly ? PCV_LXC_MOUNT_RO : 0, NULL, error)"), vm),
            "루트 대상 거절을 분기 뒤로": (at.replace("    if (!_container_path_check(container_path, error))\n        return FALSE;\n    if (!_attach_precheck(",
                                                "    if (!_attach_precheck("), vm),
            "attach TU에서 직접 umount2": (at + '\nstatic void\n_u(void)\n{\n    (void)umount2("/x", MNT_DETACH);\n}\n', vm),
            "되돌리기 ID 판정 빼기": (at, vm.replace("return m->mnt_id == *(const guint64 *)data ? 1 : 0;", "return 1;")),
            "실제 표 mount_clone 바꿈": (at, vm.replace(".mount_clone    = pcv_lxc_userns_mount_clone,", ".mount_clone    = _other,")),
        }
        for what, (ma, mv) in mutants.items():                                                             
            self.assertNotEqual((ma, mv), (at, vm), what)
            self.assertNotEqual(self._attach_violations(ma, mv), [], what)

    @staticmethod
    def _attach_violations(at: str, vm: str):
        out = []
        acode, vcode = code_only(at), code_only(vm)
        fns, vfns = dict(functions(acode)), dict(functions(vcode))
        run = fns.get("pcv_lxc_volume_mount_running", "")
                                                                                                         
        if "ops->mount_clone(src_fd, target_fd, readonly ? PCV_LXC_MOUNT_RO : 0, &id, error)" not in run:
            out.append("mount_running: mount_clone must return the new mount id")
        for bad in ("->mnt_id(", "->open_leaf("):
            if bad in run:
                out.append("mount_running: re-reads the mount id via " + bad)
        main = fns.get("pcv_lxc_volume_attach", "")
        save, revert = main.find("pcv_lxc_volume_config_save("), main.find("pcv_lxc_volume_unmount_id(lxc_path, name, new_id, &re)")
        if not 0 <= save < revert:
            out.append(f"attach: save {save} < revert by new id {revert}")
                                                                                                      
                                                                
        chk, dup, mnt = (main.find("_container_path_check(container_path, error)"), main.find("_attach_precheck("),
                         main.find("pcv_lxc_volume_mount_running("))
        if not 0 <= chk < dup < mnt:
            out.append(f"attach: root-target check {chk} < duplicate check {dup} < mount {mnt}")
        if "PCV_LXC_VOLUME_ROOT_TARGET_MSG" not in fns.get("_container_path_check", ""):
            out.append("_container_path_check: root target message")
        for bad in ("pcv_lxc_volume_unmount_attributed(", "->umount_fd(", "->make_private(", "umount2("):
            if bad in acode:
                out.append("attach TU: " + bad)
        rid = vfns.get("pcv_lxc_volume_unmount_id", "")
        if "_unmount_matching(ops, pfd, prefix, _pick_mnt_id, &mnt_id," not in rid:
            out.append("unmount_id: not through _unmount_matching with _pick_mnt_id")
        if "m->mnt_id == *(const guint64 *)data" not in vfns.get("_pick_mnt_id", ""):
            out.append("_pick_mnt_id: id comparison")
        table = re.search(r"static const PcvLxcMountOps G_vol_real_ops = \{(.*?)\};", vcode, re.S)
        slots = dict(re.findall(r"\.(\w+)\s*=\s*(\w+)", table.group(1))) if table else {}
        if slots.get("mount_clone") != "pcv_lxc_userns_mount_clone":
            out.append("real ops: mount_clone")
        return out

    @staticmethod
    def _budget_violations(text: str, header: str):
        out = []
        if not re.search(r"#define\s+PCV_LXC_VERIFY_BUDGET_US\s+5000000\b", header):
            out.append("PCV_LXC_VERIFY_BUDGET_US 5000000")
        code = code_only(text)
        fns = dict(functions(code))
        table = re.search(r"static const PcvLxcVerifyOps G_verify_real_ops = \{(.*?)\};", code, re.S)
        slots = dict(re.findall(r"\.(\w+)\s*=\s*(\w+)", table.group(1))) if table else {}
        if slots.get("now_us") != "g_get_monotonic_time":
            out.append("real now_us")
        for slot, call in (("read_text", "read("), ("list_dirs", "readdir(")):                               
            fn = fns.get(slots.get(slot, ""), "")
            loop = fn[fn.find("for (;;)"):] if "for (;;)" in fn else ""
            chk, rd = loop.find("g_get_monotonic_time() >= deadline_us"), loop.find(call)
            if not (0 <= chk < rd) or "ETIMEDOUT" not in loop:
                out.append(f"{slot}: deadline inside the read loop")
        init = fns.get("_ctx_init", "")
        if not re.search(r"deadline\s*=\s*c->ops->now_us\(\)\s*\+\s*PCV_LXC_VERIFY_BUDGET_US", init):
            out.append("_ctx_init: deadline = now_us() + budget")
        budget = fns.get("_budget_ok", "")
        if "now_us()" not in budget or "c->deadline" not in budget or "verification took longer than" not in text:
            out.append("_budget_ok")
        for name in ("_verify_init", "_verify_cgroup_procs", "_verify_cgroup_children", "_verify_pid"):
            fn = fns.get(name, "")
            calls = [m for m in re.finditer(r"->(read_text|list_dirs)\(([^;]*)\)", fn)]
            if not calls:
                out.append(f"{name}: no reads")
            for k, m in enumerate(calls):
                if "c->deadline" not in m.group(2):
                    out.append(f"{name}: {m.group(1)} without c->deadline")
                nxt = calls[k + 1].start() if k + 1 < len(calls) else len(fn)
                if "_budget_ok(" not in fn[m.end():nxt]:                                                           
                    out.append(f"{name}: {m.group(1)} not followed by _budget_ok")
        pid = fns.get("_verify_pid", "")
        loop = pid[pid.find("i < tids->len"):] if "i < tids->len" in pid else ""
        if "->read_text(" not in loop or "_budget_ok(" not in loop[loop.find("->read_text("):]:
            out.append("_verify_pid: budget inside the thread mountinfo loop")
        if "_budget_ok(" not in fns.get("_mountinfo_has_target", ""):
            out.append("_mountinfo_has_target: budget while scanning")
        return out

                                                                                                                         
                                                                             
    NSD_ALLOWED = {"syscall", "openat", "read", "write", "fchdir", "umount2", "close", "statx", "_exit", "memcpy", "memset",
                   "memcmp", "strlen", "pcv_mountinfo_parse", "pcv_mountinfo_last_under", "pcv_mountinfo_path_normalize",
                   "pcv_mountinfo_fmt_u64", "pcv_vol_attribute", "pcv_vol_ident_covers"}

    def test_ns_detach_child_allowlist(self):
                                                                                                                                   
                                                                                                         
                                                                                                    
                                                                    
                                                                                                        
                                                                                                    
                                                                              
                                                                                                        
                                                                                                    
                                                                           
        text = (SRC.parent / "cli/cli_container_ns.c").read_text()
        self.assertEqual(self._nsd_violations(text), [])
        in_ns = "_nsd_in_ns(NsdCtx *c)\n{\n"
        mutants = {
            "_nsd_in_ns에 printf": text.replace(in_ns, in_ns + '    printf("x");\n', 1),
            "호출되는 정적 함수에 g_strerror": text.replace("_nsd_line(const char *word, guint64 id, int err)\n{\n",
                                                   "_nsd_line(const char *word, guint64 id, int err)\n{\n    (void)g_strerror(err);\n", 1),
            "떼기 함수에 dlopen": text.replace("    int fd = (int)syscall(SYS_openat2,", "    dlopen(\"x\", 0);\n    int fd = (int)syscall(SYS_openat2,", 1),
            "자식 갈래에 snprintf": text.replace("            code = _nsd_in_ns(&ctx);",
                                            "            code = _nsd_in_ns(&ctx);\n        snprintf(NULL, 0, \"x\");", 1),
            "setns 앞에서 _nsd_in_ns": text.replace("        if (setns(nsfd, CLONE_NEWNS) == 0)\n            code = _nsd_in_ns(&ctx);",
                                                 "        code = _nsd_in_ns(&ctx);\n        if (setns(nsfd, CLONE_NEWNS) == 0)\n            code = 0;", 1),
            "/proc를 fork 뒤에 열기": text.replace('openat(AT_FDCWD, "/proc"', 'open("/proc"', 1),
            "호스트 procfs 대신 컨테이너 /proc": text.replace('openat(c->procfd, "thread-self/mountinfo"',
                                                     'openat(AT_FDCWD, "/proc/thread-self/mountinfo"', 1),
            "마운트 ID 대조 빼기": text.replace("stx.stx_mnt_id != m->mnt_id", "0", 1),
            "RESOLVE_NO_SYMLINKS 빼기": text.replace("RESOLVE_NO_SYMLINKS | ", "", 1),
            "줄 상한 바꾸기": text.replace("#define NSD_LINE_MAX    16384", "#define NSD_LINE_MAX    65536", 1),
                                            
            "cgroup 확인 빼기": text.replace("!pcv_proc_cgroup_v2_under(cg_text, cgroup)", "FALSE", 1),
            "호스트 NS 비교 빼기": text.replace("a.st_dev == b.st_dev && a.st_ino == b.st_ino", "FALSE", 1),
            "ns/mnt를 pid 번호로": text.replace('openat(piddir, "ns/mnt", O_RDONLY | O_CLOEXEC)',
                                             'openat(procfd, "1/ns/mnt", O_RDONLY | O_CLOEXEC)', 1),
            "고정 없이 번호로 연 ns": text.replace("nsfd = _nsd_pin_target(ctx.procfd, pid, cgroup);",
                                              'nsfd = openat(ctx.procfd, "1/ns/mnt", O_RDONLY | O_CLOEXEC);', 1),
            "거절 뒤에도 계속": text.replace("    if (nsfd < 0) {\n        pcv_cli_command_mark_runtime_failure();\n        goto out;\n    }",
                                          "    if (nsfd < 0) {\n        pcv_cli_command_mark_runtime_failure();\n    }", 1),
            "cgroup을 dirfd 대신 번호로": text.replace('_nsd_read_small(piddir, "cgroup", &cg_text)', '_nsd_read_small(procfd, "1/cgroup", &cg_text)', 1),
                                                                            
            "고정 64 상한(성공도 셈)": text.replace("limit == 0 || attempt < limit;", "attempt < NSD_ATTEMPTS;", 1),
            "첫 읽기 몫 빼기": text.replace("limit = (guint)_nsd_count(c, n) + NSD_ATTEMPTS;", "limit = NSD_ATTEMPTS;", 1),
            "제외 목록 64칸": text.replace("#define NSD_SKIP_MAX    (NSD_LINE_MAX + NSD_ATTEMPTS)", "#define NSD_SKIP_MAX    NSD_ATTEMPTS", 1),
            "넘침 방어 빼기": text.replace("if (c->n_skip < NSD_SKIP_MAX)", "if (TRUE)", 1),
            "몫 세기가 대상 판정 밖": text.replace("        if (_nsd_is_target(c, &c->lines[i]))\n            k++;", "        k++;", 1),
        }
        for what, mutated in mutants.items():                                                              
            self.assertNotEqual(mutated, text, what)
            self.assertNotEqual(self._nsd_violations(mutated), [], what)
                                                                                                            
        cli = (SRC.parent / "cli/purecvisorctl.c").read_text()
        self.assertEqual(cli.count('{"container","volume-ns-detach", cmd_container_volume_ns_detach, '
                                   '"(daemon internal) detach a volume inside a container mount namespace"},'), 1)
        self.assertLessEqual(len(cli.splitlines()), 9092)
        self.assertNotIn('#include "cli_container_ns', cli)
        guard_h = code_only((SRC.parent / "cli/cli_container_guard.h").read_text())
        self.assertIn("void cmd_container_volume_ns_detach(int argc, char *argv[]);", guard_h)
        mk = (SRC.parents[1] / "Makefile").read_text()
        cli_srcs = mk[mk.index("\nCLI_SRCS = "):]
        self.assertIn("src/cli/cli_container_ns.c", cli_srcs[:cli_srcs.index("\n\n")])
                                                                      
        self.assertEqual(self._nsd_daemon_violations((SRC / "lxc/lxc_volume_detach.c").read_text(),
                                                     (SRC / "lxc/lxc_volume_detach.h").read_text()), [])
        dtext = (SRC / "lxc/lxc_volume_detach.c").read_text()
        dmut = {"BASE 프로필": dtext.replace(".profile           = PCV_CHILD_CAP_RUNTIME", ".profile           = PCV_CHILD_CAP_BASE", 1),
                "종료 판정 빼기": dtext.replace(".should_abort      = pcv_drain_is_terminating", ".should_abort      = NULL", 1),
                "다른 실행체": dtext.replace("pcv_lxc_volume_ns_detach_argv(PCV_LXC_VOLUME_GUARD_HOOK,", "pcv_lxc_volume_ns_detach_argv(\"pcvctl\",", 1),
                "cgroup 인자 빼기": dtext.replace('g_ptr_array_add(a, g_strdup_printf("/lxc.payload.%s", name));', "", 1)}
        for what, mutated in dmut.items():
            self.assertNotEqual(mutated, dtext, what)
            self.assertNotEqual(self._nsd_daemon_violations(mutated, (SRC / "lxc/lxc_volume_detach.h").read_text()), [], what)

    @classmethod
    def _nsd_violations(cls, text: str):
        code = code_only(text)
        fns = dict(functions(code))
        out = []
        for need in ("_nsd_in_ns", "cmd_container_volume_ns_detach"):
            if need not in fns:
                out.append("missing " + need)
        if out:
            return out
        strip = lambda s: re.sub(r"'(?:\\.|[^'\\])'", "' '", re.sub(r'"(?:\\.|[^"\\\n])*"', '""', s))
        keywords = {"if", "for", "while", "switch", "return", "sizeof", "do"}
        calls = lambda s: [m.group(1) for m in re.finditer(r"\b([A-Za-z_]\w*)\s*\(", strip(s)) if m.group(1) not in keywords]
                                                               
        cmd = fns["cmd_container_volume_ns_detach"]
        pos = [cmd.find(x) for x in ('openat(AT_FDCWD, "/proc"', "nsfd = _nsd_pin_target(ctx.procfd, pid, cgroup);",
                                     "g_malloc(NSD_BUF_MAX + 1)", "g_new(PcvMountinfoLine, NSD_LINE_MAX)", "g_new(guint64, NSD_SKIP_MAX)", "fork(")]
        fork_at = pos[-1]
        setns_at = cmd.find("setns(", max(fork_at, 0))
        in_ns_at = cmd.find("_nsd_in_ns(", max(fork_at, 0))
        exit_at = cmd.find("_exit(", max(fork_at, 0))
        if not (all(p >= 0 for p in pos) and pos == sorted(pos) and fork_at < setns_at < in_ns_at < exit_at):
            out.append(f"order prepare {pos} < setns {setns_at} < _nsd_in_ns {in_ns_at} < _exit {exit_at}")
        if cmd.count("setns(") != 1 or code.count("setns(") != 1 or code.count("fork(") != 1:
            out.append("exactly one fork and one setns, in the command body")
                                                                                                               
        pin = fns.get("_nsd_pin_target", "")
        pseq = [pin.find("openat(procfd, pid_rel, O_PATH | O_DIRECTORY | O_CLOEXEC)"), pin.find('_nsd_read_small(piddir, "cgroup", &cg_text)'),
                pin.find("!pcv_proc_cgroup_v2_under(cg_text, cgroup)"), pin.find('openat(piddir, "ns/mnt", O_RDONLY | O_CLOEXEC)'),
                pin.find('fstatat(procfd, "thread-self/ns/mnt", &b, 0)'), pin.find("a.st_dev == b.st_dev && a.st_ino == b.st_ino")]
        if not (all(p >= 0 for p in pseq) and pseq == sorted(pseq)):
            out.append(f"pin: /proc/<pid> dirfd < cgroup via dirfd < cgroup check < ns/mnt via dirfd < own ns stat < compare {pseq}")
        if not re.search(r"nsfd = _nsd_pin_target\(ctx\.procfd, pid, cgroup\);\s*if \(nsfd < 0\) \{\s*"
                         r"pcv_cli_command_mark_runtime_failure\(\);\s*goto out;\s*\}", cmd):
            out.append("refusal ends before fork (runtime failure, goto out)")
        if code.count('"ns/mnt"') != 1 or "ns_rel" in code or '"%d/ns/mnt"' in code:
            out.append("ns/mnt is opened only from the pinned /proc/<pid> dirfd")
                                                                          
        child = cmd[fork_at + len("fork("):exit_at + len("_exit(")] if 0 <= fork_at < exit_at else ""
        allowed = cls.NSD_ALLOWED | {"setns"}
        stack, seen = list(calls(child)) + ["_nsd_in_ns"], set()
        while stack:
            name = stack.pop()
            if name in seen:
                continue
            seen.add(name)
            if name in fns and name != "cmd_container_volume_ns_detach":
                stack.extend(calls(fns[name]))
            elif name not in allowed:
                out.append("post-setns call: " + name)
                                                                                                                    
        in_ns = fns["_nsd_in_ns"]
        if 'openat(AT_FDCWD, "/", O_PATH | O_DIRECTORY' not in in_ns:
            out.append("_nsd_in_ns: root fd of the new namespace")
        rd = fns.get("_nsd_read", "")
        if 'openat(c->procfd, "thread-self/mountinfo"' not in rd or "pcv_mountinfo_parse(c->buf, c->lines, NSD_LINE_MAX, &truncated)" not in rd \
                or "E2BIG" not in rd:
            out.append("_nsd_read: host procfs thread-self/mountinfo, 16,384 lines, E2BIG")
        dt = fns.get("_nsd_detach", "")
        for tok in ("SYS_openat2", "O_PATH", "RESOLVE_NO_SYMLINKS", "RESOLVE_IN_ROOT", "RESOLVE_NO_MAGICLINKS", "STATX_MNT_ID",
                    "stx.stx_mnt_id != m->mnt_id", '"thread-self/fd/"', "MNT_DETACH"):
            if tok not in dt:
                out.append("_nsd_detach: " + tok)
        o2, sx, fc, um = dt.find("syscall(SYS_openat2"), dt.find("statx("), dt.find("fchdir(c->procfd)"), dt.find("umount2(")
        if not 0 <= o2 < sx < fc < um:
            out.append(f"_nsd_detach order openat2 {o2} < statx {sx} < fchdir {fc} < umount2 {um}")
        for bad in ('"/proc/self', "STATX_MNT_ID_UNIQUE", "/proc/thread-self"):
            if bad in code:
                out.append("forbidden: " + bad)
                                                                                                          
                                                                                                        
        if not re.search(r"for \(guint attempt = 0; limit == 0 \|\| attempt < limit; attempt\+\+\)", in_ns) \
                or not re.search(r"if \(limit == 0\)\s*limit = \(guint\)_nsd_count\(c, n\) \+ NSD_ATTEMPTS;", in_ns) \
                or "attempt < NSD_ATTEMPTS" in in_ns:
            out.append("_nsd_in_ns: attempt limit = first-read target count + NSD_ATTEMPTS")
        if not re.search(r"if \(c->n_skip < NSD_SKIP_MAX\)\s*c->skip\[c->n_skip\+\+\] = id;", in_ns) or in_ns.count("c->skip[") != 1:
            out.append("_nsd_in_ns: skip store bounded by NSD_SKIP_MAX")
        if not re.search(r"if \(_nsd_is_target\(c, &c->lines\[i\]\)\)\s*k\+\+;", fns.get("_nsd_count", "")) \
                or "_nsd_is_target(c, m)" not in fns.get("_nsd_pick", ""):
            out.append("_nsd_count and _nsd_pick share _nsd_is_target")
        if not re.search(r"guint64\s+\*skip;", text) or "skip[NSD_ATTEMPTS]" in text:
            out.append("NsdCtx.skip is the NSD_SKIP_MAX array allocated before fork")
        for define in (r"#define\s+NSD_BUF_MAX\s+\(\(gsize\)4 \* 1024 \* 1024\)", r"#define\s+NSD_LINE_MAX\s+16384\b",
                       r"#define\s+NSD_ATTEMPTS\s+64\b", r"#define\s+NSD_SKIP_MAX\s+\(NSD_LINE_MAX \+ NSD_ATTEMPTS\)"):
            if not re.search(define, text):
                out.append("value: " + define)
        return out

    @staticmethod
    def _nsd_daemon_violations(text: str, header: str):
        code = code_only(text)
        fns = dict(functions(code))
        out = []
        if not re.search(r"#define\s+PCV_LXC_NS_DETACH_TIMEOUT_SEC\s+5\b", header):
            out.append("PCV_LXC_NS_DETACH_TIMEOUT_SEC 5")
        real = fns.get("_real_ns_detach", "")
        for tok in (".profile           = PCV_CHILD_CAP_RUNTIME", ".timeout_sec       = PCV_LXC_NS_DETACH_TIMEOUT_SEC",
                    ".max_output_bytes  = PCV_LXC_NS_DETACH_OUTPUT_MAX", ".should_abort      = pcv_drain_is_terminating",
                    ".new_process_group = TRUE", "pcv_spawn_sync_bounded(argv, &bound,"):
            if tok not in real:
                out.append("_real_ns_detach: " + tok)
        if not re.search(r"#define\s+PCV_LXC_NS_DETACH_OUTPUT_MAX\s+\(\(gsize\)64 \* 1024\)", text + header):
            out.append("output cap 64 KiB")
        run = fns.get("pcv_lxc_volume_ns_detach", "")
        if "pcv_lxc_volume_ns_detach_argv(PCV_LXC_VOLUME_GUARD_HOOK, name, init_pid, set, target_dest)" not in run:
            out.append("pcv_lxc_volume_ns_detach: executable PCV_LXC_VOLUME_GUARD_HOOK")
        build = fns.get("pcv_lxc_volume_ns_detach_argv", "")
        head = [build.find(x) for x in ("g_strdup(pcvctl)", 'g_strdup("container")', 'g_strdup("volume-ns-detach")', "init_pid)",
                                        'g_strdup_printf("/lxc.payload.%s", name)')]
        if not (all(p >= 0 for p in head) and head == sorted(head)):
            out.append(f"argv head {head}")
        return out

    def test_mount_target_modes_and_start_target(self):
                                                                                                    
                                                                                                   
                                                                                                                    
                                                                                          
                                                                                                               
        userns = (SRC / "lxc/lxc_userns.c").read_text()
        at = (SRC / "lxc/lxc_volume_attach.c").read_text()
        vm = (SRC / "lxc/lxc_volume_mount.c").read_text()
        self.assertEqual(self._target_violations(userns, at, vm), [])
        mutants = {
            "fchmodat로 모드": (userns.replace("st.st_uid == geteuid() && fchmod(fd, 0755) < 0", "fchmodat(dir_fd, name, 0755, 0) < 0", 1), at, vm),
            "디렉터리 fchmod 빼기": (userns.replace("st.st_uid == geteuid() && fchmod(fd, 0755) < 0", "FALSE", 1), at, vm),
            "링크 허용 다시 열기": (userns.replace(".resolve = RESOLVE_IN_ROOT | RESOLVE_NO_MAGICLINKS | RESOLVE_NO_SYMLINKS,\n    };\n    int fd = (int)syscall(SYS_openat2, dir_fd",
                                              ".resolve = RESOLVE_IN_ROOT,\n    };\n    int fd = (int)syscall(SYS_openat2, dir_fd", 1), at, vm),
            "걷기가 mkdirat 직접": (userns.replace("if (!pcv_lxc_rootfs_mkdir_at(cur, *p, error)) {", "if (mkdirat(cur, *p, 0755) < 0) {", 1), at, vm),
            "파일 fchmod 빼기": (userns, at.replace("if (w >= 0 && fchmod(w, 0644) < 0)", "if (FALSE)", 1), vm),
            "시작 대상 보장 빼기": (userns, at, vm.replace("(!nested && !pcv_lxc_volume_target_ensure(lxc_path, name, &root_fd, l->container_path, S_ISDIR(sst.st_mode), &e))",
                                                    "FALSE", 1)),
            "시작 중첩 확인 빼기": (userns, at, vm.replace("!pcv_lxc_volume_nest_check(lines, i, S_ISDIR(sst.st_mode), &nested, &e) ||", "FALSE ||", 1)),
        }
        for what, (mu, ma, mv) in mutants.items():                                                       
            self.assertNotEqual((mu, ma, mv), (userns, at, vm), what)
            self.assertNotEqual(self._target_violations(mu, ma, mv), [], what)

    @staticmethod
    def _target_violations(userns: str, at: str, vm: str):
        out = []
        ufns = dict(functions(code_only(userns)))
        mk = ufns.get("pcv_lxc_rootfs_mkdir_at", "")
        for tok in ("mkdirat(dir_fd, name, 0755)", "SYS_openat2", "O_RDONLY | O_DIRECTORY | O_NOFOLLOW", "RESOLVE_IN_ROOT",
                    "RESOLVE_NO_SYMLINKS", "RESOLVE_NO_MAGICLINKS", "st.st_uid == geteuid() && fchmod(fd, 0755) < 0"):
            if tok not in mk:
                out.append("mkdir_at: " + tok)
        if re.search(r"\b(?:fchmodat|chmod)\s*\(", mk):
            out.append("mkdir_at: path chmod follows links")
        walk = ufns.get("pcv_lxc_rootfs_walk_dir", "")
        if "pcv_lxc_rootfs_mkdir_at(cur, *p, error)" not in walk or "mkdirat(" in walk:
            out.append("walk_dir: parents through pcv_lxc_rootfs_mkdir_at")
        tgt = dict(functions(code_only(at))).get("_attach_target_open", "")
        if "pcv_lxc_rootfs_mkdir_at(pfd, slash + 1, &e)" not in tgt or "mkdirat(" in tgt:
            out.append("attach leaf dir: pcv_lxc_rootfs_mkdir_at")
        if "fchmod(w, 0644)" not in tgt:
            out.append("attach leaf file: fchmod 0644")
        ens = dict(functions(code_only(at))).get("pcv_lxc_volume_target_ensure", "")
        if "_attach_target_open(ops, *rootfs_fd, container_path, src_is_dir, error)" not in ens:
            out.append("target_ensure: same rule as running attach")
        sp = dict(functions(code_only(vm))).get("pcv_lxc_volume_start_prepare", "")
        pos = [sp.find(x) for x in ("->source_open(", "pcv_lxc_volume_nest_check(", "pcv_lxc_volume_target_ensure(", "_stage_dir_open(",
                                    "->prepare_slot(", "->mount_clone(")]
        if not (all(p >= 0 for p in pos) and pos == sorted(pos)):                                                       
            out.append(f"start_prepare: source_open < nest_check < target_ensure < stage dir < slot < mount_clone {pos}")
        return out

    @staticmethod
    def _mountinfo_forbidden(text: str):
        code = code_only(text)
        pats = (r"\bg_[a-z0-9_]+\s*\(",
                r"printf|fprintf|snprintf|fopen|fdopen|fmemopen|setlocale|strto[a-z]+|sscanf|malloc|calloc|realloc|free\s*\(")
        return [m.group(0) for pat in pats for m in re.finditer(pat, code)]


if __name__ == "__main__":
    unittest.main()
