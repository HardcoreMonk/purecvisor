#!/usr/bin/env python3
import os
from pathlib import Path
import re
import resource
import shlex
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


def extract_function(source, name):
    match = re.search(r"(?m)^static[^\n]*\n" + re.escape(name) + r"\([^;]*?\)\s*\{", source)
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
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
typedef void UdsServer;
typedef enum { PCV_DPDK_META_NONE, PCV_DPDK_META_OK, PCV_DPDK_META_INVALID } PcvDpdkMetaResult;
static gchar *fixture_xml, *disk_path, *nvram_path;
static gboolean domain_present = TRUE;
static gboolean fail_undefine, fail_state, xml_unavailable, fake_zvol, fail_disk_access;
static guint undefine_calls, disk_cleanup_calls;
#define PCV_LOG_WARN(component, ...) ((void)fprintf(stderr, __VA_ARGS__))
#define PCV_LOG_INFO(component, ...) ((void)fprintf(stderr, __VA_ARGS__))
#define PCV_LOG_ERROR(component, ...) ((void)fprintf(stderr, __VA_ARGS__))
static virConnectPtr virt_conn_pool_acquire(void) { return (virConnectPtr)1; }
static void virt_conn_pool_release(virConnectPtr conn) { (void)conn; }
static virDomainPtr pure_virt_get_domain(virConnectPtr conn, const gchar *name) {
    (void)conn; (void)name; return domain_present ? (virDomainPtr)2 : NULL;
}
const char *virDomainGetName(virDomainPtr dom) { (void)dom; return "nvram-regression"; }
char *virDomainGetXMLDesc(virDomainPtr dom, unsigned int flags) {
    (void)dom; (void)flags; return xml_unavailable ? NULL : strdup(fixture_xml);
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
    (void)dom; (void)bridge; return PCV_DPDK_META_NONE;
}
static gboolean pcv_dpdk_vm_port_delete(const gchar *name, GError **error) {
    (void)name; (void)error; return TRUE;
}
static gboolean pcv_dpdk_vm_port_ensure(const gchar *bridge, const gchar *name, GError **error) {
    (void)bridge; (void)name; (void)error; return TRUE;
}
static void pcv_security_group_sync_vm(const gchar *name) { (void)name; }
static gboolean pcv_spawn_sync(const gchar *const *argv, gchar **out, gchar **err, GError **error) {
    (void)out; (void)error;
    if (g_str_equal(argv[0], "zfs")) {
        disk_cleanup_calls++;
        if (err) *err = g_strdup("injected storage failure");
        return FALSE;
    }
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
    disk_path = g_build_filename(argv[2], "disk.qcow2", NULL);
    nvram_path = g_build_filename(argv[2], "vm_VARS.fd", NULL);
    gboolean bios = g_str_equal(scenario, "bios");
    gboolean missing = g_str_equal(scenario, "missing");
    gboolean disk_fail = g_str_equal(scenario, "disk-failure");
    gboolean cleanup_fail = g_str_equal(scenario, "nvram-failure");
    gboolean unsupported = g_str_equal(scenario, "unsupported");
    gboolean varstore = g_str_equal(scenario, "varstore");
    gboolean relative = g_str_equal(scenario, "relative");
    gboolean source_form = g_str_equal(scenario, "source-file");
    fail_undefine = g_str_equal(scenario, "undefine-failure");
    fail_state = g_str_equal(scenario, "state-failure");
    xml_unavailable = g_str_equal(scenario, "xml-unavailable");
    fake_zvol = g_str_equal(scenario, "zfs-failure");
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
    fixture_xml = g_strdup_printf("<domain><os><loader>/do-not-delete/code.fd</loader>%s</os>"
        "<devices><disk device='cdrom'><source file='/do-not-delete/installer.iso'/></disk>"
        "<disk device='disk'><source file='%s'/></disk></devices></domain>", nvram, disk_path);
    g_free(nvram);
    VmDeleteCtx ctx = {.vm_id = "nvram-regression"};
    GTask *task = g_task_new(NULL, NULL, NULL, NULL);
    _vm_delete_worker(task, NULL, &ctx, NULL);
    GError *error = NULL;
    gboolean result = g_task_propagate_boolean(task, &error);
    gboolean expect_failure = disk_fail || cleanup_fail || unsupported || varstore || relative ||
        fail_undefine || fail_state || xml_unavailable || fake_zvol || fail_disk_access;
    g_assert_cmpint(result, ==, !expect_failure);
    if (expect_failure) {
        g_assert_nonnull(error);
        g_assert_true(g_file_test(nvram_path, G_FILE_TEST_EXISTS));
        if (!cleanup_fail) {
            gchar *contents = NULL;
            g_assert_true(g_file_get_contents(nvram_path, &contents, NULL, NULL));
            g_assert_cmpstr(contents, ==, "uefi-settings");
            g_free(contents);
            g_assert_true(domain_present);
            g_assert_true(g_file_test(disk_path, G_FILE_TEST_EXISTS));
        } else {
            g_assert_nonnull(strstr(error->message, "NVRAM"));
            g_assert_nonnull(strstr(error->message, nvram_path));
            g_assert_false(domain_present);
            g_assert_false(g_file_test(disk_path, G_FILE_TEST_EXISTS));
        }
        if (disk_fail || fail_disk_access) {
            g_assert_cmpuint(undefine_calls, ==, 1);
            g_assert_nonnull(strstr(error->message, "Disk file cleanup failed"));
        }
        if (fake_zvol) {
            g_assert_cmpuint(undefine_calls, ==, 1);
            g_assert_cmpuint(disk_cleanup_calls, ==, 1);
            g_assert_nonnull(strstr(error->message, "ZFS destroy failed"));
        }
        if (unsupported || varstore || relative || xml_unavailable || fail_state) {
            g_assert_cmpuint(undefine_calls, ==, 0);
            g_assert_cmpuint(disk_cleanup_calls, ==, 0);
        }
    } else {
        g_assert_no_error(error);
        g_assert_false(domain_present);
        g_assert_false(g_file_test(disk_path, G_FILE_TEST_EXISTS));
        g_assert_false(g_file_test(nvram_path, G_FILE_TEST_EXISTS));
    }
    g_clear_error(&error);
    g_object_unref(task);
    g_free(fixture_xml); g_free(disk_path); g_free(nvram_path);
    return 0;
}
'''


class VmDeleteNvramTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        cls.build = tempfile.TemporaryDirectory(prefix="pcv-nvram-test-")
        cls.addClassCleanup(cls.build.cleanup)
        source = (ROOT / "src/modules/dispatcher/handler_vm_lifecycle.c").read_text()
        names = ["_extract_domain_disk_source_attr", "_vm_delete_restore_dpdk_port"]
        if "_vm_delete_prepare_nvram(" in source:
            names += ["_xml_direct_child", "_vm_delete_prepare_nvram"]
        names += ["_vm_delete_worker"]
        context = re.search(r"typedef struct\s*\{[^{}]*\}\s*VmDeleteCtx;", source).group()
        unit = Path(cls.build.name) / "worker.c"
        unit.write_text(PREAMBLE + context + "\n" + "\n".join(extract_function(source, n) for n in names) + MAIN)
        cls.binary = Path(cls.build.name) / "worker"
        flags = subprocess.check_output(["pkg-config", "--cflags", "--libs", "gio-2.0", "libvirt", "libxml-2.0"], text=True)
        subprocess.run([os.environ.get("CC", "cc"), "-std=gnu23", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-variable", "-Wno-unused-but-set-variable", "-g", str(unit),
                        "-o", str(cls.binary), *shlex.split(flags),
                        *shlex.split(os.environ.get("PCV_NVRAM_TEST_CFLAGS", ""))], check=True)

    def test_worker_resource_effects(self):
        for scenario in ("uefi", "bios", "missing", "source-file", "disk-failure", "zfs-failure",
                         "undefine-failure", "state-failure", "xml-unavailable", "unsupported",
                         "relative", "varstore", "nvram-failure", "disk-access-failure"):
            with self.subTest(scenario=scenario), tempfile.TemporaryDirectory(prefix="pcv-nvram-fixture-") as directory:
                result = subprocess.run([str(self.binary), scenario, directory], text=True, capture_output=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
