#!/usr/bin/env python3
                          
                                                                                              
                                                                                                            
                                                                                                            
                                                
                                                                                                         
                                                                                              
                                                                                                       
                                                                                                 
                                                                                                          
                                                                                   
                                                                                  
                                                                                  
                                                                         
                                                                                                               
                                                                   
                                                                                                 
                                                                  
 
                      
                                                                             
                                            
                          
from __future__ import annotations

import json
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

LIST = ROOT / "src/modules/plugin/pcv_plugin_exports.list"
API_H = ROOT / "src/modules/plugin/pcv_plugin_api.h"
MANAGER = ROOT / "src/modules/plugin/pcv_plugin_manager.c"
MAKEFILE = ROOT / "Makefile"
PROBE = ROOT / "tests/fixtures/plugin_api_probe.c"
DAEMON = Path(os.environ.get("PCV_DAEMON_BIN") or ROOT / "bin/purecvisorsd")
                                                                                     
                                                                                  
DATA = {"_IO_stdin_used", "g_ascii_table", "g_utf8_skip", "stderr", "xmlFree"}
                                                                                                        
                                                         
PKGS = ("glib-2.0", "gio-2.0", "gmodule-no-export-2.0", "json-glib-1.0")


def list_symbols(text: str) -> list[str]:
                                                                             
    body = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    m = re.fullmatch(r"\s*\{(.*)\}\s*;\s*", body, flags=re.S)
    if not m:
        raise AssertionError("동적 목록 형식이 `{ 이름; … };` 이 아니다")
    names = [t.strip() for t in m.group(1).split(";") if t.strip()]
    bad = [n for n in names if not re.fullmatch(r"[A-Za-z_]\w*", n)]
    if bad:
        raise AssertionError(f"동적 목록에 이름이 아닌 항목: {bad}")
    return names


def header_functions(text: str) -> list[str]:
                                                                                            
    code = "\n".join(l for l in c_blocks.masked(text).splitlines() if not l.lstrip().startswith("#"))
    out = []
    for stmt in code.split(";"):
        s = " ".join(stmt.split())
        if not s or s.startswith("typedef"):
            continue
        hit = re.match(r"[A-Za-z_][\w\s*]*?\b(pcv_[a-z0-9_]+)\s*\(", s)
        if hit:
            out.append(hit.group(1))
    return out


def manager_definitions(text: str) -> dict[str, bool]:
                                                   
    return {b.name: b.is_static for b in c_blocks.scan(text)[0]}


def source_problems(list_text: str, header_text: str, manager_text: str) -> list[str]:
                              
    out = []
    names = list_symbols(list_text)
    if len(names) != len(set(names)):
        out.append(f"동적 목록에 중복: {names}")
    hdr = header_functions(header_text)
    if set(names) != set(hdr):
        out.append(f"목록 {sorted(set(names))} ≠ 헤더 선언 {sorted(set(hdr))}")
    defs = manager_definitions(manager_text)
    for n in names:
        if n not in defs:
            out.append(f"{n}: pcv_plugin_manager.c 에 정의가 없다")
        elif defs[n]:
            out.append(f"{n}: static 정의다(내보낼 수 없다)")
    return out


def _rule(makefile: str, target: str) -> tuple[str, list[str]]:
                                           
    lines = makefile.splitlines()
    for i, line in enumerate(lines):
        if line.startswith(target + ":"):
            recipe = []
            for nxt in lines[i + 1:]:
                if not nxt.startswith("\t"):
                    break
                recipe.append(nxt)
            return line, recipe
    raise AssertionError(f"Makefile 에 규칙 없음: {target}")


def makefile_problems(makefile: str) -> list[str]:
                                                                        
    out = []
    code = "\n".join(l for l in makefile.splitlines() if not l.lstrip().startswith("#"))
    if not re.search(r"^PLUGIN_EXPORTS_LIST\s*=\s*src/modules/plugin/pcv_plugin_exports\.list\s*$", code, re.M):
        out.append("PLUGIN_EXPORTS_LIST 가 src/modules/plugin/pcv_plugin_exports.list 가 아니다")
    if not re.search(r"^DAEMON_EXPORT_LDFLAGS\s*=\s*-Wl,--dynamic-list=\$\(PLUGIN_EXPORTS_LIST\)\s*$", code, re.M):
        out.append("DAEMON_EXPORT_LDFLAGS 가 -Wl,--dynamic-list=$(PLUGIN_EXPORTS_LIST) 가 아니다")
    head, recipe = _rule(code, "$(DAEMON_BIN)")
    if "$(PLUGIN_EXPORTS_LIST)" not in head:
        out.append("$(DAEMON_BIN) 의 선행 조건에 $(PLUGIN_EXPORTS_LIST) 가 없다(목록만 바뀌면 다시 링크하지 않는다)")
    link = [l for l in recipe if "$(CC)" in l]
    if len(link) != 1 or "$(DAEMON_EXPORT_LDFLAGS)" not in link[0]:
        out.append("$(DAEMON_BIN) 링크 줄에 $(DAEMON_EXPORT_LDFLAGS) 가 없다")
    if code.count("$(DAEMON_EXPORT_LDFLAGS)") != 1:
        out.append(f"$(DAEMON_EXPORT_LDFLAGS) 가 {code.count('$(DAEMON_EXPORT_LDFLAGS)')}곳에 쓰였다(데몬 링크 한 곳이어야 한다)")
    if code.count("--dynamic-list") != 1:
        out.append(f"--dynamic-list 가 {code.count('--dynamic-list')}곳에 있다(DAEMON_EXPORT_LDFLAGS 한 곳이어야 한다)")
    for opt in ("--export-dynamic", "-rdynamic", "-fvisibility", "-Wl,-E "):
        if opt in code:
            out.append(f"Makefile 에 {opt} 가 있다(목록 밖 심볼까지 내보낸다)")
    if re.search(r"^LDFLAGS_EXTRA\s*\+?=.*dynamic-list", code, re.M):
        out.append("LDFLAGS_EXTRA 에 --dynamic-list 가 있다(make LDFLAGS_EXTRA=... 가 덮어쓴다)")
    head, recipe = _rule(code, "check-server-defect-contracts")
    if "$(DAEMON_BIN)" not in head:
        out.append("check-server-defect-contracts 선행 조건에 $(DAEMON_BIN) 이 없다")
    if not any("tests/test_plugin_api_export_contract.py" in l for l in recipe):
        out.append("check-server-defect-contracts 가 tests/test_plugin_api_export_contract.py 를 돌리지 않는다")
    return out


def nm_dynamic(path: Path, *flags: str) -> dict[str, str]:
                                                  
    out = subprocess.run(["nm", "-D", *flags, str(path)], capture_output=True, text=True, check=True, timeout=60).stdout
    got = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 2:
            got[parts[-1].split("@")[0]] = parts[-2]
    return got


def export_problems(defined: dict[str, str], names: list[str]) -> list[str]:
                                            
    out = []
    want = set(names) | DATA
    have = set(defined)
    if have - want:
        out.append(f"목록 밖 동적 정의: {sorted(have - want)}")
    if want - have:
        out.append(f"동적 정의에서 빠짐: {sorted(want - have)}")
    for n in names:
        if n in defined and defined[n] not in ("T", "t"):
            out.append(f"{n}: 형식 {defined[n]}(함수 T 여야 한다)")
    return out


def _cc() -> str:
    return shutil.which("gcc-14") or shutil.which("cc") or "cc"


def _flags() -> list[str]:
    return shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", *PKGS], text=True))


INC = ["-I.", "-Iinclude", "-Isrc", "-Iinclude/purecvisor", "-Isrc/modules/plugin"]                                                       


def compile_or_fail(argv: list[str]) -> None:
                                                                          
    r = subprocess.run(argv, cwd=ROOT, capture_output=True, text=True, timeout=180)
    if r.returncode != 0:
        raise AssertionError(f"컴파일 실패 rc={r.returncode}: {' '.join(argv[:3])} …\n{r.stderr[-2000:]}")


def build_probe(workdir: Path, subdir: str = "plugins.d", defines: tuple[str, ...] = ()) -> Path:
                                                                                          
    plugdir = workdir / subdir
    plugdir.mkdir(parents=True, exist_ok=True)
    plugdir.chmod(0o755)
    so = plugdir / "probe.so"
    compile_or_fail([_cc(), "-std=gnu23", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC", "-Isrc", *defines,
                     str(PROBE), "-o", str(so), *_flags()])
    so.chmod(0o644)
    return so


DRIVER = r"""
#include <glib.h>
#include <gio/gio.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include "modules/plugin/pcv_plugin_manager.h"
#include "modules/dispatcher/rpc_completion.h"
#include "api/uds_server.h"
/* 매니저가 쓰는 로그·감사는 표준 출력으로(로그는 시험이 문구를 본다), 응답 전송은 uds_server.c 와 같이 쓰고 닫는다. */
void _pcv_log(GLogLevelFlags level, const gchar *domain, const gchar *fmt, ...) {
    (void)domain; va_list ap; va_start(ap, fmt); gchar *s = g_strdup_vprintf(fmt, ap); va_end(ap);
    printf("LOG %s %s\n", level == G_LOG_LEVEL_WARNING ? "WARN" : "INFO", s); g_free(s);
}
void pcv_audit_log(const gchar *u, const gchar *m, const gchar *t, const gchar *r, gint e, gint64 d, const gchar *s) {
    (void)u; (void)t; (void)d; (void)s; printf("AUDIT %s %s %d\n", m, r, e);
}
void pure_uds_server_send_response(UdsServer *self, GSocketConnection *connection, const gchar *response) {
    (void)self;
    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(connection));
    g_output_stream_write_all(out, response, strlen(response), NULL, NULL, NULL);
    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
}
static void call_as(const char *label, const char *method, const char *params_json) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { printf("CALL %s socketpair-failed\n", method); return; }
    GSocket *gs = g_socket_new_from_fd(sv[0], NULL);
    GSocketConnection *conn = g_socket_connection_factory_create_connection(gs);
    g_object_unref(gs);
    JsonParser *p = json_parser_new();
    JsonObject *params = NULL;   /* params_json NULL = 요청에 params 가 없다 → 처리기는 NULL 을 받는다 */
    if (params_json) {
        json_parser_load_from_data(p, params_json, -1, NULL);
        params = json_node_get_object(json_parser_get_root(p));
    }
    pcv_rpc_completion_begin(method);
    pcv_plugin_dispatch(method, params, "7", NULL, conn);
    PcvRpcCompletionResult r = pcv_rpc_completion_finish();
    int closed = g_io_stream_is_closed(G_IO_STREAM(conn));
    g_object_unref(conn);   /* 닫히지 않았으면 여기서 닫혀 상대가 EOF 를 본다 */
    GString *wire = g_string_new(NULL);
    char buf[4096]; ssize_t n;
    while ((n = read(sv[1], buf, sizeof buf)) > 0) g_string_append_len(wire, buf, n);
    close(sv[1]);
    gchar *esc = g_strescape(wire->str, NULL);
    printf("CALL %s observed=%d success=%d code=%d closed=%d wire=%s\n", label, r.response_observed, r.success, r.error_code, closed, esc);
    g_free(esc); g_string_free(wire, TRUE); g_object_unref(p);
}
static void call(const char *method, const char *params_json) { call_as(method, method, params_json); }
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    pcv_plugin_manager_init(argv[1]);
    printf("HAS echo=%d late=%d\n", pcv_plugin_has_handler("pcvtestapi.echo"), pcv_plugin_has_handler("pcvtestapi.late"));
    call("pcvtestapi.echo", "{\"msg\":\"hello\"}");
    printf("HAS late_after_echo=%d\n", pcv_plugin_has_handler("pcvtestapi.late"));
    call("pcvtestapi.fail", "{}");
    call("pcvtestapi.twice", "{}");
    call("pcvtestapi.silent", "{}");
    call("pcvtestapi.zero", "{}");
    call("pcvtestapi.info", "{}");
    call_as("pcvtestapi.echo#noparams", "pcvtestapi.echo", NULL);
    JsonNode *orphan = json_node_init_string(json_node_alloc(), "orphan");   /* NULL 연결: FALSE, 노드는 도우미가 해제 */
    printf("NULLCONN success=%d error=%d\n", pcv_plugin_respond_success("1", NULL, NULL, orphan),
           pcv_plugin_respond_error("1", NULL, NULL, 1, "x"));
    pcv_plugin_manager_shutdown();
    return 0;
}
"""


def build_driver(manager_src: str, workdir: Path, list_path: Path | None = LIST) -> Path:
                                                                                                            
    (workdir / "manager.c").write_text(manager_src)
    (workdir / "driver.c").write_text(DRIVER)
    drv = workdir / "driver"
    dyn = [f"-Wl,--dynamic-list={list_path}"] if list_path else []
    compile_or_fail([_cc(), "-std=gnu23", "-D_GNU_SOURCE", "-w", *INC, *dyn,
                     str(workdir / "manager.c"), "src/modules/dispatcher/rpc_utils.c", "src/modules/dispatcher/rpc_completion.c",
                     str(workdir / "driver.c"), "-o", str(drv), *_flags()])
    return drv


def run_driver(drv: Path, plugdir: Path, mark: Path) -> subprocess.CompletedProcess:
                                                                                
    pre = ["prlimit", "--core=1:1", "--"] if shutil.which("prlimit") else []
    return subprocess.run([*pre, str(drv), str(plugdir)], capture_output=True, text=True, timeout=60,
                          env={**os.environ, "PCV_TEST_PLUGIN_MARK": str(mark), "G_MESSAGES_DEBUG": ""})


def parse_calls(stdout: str) -> dict[str, dict]:
    calls = {}
    for line in stdout.splitlines():
        m = re.match(r"CALL (\S+) observed=(\d) success=(\d) code=(-?\d+) closed=(\d) wire=(.*)$", line)
        if m:
            wire = m.group(6).encode().decode("unicode_escape")
            calls[m.group(1)] = {"observed": int(m.group(2)), "success": int(m.group(3)), "code": int(m.group(4)),
                                 "closed": int(m.group(5)), "lines": [json.loads(l) for l in wire.splitlines() if l.strip()]}
    return calls


class PluginApiExportContract(unittest.TestCase):
    list_text = LIST.read_text() if LIST.exists() else ""
    header_text = API_H.read_text()
    manager_text = MANAGER.read_text()
    makefile_text = MAKEFILE.read_text()

    def setUp(self):
        self._td = trusted_tempdir("pcv-plugin-export-")
        self.addCleanup(self._td.cleanup)
        self.workdir = Path(self._td.name)

                                                                    
    def test_list_header_definitions_agree(self):
        self.assertTrue(LIST.exists(), f"{LIST.relative_to(ROOT)} 가 없다")
        self.assertEqual(list_symbols(self.list_text),
                         ["pcv_plugin_registry_add", "pcv_plugin_respond_success", "pcv_plugin_respond_error"])
        self.assertEqual(source_problems(self.list_text, self.header_text, self.manager_text), [])

    def test_source_counterfactuals(self):
        less = self.list_text.replace("  pcv_plugin_respond_error;\n", "")
        self.assertTrue(any("헤더 선언" in p for p in source_problems(less, self.header_text, self.manager_text)))
        more = self.list_text.replace("};", "  pcv_plugin_dispatch;\n};")
        self.assertTrue(source_problems(more, self.header_text, self.manager_text))
        static = self.manager_text.replace("gboolean pcv_plugin_respond_error(", "static gboolean pcv_plugin_respond_error(", 1)
        self.assertNotEqual(static, self.manager_text)
        self.assertTrue(any("static" in p for p in source_problems(self.list_text, self.header_text, static)))

                                                                    
    def test_makefile_wiring(self):
        self.assertEqual(makefile_problems(self.makefile_text), [])

    def test_makefile_counterfactuals(self):
        mk = self.makefile_text
        cases = {
            "링크 줄": mk.replace("$(LDFLAGS) $(DAEMON_EXPORT_LDFLAGS)", "$(LDFLAGS)", 1),
            "선행 조건": mk.replace("$(DAEMON_BIN): $(DAEMON_OBJS) $(PLUGIN_EXPORTS_LIST)", "$(DAEMON_BIN): $(DAEMON_OBJS)", 1),
            "LDFLAGS_EXTRA": mk.replace("LDFLAGS_EXTRA += -Wl,--gc-sections",
                                        "LDFLAGS_EXTRA += -Wl,--gc-sections -Wl,--dynamic-list=$(PLUGIN_EXPORTS_LIST)", 1),
            "export-dynamic": mk.replace("LDFLAGS_EXTRA += -Wl,--gc-sections", "LDFLAGS_EXTRA += -Wl,--gc-sections -rdynamic", 1),
            "시험 등록": mk.replace("\t@python3 tests/test_plugin_api_export_contract.py\n", "", 1),
        }
        for label, bad in cases.items():
            with self.subTest(label):
                self.assertNotEqual(bad, mk, f"변이 표본이 Makefile 에 없다: {label}")
                self.assertTrue(makefile_problems(bad), label)

                                                                    
    def test_daemon_dynamic_symbols_exact(self):
        self.assertTrue(DAEMON.is_file(), f"{DAEMON} 없음 — make daemon 뒤 실행(make test 는 $(DAEMON_BIN) 을 먼저 만든다)")
        defined = nm_dynamic(DAEMON, "--defined-only")
        self.assertEqual(export_problems(defined, list_symbols(self.list_text)), [], f"{DAEMON}: {sorted(defined)}")

    def test_export_check_counterfactuals(self):
        names = list_symbols(self.list_text)
        good = {**{n: "T" for n in names}, **{d: "B" for d in DATA}}
        self.assertEqual(export_problems(good, names), [])
        self.assertTrue(export_problems({d: "B" for d in DATA}, names))                               
        self.assertTrue(export_problems({**good, "pcv_plugin_dispatch": "T"}, names))                            
        self.assertTrue(export_problems({**good, names[0]: "D"}, names))                    

                                                                    
    def test_probe_plugin_needs_only_exported_symbols(self):
        so = build_probe(self.workdir)
        undef = {n for n in nm_dynamic(so, "--undefined-only") if n.startswith("pcv_")}
        names = set(list_symbols(self.list_text))
        self.assertEqual(undef, names, "시험 플러그인은 데몬 제공 API 세 함수를 모두, 그것만 불러야 한다")
        if DAEMON.is_file():
            self.assertLessEqual(undef, set(nm_dynamic(DAEMON, "--defined-only")))

                                                                    
    def test_respond_helpers_exactly_once(self):
        so = build_probe(self.workdir)
        drv = build_driver(self.manager_text, self.workdir)
        mark = self.workdir / "mark.txt"
        r = run_driver(drv, so.parent, mark)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        out = r.stdout
        self.assertIn("HAS echo=1 late=0", out)
        self.assertIn("HAS late_after_echo=0", out, "register 콜백 밖 registry_add 는 거절돼야 한다")
        self.assertIn("pcv_plugin_registry_add called outside pcv_plugin_register", out)
        self.assertIn("AUDIT plugin.load ok 0", out)
        calls = parse_calls(out)
        self.assertEqual(set(calls), {"pcvtestapi.echo", "pcvtestapi.fail", "pcvtestapi.twice", "pcvtestapi.silent",
                                      "pcvtestapi.zero", "pcvtestapi.info", "pcvtestapi.echo#noparams"}, out)
        for m, c in calls.items():
            with self.subTest(m):
                self.assertEqual(len(c["lines"]), 1, f"{m}: 응답이 정확히 한 줄이어야 한다: {c}")
                self.assertEqual(c["closed"], 1, f"{m}: 응답 뒤 연결이 닫혀야 한다")
                self.assertEqual(c["observed"], 1, f"{m}: ADR-0038 관측이 없다(감사가 '응답 없음'으로 센다)")
                self.assertEqual(c["lines"][0]["jsonrpc"], "2.0")
                self.assertEqual(c["lines"][0]["id"], "7")
        echo = calls["pcvtestapi.echo"]
        self.assertEqual((echo["success"], echo["code"]), (1, 0))
        self.assertEqual(echo["lines"][0]["result"], {"plugin": "pcv-test-api", "echo": "hello"})
        fail = calls["pcvtestapi.fail"]
        self.assertEqual((fail["success"], fail["code"]), (0, 1001))
        self.assertEqual(fail["lines"][0]["error"], {"code": 1001, "message": "pcvtestapi failure requested"})
        twice = calls["pcvtestapi.twice"]
        self.assertEqual((twice["success"], twice["code"]), (1, 0))
        self.assertEqual(twice["lines"][0]["result"], "first")
        silent = calls["pcvtestapi.silent"]
        self.assertEqual((silent["success"], silent["code"]), (0, -32603))
        self.assertEqual(silent["lines"][0]["error"], {"code": -32603, "message": "plugin method returned without a response"})
        self.assertIn("Plugin method 'pcvtestapi.silent' (owner 'pcv-test-api') returned without a response", out)
        zero = calls["pcvtestapi.zero"]
        self.assertEqual((zero["success"], zero["code"]), (0, -32603))
        self.assertEqual(zero["lines"][0]["error"], {"code": -32603, "message": "Unknown error"})
        info = calls["pcvtestapi.info"]
        self.assertEqual((info["success"], info["code"]), (1, 0))
        self.assertEqual(info["lines"][0]["result"], {"plugin": "pcv-test-api", "info": True})
        noparams = calls["pcvtestapi.echo#noparams"]
        self.assertEqual((noparams["success"], noparams["code"]), (1, 0))
        self.assertEqual(noparams["lines"][0]["result"], {"plugin": "pcv-test-api", "echo": None}, "params NULL 을 처리기가 다뤄야 한다")
        self.assertIn("NULLCONN success=0 error=0", out)
        marks = mark.read_text().split("\n")
        self.assertIn("echo sent=1", marks)
        self.assertIn("fail sent=1", marks)
        self.assertIn("twice first=1 second=0 third=0", marks)
        self.assertIn("silent returned", marks)
        self.assertEqual(sum(1 for l in marks if l.startswith("shutdown pid=")), 1)

    def test_driver_without_list_cannot_load_probe(self):
                                                                                             
        so = build_probe(self.workdir)
        drv = build_driver(self.manager_text, self.workdir, None)
        r = run_driver(drv, so.parent, self.workdir / "mark.txt")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("HAS echo=0 late=0", r.stdout)
        self.assertRegex(r.stdout, r"undefined symbol: pcv_plugin_(registry_add|respond_success|respond_error)")
        self.assertIn("AUDIT plugin.load fail 2", r.stdout)


if __name__ == "__main__":
    unittest.main()
