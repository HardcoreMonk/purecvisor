#!/usr/bin/env python3
                          
                                                                                                
                                                                                                   
                                                                                        
                                                                                                              
                                               
                                                                                      
                                                                            
                                                                                                       
                                                                                                        
 
                                                                                          
                                               
                             
from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts" / "split"))
import c_blocks              

GAUGE_API = ("pcv_prom_gauge_set_labels", "pcv_prom_gauge_set")
COUNTER_API = ("pcv_prom_source_counter_set_labels", "pcv_prom_inc", "pcv_prom_inc_labels")
HIST_SUFFIX = ("_bucket", "_sum", "_count")
                                                                              
TASK7_PENDING: set[str] = set()                    

LIT_CALL = re.compile(r"\b(pcv_prom_\w+)\s*\(\s*\"([^\"]+)\"")
FMT_SNPRINTF = re.compile(r"\bg_snprintf\s*\(\s*(\w+)\s*,[^;]*?\"([^\"]*_total)\"")
FMT_CGROUP = re.compile(r"\bpcv_cgroup_psi_format_metric\s*\([^;]*?\"(seconds_total)\"\s*,\s*(\w+)\s*,")


def _no_comments(src: str) -> str:
    kinds = c_blocks.classify(src)
    return "".join(ch if k != "m" or ch == "\n" else " " for ch, k in zip(src, kinds))


def src_sources() -> dict[str, str]:
    return {str(p.relative_to(ROOT)): p.read_text(encoding="utf-8", errors="replace")
            for p in sorted((ROOT / "src").rglob("*.c"))}


def check_total_gauges(sources: dict[str, str]) -> list[str]:
    out = []
    for rel, raw in sources.items():
        text = _no_comments(raw)
        for m in LIT_CALL.finditer(text):
            api, name = m.group(1), m.group(2)
            if api in GAUGE_API and name.endswith("_total") and name not in TASK7_PENDING:
                out.append(f"{rel}:{text.count(chr(10), 0, m.start()) + 1}: {name} 를 {api} 로 낸다(원천 counter API 를 쓴다)")
        sites = [(m.group(1), m.group(2), m.end()) for m in FMT_SNPRINTF.finditer(text)]
        sites += [(m.group(2), m.group(1), m.end()) for m in FMT_CGROUP.finditer(text)]
        for var, fmt, pos in sites:
            nxt = re.compile(r"\b(pcv_prom_\w+)\s*\(\s*" + re.escape(var) + r"\b").search(text, pos)
            if nxt and nxt.group(1) in GAUGE_API:
                out.append(f"{rel}:{text.count(chr(10), 0, pos) + 1}: 조립 이름 {fmt!r} 를 {nxt.group(1)} 로 낸다")
    return out


def check_counter_names(sources: dict[str, str]) -> list[str]:
    out = []
    for rel, raw in sources.items():
        if rel.endswith("prometheus_exporter.c"):
            continue                                                    
        for m in LIT_CALL.finditer(_no_comments(raw)):
            api, name = m.group(1), m.group(2)
            if api in COUNTER_API and not (name.endswith("_total") or name.endswith(HIST_SUFFIX)):
                out.append(f"{rel}: {api}({name!r}) — counter 이름은 _total 로 끝난다")
    return out


HEALING_AGENT = ("purecvisor_healing_actions_total", "purecvisor_healing_policy_triggered_total",
                 "purecvisor_agent_requests_total")


def check_healing_agent(sources: dict[str, str]) -> list[str]:
                                                                             
    out = []
    for rel, raw in sources.items():
        text = _no_comments(raw)
        for m in LIT_CALL.finditer(text):
            if m.group(2) in HEALING_AGENT and m.group(1) != "pcv_prom_inc_labels":
                out.append(f"{rel}: {m.group(2)} 를 {m.group(1)} 로 낸다(라벨별 증가가 아니다)")
        for m in re.finditer(r"\bpcv_prom_\w+\s*\([^;]*\bG\.total_(executed|triggered|queries)\b", text):
            out.append(f"{rel}: G.total_{m.group(1)} 가 지표 값으로 쓰인다")
    return out


def check_no_cb_failures_total(files: dict[str, str]) -> list[str]:
    return [f"{rel}: {tok}" for rel, text in files.items()
            for tok in ("purecvisor_circuit_breaker_failures_total", "cb_get_prometheus_metrics") if tok in text]


def _ui_and_src() -> dict[str, str]:
    files = src_sources()
    for p in sorted((ROOT / "ui" / "modules").glob("*.js")):
        files[str(p.relative_to(ROOT))] = p.read_text(encoding="utf-8", errors="replace")
    for name in ("app.js",):
        files["ui/" + name] = (ROOT / "ui" / name).read_text(encoding="utf-8", errors="replace")
    for p in sorted((ROOT / "src").rglob("*.h")):
        files[str(p.relative_to(ROOT))] = p.read_text(encoding="utf-8", errors="replace")
    return files


NAME_API = GAUGE_API + COUNTER_API
DIRECT_TYPE = re.compile(r"# TYPE ((?:purecvisor|node|pcv)_[a-z0-9_]+) ")
FIND_OR_CREATE = re.compile(r"\b_find_or_create\s*\(\s*\"([^\"]+)\"")
TABLE_NAME = re.compile(r"\"((?:purecvisor|node|pcv)_[a-z0-9_]+)\"")
DIRECT_FILE = "src/api/rest_public_render.c"
TABLE_FILES = ("src/modules/daemons/vm_diag_view.c",)                              


def direct_names(text: str) -> set[str]:
                                                         
    return set(DIRECT_TYPE.findall(_no_comments(text)))


def registry_names(sources: dict[str, str]) -> set[str]:
                                                                       
    out: set[str] = set()
    for rel, raw in sources.items():
        if rel == DIRECT_FILE:
            continue
        text = _no_comments(raw)
        out |= {m.group(2) for m in LIT_CALL.finditer(text) if m.group(1) in NAME_API}
        out |= set(FIND_OR_CREATE.findall(text))
        if rel in TABLE_FILES:
            out |= set(TABLE_NAME.findall(text))
    return out


def fixed_names(sources: dict[str, str]) -> set[str]:
    return registry_names(sources) | direct_names(sources.get(DIRECT_FILE, ""))


class PromNamesContract(unittest.TestCase):
    def test_direct_and_registry_names_disjoint(self):
        srcs = src_sources()
        direct = direct_names(srcs[DIRECT_FILE])
        self.assertIn("purecvisor_host_disk_percent", direct)
        self.assertIn("purecvisor_vm_diag_sample_age_seconds", direct)
        self.assertEqual(direct & registry_names(srcs), set())
                                          
        bad = srcs[DIRECT_FILE] + '\nstatic const char *x = "# TYPE purecvisor_host_cpu_percent gauge\\n";\n'
        self.assertEqual(direct_names(bad) & registry_names(srcs), {"purecvisor_host_cpu_percent"})

    def test_healing_agent_inc_labels(self):
        srcs = src_sources()
        self.assertEqual(check_healing_agent(srcs), [])
        bad = dict(srcs, **{"src/x.c": 'void f(void){ pcv_prom_gauge_set_labels("purecvisor_agent_requests_total", l,\n'
                                        '  (gdouble)(++G.total_queries)); }\n'})
        self.assertGreaterEqual(len(check_healing_agent(bad)), 2)

    def test_no_cb_failures_total(self):
        files = _ui_and_src()
        self.assertEqual(check_no_cb_failures_total(files), [])
        self.assertTrue(check_no_cb_failures_total({"ui/x.js": "k === 'purecvisor_circuit_breaker_failures_total'"}))

    def test_total_literals_use_counter_api(self):
        srcs = src_sources()
        self.assertEqual(check_total_gauges(srcs), [])
        bad = dict(srcs, **{"src/x.c": 'void f(void){ pcv_prom_gauge_set_labels("purecvisor_x_total", "", 1); }\n'})
        self.assertTrue(check_total_gauges(bad))
        bad = dict(srcs, **{"src/y.c": 'void f(void){ char m[64]; g_snprintf(m, sizeof(m), "node_%s_total", "a");\n'
                                        '  pcv_prom_gauge_set_labels(m, "", 1); }\n'})
        self.assertTrue(check_total_gauges(bad))
                          
        ok = dict(srcs, **{"src/z.c": '/* pcv_prom_gauge_set_labels("node_cpu_seconds_total", "", 1); */\n'})
        self.assertEqual(check_total_gauges(ok), [])

    def test_counter_api_names_end_total(self):
        srcs = src_sources()
        self.assertEqual(check_counter_names(srcs), [])
        bad = dict(srcs, **{"src/x.c": 'void f(void){ pcv_prom_source_counter_set_labels("purecvisor_x_bytes", "", 1); }\n'})
        self.assertTrue(check_counter_names(bad))
        ok = dict(srcs, **{"src/h.c": 'void f(void){ pcv_prom_inc("purecvisor_wait_ms_bucket", "le", "10"); }\n'})
        self.assertEqual(check_counter_names(ok), [])


if __name__ == "__main__":
    if sys.argv[1:] == ["--count-fixed-names"]:
        print(len(fixed_names(src_sources())))
        sys.exit(0)
    unittest.main(verbosity=1)
