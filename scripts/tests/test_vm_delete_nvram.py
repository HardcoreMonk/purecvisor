#!/usr/bin/env python3
import os
import sys
from pathlib import Path
import re
import resource
import shlex
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import rpc_extract


def extract_function(source, name):

    match = re.search(r"(?m)^[A-Za-z_][^\n]*\n" + re.escape(name) + r"\([^;]*?\)\s*\{", source)
    if not match:
        raise ValueError(name)
    depth = 1
    tokens = re.finditer(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|/\*.*?\*/|//[^\n]*|[{}]', source[match.end():], re.S)
    for token in tokens:
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                return source[match.start():match.end() + token.end()]
    raise ValueError(name + " is unterminated")


PREAMBLE = r'''
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <errno.h>
#include <sys/stat.h>
#include "modules/dispatcher/vm_delete_plan.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
typedef struct PcvVmLock PcvVmLock;   /* VmDeleteCtx.lock 선언용 — 시험 단위는 핸들을 만지지 않는다 */
typedef struct _PcvJobRef PcvJobRef;  /* VmDeleteCtx.job_ref 선언용 — 워커는 Job을 만지지 않는다 */
typedef void UdsServer;
typedef enum { PCV_DPDK_META_NONE, PCV_DPDK_META_OK, PCV_DPDK_META_INVALID } PcvDpdkMetaResult;
static gchar *fixture_xml, *disk_path, *nvram_path;
static gboolean domain_present = TRUE;
static gboolean fail_undefine, fail_state, xml_unavailable, fake_zvol, fail_disk_access, zfs_destroy_ok;
static guint undefine_calls, disk_cleanup_calls, fuser_calls;
/* ADR-0098 거절·연결 시험 */
static gboolean conn_unavailable, lookup_error_other, list_domains_fail, clones_query_fail, other_xml_fail;
static const gchar *clones_output = "";     /* 시험 zfs list 출력 */
static const gchar *other_domain_xml;       /* 다른 도메인 하나의 XML. NULL이면 다른 도메인 없음 */
static const gchar *backing_output;         /* 시험 qemu-img info 출력. NULL이면 실행 실패 */
static guint port_delete_calls;
static PcvDpdkMetaResult dpdk_meta_result = PCV_DPDK_META_NONE;
static const gchar *last_destroy_flag;
static virError fixture_error;
static virErrorPtr fixture_last_error(void) {
    if (!domain_present) { fixture_error.code = lookup_error_other ? VIR_ERR_SYSTEM_ERROR : VIR_ERR_NO_DOMAIN;
                           fixture_error.message = (char *)"fixture lookup error"; return &fixture_error; }
    return NULL;
}
#define virGetLastError fixture_last_error
static gint fuser_exit;                    /* 시험 fuser 종료 코드(-1 = 실행 실패) */
static const gchar *fuser_stderr = "";     /* 시험 fuser stderr */
#define PCV_LOG_WARN(component, ...) ((void)fprintf(stderr, __VA_ARGS__))
#define PCV_LOG_INFO(component, ...) ((void)fprintf(stderr, __VA_ARGS__))
#define PCV_LOG_ERROR(component, ...) ((void)fprintf(stderr, __VA_ARGS__))
static virConnectPtr virt_conn_pool_acquire(void) { return conn_unavailable ? NULL : (virConnectPtr)1; }
static void virt_conn_pool_release(virConnectPtr conn) { (void)conn; }
static virDomainPtr pure_virt_get_domain(virConnectPtr conn, const gchar *name) {
    (void)conn; (void)name; return domain_present ? (virDomainPtr)2 : NULL;
}
const char *virDomainGetName(virDomainPtr dom) { return dom == (virDomainPtr)3 ? "other-vm" : "nvram-regression"; }
char *virDomainGetXMLDesc(virDomainPtr dom, unsigned int flags) {
    (void)flags;
    if (dom == (virDomainPtr)3) return other_xml_fail ? NULL : strdup(other_domain_xml);
    return xml_unavailable ? NULL : strdup(fixture_xml);
}
int virDomainIsActive(virDomainPtr dom) { (void)dom; return 0; }
int virConnectListAllDomains(virConnectPtr conn, virDomainPtr **domains, unsigned int flags) {
    (void)conn; (void)flags;
    if (list_domains_fail) return -1;
    /* 자기 자신도 목록에 나온다. 건너뛰는지 보려고 자기 XML이 참조로 잡히면 거절이 나오게 둔다. */
    int count = 0;
    *domains = calloc(2, sizeof(virDomainPtr));
    if (domain_present) (*domains)[count++] = (virDomainPtr)2;
    if (other_domain_xml) (*domains)[count++] = (virDomainPtr)3;
    return count;
}
int virDomainGetInfo(virDomainPtr dom, virDomainInfoPtr info) {
    (void)dom; info->state = VIR_DOMAIN_SHUTOFF; return fail_state ? -1 : 0;
}
int virDomainDestroy(virDomainPtr dom) { (void)dom; return 0; }
int virDomainFree(virDomainPtr dom) { (void)dom; return 0; }
int virDomainUndefineFlags(virDomainPtr dom, unsigned int flags) {
    (void)dom; undefine_calls++;
    if (fail_undefine) return -1;
    if (strstr(fixture_xml, "<nvram") &&
        !(flags & (VIR_DOMAIN_UNDEFINE_NVRAM | VIR_DOMAIN_UNDEFINE_KEEP_NVRAM))) return -1;
    if (flags & VIR_DOMAIN_UNDEFINE_NVRAM) g_unlink(nvram_path);
    domain_present = FALSE; return 0;
}
int virDomainUndefine(virDomainPtr dom) { return virDomainUndefineFlags(dom, 0); }
virDomainPtr virDomainDefineXML(virConnectPtr conn, const char *xml) {
    (void)conn; (void)xml; domain_present = TRUE; return (virDomainPtr)2;
}
static const gchar *pcv_config_get_zvol_pool(void) { return "nvram-regression/vms"; }
static PcvDpdkMetaResult pcv_vm_dpdk_metadata_read(virDomainPtr dom, gchar **bridge) {
    (void)dom; (void)bridge; return dpdk_meta_result;
}
static gboolean pcv_dpdk_vm_port_delete(const gchar *name, GError **error) {
    (void)name; (void)error; port_delete_calls++; return TRUE;
}
static gboolean pcv_dpdk_vm_port_ensure(const gchar *bridge, const gchar *name, GError **error) {
    (void)bridge; (void)name; (void)error; return TRUE;
}
static void pcv_security_group_sync_vm(const gchar *name) { (void)name; }
/* 실제 psmisc fuser: 장치를 연 프로세스가 없으면 출력 없이 1, 장치가 없으면 stderr 문구 + 1,
 * 프로세스를 찾아 죽였으면 0(장치 이름은 stderr). 실행 자체 실패는 FALSE + GError. */
static gboolean fixture_fuser(gchar **err, gint *exit_status, GError **error) {
    fuser_calls++;
    if (fuser_exit < 0) {
        g_set_error(error, G_SPAWN_ERROR, G_SPAWN_ERROR_NOENT, "Failed to execute child process \"fuser\"");
        return FALSE;
    }
    *exit_status = fuser_exit;
    if (err) *err = g_strdup(fuser_stderr);
    return TRUE;
}
/* pcv_spawn_sync 계약 그대로: 0이 아닌 종료는 FALSE + "fuser: <stderr 또는 non-zero status>" */
static gboolean pcv_spawn_sync(const gchar *const *argv, gchar **out, gchar **err, GError **error) {
    (void)out;
    if (g_str_equal(argv[0], "fuser")) {
        gint status = -1;
        gchar *text = NULL;
        if (!fixture_fuser(&text, &status, error)) return FALSE;
        if (status != 0)
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "fuser: %s",
                        err && *text ? text : "process exited with non-zero status");
        if (err) *err = text; else g_free(text);
        return status == 0;
    }
    return TRUE;
}
/* pcv_zfs_destroy_locked 계약: 실패 때 stderr를 돌려준다(풀 락은 이 시험의 관심 밖). */
static gboolean pcv_zfs_destroy_locked(const gchar *flag, const gchar *target, gchar **err, GError **error) {
    (void)target; (void)error;
    disk_cleanup_calls++;
    last_destroy_flag = flag;
    if (zfs_destroy_ok) return TRUE;
    if (err) *err = g_strdup("injected storage failure");
    return FALSE;
}
/* pcv_spawn_sync_status 계약: 정상 종료면 코드와 무관하게 TRUE + exit_status, stderr는 빈 문자열이라도 non-NULL */
static G_GNUC_UNUSED gboolean pcv_spawn_sync_status(const gchar *const *argv, gchar **out, gchar **err,
                                                    gint *exit_status, GError **error) {
    (void)out;
    *exit_status = -1;
    if (g_str_equal(argv[0], "fuser")) return fixture_fuser(err, exit_status, error);
    *exit_status = 0;
    if (err) *err = g_strdup("");
    return TRUE;
}
/* 거절 검사의 조회: zfs list·qemu-img info */
static gboolean pcv_spawn_sync_timeout(const gchar *const *argv, gchar **out, gchar **err, guint timeout_sec, GError **error) {
    (void)err; (void)timeout_sec;
    if (g_str_equal(argv[0], "zfs")) {
        if (clones_query_fail) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "fixture zfs list failed"); return FALSE; }
        *out = g_strdup(clones_output);
        return TRUE;
    }
    if (!backing_output) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "fixture qemu-img failed"); return FALSE; }
    *out = g_strdup(backing_output);
    return TRUE;
}
static int fixture_access(const gchar *path, int mode) {
    if (g_str_has_prefix(path, "/dev/zvol/")) return fake_zvol ? 0 : -1;
    if (fail_disk_access && g_str_equal(path, disk_path)) { errno = EACCES; return -1; }
    return access(path, mode);
}
static int fixture_unlink(const gchar *path) {
    if (fail_disk_access && g_str_equal(path, disk_path)) { errno = EACCES; return -1; }
    return unlink(path);
}
#define access fixture_access
#define unlink fixture_unlink
#define g_usleep(usec) ((void)(usec))
'''


MAIN = r'''
int main(int argc, char **argv) {
    g_assert_cmpint(argc, ==, 3);
    g_assert_cmpint(virInitialize(), ==, 0);
    const gchar *scenario = argv[1];
    disk_path = g_build_filename(argv[2], "nvram-regression.qcow2", NULL);
    nvram_path = g_build_filename(argv[2], "vm_VARS.fd", NULL);
    gboolean bios = g_str_equal(scenario, "bios");
    gboolean missing = g_str_equal(scenario, "missing");
    gboolean disk_fail = g_str_equal(scenario, "disk-failure");
    gboolean cleanup_fail = g_str_equal(scenario, "nvram-failure");
    gboolean unsupported = g_str_equal(scenario, "unsupported");
    gboolean varstore = g_str_equal(scenario, "varstore");
    gboolean relative = g_str_equal(scenario, "relative");
    gboolean source_form = g_str_equal(scenario, "source-file");
    gboolean absent = g_str_equal(scenario, "absent");
    fail_undefine = g_str_equal(scenario, "undefine-failure");
    fail_state = g_str_equal(scenario, "state-failure");
    xml_unavailable = g_str_equal(scenario, "xml-unavailable");
    gboolean zfs_fail = g_str_equal(scenario, "zfs-failure");
    /*  Level 3: zvol 삭제의 fuser 판정 — 연 프로세스 없음(1, stderr 없음)·죽임(0)은 정상, 나머지는 부분 성공 */
    gboolean fuser_unused = g_str_equal(scenario, "fuser-unused");
    gboolean fuser_killed = g_str_equal(scenario, "fuser-killed");
    gboolean fuser_missing = g_str_equal(scenario, "fuser-missing");
    gboolean fuser_exit2 = g_str_equal(scenario, "fuser-exit2");
    gboolean fuser_spawn = g_str_equal(scenario, "fuser-spawn-failure");
    gboolean exorcism_warning = fuser_missing || fuser_exit2 || fuser_spawn;
    gboolean fuser_case = fuser_unused || fuser_killed || exorcism_warning;
    /* ADR-0098·VM-DELETE-CONN-AS-ABSENT */
    conn_unavailable = g_str_equal(scenario, "conn-unavailable");
    lookup_error_other = g_str_equal(scenario, "lookup-error");
    gboolean clone_refused = g_str_equal(scenario, "clone-refused");
    clones_query_fail = g_str_equal(scenario, "clones-query-failure");
    list_domains_fail = g_str_equal(scenario, "list-failure");
    other_xml_fail = g_str_equal(scenario, "other-xml-failure");
    gboolean file_referenced = g_str_equal(scenario, "file-referenced");
    gboolean backing_refused = g_str_equal(scenario, "backing-refused");
    gboolean backing_query_fail = g_str_equal(scenario, "backing-query-failure");
    gboolean attached_zvol = g_str_equal(scenario, "attached-zvol");
    gboolean foreign_first = g_str_equal(scenario, "foreign-first");
    gboolean zvol_boot = g_str_equal(scenario, "zvol-boot");
    gboolean refused = clone_refused || clones_query_fail || list_domains_fail || other_xml_fail ||
        file_referenced || backing_refused || backing_query_fail;
    if (clone_refused) clones_output = "nvram-regression/vms/nvram-regression@a\tnvram-regression/vms/kid\n";
    fake_zvol = zfs_fail || fuser_case || zvol_boot || attached_zvol || clone_refused || clones_query_fail;
    zfs_destroy_ok = fuser_case || zvol_boot || attached_zvol;
    if (refused) dpdk_meta_result = PCV_DPDK_META_OK;   /* 거절이면 포트 제거까지 가지 않아야 한다 */
    fuser_exit = fuser_killed ? 0 : fuser_exit2 ? 2 : fuser_spawn ? -1 : 1;
    fuser_stderr = fuser_killed ? "/dev/zvol/nvram-regression/vms/nvram-regression:"
        : fuser_missing ? "Specified filename /dev/zvol/nvram-regression/vms/nvram-regression does not exist." : "";
    fail_disk_access = g_str_equal(scenario, "disk-access-failure");
    if (disk_fail) g_assert_cmpint(g_mkdir(disk_path, 0700), ==, 0);
    else g_assert_true(g_file_set_contents(disk_path, "guest-disk", -1, NULL));
    if (cleanup_fail) g_assert_cmpint(g_mkdir(nvram_path, 0700), ==, 0);
    else if (!bios && !missing) g_assert_true(g_file_set_contents(nvram_path, "uefi-settings", -1, NULL));
    gchar *nvram = bios ? g_strdup("") : source_form
        ? g_strdup_printf("<nvram type='file'><source file='%s'/></nvram>", nvram_path)
        : varstore ? g_strdup_printf("<varstore path='%s'/>", nvram_path)
        : unsupported ? g_strdup("<nvram type='block'><source dev='/dev/example'/></nvram>")
        : relative ? g_strdup("<nvram>relative-vars.fd</nvram>")
        : g_strdup_printf("<nvram template='/do-not-delete/template.fd'>%s</nvram>", nvram_path);
    gboolean zvol_first = fuser_case || zfs_fail || clone_refused || clones_query_fail || zvol_boot;
    gchar *disks = zvol_first
        ? g_strdup_printf("<disk device='disk'><source dev='/dev/zvol/nvram-regression/vms/nvram-regression'/></disk>"
                          "<disk device='disk'><source file='%s'/></disk>", disk_path)
        : foreign_first
        ? g_strdup_printf("<disk device='disk'><source dev='/dev/zvol/nvram-regression/vms/other'/></disk>"
                          "<disk device='disk'><source file='%s'/></disk>", disk_path)
        : attached_zvol
        ? g_strdup_printf("<disk device='disk'><source file='%s'/></disk>"
                          "<disk device='disk'><source dev='/dev/zvol/nvram-regression/vms/other'/></disk>", disk_path)
        : g_strdup_printf("<disk device='disk'><source file='%s'/></disk>", disk_path);
    fixture_xml = g_strdup_printf("<domain><os><loader>/do-not-delete/code.fd</loader>%s</os>"
        "<devices><disk device='cdrom'><source file='/do-not-delete/installer.iso'/></disk>"
        "%s</devices></domain>", nvram, disks);
    g_free(disks);
    g_free(nvram);
    gchar *other_xml = NULL;
    if (other_xml_fail || file_referenced)
        other_xml = g_strdup_printf("<domain><devices><disk device='disk'><source file='%s'/></disk></devices></domain>", disk_path);
    else if (backing_refused || backing_query_fail) {
        gchar *overlay = g_build_filename(argv[2], "overlay.qcow2", NULL);
        g_assert_true(g_file_set_contents(overlay, "overlay", -1, NULL));
        other_xml = g_strdup_printf("<domain><devices><disk device='disk'><driver type='qcow2'/><source file='%s'/></disk></devices></domain>", overlay);
        g_free(overlay);
    }
    other_domain_xml = other_xml;
    gchar *backing_json = backing_refused
        ? g_strdup_printf("[{\"filename\":\"x\",\"full-backing-filename\":\"%s\"},{\"filename\":\"y\"}]", disk_path) : NULL;
    backing_output = backing_json;
    VmDeleteCtx ctx = {.vm_id = "nvram-regression", .warnings = g_ptr_array_new_with_free_func(g_free)};
    if (absent || lookup_error_other) domain_present = FALSE;   /* 정의·zvol·파일 디스크가 모두 없다 → 멱등 성공 + already_absent */
    GTask *task = g_task_new(NULL, NULL, NULL, NULL);
    _vm_delete_worker(task, NULL, &ctx, NULL);
    GError *error = NULL;
    gboolean result = g_task_propagate_boolean(task, &error);
    gboolean expect_failure = disk_fail || unsupported || varstore || relative ||
        fail_undefine || fail_state || xml_unavailable || zfs_fail || fail_disk_access ||
        conn_unavailable || lookup_error_other || refused;
    g_assert_cmpint(result, ==, !expect_failure);
    g_assert_cmpint(ctx.already_absent, ==, absent);
    if (lookup_error_other) domain_present = TRUE;   /* 조회 오류일 뿐 정의는 있다 — 아래 공통 단언용 */
    if (conn_unavailable || lookup_error_other || refused) {
        /* 아무것도 안 바뀐다: 포트·정의·저장소 호출 0, 파일 그대로, 경고 없음, 이미 없음이 아니다 */
        g_assert_cmpuint(undefine_calls, ==, 0);
        g_assert_cmpuint(disk_cleanup_calls, ==, 0);
        g_assert_cmpuint(fuser_calls, ==, 0);
        g_assert_cmpuint(port_delete_calls, ==, 0);
        g_assert_false(ctx.already_absent);
        g_assert_nonnull(strstr(error->message, "Nothing was changed"));
    }
    if (clone_refused) {
        g_assert_nonnull(strstr(error->message, "nvram-regression/vms/kid"));
        g_assert_nonnull(strstr(error->message, "zfs promote"));
    }
    if (file_referenced || backing_refused) g_assert_nonnull(strstr(error->message, "other-vm"));
    if (backing_refused) g_assert_nonnull(strstr(error->message, "backing file"));
    if (zvol_boot) {
        /* 자기 zvol만 파괴하고 -R이 아니라 -r이다. 뒤 블록의 이름 맞는 파일은 지우지 않고 경고한다 */
        g_assert_cmpuint(disk_cleanup_calls, ==, 1);
        g_assert_cmpstr(last_destroy_flag, ==, "-r");
        g_assert_true(g_file_test(disk_path, G_FILE_TEST_EXISTS));
        g_assert_cmpuint(ctx.warnings->len, ==, 1);
        g_assert_nonnull(strstr(g_ptr_array_index(ctx.warnings, 0), disk_path));
        g_ptr_array_unref(ctx.warnings);
        g_clear_error(&error);
        g_object_unref(task);
        return 0;
    }
    if (attached_zvol) {
        /* 파일 부팅 + 붙인 zvol: 파일만 지우고 zvol은 fuser·destroy 모두 없이 경고로 남는다 */
        g_assert_cmpuint(disk_cleanup_calls, ==, 0);
        g_assert_cmpuint(fuser_calls, ==, 0);
        g_assert_false(g_file_test(disk_path, G_FILE_TEST_EXISTS));
        g_assert_cmpuint(ctx.warnings->len, ==, 1);
        g_assert_nonnull(strstr(g_ptr_array_index(ctx.warnings, 0), "nvram-regression/vms/other"));
        g_ptr_array_unref(ctx.warnings);
        g_clear_error(&error);
        g_object_unref(task);
        return 0;
    }
    if (foreign_first) {
        /* 첫 블록이 남의 것: 저장소 호출 없이 정의만 지우고 둘 다 경고한다 */
        g_assert_no_error(error);
        g_assert_false(domain_present);
        g_assert_cmpuint(disk_cleanup_calls, ==, 0);
        g_assert_true(g_file_test(disk_path, G_FILE_TEST_EXISTS));
        g_assert_cmpuint(ctx.warnings->len, ==, 2);
        g_ptr_array_unref(ctx.warnings);
        g_object_unref(task);
        return 0;
    }
    if (expect_failure) {
        g_assert_nonnull(error);
        g_assert_cmpuint(ctx.warnings->len, ==, 0);
        g_assert_true(g_file_test(nvram_path, G_FILE_TEST_EXISTS));
        gchar *contents = NULL;
        g_assert_true(g_file_get_contents(nvram_path, &contents, NULL, NULL));
        g_assert_cmpstr(contents, ==, "uefi-settings");
        g_free(contents);
        g_assert_true(domain_present);
        g_assert_true(g_file_test(disk_path, G_FILE_TEST_EXISTS));
        if (disk_fail || fail_disk_access) {
            g_assert_cmpuint(undefine_calls, ==, 1);
            g_assert_nonnull(strstr(error->message, "Disk file cleanup failed"));
        }
        if (zfs_fail) {
            g_assert_cmpuint(undefine_calls, ==, 1);
            g_assert_cmpuint(disk_cleanup_calls, ==, 1);
            g_assert_nonnull(strstr(error->message, "ZFS destroy failed"));
        }
        if (unsupported || varstore || relative || xml_unavailable || fail_state) {
            g_assert_cmpuint(undefine_calls, ==, 0);
            g_assert_cmpuint(disk_cleanup_calls, ==, 0);
        }
    } else if (absent) {
        g_assert_no_error(error);
        g_assert_cmpuint(ctx.warnings->len, ==, 0);
        g_assert_cmpuint(undefine_calls, ==, 0);
        g_assert_true(g_file_test(disk_path, G_FILE_TEST_EXISTS));   /* 정의가 없어 경로를 모른다 — 건드리지 않는다 */
    } else {
        g_assert_no_error(error);
        g_assert_false(domain_present);
        g_assert_true(fuser_case || !g_file_test(disk_path, G_FILE_TEST_EXISTS));
        if (fuser_case) {
            g_assert_cmpuint(fuser_calls, ==, 1);
            g_assert_cmpuint(disk_cleanup_calls, ==, 1);   /* fuser 결과와 무관하게 zfs destroy까지 간다 */
        }
        if (fuser_case) {
            /* 시나리오 fixture의 두 번째 블록(이름 맞는 파일)은 소유가 아니라 남고 경고 하나가 더 붙는다 */
            guint expected = exorcism_warning ? 2 : 1;
            g_assert_cmpuint(ctx.warnings->len, ==, expected);
            if (exorcism_warning)
                g_assert_cmpstr(g_ptr_array_index(ctx.warnings, 0), ==, "storage exorcism was partial; see daemon log");
            g_assert_nonnull(strstr(g_ptr_array_index(ctx.warnings, expected - 1), disk_path));
            g_assert_false(g_file_test(nvram_path, G_FILE_TEST_EXISTS));
        } else if (exorcism_warning) {
            /* 진짜 fuser 실패(실행 실패·exit 2·stderr 문구 + 1)만 부분 성공 경고로 남는다 */
            g_assert_cmpuint(ctx.warnings->len, ==, 1);
            g_assert_cmpstr(g_ptr_array_index(ctx.warnings, 0), ==, "storage exorcism was partial; see daemon log");
            g_assert_false(g_file_test(nvram_path, G_FILE_TEST_EXISTS));
        } else if (cleanup_fail) {
            /*  A3: 정의·디스크가 지워졌으므로 NVRAM 정리 실패는 성공 + 경고 1건이고 파일은 남는다 */
            g_assert_cmpuint(ctx.warnings->len, ==, 1);
            const gchar *warning = g_ptr_array_index(ctx.warnings, 0);
            g_assert_true(g_str_has_prefix(warning, "NVRAM cleanup failed: "));
            g_assert_nonnull(strstr(warning, nvram_path));
            g_assert_true(g_str_has_suffix(warning, "; manual cleanup is required"));
            g_assert_true(g_file_test(nvram_path, G_FILE_TEST_IS_DIR));
        } else {
            g_assert_cmpuint(ctx.warnings->len, ==, 0);
            g_assert_false(g_file_test(nvram_path, G_FILE_TEST_EXISTS));
        }
    }
    g_ptr_array_unref(ctx.warnings);
    g_clear_error(&error);
    g_object_unref(task);
    g_free(fixture_xml); g_free(disk_path); g_free(nvram_path); g_free(other_xml); g_free(backing_json);
    return 0;
}
'''


class VmDeleteNvramTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):

        try:
            resource.setrlimit(resource.RLIMIT_CORE, (1, 1))
        except (ValueError, OSError):
            resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        cls.build = tempfile.TemporaryDirectory(prefix="pcv-nvram-test-")
        cls.addClassCleanup(cls.build.cleanup)
        source = rpc_extract.vm_family_text("vm_lifecycle")
        names = ["_vm_delete_restore_dpdk_port", "pcv_disp_xml_direct_child", "_vm_delete_prepare_nvram",
                 "_vm_delete_same_storage", "_vm_delete_refuse_clones", "_vm_delete_refuse_backing",
                 "_vm_delete_refuse_other_domain", "_vm_delete_refuse_dependents", "_vm_delete_worker"]
        context = re.search(r"typedef struct\s*\{[^{}]*\}\s*VmDeleteCtx;", source).group()
        unit = Path(cls.build.name) / "worker.c"
        defines = "#define VM_DELETE_QUERY_TIMEOUT_SEC 60\n#define VM_DELETE_REFUSAL_MAX_NAMES 8\n"
        unit.write_text(PREAMBLE + context + "\n" + defines + "\n".join(extract_function(source, n) for n in names) + MAIN)
        cls.binary = Path(cls.build.name) / "worker"
        flags = subprocess.check_output(["pkg-config", "--cflags", "--libs", "gio-2.0", "libvirt", "libxml-2.0"], text=True)
        flags = subprocess.check_output(["pkg-config", "--cflags", "--libs", "gio-2.0", "libvirt", "libxml-2.0", "json-glib-1.0"], text=True)

        subprocess.run([os.environ.get("CC", "cc"), "-std=gnu23", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-variable", "-Wno-unused-but-set-variable", "-g", str(unit),
                        str(ROOT / "src/modules/dispatcher/vm_delete_plan.c"), "-I", str(ROOT / "src"),
                        "-o", str(cls.binary), *shlex.split(flags),
                        *shlex.split(os.environ.get("PCV_NVRAM_TEST_CFLAGS", ""))], check=True)

    def run_scenario(self, scenario):
        with tempfile.TemporaryDirectory(prefix="pcv-nvram-fixture-") as directory:
            return subprocess.run([str(self.binary), scenario, directory], text=True, capture_output=True)

    def test_worker_resource_effects(self):
        for scenario in ("uefi", "bios", "missing", "source-file", "disk-failure", "zfs-failure",
                         "undefine-failure", "state-failure", "xml-unavailable", "unsupported",
                         "relative", "varstore", "nvram-failure", "disk-access-failure", "absent",
                         "fuser-unused", "fuser-killed", "fuser-missing", "fuser-exit2", "fuser-spawn-failure",
                         "conn-unavailable", "lookup-error", "clone-refused", "clones-query-failure",
                         "list-failure", "other-xml-failure", "file-referenced", "backing-refused",
                         "backing-query-failure", "attached-zvol", "foreign-first", "zvol-boot"):
            with self.subTest(scenario=scenario):
                result = self.run_scenario(scenario)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                if scenario.startswith("fuser-"):

                    partial = scenario in ("fuser-missing", "fuser-exit2", "fuser-spawn-failure")
                    self.assertEqual("fuser partial" in result.stderr, partial, result.stderr)

    def test_fuser_failure_log_keeps_reason(self):

        result = self.run_scenario("fuser-missing")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("does not exist", result.stderr)
        self.assertIn("exit 1", result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
