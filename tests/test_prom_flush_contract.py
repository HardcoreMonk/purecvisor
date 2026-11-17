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
sys.path.insert(0, str(ROOT / "scripts" / "split"))
import c_blocks              

MAIN = ROOT / "src/main.c"
HOT = ROOT / "src/api/hot_reload.c"
EXPORTER = ROOT / "src/modules/daemons/prometheus_exporter.c"
FLUSH = "pcv_prom_flush("


def body(src: str, name: str) -> str:
                           
    m = c_blocks.masked(src)
    for b in c_blocks.scan(src)[0]:
        if b.name == name:
            return m[b.body_open:b.body_close + 1]
    raise AssertionError(f"함수 없음: {name}")


def _order(b: str, where: str, before: tuple[str, ...], after: tuple[str, ...]) -> list[str]:
                                                                         
                                                                                             
    out = []
    n = b.count(FLUSH)
    if n != 1:
        return [f"{where}: {FLUSH} 출현이 {n}번이다(1번이어야 한다)"]
    pos = b.find(FLUSH)
    for tok in before:
        p = b.rfind(tok)
        if p < 0:
            out.append(f"{where}: {tok} 가 없다")
        elif not p < pos:
            out.append(f"{where}: {FLUSH} 가 {tok} 보다 앞이다")
    for tok in after:
        p = b.rfind(tok)
        if p < 0:
            out.append(f"{where}: {tok} 가 없다")
        elif not pos < p:
            out.append(f"{where}: {FLUSH} 가 {tok} 보다 뒤다")
    return out


def main_problems(main_src: str) -> list[str]:
                            
    return _order(body(main_src, "main"), "main.c main()", ("g_main_loop_run(", "pcv_audit_shutdown("),
                  ("pcv_config_shutdown(", "pcv_log_shutdown("))


def upgrade_problems(hot_src: str) -> list[str]:
                                  
    return _order(body(hot_src, "_do_upgrade"), "hot_reload.c _do_upgrade", ("PCV_UPGRADE_EXECUTING", "g_setenv("),
                  ("execv(",))


def shutdown_leftovers(sources: dict[str, str]) -> list[str]:
                                                     
    return sorted(rel for rel, s in sources.items() if re.search(r"\bpcv_prom_shutdown\b", c_blocks.masked(s)))


def src_sources() -> dict[str, str]:
    out = {}
    for p in sorted((ROOT / "src").rglob("*")):
        if p.suffix in (".c", ".h") and p.is_file():
            out[str(p.relative_to(ROOT))] = p.read_text(errors="replace")
    return out


DRIVER = r"""
#include <glib.h>
#include <stdio.h>
#include <string.h>
#include "modules/daemons/prometheus_exporter.h"
void _pcv_log(GLogLevelFlags level, const gchar *domain, const gchar *fmt, ...) { (void)level; (void)domain; (void)fmt; }
/* argv[1]: "write" = 초기화 전 flush(no-op) → init → inc 3 → flush → inc 1 → flush ×2 → inc 1(저장 안 함) → 렌더 출력
 *          "read"  = init(파일 복원) → 렌더 출력 */
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    if (strcmp(argv[1], "write") == 0) {
        pcv_prom_flush();
        if (g_file_test(argv[2], G_FILE_TEST_EXISTS)) { fputs("flush-before-init-wrote\n", stdout); return 3; }
        pcv_prom_init();
        for (int i = 0; i < 3; i++) pcv_prom_inc("purecvisor_flush_probe_total", "k", "a");
        pcv_prom_flush();
        pcv_prom_inc("purecvisor_flush_probe_total", "k", "a");
        pcv_prom_flush();
        pcv_prom_flush();
        pcv_prom_inc("purecvisor_flush_probe_total", "k", "a");
    } else {
        pcv_prom_init();
    }
    gchar *out = pcv_prom_render();
    fputs(out, stdout);
    g_free(out);
    return 0;
}
"""


def build(source: str, workdir: Path) -> Path:
                                                                                    
    patched = re.sub(r'(#define PCV_PROM_CHECKPOINT_PATH) .*', rf'\1 "{workdir}/prom_counters.json"', source)
    patched = patched.replace('g_mkdir_with_parents("/var/lib/purecvisor", 0755)', f'g_mkdir_with_parents("{workdir}", 0755)')
    (workdir / "exporter.c").write_text(patched)
    (workdir / "driver.c").write_text(DRIVER)
    flags = shlex.split(subprocess.check_output(
        ["pkg-config", "--cflags", "--libs", "glib-2.0", "gio-2.0", "json-glib-1.0"], text=True))
    binary = workdir / "flush"
    cmd = [shutil.which("gcc-14") or "cc", "-std=gnu23", "-D_GNU_SOURCE", "-w", "-I.", "-Iinclude", "-Isrc", "-Iinclude/purecvisor",
           "-Isrc/modules/daemons", str(workdir / "exporter.c"), str(ROOT / "src/modules/daemons/prom_labels.c"),
           str(workdir / "driver.c"), "-o", str(binary), *flags]
    subprocess.run(cmd, cwd=ROOT, check=True, capture_output=True, text=True, timeout=120)
    return binary


def run(binary: Path, mode: str, workdir: Path) -> subprocess.CompletedProcess:
    return subprocess.run([str(binary), mode, str(workdir / "prom_counters.json")], capture_output=True, text=True,
                          timeout=30, env={**os.environ, "G_MESSAGES_DEBUG": ""})


def probe_value(text: str) -> float | None:
    m = re.search(r'^purecvisor_flush_probe_total\{k="a"\} ([0-9.]+)$', text, re.M)
    return float(m.group(1)) if m else None


class PromFlushContract(unittest.TestCase):
    main_src = MAIN.read_text()
    hot_src = HOT.read_text()
    exporter_src = EXPORTER.read_text()

    def setUp(self):
        self._td = tempfile.TemporaryDirectory(prefix="pcv-prom-flush-")
        self.addCleanup(self._td.cleanup)
        self.workdir = Path(self._td.name)

    def test_main_cleanup_order(self):
        self.assertEqual(main_problems(self.main_src), [])

    def test_main_order_counterfactuals(self):
                                                         
        call = "    pcv_prom_flush();"
        self.assertEqual(self.main_src.count(call), 1)
        line = next(l for l in self.main_src.splitlines() if l.startswith(call))
        gone = self.main_src.replace(line + "\n", "")
        self.assertTrue(main_problems(gone))
        early = gone.replace("    pcv_job_queue_shutdown();", "    pcv_prom_flush();\n    pcv_job_queue_shutdown();", 1)
        self.assertTrue(any("pcv_audit_shutdown(" in p for p in main_problems(early)))
        head, sep, tail = gone.rpartition("    pcv_log_shutdown();")
        late = head + sep + tail.replace("\n", "\n    pcv_prom_flush();\n", 1)
        self.assertTrue(any("pcv_log_shutdown(" in p for p in main_problems(late)))
        twice = self.main_src.replace("    pcv_spawn_launcher_shutdown();", "    pcv_prom_flush();\n    pcv_spawn_launcher_shutdown();", 1)
        self.assertTrue(any("2번" in p for p in main_problems(twice)))

    def test_upgrade_flush_before_exec(self):
        self.assertEqual(upgrade_problems(self.hot_src), [])

    def test_upgrade_counterfactuals(self):
        gone = re.sub(r"\n    pcv_prom_flush\(\);\n", "\n", self.hot_src, count=1)
        self.assertNotEqual(gone, self.hot_src)
        self.assertTrue(upgrade_problems(gone))
        after = gone.replace("    execv(G.binary_path, argv);", "    execv(G.binary_path, argv);\n    pcv_prom_flush();", 1)
        self.assertTrue(any("execv(" in p for p in upgrade_problems(after)))

    def test_no_teardown_symbol(self):
        self.assertEqual(shutdown_leftovers(src_sources()), [])
        bad = {"src/x.c": "/* 옛 pcv_prom_shutdown 은 지웠다 */\nvoid f(void){ pcv_prom_shutdown(); }\n"}
        self.assertEqual(shutdown_leftovers(bad), ["src/x.c"])
        self.assertEqual(shutdown_leftovers({"src/y.c": "/* pcv_prom_shutdown 주석만 */\n"}), [])

    def test_flush_saves_and_next_process_restores(self):
        binary = build(self.exporter_src, self.workdir)
        w = run(binary, "write", self.workdir)
        self.assertEqual(w.returncode, 0, w.stdout + w.stderr)
        self.assertEqual(probe_value(w.stdout), 5.0)                       
        data = json.loads((self.workdir / "prom_counters.json").read_text())
        saved = [c for c in data["counters"] if c["name"] == "purecvisor_flush_probe_total"]
        self.assertEqual(saved, [{"name": "purecvisor_flush_probe_total", "labels": 'k="a"', "value": 4.0}])
        self.assertFalse((self.workdir / "prom_counters.json.tmp").exists())
        r = run(binary, "read", self.workdir)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(probe_value(r.stdout), 4.0)                                 

    def test_base_without_flush_is_red(self):
                                                                        
        start = self.exporter_src.index("void pcv_prom_flush(void)")
        end = self.exporter_src.index("\n}\n", start) + 3
        broken = self.exporter_src[:start] + self.exporter_src[end:]
        with self.assertRaises(subprocess.CalledProcessError):
            build(broken, self.workdir)


if __name__ == "__main__":
    unittest.main()
