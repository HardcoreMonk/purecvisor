#!/usr/bin/env python3
                          
                                                                                                            
                                                                            
                                                                                                       
                                                                                     
                                                                                              
                                                                                       
                                                                                                           
                                                                                                              
                                                                                        
                                                                                              
                                          
                                                                           
                                                                        
 
                      
                                                                           
                       
from __future__ import annotations

import os
import re
import shlex
import shutil
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts" / "split"))
sys.path.insert(0, str(ROOT / "tests"))
import c_blocks              
from plugin_trusted_tmp import trusted_tempdir                                                             

MAIN = ROOT / "src/main.c"
HOT = ROOT / "src/api/hot_reload.c"
MANAGER = ROOT / "src/modules/plugin/pcv_plugin_manager.c"
CALL = "pcv_plugin_manager_shutdown("
AFTER = ("pcv_plugin_manager_init(", "g_main_loop_run(", "pcv_worker_pool_shutdown(", "pcv_rpc_offload_shutdown(",
         "pcv_grpc_server_stop(", "pcv_rest_server_stop(", "g_object_unref(server)", "g_object_unref(dispatcher)")
BEFORE = ("virt_conn_pool_shutdown(", "pcv_audit_shutdown(", "pcv_config_shutdown(", "pcv_spawn_launcher_shutdown(",
          "pcv_log_shutdown(")


def body(src: str, name: str) -> str:
                           
    m = c_blocks.masked(src)
    for b in c_blocks.scan(src)[0]:
        if b.name == name:
            return m[b.body_open:b.body_close + 1]
    raise AssertionError(f"함수 없음: {name}")


def main_problems(main_src: str) -> list[str]:
                                                    
    b = body(main_src, "main")
    n = b.count(CALL)
    if n != 1:
        return [f"main.c main(): {CALL} 출현이 {n}번이다(1번이어야 한다)"]
    pos = b.find(CALL)
    out = []
    for tok in AFTER:
        p = b.rfind(tok)
        if p < 0:
            out.append(f"main.c main(): {tok} 가 없다")
        elif not p < pos:
            out.append(f"main.c main(): {CALL} 가 {tok} 보다 앞이다(처리기를 부를 길이 아직 살아 있다)")
    for tok in BEFORE:
        p = b.rfind(tok)
        if p < 0:
            out.append(f"main.c main(): {tok} 가 없다")
        elif not pos < p:
            out.append(f"main.c main(): {CALL} 가 {tok} 보다 뒤다(플러그인 콜백이 해제된 유틸리티를 쓸 수 있다)")
    return out


def upgrade_problems(hot_src: str) -> list[str]:
                                         
    return [f"hot_reload.c _do_upgrade 가 {CALL} 를 부른다"] if CALL in body(hot_src, "_do_upgrade") else []


DEFAULT_ITER = re.compile(r"\bg_main_context_iteration\s*\(\s*(NULL|g_main_context_default\s*\(\s*\))")
DEFAULT_LOOP = re.compile(r"\bg_main_loop_new\s*\(\s*NULL\b")
LOOP_OK = {("src/main.c", "main"), ("src/core/daemon.c", "pv_init")}


def default_context_problems(sources: dict[str, str]) -> list[str]:
                                                          
    out = []
    for rel, src in sources.items():
        m = c_blocks.masked(src)
        blocks = c_blocks.scan(src)[0]

        def owner(i: int) -> str:
            for b in blocks:
                if b.body_open <= i <= b.body_close:
                    return b.name
            return "(top)"
        for hit in DEFAULT_ITER.finditer(m):
            fn = owner(hit.start())
            if not fn.endswith("_for_test"):
                out.append(f"{rel}:{fn}: 기본 컨텍스트 g_main_context_iteration")
        for hit in DEFAULT_LOOP.finditer(m):
            fn = owner(hit.start())
            if (rel, fn) not in LOOP_OK:
                out.append(f"{rel}:{fn}: 기본 컨텍스트 g_main_loop_new(NULL…)")
    return out


def src_sources() -> dict[str, str]:
    return {str(p.relative_to(ROOT)): p.read_text(errors="replace")
            for p in sorted((ROOT / "src").rglob("*.c")) if p.is_file()}


PLUGIN = r"""
#include <stdio.h>
#include <stdlib.h>
#include "modules/plugin/pcv_plugin_api.h"
static const PcvPluginMeta META = { "pcv-test-probe", "1.0", "tests", PCV_PLUGIN_ABI_VERSION };
static void mark(const char *what) {
    const char *p = getenv("PCV_TEST_PLUGIN_MARK");
    FILE *f = p ? fopen(p, "a") : NULL;
    if (f) { fprintf(f, "%s\n", what); fclose(f); }
}
static void ping(JsonObject *params, const gchar *rpc_id, gpointer server, GSocketConnection *c) {
    (void)params; (void)rpc_id; (void)server; (void)c; mark("handler");
}
const PcvPluginMeta *pcv_plugin_get_meta(void) { return &META; }
void pcv_plugin_register(PcvPluginRegistry *reg) { pcv_plugin_registry_add(reg, "pcvtestprobe.ping", ping); }
void pcv_plugin_shutdown(void) { mark("shutdown"); }
"""

DRIVER = r"""
#include <glib.h>
#include <stdio.h>
#include <gmodule.h>
#include "modules/plugin/pcv_plugin_manager.h"
void _pcv_log(GLogLevelFlags level, const gchar *domain, const gchar *fmt, ...) { (void)level; (void)domain; (void)fmt; }
void pcv_audit_log(const gchar *u, const gchar *m, const gchar *t, const gchar *r, gint e, gint64 d, const gchar *s) {
    (void)u; (void)m; (void)t; (void)r; (void)e; (void)d; (void)s;
}
/* 응답 도우미가 부르는 전송 함수 — 이 시험은 연결 없이(NULL) dispatch 하므로 불리지 않는다 */
void pure_uds_server_send_response(void *self, void *connection, const gchar *response) { (void)self; (void)connection; (void)response; }
int main(int argc, char **argv) {
    if (argc < 3) return 2;
    /* 같은 .so 를 한 번 더 열어 둔다 — 매니저가 닫아도 내려가지 않아, 반사실(base)의 정리 뒤 호출이 죽지 않고 표지로 드러난다 */
    GModule *pin = g_module_open(argv[2], G_MODULE_BIND_LOCAL);
    if (!pin) { printf("pin-failed %s\n", g_module_error()); return 3; }
    pcv_plugin_manager_init(argv[1]);
    printf("loaded=%d\n", pcv_plugin_has_handler("pcvtestprobe.ping"));
    pcv_plugin_dispatch("pcvtestprobe.ping", NULL, "1", NULL, NULL);
    pcv_plugin_manager_shutdown();
    printf("after=%d\n", pcv_plugin_has_handler("pcvtestprobe.ping"));
    fflush(stdout);
    pcv_plugin_dispatch("pcvtestprobe.ping", NULL, "2", NULL, NULL);   /* 정리 뒤: 무동작이어야 한다 */
    pcv_plugin_manager_shutdown();                                     /* 두 번째: 콜백을 다시 부르지 않아야 한다 */
    pcv_plugin_manager_init(argv[1]);                                  /* 다시 init: 다시 로딩된다 */
    printf("reloaded=%d\n", pcv_plugin_has_handler("pcvtestprobe.ping"));
    pcv_plugin_manager_shutdown();
    return 0;
}
"""


def _flags() -> list[str]:
    return shlex.split(subprocess.check_output(
        ["pkg-config", "--cflags", "--libs", "glib-2.0", "gio-2.0", "gmodule-2.0", "json-glib-1.0"], text=True))


def build(manager_src: str, workdir: Path) -> tuple[Path, Path]:
                                                                             
    cc = shutil.which("gcc-14") or "cc"
    inc = ["-I.", "-Iinclude", "-Isrc", "-Iinclude/purecvisor", "-Isrc/modules/plugin"]
    (workdir / "manager.c").write_text(manager_src)
    (workdir / "driver.c").write_text(DRIVER)
    (workdir / "probe.c").write_text(PLUGIN)
    plugdir = workdir / "plugins.d"
    plugdir.mkdir(exist_ok=True)
    plugdir.chmod(0o755)                                                  
    so = plugdir / "probe.so"
    subprocess.run([cc, "-std=gnu23", "-D_GNU_SOURCE", "-w", "-shared", "-fPIC", *inc, str(workdir / "probe.c"), "-o", str(so),
                    *_flags()], cwd=ROOT, check=True, capture_output=True, text=True, timeout=120)
    so.chmod(0o644)
    drv = workdir / "driver"
                                                                                  
    subprocess.run([cc, "-std=gnu23", "-D_GNU_SOURCE", "-w", "-rdynamic", *inc, str(workdir / "manager.c"), str(workdir / "driver.c"),
                    "src/modules/dispatcher/rpc_utils.c", "src/modules/dispatcher/rpc_completion.c",
                    "-o", str(drv), *_flags()], cwd=ROOT, check=True, capture_output=True, text=True, timeout=120)
    return drv, plugdir


def run(drv: Path, plugdir: Path, mark: Path) -> subprocess.CompletedProcess:
                                                                                
    pre = ["prlimit", "--core=1:1", "--"] if shutil.which("prlimit") else []
    return subprocess.run([*pre, str(drv), str(plugdir), str(plugdir / "probe.so")], capture_output=True, text=True, timeout=30,
                          env={**os.environ, "PCV_TEST_PLUGIN_MARK": str(mark), "G_MESSAGES_DEBUG": ""})


class PluginShutdownContract(unittest.TestCase):
    main_src = MAIN.read_text()
    hot_src = HOT.read_text()
    manager_src = MANAGER.read_text()

    def setUp(self):
        self._td = trusted_tempdir("pcv-plugin-shutdown-")
        self.addCleanup(self._td.cleanup)
        self.workdir = Path(self._td.name)

    def test_main_cleanup_order(self):
        self.assertEqual(main_problems(self.main_src), [])

    def test_main_order_counterfactuals(self):
        line = next(l for l in self.main_src.splitlines() if l.startswith("    pcv_plugin_manager_shutdown();"))
        gone = self.main_src.replace(line + "\n", "")
        self.assertTrue(main_problems(gone))
        early = gone.replace("    pcv_grpc_server_stop();", "    pcv_plugin_manager_shutdown();\n    pcv_grpc_server_stop();", 1)
        self.assertTrue(any("pcv_grpc_server_stop(" in p for p in main_problems(early)))
        head, sep, tail = gone.rpartition("    virt_conn_pool_shutdown();")
        late = head + sep + tail.replace("\n", "\n    pcv_plugin_manager_shutdown();\n", 1)
        self.assertTrue(any("virt_conn_pool_shutdown(" in p for p in main_problems(late)))

    def test_upgrade_does_not_unload(self):
        self.assertEqual(upgrade_problems(self.hot_src), [])
        bad = self.hot_src.replace("    execv(G.binary_path, argv);", "    pcv_plugin_manager_shutdown();\n    execv(G.binary_path, argv);", 1)
        self.assertNotEqual(bad, self.hot_src)
        self.assertTrue(upgrade_problems(bad))

    def test_default_context_not_iterated_after_loop(self):
        self.assertEqual(default_context_problems(src_sources()), [])
        bad = {"src/x.c": "void x_shutdown(void) { while (g_main_context_iteration(NULL, FALSE)) ; }\n",
               "src/y.c": "void y_flush_for_test(void) { g_main_context_iteration(NULL, FALSE); }\n"
                          "void y_stop(void) { GMainLoop *l = g_main_loop_new(NULL, FALSE); }\n"}
        self.assertEqual(default_context_problems(bad),
                         ["src/x.c:x_shutdown: 기본 컨텍스트 g_main_context_iteration", "src/y.c:y_stop: 기본 컨텍스트 g_main_loop_new(NULL…)"])

    def test_real_plugin_callback_exactly_once(self):
        drv, plugdir = build(self.manager_src, self.workdir)
        mark = self.workdir / "mark.txt"
        r = run(drv, plugdir, mark)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(r.stdout.split(), ["loaded=1", "after=0", "reloaded=1"])
                                                                                                     
        self.assertEqual(mark.read_text().split(), ["handler", "shutdown", "shutdown"])

    def test_base_manager_is_red(self):
                                                                              
                                                                                    
        start = self.manager_src.index("void pcv_plugin_manager_shutdown(void)\n{")
        end = self.manager_src.index("\n}\n", start) + 3
        old = ("void pcv_plugin_manager_shutdown(void)\n{\n"
               "    for (gint i = 0; i < G.plugin_count; i++) {\n"
               "        if (G.plugins[i].shutdown_fn) G.plugins[i].shutdown_fn();\n"
               "        if (G.plugins[i].module) g_module_close(G.plugins[i].module);\n"
               "    }\n    g_clear_pointer(&G.plugin_dir, g_free);\n    G.initialized = FALSE;\n}\n")
        broken = self.manager_src[:start] + old + self.manager_src[end:]
        broken = broken.replace("    if (!G.initialized || !method) return;", "    ", 1)
        drv, plugdir = build(broken, self.workdir)
        mark = self.workdir / "mark.txt"
        r = run(drv, plugdir, mark)
        got = mark.read_text().split() if mark.exists() else []
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
                                                                                    
        self.assertEqual(got, ["handler", "shutdown", "handler", "shutdown", "shutdown"])


if __name__ == "__main__":
    unittest.main()
