#!/usr/bin/env python3
                          
                                                                                                
                                                                                                 
                                                                                       
                                                                                                  
                                                          
                                                                                       
                                                                                                          
                                                                                        
                                
                                                                                    
                                                                                        
                                                                                              
                                                                                             
                                                                               
                                                                  
 
                      
                                                                       
                             
from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "scripts" / "split"))
import test_plugin_api_export_contract as exp                                          
from plugin_trusted_tmp import chain_problem, trusted_tempdir              

MANAGER = Path(os.environ.get("PCV_PLUGIN_MANAGER_SRC") or ROOT / "src/modules/plugin/pcv_plugin_manager.c")

DRIVER = r"""
#include <glib.h>
#include <gio/gio.h>
#include <link.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "modules/plugin/pcv_plugin_manager.h"
#include "api/uds_server.h"
void _pcv_log(GLogLevelFlags level, const gchar *domain, const gchar *fmt, ...) {
    (void)domain; va_list ap; va_start(ap, fmt); gchar *s = g_strdup_vprintf(fmt, ap); va_end(ap);
    printf("LOG %s %s\n", level == G_LOG_LEVEL_WARNING ? "WARN" : "INFO", s); g_free(s);
}
void pcv_audit_log(const gchar *u, const gchar *m, const gchar *t, const gchar *r, gint e, gint64 d, const gchar *s) {
    (void)u; (void)d; (void)s; printf("AUDIT %s %s %d %s\n", m, r, e, t ? t : "");
}
void pure_uds_server_send_response(UdsServer *self, GSocketConnection *connection, const gchar *response) {
    (void)self; (void)connection; (void)response;
}
static int phdr(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size; (void)data;
    if (info->dlpi_name && strncmp(info->dlpi_name, "/proc/self/fd/", 14) == 0) printf("PHDR %s\n", info->dlpi_name);
    return 0;
}
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    pcv_plugin_manager_init(argv[1]);
    printf("HAS echo=%d\n", pcv_plugin_has_handler("pcvtestapi.echo"));
    JsonArray *a = pcv_plugin_list();
    printf("COUNT %u\n", json_array_get_length(a));
    for (guint i = 0; i < json_array_get_length(a); i++)
        printf("NAME %s\n", json_object_get_string_member(json_array_get_object_element(a, i), "name"));
    json_array_unref(a);
    dl_iterate_phdr(phdr, NULL);   /* 검사한 fd 로 연 객체(로딩된 것 + 상주하는 거절 .so)는 /proc/self/fd/N 이름이다 */
    pcv_plugin_manager_shutdown();
    return 0;
}
"""


def build_driver(manager_src: str, workdir: Path) -> Path:
    workdir.mkdir(parents=True, exist_ok=True)
    (workdir / "manager.c").write_text(manager_src)
    (workdir / "driver.c").write_text(DRIVER)
    drv = workdir / "driver"
    exp.compile_or_fail([exp._cc(), "-std=gnu23", "-D_GNU_SOURCE", "-w", *exp.INC, f"-Wl,--dynamic-list={exp.LIST}",
                         str(workdir / "manager.c"), "src/modules/dispatcher/rpc_utils.c", "src/modules/dispatcher/rpc_completion.c",
                         str(workdir / "driver.c"), "-o", str(drv), *exp._flags()])
    return drv


def run(drv: Path, plugdir: Path) -> dict:
    pre = ["prlimit", "--core=1:1", "--"] if shutil.which("prlimit") else []
    r = subprocess.run([*pre, str(drv), str(plugdir)], capture_output=True, text=True, timeout=60,
                       env={**os.environ, "G_MESSAGES_DEBUG": ""})
    out = r.stdout
    audit = [l[len("AUDIT plugin.load "):] for l in out.splitlines() if l.startswith("AUDIT plugin.load ")]
    has = re.search(r"HAS echo=(\d)", out)
    cnt = re.search(r"COUNT (\d+)", out)
    return {"rc": r.returncode, "out": out + r.stderr, "audit": audit, "has": int(has.group(1)) if has else -1,
            "count": int(cnt.group(1)) if cnt else -1,
            "names": sorted(l[5:] for l in out.splitlines() if l.startswith("NAME ")),
            "phdr": [l[5:] for l in out.splitlines() if l.startswith("PHDR ")]}


class PluginTrustContract(unittest.TestCase):
    manager_text = MANAGER.read_text()

    @classmethod
    def setUpClass(cls):
        cls._td = trusted_tempdir("pcv-plugin-trust-")
        cls.base = Path(cls._td.name)
        cls.drv = build_driver(cls.manager_text, cls.base / "drv")
        cls.so = exp.build_probe(cls.base, "src-probe")                           
        cls.so_empty = exp.build_probe(cls.base, "src-empty", ('-DPCV_PROBE_NAME=""',))
        cls.so_null = exp.build_probe(cls.base, "src-null", ("-DPCV_PROBE_NAME=NULL",))
        cls.so_second = exp.build_probe(cls.base, "src-second", ('-DPCV_PROBE_NAME="second"',))
                                                                                                                 
        cls.so_res_abi = exp.build_probe(cls.base, "src-res-abi", ("-DPCV_PROBE_ABI=99", "-Wl,-z,nodelete"))
        cls.so_res_name = exp.build_probe(cls.base, "src-res-name", ('-DPCV_PROBE_NAME=""', "-Wl,-z,nodelete"))

    @classmethod
    def tearDownClass(cls):
        cls._td.cleanup()

    def setUp(self):
        self.case = Path(tempfile.mkdtemp(prefix="case-", dir=self.base))                  
        self.addCleanup(shutil.rmtree, self.case, True)

    def plugdir(self, mode=0o755, parent: Path | None = None, so_mode=0o644, src: Path | None = None, extra: int = 0) -> Path:
        d = (parent or self.case) / "plugins.d"
        d.mkdir(parents=True)
        shutil.copy(src or self.so, d / "probe.so")
        (d / "probe.so").chmod(so_mode)
        for i in range(extra):                                                  
            shutil.copy(self.so, d / f"probe{i + 2}.so")
            (d / f"probe{i + 2}.so").chmod(0o644)
        d.chmod(mode)
        return d

    def assertRejected(self, r: dict, code: int, frag: str):
        self.assertEqual(r["rc"], 0, r["out"])
        self.assertEqual((r["has"], r["count"]), (0, 0), r["out"])
        hits = [a for a in r["audit"] if a.startswith(f"fail {code} ") and frag in a]
        self.assertTrue(hits, f"감사 fail {code}·'{frag}' 없음: {r['audit']}\n{r['out']}")

                                                                    
    def test_trusted_base_chain(self):
        self.assertIsNone(chain_problem(self.case))

    def test_normal_loads(self):
        r = run(self.drv, self.plugdir())
        self.assertEqual((r["rc"], r["has"], r["count"]), (0, 1, 1), r["out"])
        self.assertIn("ok 0 pcv-test-api", r["audit"], r["out"])

    def test_dir_symlink_to_trusted_dir_loads(self):
        real = self.plugdir()
        link = self.case / "via-link"
        link.symlink_to(real)
        r = run(self.drv, link)                                            
        self.assertEqual((r["has"], r["count"]), (1, 1), r["out"])

    def test_missing_dir_is_unchanged(self):
        r = run(self.drv, self.case / "nope")
        self.assertEqual((r["rc"], r["has"], r["audit"]), (0, 0, []), r["out"])
        self.assertIn("unavailable", r["out"])

                                                                      
    def test_so_group_writable_rejected(self):
        self.assertRejected(run(self.drv, self.plugdir(so_mode=0o664)), 8, "group/other writable (mode 0664)")

    def test_so_other_writable_rejected(self):
        self.assertRejected(run(self.drv, self.plugdir(so_mode=0o646)), 8, "group/other writable (mode 0646)")

    def test_so_symlink_rejected(self):
        d = self.plugdir()
        real = self.case / "real.so"
        (d / "probe.so").rename(real)
        (d / "probe.so").symlink_to(real)
        self.assertRejected(run(self.drv, d), 6, "probe.so")

    def test_one_bad_so_does_not_block_others(self):
        d = self.plugdir(so_mode=0o664, extra=1)                              
        r = run(self.drv, d)
        self.assertEqual((r["has"], r["count"]), (1, 1), r["out"])
        self.assertTrue(any(a.startswith("fail 8 ") for a in r["audit"]), r["audit"])

                                                                
    def test_dir_other_writable_rejects_all(self):
        self.assertRejected(run(self.drv, self.plugdir(mode=0o757, extra=1)), 7, "plugins.d: group/other writable (mode 0757)")

    def test_dir_group_writable_rejects_all(self):
        self.assertRejected(run(self.drv, self.plugdir(mode=0o775, extra=1)), 7, "plugins.d: group/other writable (mode 0775)")

    def test_parent_component_other_writable_rejected(self):
        parent = self.case / "parent"
        parent.mkdir()
        d = self.plugdir(parent=parent)
        parent.chmod(0o757)
        self.assertRejected(run(self.drv, d), 7, f"{parent}: group/other writable (mode 0757)")

    def test_dir_symlink_to_untrusted_location_rejected(self):
                                                                       
        with tempfile.TemporaryDirectory(prefix="pcv-plugin-untrusted-", dir="/tmp") as t:
            real = Path(t) / "plugins.d"
            real.mkdir()
            real.chmod(0o755)
            shutil.copy(self.so, real / "probe.so")
            (real / "probe.so").chmod(0o644)
            link = self.case / "link"
            link.symlink_to(real)
            self.assertRejected(run(self.drv, link), 7, "/tmp: group/other writable (mode 1777)")

                                                                 
    def test_empty_name_rejected(self):
        self.assertRejected(run(self.drv, self.plugdir(src=self.so_empty)), 9, "plugin name is empty")

    def test_null_name_rejected(self):
        self.assertRejected(run(self.drv, self.plugdir(src=self.so_null)), 9, "plugin name is NULL")


                                                              
    def place(self, files: list[tuple[str, Path]]) -> Path:
                                                                                           
        d = self.case / f"plugins-{len(list(self.case.iterdir()))}.d"
        d.mkdir()
        for name, src in files:
            shutil.copy(src, d / name)
            (d / name).chmod(0o644)
        d.chmod(0o755)
        return d

    def test_loaded_plugin_is_opened_by_checked_fd(self):
        r = run(self.drv, self.plugdir())
        self.assertEqual(r["count"], 1, r["out"])
        self.assertEqual(len(r["phdr"]), 1, f"로딩된 플러그인 이름이 /proc/self/fd/N 이어야 한다(검사한 fd 로 연다): {r['out']}")

    def test_two_different_plugins_both_load(self):
        for order in ((("a.so", self.so), ("b.so", self.so_second)), (("a.so", self.so_second), ("b.so", self.so))):
            with self.subTest(order=[n for n, _ in order]):
                r = run(self.drv, self.place(list(order)))
                self.assertEqual((r["count"], r["names"]), (2, ["pcv-test-api", "second"]), r["out"])
                self.assertIn("ok 0 pcv-test-api", r["audit"], r["out"])
                self.assertIn("ok 0 second", r["audit"], r["out"])

    def test_resident_rejected_so_does_not_capture_later_plugins(self):
        for bad, code in ((self.so_res_abi, 4), (self.so_res_name, 9)):
            for label, files in (("bad-first", [("bad.so", bad), ("g1.so", self.so), ("g2.so", self.so_second)]),
                                 ("bad-last", [("g1.so", self.so), ("g2.so", self.so_second), ("bad.so", bad)])):
                with self.subTest(code=code, created=label):
                    r = run(self.drv, self.place(files))
                    self.assertEqual((r["count"], r["names"]), (2, ["pcv-test-api", "second"]), r["out"])
                    self.assertTrue(any(a.startswith(f"fail {code} ") and "bad.so" in a for a in r["audit"]), r["audit"])
                                                                                   
                    self.assertEqual(len(r["phdr"]), 3, f"상주 거절 .so 가 link_map 에 남아야 시험이 뜻을 가진다: {r['out']}")

    def test_origin_runpath_is_not_resolved(self):
                                                                                                 
                                                                 
        d = self.case / "plugins.d"
        (d / "lib").mkdir(parents=True)
        (self.case / "dep.c").write_text("int pcvtest_dep_value(void) { return 7; }\n")
        exp.compile_or_fail([exp._cc(), "-shared", "-fPIC", str(self.case / "dep.c"), "-o", str(d / "lib" / "libpcvtestdep.so")])
        (self.case / "origin.c").write_text(
            '#include "modules/plugin/pcv_plugin_api.h"\n'
            "extern int pcvtest_dep_value(void);\n"
            'static PcvPluginMeta META = { "origin", "1.0", "tests", PCV_PLUGIN_ABI_VERSION };\n'
            "const PcvPluginMeta *pcv_plugin_get_meta(void) { return pcvtest_dep_value() == 7 ? &META : NULL; }\n"
            "void pcv_plugin_register(PcvPluginRegistry *reg) { (void)reg; }\n")
        flags = exp._flags()
        for rpath, so in (("$ORIGIN/lib", d / "origin.so"), (str(d / "lib"), self.case / "abs.so")):
            exp.compile_or_fail([exp._cc(), "-std=gnu23", "-shared", "-fPIC", "-Isrc", str(self.case / "origin.c"), "-o", str(so),
                                 f"-L{d / 'lib'}", "-lpcvtestdep", f"-Wl,-rpath,{rpath}", "-Wl,--enable-new-dtags", *flags])
            so.chmod(0o644)
        (d / "lib").chmod(0o755)
        (d / "lib" / "libpcvtestdep.so").chmod(0o644)
        d.chmod(0o755)
        r = run(self.drv, d)
        self.assertEqual(r["count"], 0, r["out"])
        self.assertTrue(any(a.startswith("fail 2 ") and a.endswith("origin.so") for a in r["audit"]), r["audit"])
        self.assertIn("libpcvtestdep.so", r["out"])
        (d / "origin.so").unlink()
        shutil.copy(self.case / "abs.so", d / "abs.so")
        (d / "abs.so").chmod(0o644)
        r = run(self.drv, d)
        self.assertEqual((r["count"], r["names"]), (1, ["origin"]), r["out"])

                                                            
    def mutated(self, old: str, new: str) -> Path:
        self.assertIn(old, self.manager_text)
        return build_driver(self.manager_text.replace(old, new), self.case / f"drv-m{abs(hash(old)) % 10000}")

    def test_counterfactual_open_by_path_is_red(self):
        drv = self.mutated("    GModule *mod = g_module_open(modpath, G_MODULE_BIND_LOCAL);",
                           "    GModule *mod = g_module_open(path, G_MODULE_BIND_LOCAL);")
        r = run(drv, self.plugdir())
        self.assertEqual((r["count"], r["phdr"]), (1, []), r["out"])                                 

    def test_counterfactual_success_fd_closed_is_red(self):
        drv = self.mutated("    G.plugins[idx].fd = fd;", "    G.plugins[idx].fd = -1; close(fd);")
        r = run(drv, self.place([("a.so", self.so), ("b.so", self.so_second)]))
        self.assertNotEqual(r["names"], ["pcv-test-api", "second"], r["out"])                     

    def test_counterfactual_reject_fd_closed_is_red(self):
        drv = self.mutated("        _hold_fd(fd);", "        close(fd);")
        r = run(drv, self.place([("bad.so", self.so_res_abi), ("g1.so", self.so), ("g2.so", self.so_second)]))
        r2 = run(drv, self.place([("g1.so", self.so), ("g2.so", self.so_second), ("bad.so", self.so_res_abi)]))
        self.assertTrue(r["count"] < 2 or r2["count"] < 2, r["out"] + r2["out"])                              

                                                                   
    def test_counterfactual_without_file_check_loads_writable_so(self):
                                                                     
        old = "    gchar *untrusted = _trust_problem(&fst);\n"
        self.assertIn(old, self.manager_text)
        broken = self.manager_text.replace(old, "    gchar *untrusted = NULL;\n", 1)
        drv = build_driver(broken, self.case / "drv-broken")
        r = run(drv, self.plugdir(so_mode=0o664))
        self.assertEqual((r["has"], r["count"]), (1, 1), r["out"])

    def test_counterfactual_without_dir_check_loads_from_writable_dir(self):
                                                      
        old = "    gchar *problem = _dir_chain_problem(G.plugin_dir, &real);\n"
        self.assertIn(old, self.manager_text)
        broken = self.manager_text.replace(old, "    gchar *problem = NULL; real = g_strdup(G.plugin_dir);\n", 1)
        drv = build_driver(broken, self.case / "drv-broken")
        r = run(drv, self.plugdir(mode=0o757))
        self.assertEqual((r["has"], r["count"]), (1, 1), r["out"])


if __name__ == "__main__":
    unittest.main()
