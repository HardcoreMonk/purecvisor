#!/usr/bin/env python3
                          
                                                                                                      
                                                                                   
                                                                                        
                                                                                      
                                                                                   
                                                                   
                                                                      
 
                                                                             
                                                               
                                                    
from __future__ import annotations

import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXPORTER = ROOT / "src/modules/daemons/prometheus_exporter.c"

DRIVER = r"""
#include <glib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "modules/daemons/prometheus_exporter.h"
void _pcv_log(GLogLevelFlags level, const gchar *domain, const gchar *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "LOG[%s] ", (level & G_LOG_LEVEL_WARNING) ? "WARN" : "INFO");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    (void)domain;
}
static void render(void) { gchar *o = pcv_prom_render(); fputs(o, stdout); g_free(o); }
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    const char *m = argv[1];
    pcv_prom_init();
    if (!strcmp(m, "type")) {
        pcv_prom_source_counter_set_labels("x_total", "vm=\"a\"", 5);
        render();
    } else if (!strcmp(m, "flush")) {
        pcv_prom_source_counter_set_labels("src_total", "k=\"a\"", 7);
        pcv_prom_inc("inc_total", "k", "a");
        pcv_prom_gauge_set_labels("g_now", "", 3);
        pcv_prom_flush();
        render();
    } else if (!strcmp(m, "sweep")) {
        pcv_prom_source_counter_set_labels("old_src_total", "vm=\"old\"", 1);
        pcv_prom_gauge_set_labels("old_gauge", "vm=\"old\"", 1);
        pcv_prom_source_counter_set_labels("host_src_total", "", 9);
        pcv_prom_inc("old_inc_total", "vm", "old");
        g_usleep(300000);
        render();
    } else if (!strcmp(m, "group")) {
        pcv_prom_gauge_set_labels("a", "x=\"1\"", 1);
        pcv_prom_gauge_set_labels("b", "", 2);
        pcv_prom_gauge_set_labels("a", "x=\"2\"", 3);
        pcv_prom_gauge_set_labels("c", "", 4);
        pcv_prom_gauge_set_labels("b", "y=\"1\"", 5);
        render();
    } else if (!strcmp(m, "mismatch")) {
        pcv_prom_gauge_set_labels("mixed_total", "k=\"a\"", 1);
        pcv_prom_source_counter_set_labels("mixed_total", "k=\"a\"", 2);
        pcv_prom_source_counter_set_labels("mixed_total", "k=\"a\"", 3);
        render();
    } else if (!strcmp(m, "restore")) {   /* 체크포인트 파일(시험이 씀)을 복원한 슬롯 — 청소·저장 동작으로 종류를 본다 */
        g_usleep(300000);
        pcv_prom_flush();
        render();
    } else if (!strcmp(m, "escape")) {   /* exporter 내부 라벨 조립의 값 이스케이프 */
        pcv_prom_inc("purecvisor_x_total", "method", "a\"b\\c\nd");
        pcv_prom_gauge_set("purecvisor_y", "dev", "q\"w", 1);
        pcv_prom_rpc_end("vm.\"list", 1, 2.5);
        pcv_prom_zfs_inflight_lock_observe("p", "o\"p", "ok", 5);
        pcv_prom_inc("purecvisor_z_total", NULL, NULL);
        render();
    } else if (!strcmp(m, "inc_labels")) {
#ifdef PCV_HAVE_INC_LABELS
        pcv_prom_inc_labels("purecvisor_healing_actions_total", "policy=\"a\",action=\"r\"");
        pcv_prom_inc_labels("purecvisor_healing_actions_total", "policy=\"a\",action=\"r\"");
        pcv_prom_inc_labels("purecvisor_healing_actions_total", "policy=\"b\",action=\"r\"");
        pcv_prom_flush();
        render();
#else
        return 4;
#endif
    } else {
        return 2;
    }
    return 0;
}
"""


def build(source: str, workdir: Path, defines: tuple[str, ...] = ()) -> Path:
                                                                        
    patched = re.sub(r'(#define PCV_PROM_CHECKPOINT_PATH) .*', rf'\1 "{workdir}/prom_counters.json"', source)
    patched = re.sub(r'(#define PCV_PROM_STALE_TTL_US) .*', r'\1 (200 * 1000)', patched)
    patched = patched.replace('g_mkdir_with_parents("/var/lib/purecvisor", 0755)', f'g_mkdir_with_parents("{workdir}", 0755)')
    (workdir / "exporter.c").write_text(patched)
    (workdir / "driver.c").write_text(DRIVER)
    flags = shlex.split(subprocess.check_output(
        ["pkg-config", "--cflags", "--libs", "glib-2.0", "gio-2.0", "json-glib-1.0"], text=True))
    binary = workdir / "registry"
    cmd = [shutil.which("gcc-14") or "cc", "-std=gnu23", "-D_GNU_SOURCE", "-w", *[f"-D{d}" for d in defines],
           "-I.", "-Iinclude", "-Isrc", "-Iinclude/purecvisor", "-Isrc/modules/daemons",
           str(workdir / "exporter.c"), str(ROOT / "src/modules/daemons/prom_labels.c"), str(workdir / "driver.c"),
           "-o", str(binary), *flags]
    subprocess.run(cmd, cwd=ROOT, check=True, capture_output=True, text=True, timeout=180)
    return binary


def run(binary: Path, mode: str) -> subprocess.CompletedProcess:
    return subprocess.run([str(binary), mode], capture_output=True, text=True, timeout=30,
                          env={**os.environ, "G_MESSAGES_DEBUG": ""})


def series(text: str) -> list[str]:
    return [l for l in text.splitlines() if l and not l.startswith("#")]


def types(text: str) -> dict[str, list[str]]:
    out: dict[str, list[str]] = {}
    for l in text.splitlines():
        m = re.match(r"# TYPE (\S+) (\S+)$", l)
        if m:
            out.setdefault(m.group(1), []).append(m.group(2))
    return out


def saved(workdir: Path) -> list[tuple[str, str, float]]:
    data = json.loads((workdir / "prom_counters.json").read_text())
    return [(c["name"], c["labels"], c["value"]) for c in data["counters"]]


class PromRegistryContract(unittest.TestCase):
    src = EXPORTER.read_text()
    _bin: Path | None = None

    @classmethod
    def setUpClass(cls):
        cls._td = tempfile.TemporaryDirectory(prefix="pcv-prom-reg-")
        cls.workdir = Path(cls._td.name)
        cls.binary = build(cls.src, cls.workdir, ("PCV_HAVE_INC_LABELS",))

    @classmethod
    def tearDownClass(cls):
        cls._td.cleanup()

    def setUp(self):
        ck = self.workdir / "prom_counters.json"
        if ck.exists():
            ck.unlink()

    def test_source_counter_type_counter(self):
        r = run(self.binary, "type")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(types(r.stdout).get("x_total"), ["counter"])
        self.assertIn('x_total{vm="a"} 5', r.stdout)

    def test_source_counter_not_checkpointed(self):
        r = run(self.binary, "flush")
        self.assertEqual(r.returncode, 0, r.stderr)
        names = {n for n, _, _ in saved(self.workdir)}
        self.assertIn("inc_total", names)                                     
        self.assertNotIn("src_total", names)                              
        self.assertNotIn("g_now", names)

    def test_source_counter_vm_label_swept(self):
                                                                                                  
        r = run(self.binary, "sweep")
        self.assertEqual(r.returncode, 0, r.stderr)
        s = series(r.stdout)
        self.assertFalse([l for l in s if l.startswith("old_src_total")], r.stdout)
        self.assertFalse([l for l in s if l.startswith("old_gauge")], r.stdout)
        self.assertIn("host_src_total 9", s)
        self.assertIn('old_inc_total{vm="old"} 1', s)

    def test_sweep_counterfactual_red(self):
                                                                                      
        old = "if (G.metrics[i].kind == PCV_PROM_KIND_COUNTER) continue;"
        self.assertEqual(self.src.count(old), 1)
        bad = self.src.replace(old, "if (G.metrics[i].kind != PCV_PROM_KIND_GAUGE) continue;", 1)
        with tempfile.TemporaryDirectory(prefix="pcv-prom-reg-cf-") as td:
            b = build(bad, Path(td))
            r = run(b, "sweep")
            self.assertTrue([l for l in series(r.stdout) if l.startswith("old_src_total")], r.stdout)

    def test_render_grouped(self):
        r = run(self.binary, "group")
        self.assertEqual(r.returncode, 0, r.stderr)
        lines = [l for l in r.stdout.splitlines() if not l.startswith("purecvisor_info") and "purecvisor_info" not in l]
        self.assertEqual(lines, ["# TYPE a gauge", 'a{x="1"} 1', 'a{x="2"} 3', "# TYPE b gauge", "b 2", 'b{y="1"} 5',
                                 "# TYPE c gauge", "c 4"])
        self.assertTrue(all(len(v) == 1 for v in types(r.stdout).values()))

    def test_kind_mismatch_warn(self):
        r = run(self.binary, "mismatch")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(types(r.stdout).get("mixed_total"), ["gauge"])            
        self.assertIn('mixed_total{k="a"} 3', r.stdout)
        warns = [l for l in r.stderr.splitlines() if l.startswith("LOG[WARN]") and "kind mismatch" in l]
        self.assertEqual(len(warns), 1, r.stderr)                                     

    def test_restore_creates_counter_only(self):
                                                                                   
        (self.workdir / "prom_counters.json").write_text(json.dumps(
            {"saved_at": 1, "counters": [{"name": "r_total", "labels": 'vm="x"', "value": 4.0}]}))
        r = run(self.binary, "restore")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(types(r.stdout).get("r_total"), ["counter"])
        self.assertIn('r_total{vm="x"} 4', r.stdout)
        self.assertIn(("r_total", 'vm="x"', 4.0), saved(self.workdir))

    def test_inc_labels_per_label_checkpointed(self):
                                                               
        r = run(self.binary, "inc_labels")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('purecvisor_healing_actions_total{policy="a",action="r"} 2', r.stdout)
        self.assertIn('purecvisor_healing_actions_total{policy="b",action="r"} 1', r.stdout)
        self.assertEqual(types(r.stdout).get("purecvisor_healing_actions_total"), ["counter"])
        got = {(n, l): v for n, l, v in saved(self.workdir)}
        self.assertEqual(got.get(("purecvisor_healing_actions_total", 'policy="a",action="r"')), 2.0)
        self.assertEqual(got.get(("purecvisor_healing_actions_total", 'policy="b",action="r"')), 1.0)

    def test_exporter_labels_escaped(self):
                                                                                               
        r = run(self.binary, "escape")
        self.assertEqual(r.returncode, 0, r.stderr)
        out = r.stdout
        self.assertIn('purecvisor_x_total{method="a\\"b\\\\c\\nd"} 1', out)
        self.assertIn('purecvisor_y{dev="q\\"w"} 1', out)
        self.assertIn('purecvisor_rpc_requests_total{method="vm.\\"list",status="ok"} 1', out)
        self.assertIn('purecvisor_rpc_duration_ms{method="vm.\\"list"} 2.5', out)
        self.assertIn('purecvisor_zfs_inflight_lock_wait_ms_bucket{op="o\\"p",result="ok",le="10"} 1', out)
        self.assertIn('purecvisor_zfs_inflight_lock_wait_ms_bucket{op="o\\"p",result="ok",le="+Inf"} 1', out)
        self.assertIn("purecvisor_z_total 1", out)                                     
        sys.path.insert(0, str(ROOT / "scripts"))
        import check_prom_exposition as cpe              
        self.assertEqual(cpe.check(out), [])

    def test_render_counterfactual_red(self):
                                                                        
        start = self.src.index("    for (guint n = 0; n < order->len; n++) {")
        end = self.src.index("    g_ptr_array_unref(order);", start)
        flat = ('    for (gint i = 0; i < G.count; i++) { PromMetric *m = &G.metrics[i];\n'
                '        if (m->labels[0] && m->labels[0] != \'=\') g_string_append_printf(buf, "%s{%s} %.17g\\n", m->name, m->labels, m->value);\n'
                '        else g_string_append_printf(buf, "%s %.17g\\n", m->name, m->value); }\n')
        bad = self.src[:start] + flat + self.src[end:]
        with tempfile.TemporaryDirectory(prefix="pcv-prom-reg-cf-") as td:
            b = build(bad, Path(td))
            r = run(b, "group")
            lines = [l for l in r.stdout.splitlines() if "purecvisor_info" not in l]
            self.assertNotEqual(lines[:3], ["# TYPE a gauge", 'a{x="1"} 1', 'a{x="2"} 3'])


if __name__ == "__main__":
    unittest.main(verbosity=1)
