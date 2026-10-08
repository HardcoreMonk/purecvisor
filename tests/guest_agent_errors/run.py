#!/usr/bin/env python3













import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import resource
import shlex
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
WORKERS = ('fsinfo', 'ping', 'exec', 'shutdown')


def function(source, name):




    position = source.index('\n' + name + '(')
    start = source.rfind('\nstatic', 0, position)
    body = source.index('{', position)
    depth, state, escaped, index = 0, None, False, body
    while index < len(source):
        char, pair = source[index], source[index:index + 2]
        if state == 'line':
            if char == '\n': state = None
        elif state == 'comment':
            if pair == '*/': state = None; index += 1
        elif state:
            if escaped: escaped = False
            elif char == '\\': escaped = True
            elif char == state: state = None
        elif pair == '//': state = 'line'; index += 1
        elif pair == '/*': state = 'comment'; index += 1
        elif char in ('"', "'"): state = char
        elif char == '{': depth += 1
        elif char == '}':
            depth -= 1
            if depth == 0: return source[start:index + 1]
        index += 1
    raise ValueError(name)


FIXTURE = r'''
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <libvirt/libvirt.h>
#include <libvirt/libvirt-qemu.h>
#include <libvirt/virterror.h>
#include <string.h>
#include <stdlib.h>
#include "modules/dispatcher/rpc_utils.h"
typedef struct { gchar *vm_id,*action; } VmLifecycleCtx;
typedef struct { gchar *vm_id,*command; } GuestExecCtx;
static gchar *last_error;
static const gchar *scenario;
static guint releases,frees;
virConnectPtr virt_conn_pool_acquire(void) { return (virConnectPtr)1; }
void virt_conn_pool_release(virConnectPtr conn) {
    g_assert_true(conn==(virConnectPtr)1); releases++; g_clear_pointer(&last_error,g_free);
}
virDomainPtr pure_virt_get_domain(virConnectPtr conn,const gchar *name) {
    g_assert_true(conn==(virConnectPtr)1); g_assert_cmpstr(name,==,"fixture-vm"); return (virDomainPtr)2;
}
int virDomainIsActive(virDomainPtr dom) { g_assert_true(dom==(virDomainPtr)2); return 1; }
int virDomainFree(virDomainPtr dom) {
    g_assert_true(dom==(virDomainPtr)2); frees++; g_clear_pointer(&last_error,g_free); return 0;
}
const char *virGetLastErrorMessage(void) { return last_error; }
void virResetLastError(void) { g_clear_pointer(&last_error,g_free); }
static void agent_error(void) {
    g_clear_pointer(&last_error,g_free);
    if (!g_str_equal(scenario,"null")) last_error=g_strdup(g_str_equal(scenario,"invalid")
        ? "fixture invalid \xff error" : "fixture original agent error");
}
char *virDomainQemuAgentCommand(virDomainPtr dom,const char *command,int timeout,unsigned int flags) {
    g_assert_true(dom==(virDomainPtr)2); g_assert_cmpuint(flags,==,0);
    g_assert_true(timeout==5 || timeout==10 || timeout==30);
    if (!g_str_equal(scenario,"success")) { agent_error(); return NULL; }
    if (strstr(command,"guest-exec-status")) return g_strdup("{\"return\":{\"exited\":true,\"exitcode\":0,\"out-data\":\"T0s=\"}}");
    if (strstr(command,"guest-exec")) return g_strdup("{\"return\":{\"pid\":3}}");
    if (strstr(command,"guest-get-fsinfo")) return g_strdup("{\"return\":[]}");
    return g_strdup("{\"return\":{}}");
}
int virDomainShutdownFlags(virDomainPtr dom,unsigned int flags) {
    g_assert_true(dom==(virDomainPtr)2); g_assert_cmpuint(flags,==,VIR_DOMAIN_SHUTDOWN_GUEST_AGENT);
    if (g_str_equal(scenario,"success")) return 0;
    agent_error(); return -1;
}
int virDomainShutdown(virDomainPtr dom) { g_assert_true(dom==(virDomainPtr)2); agent_error(); return -1; }
void pcv_rpc_completion_observe_response(gboolean ok G_GNUC_UNUSED,gint code G_GNUC_UNUSED) {}
#include "workers.h"
static void test_worker(gconstpointer data) {
    const gchar *label=data; gchar **parts=g_strsplit(label,"/",2); scenario=parts[1]; releases=frees=0;
    GTask *task=g_task_new(NULL,NULL,NULL,NULL);
    VmLifecycleCtx ctx={.vm_id="fixture-vm"}; GuestExecCtx exec={.vm_id="fixture-vm",.command="printf OK"};
    if (g_str_equal(parts[0],"fsinfo")) _guest_fsinfo_worker(task,NULL,&ctx,NULL);
    else if (g_str_equal(parts[0],"ping")) _guest_ping_worker(task,NULL,&ctx,NULL);
    else if (g_str_equal(parts[0],"exec")) _guest_exec_worker(task,NULL,&exec,NULL);
    else _guest_shutdown_worker(task,NULL,&ctx,NULL);
    GError *error=NULL;
    gboolean pointer=g_str_equal(parts[0],"fsinfo") || g_str_equal(parts[0],"exec");
    JsonNode *result=pointer ? g_task_propagate_pointer(task,&error) : NULL;
    gboolean ok=pointer ? result!=NULL : g_task_propagate_boolean(task,&error);
    if (g_str_equal(scenario,"success")) {
        g_assert_true(ok); g_assert_no_error(error);
        if (result) json_node_free(result);
    } else {
        g_assert_false(ok); g_assert_error(error,G_IO_ERROR,G_IO_ERROR_FAILED);
        g_assert_true(g_utf8_validate(error->message,-1,NULL));
        g_assert_nonnull(strstr(error->message,g_str_equal(scenario,"null") ? "unknown error"
            : g_str_equal(scenario,"invalid") ? "fixture invalid" : "fixture original agent error"));
        gchar *response=pure_rpc_build_error_response("fixture-rpc",PURE_RPC_ERR_ZFS_OPERATION,error->message);
        g_assert_true(g_utf8_validate(response,-1,NULL));
        JsonParser *parser=json_parser_new(); g_assert_true(json_parser_load_from_data(parser,response,-1,NULL));
        JsonObject *reply=json_node_get_object(json_parser_get_root(parser));
        g_assert_cmpstr(json_object_get_string_member(json_object_get_object_member(reply,"error"),"message"),==,error->message);
        g_object_unref(parser); g_free(response); g_clear_error(&error);
    }
    g_assert_cmpuint(frees,==,1); g_assert_cmpuint(releases,==,1); g_assert_null(last_error);
    g_free(ctx.action); g_object_unref(task); g_strfreev(parts);
}
int main(int argc,char **argv) {
    g_test_init(&argc,&argv,NULL);
    const gchar *workers[]={"fsinfo","ping","exec","shutdown"};
    const gchar *cases[]={"normal","null","invalid","success"};
    for (guint i=0;i<G_N_ELEMENTS(workers);i++) for (guint j=0;j<G_N_ELEMENTS(cases);j++) {
        gchar *label=g_strconcat(workers[i],"/",cases[j],NULL),*path=g_strconcat("/guest-errors/",label,NULL);
        g_test_add_data_func_full(path,label,test_worker,g_free); g_free(path);
    }
    return g_test_run();
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    source, output = args.source.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    handler = source / 'src/modules/dispatcher/handler_vm_lifecycle.c'
    text, old = handler.read_text(), args.baseline.read_text()
    names = ['_guest_fsinfo_get_int64', '_guest_fsinfo_should_count', *['_guest_' + name + '_worker' for name in WORKERS]]
    result = {'scope': 'actual product worker and RPC formatter; libvirt boundary fixture; no host or VM calls',
              'source_sha256': hashlib.sha256(handler.read_bytes()).hexdigest(), 'runs': [], 'mutations': []}
    flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'gio-2.0', 'json-glib-1.0', 'libvirt'], text=True))
    with tempfile.TemporaryDirectory(prefix='pcv-guest-errors-') as temporary:
        stage = Path(temporary)
        fixture = stage / 'probe.c'; fixture.write_text(FIXTURE)
        for label in ('normal', 'asan-ubsan', *['restore-' + name for name in WORKERS]):
            fragments = [function(old if label == 'restore-' + name.removeprefix('_guest_').removesuffix('_worker') else text, name) for name in names]
            (stage / 'workers.h').write_text('\n'.join(fragments))
            binary = stage / label
            command = ['gcc-14', '-std=gnu23', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror', '-g', '-O0',
                       '-ffunction-sections', '-fdata-sections', '-I' + str(source / 'src'), '-I' + str(source / 'include'),
                       '-I' + str(source / 'include/purecvisor'), '-I' + str(stage)]
            if label != 'normal':
                command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
            command += [str(fixture), str(source / 'src/modules/dispatcher/rpc_utils.c'), *flags, '-Wl,--gc-sections', '-o', str(binary)]
            built = subprocess.run(command, capture_output=True, text=True, timeout=60)
            (output / (label + '-build.log')).write_text(built.stdout + built.stderr)
            assert built.returncode == 0 and not built.stderr, label + ' build failed'
            selector = ['-p', '/guest-errors/' + label.removeprefix('restore-') + '/normal'] if label.startswith('restore-') else []
            tested = subprocess.run([str(binary), *selector], capture_output=True, text=True, timeout=60,
                                   env={**os.environ, 'G_DEBUG': 'fatal-warnings', 'ASAN_OPTIONS': 'detect_leaks=1:halt_on_error=1',
                                        'UBSAN_OPTIONS': 'halt_on_error=1'})
            (output / (label + '-tests.log')).write_text(tested.stdout + tested.stderr)
            if selector:
                assert tested.returncode != 0, label + ' control removal undetected'
                result['mutations'].append({'control': label, 'detected': True, 'exit_code': tested.returncode})
            else:
                assert tested.returncode == 0 and not tested.stderr, label + ' tests failed'
                cases = re.findall(r'^ok \d+ (.+)$', tested.stdout, re.M)
                assert len(cases) == 16
                result['runs'].append({'mode': label, 'cases': cases, 'count': 16, 'warnings': 0, 'exit_code': 0})
    result['local_gate'] = 'PASSED'
    (output / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'cases': 16, 'sanitizers': 'passed', 'mutations': 4, 'actual_vm_calls': 0}))


if __name__ == '__main__':
    main()
