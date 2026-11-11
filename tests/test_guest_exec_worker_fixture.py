#!/usr/bin/env python3
                          
                                                                                                        
                                                                                                    
                                                                                                  
                                                                                  
                                                    
                                                                                      
                                                            
                                                                                      
                                                          
                                                                                  
                                                                                      
                                                                                                  
 
                                                                    
                                                                                  
                                                            
                                                                      
from __future__ import annotations

import importlib.util
import os
import re
import resource
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HANDLER = ROOT / "src/modules/dispatcher/handler_vm_guest.c"
REQUEST_C = ROOT / "src/modules/dispatcher/guest_exec_request.c"
RPC_UTILS_C = ROOT / "src/modules/dispatcher/rpc_utils.c"
RUN_PY = ROOT / "tests/guest_agent_errors/run.py"
SCENARIOS = ("windows", "linux", "osinfo-unsupported", "agent-unresponsive",
             "multiline", "cp949", "call-order", "budget")


def _load_extractor():
                                                                   
    spec = importlib.util.spec_from_file_location("guest_agent_errors_run", RUN_PY)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.function


FIXTURE = r'''
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <libvirt/libvirt.h>
#include <libvirt/libvirt-qemu.h>
#include <libvirt/virterror.h>
#include <string.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include "modules/dispatcher/rpc_utils.h"
#include "modules/dispatcher/guest_exec_request.h"

typedef struct { gchar *vm_id, *command; } GuestExecCtx;

/* 가짜 단조 시계(P7) — 워커의 g_get_monotonic_time·g_usleep이 이 둘로 바뀐다. */
static gint64 fake_now_us;
static gint64 pcv_fake_now(void) { return fake_now_us; }
static void pcv_fake_sleep(gulong us) { fake_now_us += (gint64)us; }

typedef struct { gchar *command; int timeout; gint64 at_us; } AgentCall;
static const gchar *scenario;
static GPtrArray *calls;
static gchar *last_error;
static int last_code;
static guint releases, frees;

static void agent_call_free(gpointer p) { AgentCall *c = p; g_free(c->command); g_free(c); }
virConnectPtr virt_conn_pool_acquire(void) { return (virConnectPtr)1; }
void virt_conn_pool_release(virConnectPtr conn) {
    g_assert_true(conn == (virConnectPtr)1); releases++; g_clear_pointer(&last_error, g_free);
}
virDomainPtr pure_virt_get_domain(virConnectPtr conn, const gchar *name) {
    g_assert_true(conn == (virConnectPtr)1); g_assert_cmpstr(name, ==, "fixture-vm"); return (virDomainPtr)2;
}
int virDomainIsActive(virDomainPtr dom) { g_assert_true(dom == (virDomainPtr)2); return 1; }
int virDomainFree(virDomainPtr dom) {
    g_assert_true(dom == (virDomainPtr)2); frees++; g_clear_pointer(&last_error, g_free); return 0;
}
const char *virGetLastErrorMessage(void) { return last_error; }
int virGetLastErrorCode(void) { return last_code; }
void pcv_rpc_completion_observe_response(gboolean ok G_GNUC_UNUSED, gint code G_GNUC_UNUSED) {}

static char *agent_fail(int code, const gchar *message) {
    g_free(last_error); last_error = g_strdup(message); last_code = code; return NULL;
}
static gboolean is_scenario(const gchar *name) { return g_str_equal(scenario, name); }

char *virDomainQemuAgentCommand(virDomainPtr dom, const char *command, int timeout, unsigned int flags) {
    g_assert_true(dom == (virDomainPtr)2); g_assert_cmpuint(flags, ==, 0);
    AgentCall *c = g_new0(AgentCall, 1);
    c->command = g_strdup(command); c->timeout = timeout; c->at_us = fake_now_us;
    g_ptr_array_add(calls, c);
    fake_now_us += 1000;                                   /* agent 왕복 1 ms */
    if (strstr(command, "guest-get-osinfo")) {
        if (is_scenario("windows"))
            return g_strdup("{\"return\":{\"id\":\"mswindows\",\"name\":\"Microsoft Windows\"}}");
        if (is_scenario("osinfo-unsupported"))
            return agent_fail(VIR_ERR_AGENT_COMMAND_FAILED, "fixture: The command guest-get-osinfo has not been found");
        if (is_scenario("agent-unresponsive"))
            return agent_fail(VIR_ERR_AGENT_UNRESPONSIVE, "fixture: Guest agent is not responding");
        return g_strdup("{\"return\":{\"id\":\"ubuntu\",\"name\":\"Ubuntu\"}}");
    }
    if (strstr(command, "guest-exec-status")) {
        if (is_scenario("budget"))
            return g_strdup("{\"return\":{\"exited\":false}}");
        if (is_scenario("cp949"))                              /* c7 d1 b1 db = CP949 "한글" */
            return g_strdup("{\"return\":{\"exited\":true,\"exitcode\":0,\"out-data\":\"x9Gx2w==\"}}");
        return g_strdup("{\"return\":{\"exited\":true,\"exitcode\":0,\"out-data\":\"T0s=\"}}");
    }
    if (strstr(command, "\"guest-exec\""))
        return g_strdup("{\"return\":{\"pid\":3}}");
    g_assert_not_reached();
    return NULL;
}

#define g_get_monotonic_time pcv_fake_now
#define g_usleep pcv_fake_sleep
#include "worker.h"
#undef g_get_monotonic_time
#undef g_usleep

static const gchar *const k_ps_flags[] = { "-NoLogo", "-NoProfile", "-NonInteractive", "-EncodedCommand" };

/* 기록된 호출 중 kind(osinfo·exec·status)인 것만 센다. */
static guint count_calls(const gchar *kind, guint *first) {
    guint n = 0;
    for (guint i = 0; i < calls->len; i++) {
        const gchar *cmd = ((AgentCall *)g_ptr_array_index(calls, i))->command;
        gboolean hit = g_str_equal(kind, "osinfo") ? strstr(cmd, "guest-get-osinfo") != NULL
                     : g_str_equal(kind, "status") ? strstr(cmd, "guest-exec-status") != NULL
                     : strstr(cmd, "\"guest-exec\"") != NULL;
        if (hit && n++ == 0 && first) *first = i;
    }
    return n;
}

/* 기록된 guest-exec 요청을 파싱해 arguments 객체를 돌려준다(parser 소유). */
static JsonObject *exec_arguments(JsonParser *parser) {
    guint idx = 0;
    g_assert_cmpuint(count_calls("exec", &idx), ==, 1);
    const gchar *json = ((AgentCall *)g_ptr_array_index(calls, idx))->command;
    GError *err = NULL;
    g_assert_true(json_parser_load_from_data(parser, json, -1, &err));
    g_assert_no_error(err);
    return json_object_get_object_member(json_node_get_object(json_parser_get_root(parser)), "arguments");
}

/* 예산 공식(spec 5.8 G5·판정 D1)을 독립 계산한다. */
static int expected_timeout(gint64 start, gint64 at) {
    gint64 elapsed = MAX(0, at - start);
    gint64 left = (gint64)15 * G_USEC_PER_SEC - elapsed;
    return (int)MAX(1, MIN(30, left / G_USEC_PER_SEC));
}

static void run_scenario(gconstpointer data) {
    scenario = data;
    calls = g_ptr_array_new_with_free_func(agent_call_free);
    releases = frees = 0; last_code = 0; g_clear_pointer(&last_error, g_free);
    const gint64 start = fake_now_us = G_GINT64_CONSTANT(1000000000);
    const gchar *command = is_scenario("multiline") ? "printf 'a\tb'\nprintf \"q\\\\\"\x01 done 한글"
                         : is_scenario("windows") ? "Write-Output '}'; exit 0 #pcv"
                         : "printf OK";
    GuestExecCtx ctx = { .vm_id = "fixture-vm", .command = (gchar *)command };
    GTask *task = g_task_new(NULL, NULL, NULL, NULL);
    _guest_exec_worker(task, NULL, &ctx, NULL);
    GError *error = NULL;
    JsonNode *result = g_task_propagate_pointer(task, &error);
    g_assert_cmpuint(frees, ==, 1); g_assert_cmpuint(releases, ==, 1);

    guint osinfo_idx = 0, exec_idx = 0;
    g_assert_cmpuint(count_calls("osinfo", &osinfo_idx), ==, 1);
    g_assert_cmpuint(osinfo_idx, ==, 0);                                     /* osinfo가 맨 먼저 */
    g_assert_cmpint(((AgentCall *)g_ptr_array_index(calls, 0))->timeout, ==, PCV_GUEST_EXEC_OSINFO_TIMEOUT_SEC);

    if (is_scenario("agent-unresponsive")) {                                  /* 4: guest-exec 0회, 오류 */
        g_assert_null(result);
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED);
        g_assert_nonnull(strstr(error->message, "guest agent on 'fixture-vm' is not responding"));
        g_assert_nonnull(strstr(error->message, "Guest agent is not responding"));
        g_assert_cmpuint(count_calls("exec", NULL), ==, 0);
        g_assert_cmpuint(count_calls("status", NULL), ==, 0);
        g_clear_error(&error); g_object_unref(task); g_ptr_array_unref(calls);
        return;
    }
    g_assert_no_error(error);
    g_assert_nonnull(result);
    JsonObject *res = json_node_get_object(result);

    for (guint i = 1; i < calls->len; i++) {                                 /* 7: exec·status 상한 = 예산 공식, 1..30 */
        const AgentCall *c = g_ptr_array_index(calls, i);
        g_assert_cmpint(c->timeout, >=, 1);
        g_assert_cmpint(c->timeout, <=, 30);
        g_assert_cmpint(c->timeout, ==, expected_timeout(start, c->at_us));
    }
    g_assert_cmpuint(count_calls("exec", &exec_idx), ==, 1);
    g_assert_cmpuint(exec_idx, ==, 1);                                       /* osinfo 다음이 guest-exec */

    JsonParser *parser = json_parser_new();
    JsonObject *args = exec_arguments(parser);
    JsonArray *argv = json_object_get_array_member(args, "arg");
    if (is_scenario("windows")) {                                             /* 1 */
        g_assert_cmpstr(json_object_get_string_member(args, "path"), ==, "powershell.exe");
        g_assert_cmpuint(json_array_get_length(argv), ==, 5);
        for (guint k = 0; k < G_N_ELEMENTS(k_ps_flags); k++)
            g_assert_cmpstr(json_array_get_string_element(argv, k), ==, k_ps_flags[k]);
        gsize n = 0;
        g_autofree guchar *utf16 = g_base64_decode(json_array_get_string_element(argv, 4), &n);
        g_autofree gchar *script = g_convert((const gchar *)utf16, (gssize)n, "UTF-8", "UTF-16LE", NULL, NULL, NULL);
        g_autofree gchar *b64 = g_base64_encode((const guchar *)command, strlen(command));
        g_assert_nonnull(script);
        g_assert_nonnull(strstr(script, b64));
        g_assert_null(strstr(script, command));
        g_assert_cmpstr(json_object_get_string_member(res, "shell"), ==, "powershell");
        g_assert_cmpstr(json_object_get_string_member(res, "os_id"), ==, "mswindows");
    } else {                                                                  /* 2·3·5·6·7·8 */
        g_assert_cmpstr(json_object_get_string_member(args, "path"), ==, "/bin/sh");
        g_assert_cmpuint(json_array_get_length(argv), ==, 2);
        g_assert_cmpstr(json_array_get_string_element(argv, 0), ==, "-c");
        g_assert_cmpstr(json_array_get_string_element(argv, 1), ==, command);  /* 5: 여러 줄·탭·제어문자 원문 */
        g_assert_cmpstr(json_object_get_string_member(res, "shell"), ==, "sh");
        if (is_scenario("osinfo-unsupported"))
            g_assert_false(json_object_has_member(res, "os_id"));
        else
            g_assert_cmpstr(json_object_get_string_member(res, "os_id"), ==, "ubuntu");
    }
    g_object_unref(parser);

    if (is_scenario("budget")) {                                              /* 8: 15초 + 1초 이내(판정 D4)에 멈춤 */
        guint status_n = count_calls("status", NULL);
        const AgentCall *last = g_ptr_array_index(calls, calls->len - 1);
        g_assert_false(json_object_get_boolean_member(res, "exited"));
        g_assert_cmpint(json_object_get_int_member(res, "exitcode"), ==, -1);
        g_assert_cmpint(last->timeout, >=, 1);
        g_assert_cmpint(last->at_us - start, >=, 15 * G_USEC_PER_SEC - 200 * 1000 - 2000);
        g_assert_cmpint(fake_now_us - start, <=, 16 * G_USEC_PER_SEC);
        g_test_message("budget: %u status calls, stopped at %.3f s", status_n, (double)(fake_now_us - start) / G_USEC_PER_SEC);
    } else {
        g_assert_true(json_object_get_boolean_member(res, "exited"));
        g_assert_cmpint(json_object_get_int_member(res, "exitcode"), ==, 0);
    }

    gchar *wire = pure_rpc_build_success_response("fixture-rpc", result);   /* 6: 응답 직렬화가 다시 파싱된다 */
    g_assert_true(g_utf8_validate(wire, -1, NULL));
    JsonParser *back = json_parser_new();
    GError *perr = NULL;
    g_assert_true(json_parser_load_from_data(back, wire, -1, &perr));
    g_assert_no_error(perr);
    JsonObject *rres = json_object_get_object_member(json_node_get_object(json_parser_get_root(back)), "result");
    if (is_scenario("cp949")) {
        g_assert_cmpstr(json_object_get_string_member(rres, "stdout_b64"), ==, "x9Gx2w==");
        g_assert_true(g_utf8_validate(json_object_get_string_member(rres, "stdout"), -1, NULL));
    } else if (!is_scenario("budget")) {
        g_assert_cmpstr(json_object_get_string_member(rres, "stdout"), ==, "OK");
        g_assert_false(json_object_has_member(rres, "stdout_b64"));
    }
    g_object_unref(back); g_free(wire); g_object_unref(task); g_ptr_array_unref(calls);
}

int main(int argc, char **argv) {
#ifndef __SANITIZE_ADDRESS__                               /* LeakSanitizer는 ptrace가 필요해 dumpable을 끄지 않는다 */
    prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);                    /* 반사실 abort가 코어·크래시 보고로 번지지 않게 */
#endif
    g_test_init(&argc, &argv, NULL);
    static const gchar *const names[] = { @SCENARIOS@ };
    for (guint i = 0; i < G_N_ELEMENTS(names); i++) {
        gchar *path = g_strconcat("/guest-exec-worker/", names[i], NULL);
        g_test_add_data_func(path, names[i], run_scenario);
        g_free(path);
    }
    return g_test_run();
}
'''

OLD_PRINTF = r'''GString *safe_cmd = g_string_new(NULL);
    for (const gchar *p = ctx->command; *p; p++) {
        if (*p == '"' || *p == '\\')
            g_string_append_c(safe_cmd, '\\');
        g_string_append_c(safe_cmd, *p);
    }
    gchar *exec_json = g_strdup_printf(
        "{\"execute\":\"guest-exec\",\"arguments\":"
        "{\"path\":\"/bin/sh\",\"arg\":[\"-c\",\"%s\"],\"capture-output\":true}}",
        safe_cmd->str);
    g_string_free(safe_cmd, TRUE);
    (void)shell;'''

                                 
OSINFO_CALL = re.compile(r'virDomainQemuAgentCommand\(dom,\s*"\{\\"execute\\":\\"guest-get-osinfo\\"\}",\s*'
                         r'PCV_GUEST_EXEC_OSINFO_TIMEOUT_SEC,\s*0\)')
EXEC_BUILD = "gchar *exec_json = pcv_guest_exec_request_json(shell, ctx->command);"
POLL_LOOP = "for (;;) {"


def _sub_once(text: str, old, new: str) -> str:
    if isinstance(old, re.Pattern):
        out, n = old.subn(lambda _m: new, text)
    else:
        n = text.count(old)
        out = text.replace(old, new)
    if n != 1:
        raise AssertionError(f"반사실 기준이 워커에 정확히 한 번 있어야 함: {old!r} ({n})")
    return out


MUTATIONS = (
    ("no-osinfo", "windows", lambda w: _sub_once(w, OSINFO_CALL, "NULL")),
    ("old-printf", "multiline", lambda w: _sub_once(w, EXEC_BUILD, OLD_PRINTF)),
    ("cap-50", "budget", lambda w: _sub_once(w, POLL_LOOP, "for (int attempt = 0; attempt < 50; attempt++) {")),
)


def _toolchain_missing() -> str | None:
    if not shutil.which("gcc-14"):
        return "gcc-14 없음"
    if not shutil.which("pkg-config"):
        return "pkg-config 없음"
    probe = subprocess.run(["pkg-config", "--exists", "gio-2.0", "json-glib-1.0", "libvirt"])
    if probe.returncode != 0:
        return "pkg-config gio-2.0 json-glib-1.0 libvirt 없음"
    return None


def _limit_core():
                                                                          
    try:
        resource.setrlimit(resource.RLIMIT_CORE, (1, 1))
    except (ValueError, OSError):
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


class GuestExecWorkerFixture(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        missing = _toolchain_missing()
        if missing:
            raise unittest.SkipTest(f"hermetic 결선 시험 도구 없음: {missing}")
        cls.function = staticmethod(_load_extractor())
        cls.worker = cls.function(HANDLER.read_text(), "_guest_exec_worker")
        cls.flags = shlex.split(subprocess.check_output(
            ["pkg-config", "--cflags", "--libs", "gio-2.0", "json-glib-1.0", "libvirt"], text=True))
        cls.tmp = tempfile.TemporaryDirectory(prefix="pcv-guest-exec-worker-")
        cls.stage = Path(cls.tmp.name)
        names = ", ".join(f'"{s}"' for s in SCENARIOS)
        (cls.stage / "fixture.c").write_text(FIXTURE.replace("@SCENARIOS@", names))

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def build(self, label: str, worker: str, sanitize: bool) -> Path:
        d = self.stage / label
        d.mkdir()
        (d / "worker.h").write_text(worker)
        binary = d / "fixture"
        cmd = ["gcc-14", "-std=gnu23", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror", "-g", "-O0",
               "-I" + str(ROOT / "src"), "-I" + str(ROOT / "include"), "-I" + str(ROOT / "include/purecvisor"),
               "-I" + str(d)]
        if sanitize:
            cmd += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        cmd += [str(self.stage / "fixture.c"), str(REQUEST_C), str(RPC_UTILS_C), *self.flags, "-o", str(binary)]
        built = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
        self.assertEqual(built.returncode, 0, f"{label} 빌드 실패:\n{built.stdout}{built.stderr}")
        self.assertEqual(built.stderr.strip(), "", f"{label} 빌드 경고:\n{built.stderr}")
        return binary

    def run_binary(self, binary: Path, *selector: str) -> subprocess.CompletedProcess:
        env = {**os.environ, "G_DEBUG": "fatal-warnings", "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1",
               "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"}
        return subprocess.run([str(binary), *selector], capture_output=True, text=True, timeout=120,
                              env=env, preexec_fn=_limit_core)

    def check_all_pass(self, sanitize: bool):
        label = "asan-ubsan" if sanitize else "normal"
        binary = self.build(label, self.worker, sanitize)
        res = self.run_binary(binary)
        out = res.stdout + res.stderr
        self.assertEqual(res.returncode, 0, f"{label} 실패:\n{out}")
        ok = re.findall(r"^ok \d+ /guest-exec-worker/(\S+)$", res.stdout, re.M)
        self.assertEqual(sorted(ok), sorted(SCENARIOS), out)
        for line in re.findall(r"^# budget: .*$", res.stdout, re.M):                          
            print(f"[{label}] {line[2:]}")

    def test_scenarios_normal(self):
        self.check_all_pass(sanitize=False)

    def test_scenarios_asan_ubsan(self):
        self.check_all_pass(sanitize=True)

    def test_counterfactuals_red(self):
        for name, target, mutate in MUTATIONS:
            with self.subTest(mutation=name):
                binary = self.build("cf-" + name, mutate(self.worker), sanitize=False)
                res = self.run_binary(binary, "-p", "/guest-exec-worker/" + target)
                self.assertNotEqual(res.returncode, 0,
                                    f"반사실 {name}이 {target}을 실패시키지 못함:\n{res.stdout}{res.stderr}")
                self.assertIn("not ok", res.stdout + res.stderr)
                print(f"[counterfactual {name}] {target}: rc={res.returncode} "
                      f"{next((ln for ln in (res.stdout + res.stderr).splitlines() if 'ERROR' in ln), '')}")


if __name__ == "__main__":
    sys.exit(0 if unittest.main(exit=False).result.wasSuccessful() else 1)
