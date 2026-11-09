
                                                                                               
                                                                                               
                                                                                
                                                                        
                             
   
                          
                                                             
  
                                                                               
                 
                                                                               
                                                                   
                      
  
                          
                                                  
                                           
                                            
  
          
                                                           
                                                       
                              
                                                         
                                                        
                                                    
  
                                                            
                                                
                                                                               
   
#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libvirt/libvirt.h>
#include <libvirt-gobject/libvirt-gobject.h>
#include "../src/modules/virt/vm_manager.h"
#include "../src/modules/virt/virt_conn_pool.h"
#include "../src/modules/resource/resource_registry.h"
#include "../src/utils/pcv_spawn.h"

                                                              
                                                                   
extern gchar *_overlay_ethernet_iface_xml(void);
extern gchar *_overlay_metadata_xml(const gchar *network_mode, const gchar *tenant);
extern gboolean _overlay_metadata_parse(const gchar *metadata_xml,
                                         gchar **mode_out, gchar **tenant_out);
extern gchar *_dpdk_metadata_xml(const gchar *nic_type, const gchar *bridge_name);
extern gboolean _dpdk_metadata_parse(const gchar *metadata_xml, gchar **bridge_out);

extern gboolean purecvisor_vm_sriov_pci_parse(const gchar *pci, guint out[4]);
extern gboolean purecvisor_vm_sriov_nic_check(const gchar *nic_type, const gchar *network_bridge,
                                              const gchar *network_mode, GError **error);




static gboolean
vm_manager_qemu_img_available(void)
{
    g_autofree gchar *path = g_find_program_in_path("qemu-img");
    return path != NULL;
}

static gchar *
vm_manager_file_disk_tmpdir(void)
{
    GError *error = NULL;
    gchar *directory = g_dir_make_tmp("pcv-vm-file-disk-XXXXXX", &error);
    g_assert_no_error(error);
    g_assert_nonnull(directory);
    return directory;
}

static void
assert_file_disk_shape(const gchar *format, const gchar *target)
{
    const gchar *info_argv[] = {
        "qemu-img", "info", "--output=json", target, NULL
    };
    GError *error = NULL;
    g_autofree gchar *info_json = NULL;
    g_autoptr(JsonParser) parser = json_parser_new();

    g_assert_true(pcv_spawn_sync(info_argv, &info_json, NULL, &error));
    g_assert_no_error(error);
    g_assert_true(json_parser_load_from_data(parser, info_json, -1, &error));
    g_assert_no_error(error);
    JsonObject *info = json_node_get_object(json_parser_get_root(parser));
    g_assert_cmpstr(json_object_get_string_member(info, "format"), ==, format);
    g_assert_cmpint(json_object_get_int_member(info, "virtual-size"), ==,
                    (gint64)1024 * 1024 * 1024);
}

static void
assert_file_disk_prefix(const gchar *format, const gchar *target,
                        const gchar *expected, gsize expected_len,
                        const gchar *scratch)
{
    g_autofree gchar *if_arg = g_strdup_printf("if=%s", target);
    g_autofree gchar *of_arg = g_strdup_printf("of=%s", scratch);
    const gchar *read_argv[] = {
        "qemu-img", "dd", "-f", format, "-O", "raw",
        "bs=4096", "count=1", if_arg, of_arg, NULL
    };
    GError *error = NULL;
    g_autofree gchar *actual = NULL;
    gsize actual_len = 0;

    g_assert_true(pcv_spawn_sync(read_argv, NULL, NULL, &error));
    g_assert_no_error(error);
    g_assert_true(g_file_get_contents(scratch, &actual, &actual_len, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(actual_len, >=, expected_len);
    g_assert_cmpint(memcmp(actual, expected, expected_len), ==, 0);
    assert_file_disk_shape(format, target);
}

static void
test_file_disk_base_image_populates_qcow2_and_raw(void)
{
    if (!vm_manager_qemu_img_available()) {
        g_test_skip("qemu-img not available");
        return;
    }

    g_autofree gchar *directory = vm_manager_file_disk_tmpdir();
    g_autofree gchar *source = g_build_filename(directory, "base.raw", NULL);
    g_autofree gchar *qcow2_target = g_build_filename(directory, "guest.qcow2", NULL);
    g_autofree gchar *raw_target = g_build_filename(directory, "guest.img", NULL);
    g_autofree gchar *scratch = g_build_filename(directory, "prefix.raw", NULL);
    const gchar payload[] = "purecvisor-base-image-content";
    g_autofree gchar *source_bytes = g_malloc0(1024 * 1024);
    GError *error = NULL;

    pcv_spawn_launcher_init();
    memcpy(source_bytes, payload, sizeof payload);
    g_assert_true(g_file_set_contents(source, source_bytes, 1024 * 1024, &error));
    g_assert_no_error(error);

    g_assert_true(purecvisor_vm_provision_file_disk(
        "qcow2", qcow2_target, 1, source, NULL, &error));
    g_assert_no_error(error);
    assert_file_disk_prefix("qcow2", qcow2_target, payload, sizeof payload, scratch);

    g_assert_cmpint(g_remove(scratch), ==, 0);
    g_assert_true(purecvisor_vm_provision_file_disk(
        "raw", raw_target, 1, source, NULL, &error));
    g_assert_no_error(error);
    assert_file_disk_prefix("raw", raw_target, payload, sizeof payload, scratch);

    g_assert_cmpint(g_remove(scratch), ==, 0);
    g_assert_cmpint(g_remove(raw_target), ==, 0);
    g_assert_cmpint(g_remove(qcow2_target), ==, 0);
    g_assert_cmpint(g_remove(source), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
    pcv_spawn_launcher_shutdown();
}

static void
test_file_disk_without_base_creates_requested_shape(void)
{
    if (!vm_manager_qemu_img_available()) {
        g_test_skip("qemu-img not available");
        return;
    }

    g_autofree gchar *directory = vm_manager_file_disk_tmpdir();
    g_autofree gchar *target = g_build_filename(directory, "empty.qcow2", NULL);
    GError *error = NULL;

    pcv_spawn_launcher_init();
    g_assert_true(purecvisor_vm_provision_file_disk(
        "qcow2", target, 1, NULL, NULL, &error));
    g_assert_no_error(error);
    assert_file_disk_shape("qcow2", target);

    g_assert_cmpint(g_remove(target), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
    pcv_spawn_launcher_shutdown();
}

static void
test_file_disk_missing_base_removes_target(void)
{
    g_autofree gchar *directory = vm_manager_file_disk_tmpdir();
    g_autofree gchar *source = g_build_filename(directory, "missing.qcow2", NULL);
    g_autofree gchar *target = g_build_filename(directory, "guest.qcow2", NULL);
    GError *error = NULL;

    g_assert_false(purecvisor_vm_provision_file_disk(
        "qcow2", target, 1, source, NULL, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error(&error);
    g_assert_false(g_file_test(target, G_FILE_TEST_EXISTS));
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

static void
test_file_disk_backing_base_is_refused(void)
{
    if (!vm_manager_qemu_img_available()) {
        g_test_skip("qemu-img not available");
        return;
    }
    g_autofree gchar *directory = vm_manager_file_disk_tmpdir();
    g_autofree gchar *secret = g_build_filename(directory, "secret.raw", NULL);
    g_autofree gchar *backed = g_build_filename(directory, "backed.qcow2", NULL);
    g_autofree gchar *target = g_build_filename(directory, "guest.img", NULL);
    g_autofree gchar *secret_bytes = g_malloc0(1024 * 1024);
    GError *error = NULL;
    memcpy(secret_bytes, "SECRET-BACKING-MARKER", 21);
    g_assert_true(g_file_set_contents(secret, secret_bytes, 1024 * 1024, &error));
    pcv_spawn_launcher_init();
    const gchar *mk[] = {"qemu-img", "create", "-f", "qcow2", "-b", secret, "-F", "raw", backed, "1M", NULL};
    g_assert_true(pcv_spawn_sync(mk, NULL, NULL, &error));
    g_assert_no_error(error);
    g_assert_false(purecvisor_vm_provision_file_disk("raw", target, 1, backed, NULL, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_assert_nonnull(strstr(error->message, "backing file"));
    g_clear_error(&error);
    g_assert_false(g_file_test(target, G_FILE_TEST_EXISTS));
    g_assert_cmpint(g_remove(backed), ==, 0);
    g_assert_cmpint(g_remove(secret), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
    pcv_spawn_launcher_shutdown();
}

static void
test_file_disk_convert_failure_removes_target(void)
{
    if (!vm_manager_qemu_img_available()) {
        g_test_skip("qemu-img not available");
        return;
    }

    g_autofree gchar *directory = vm_manager_file_disk_tmpdir();
    g_autofree gchar *source = g_build_filename(directory, "oversized.raw", NULL);
    g_autofree gchar *target = g_build_filename(directory, "guest.qcow2", NULL);
    GError *error = NULL;

    pcv_spawn_launcher_init();
    const gchar *source_argv[] = {
        "qemu-img", "create", "-f", "raw", source, "2G", NULL
    };
    g_assert_true(pcv_spawn_sync(source_argv, NULL, NULL, &error));
    g_assert_no_error(error);
    g_assert_false(purecvisor_vm_provision_file_disk(
        "qcow2", target, 1, source, NULL, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_clear_error(&error);
    g_assert_false(g_file_test(target, G_FILE_TEST_EXISTS));

    g_assert_cmpint(g_remove(source), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
    pcv_spawn_launcher_shutdown();
}

static void
test_file_disk_existing_target_is_preserved(void)
{
    g_autofree gchar *directory = vm_manager_file_disk_tmpdir();
    g_autofree gchar *target = g_build_filename(directory, "guest.qcow2", NULL);
    const gchar sentinel[] = "preexisting-user-disk";
    GError *error = NULL;
    g_autofree gchar *actual = NULL;

    g_assert_true(g_file_set_contents(target, sentinel, -1, &error));
    g_assert_no_error(error);
    g_assert_false(purecvisor_vm_provision_file_disk(
        "qcow2", target, 1, NULL, NULL, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS);
    g_clear_error(&error);
    g_assert_true(g_file_get_contents(target, &actual, NULL, &error));
    g_assert_no_error(error);
    g_assert_cmpstr(actual, ==, sentinel);

    g_assert_cmpint(g_remove(target), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

static void
file_disk_swap_base(const gchar *base, const gchar *keep, gsize size, PcvResPin *pin_out)
{
    g_autofree gchar *bytes = g_malloc0(size);
    GError *error = NULL;
    PcvResNorm n = {0};

    memcpy(bytes, "original", 8);
    g_assert_true(g_file_set_contents(base, bytes, (gssize)size, &error));
    g_assert_no_error(error);
    g_assert_true(pcv_res_normalize(PCV_RES_IMAGE, base, &n, &error));
    g_assert_no_error(error);
    if (pin_out) {
        *pin_out = (PcvResPin){ n.dev, n.ino, n.gen };
        g_assert_true(pcv_res_pin_matches(base, pin_out));
    }
    pcv_res_norm_clear(&n);
    g_assert_cmpint(link(base, keep), ==, 0);
    g_assert_cmpint(g_unlink(base), ==, 0);
    memcpy(bytes, "swapped!", 8);
    g_assert_true(g_file_set_contents(base, bytes, (gssize)size, &error));
    g_assert_no_error(error);
}

                                                      

static void
test_file_disk_pinned_base_swap_rejected(void)
{
    g_autofree gchar *directory = vm_manager_file_disk_tmpdir();
    g_autofree gchar *base = g_build_filename(directory, "base.raw", NULL);
    g_autofree gchar *keep = g_build_filename(directory, "keep.raw", NULL);
    g_autofree gchar *target = g_build_filename(directory, "guest.qcow2", NULL);
    GError *error = NULL;
    PcvResPin pin = { 0 };

    file_disk_swap_base(base, keep, 1024 * 1024, &pin);
    g_assert_false(purecvisor_vm_provision_file_disk("qcow2", target, 1, base, &pin, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error(&error);
    g_assert_false(g_file_test(target, G_FILE_TEST_EXISTS));

    g_assert_cmpint(g_remove(base), ==, 0);
    g_assert_cmpint(g_remove(keep), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

static void
test_file_disk_unpinned_base_swap_passes(void)
{
    if (!vm_manager_qemu_img_available()) {
        g_test_skip("qemu-img not available");
        return;
    }

    g_autofree gchar *directory = vm_manager_file_disk_tmpdir();
    g_autofree gchar *base = g_build_filename(directory, "base.raw", NULL);
    g_autofree gchar *keep = g_build_filename(directory, "keep.raw", NULL);
    g_autofree gchar *target = g_build_filename(directory, "guest.img", NULL);
    GError *error = NULL;

    pcv_spawn_launcher_init();
    file_disk_swap_base(base, keep, 1024 * 1024, NULL);
    g_assert_true(purecvisor_vm_provision_file_disk("raw", target, 1, base, NULL, &error));
    g_assert_no_error(error);

    g_assert_cmpint(g_remove(target), ==, 0);
    g_assert_cmpint(g_remove(base), ==, 0);
    g_assert_cmpint(g_remove(keep), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
    pcv_spawn_launcher_shutdown();
}

typedef struct {
    gchar *root, *bin, *log, *base, *saved_path;
} ZvolEnv;

#define QIMG_INFO_RAW "[ \"$1\" = info ] && { echo '{\"format\":\"raw\"}'; exit 0; }\n"

static void
zvol_env_mock(const ZvolEnv *e, const gchar *tool, const gchar *body)
{
    g_autofree gchar *file = g_build_filename(e->bin, tool, NULL);
    g_autofree gchar *qlog = g_shell_quote(e->log);
    g_autofree gchar *script = g_strdup_printf("#!/bin/sh\nprintf '%%s %%s\\n' \"${0##*/}\" \"$*\" >> %s\n%s", qlog, body);
    g_assert_true(g_file_set_contents(file, script, -1, NULL));
    g_assert_cmpint(g_chmod(file, 0755), ==, 0);
}

static void
zvol_env_begin(ZvolEnv *e)
{
    e->root = vm_manager_file_disk_tmpdir();
    e->bin = g_build_filename(e->root, "bin", NULL);
    e->log = g_build_filename(e->root, "calls.log", NULL);
    e->base = g_build_filename(e->root, "base.raw", NULL);
    g_assert_cmpint(g_mkdir_with_parents(e->bin, 0700), ==, 0);
    g_assert_true(g_file_set_contents(e->base, "raw-bytes", -1, NULL));
    zvol_env_mock(e, "zfs", "exit 0\n");
    zvol_env_mock(e, "udevadm", "exit 0\n");
    zvol_env_mock(e, "qemu-img", QIMG_INFO_RAW "exit 0\n");
    e->saved_path = g_strdup(g_getenv("PATH"));
    g_autofree gchar *path = g_strdup_printf("%s:%s", e->bin, e->saved_path ? e->saved_path : "");
    g_setenv("PATH", path, TRUE);
    pcv_spawn_launcher_init();
}

static void
zvol_env_end(ZvolEnv *e)
{
    pcv_spawn_launcher_shutdown();
    if (e->saved_path) g_setenv("PATH", e->saved_path, TRUE); else g_unsetenv("PATH");
    g_autofree gchar *cmd = g_strdup_printf("rm -rf %s", e->root);
    g_assert_true(g_spawn_command_line_sync(cmd, NULL, NULL, NULL, NULL));
    g_free(e->root); g_free(e->bin); g_free(e->log); g_free(e->base); g_free(e->saved_path);
}

static gchar *
zvol_env_calls(const ZvolEnv *e)
{
    gchar *text = NULL;
    return g_file_get_contents(e->log, &text, NULL, NULL) ? text : g_strdup("");
}

static void
test_zvol_disk_base_format_is_probed(void)
{
    ZvolEnv e = {0};
    zvol_env_begin(&e);
    GError *error = NULL;
    g_assert_true(purecvisor_vm_provision_zvol_disk("zp/vms/a", 2, e.base, NULL, &error));
    g_assert_no_error(error);
    g_autofree gchar *calls = zvol_env_calls(&e);
    g_autofree gchar *want = g_strdup_printf(
        "qemu-img info --output=json --force-share %s\n"
        "zfs create -V 2G zp/vms/a\n"
        "udevadm settle --timeout=5\n"
        "qemu-img info --output=json --force-share %s\n"
        "qemu-img convert -n -f raw -O raw %s /dev/zvol/zp/vms/a\n", e.base, e.base, e.base);
    g_assert_cmpstr(calls, ==, want);
    zvol_env_end(&e);
}

static void
test_zvol_disk_convert_failure_rolls_back(void)
{
    ZvolEnv e = {0};
    zvol_env_begin(&e);
    zvol_env_mock(&e, "qemu-img", QIMG_INFO_RAW "echo 'Could not open image: boom' >&2\nexit 1\n");
    GError *error = NULL;
    g_assert_false(purecvisor_vm_provision_zvol_disk("zp/vms/b", 2, e.base, NULL, &error));
    g_assert_nonnull(error);
    g_assert_nonnull(strstr(error->message, "conversion to raw failed"));
    g_assert_nonnull(strstr(error->message, "boom"));
    g_clear_error(&error);
    g_autofree gchar *calls = zvol_env_calls(&e);
    g_assert_true(g_str_has_suffix(calls, "udevadm settle --timeout=5\nzfs destroy -f zp/vms/b\n"));
    zvol_env_end(&e);
}

static void
test_zvol_disk_rollback_retries_busy(void)
{
    ZvolEnv e = {0};
    zvol_env_begin(&e);
    zvol_env_mock(&e, "qemu-img", QIMG_INFO_RAW "exit 1\n");
    g_autofree gchar *qlog = g_shell_quote(e.log);
    g_autofree gchar *body = g_strdup_printf(
        "[ \"$1\" = destroy ] && [ \"$(grep -c '^zfs destroy' %s)\" -le 2 ] && { echo 'cannot destroy: dataset is busy' >&2; exit 1; }\nexit 0\n",
        qlog);
    zvol_env_mock(&e, "zfs", body);
    GError *error = NULL;
    g_assert_false(purecvisor_vm_provision_zvol_disk("zp/vms/r", 1, e.base, NULL, &error));
    g_assert_null(strstr(error->message, "remove it manually"));
    g_clear_error(&error);
    g_autofree gchar *calls = zvol_env_calls(&e);
    g_auto(GStrv) lines = g_strsplit(calls, "\n", -1);
    guint destroys = 0;
    for (guint i = 0; lines[i]; i++)
        destroys += g_str_has_prefix(lines[i], "zfs destroy -f zp/vms/r") ? 1 : 0;
    g_assert_cmpuint(destroys, ==, 3);
    zvol_env_end(&e);
}

static void
test_zvol_disk_rollback_failure_is_reported(void)
{
    ZvolEnv e = {0};
    zvol_env_begin(&e);
    zvol_env_mock(&e, "qemu-img", QIMG_INFO_RAW "exit 1\n");
    zvol_env_mock(&e, "zfs", "[ \"$1\" = destroy ] && { echo 'cannot destroy: dataset is busy' >&2; exit 1; }\nexit 0\n");
    GError *error = NULL;
    g_assert_false(purecvisor_vm_provision_zvol_disk("zp/vms/s", 1, e.base, NULL, &error));
    g_assert_nonnull(strstr(error->message, "remove it manually"));
    g_clear_error(&error);
    zvol_env_end(&e);
}

static void
zvol_env_mock_second_info(const ZvolEnv *e, const gchar *second_json)
{
    g_autofree gchar *counter = g_build_filename(e->root, "info-count", NULL);
    g_autofree gchar *body = g_strdup_printf(
        "if [ \"$1\" = info ]; then\n"
        "  if [ -e '%s' ]; then echo '%s'; else : > '%s'; echo '{\"format\":\"raw\"}'; fi\n"
        "  exit 0\nfi\nexit 0\n", counter, second_json, counter);
    zvol_env_mock(e, "qemu-img", body);
}

static void
test_zvol_disk_recheck_after_lock_wait(void)
{
    const gchar *seconds[] = {"{\"format\":\"qcow2\"}", "{\"format\":\"qcow2\",\"backing-filename\":\"/x\"}", "not json"};
    for (guint i = 0; i < G_N_ELEMENTS(seconds); i++) {
        ZvolEnv e = {0};
        zvol_env_begin(&e);
        zvol_env_mock_second_info(&e, seconds[i]);
        GError *error = NULL;
        g_assert_false(purecvisor_vm_provision_zvol_disk("zp/vms/w", 1, e.base, NULL, &error));
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED);
        g_assert_nonnull(strstr(error->message, "changed while waiting"));
        g_clear_error(&error);
        g_autofree gchar *calls = zvol_env_calls(&e);
        g_assert_null(strstr(calls, "convert"));
        g_assert_true(g_str_has_suffix(calls, "zfs destroy -f zp/vms/w\n"));
        zvol_env_end(&e);
    }
}

static void
test_zvol_disk_pin_recheck_after_lock_wait(void)
{
    ZvolEnv e = {0};
    zvol_env_begin(&e);
    GError *error = NULL;
    PcvResNorm n = {0};
    g_assert_true(pcv_res_normalize(PCV_RES_IMAGE, e.base, &n, &error));
    g_assert_no_error(error);
    PcvResPin pin = { n.dev, n.ino, n.gen };
    pcv_res_norm_clear(&n);
    g_autofree gchar *qbase = g_shell_quote(e.base);
    g_autofree gchar *body = g_strdup_printf(
        "[ \"$1\" = create ] && { rm -f %s; printf swapped > %s; }\nexit 0\n", qbase, qbase);
    zvol_env_mock(&e, "zfs", body);
    g_assert_false(purecvisor_vm_provision_zvol_disk("zp/vms/p", 1, e.base, &pin, &error));
    g_assert_nonnull(strstr(error->message, "changed while waiting"));
    g_clear_error(&error);
    g_autofree gchar *calls = zvol_env_calls(&e);
    g_assert_null(strstr(calls, "convert"));
    g_assert_true(g_str_has_suffix(calls, "zfs destroy -f zp/vms/p\n"));
    zvol_env_end(&e);
}

static void
test_zvol_disk_refused_base_creates_nothing(void)
{
    ZvolEnv e = {0};
    zvol_env_begin(&e);
    zvol_env_mock(&e, "qemu-img", "[ \"$1\" = info ] && { echo '{\"format\":\"qcow2\",\"backing-filename\":\"/x\"}'; exit 0; }\nexit 0\n");
    GError *error = NULL;
    g_assert_false(purecvisor_vm_provision_zvol_disk("zp/vms/x", 1, e.base, NULL, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_assert_nonnull(strstr(error->message, "backing file"));
    g_clear_error(&error);
    zvol_env_mock(&e, "qemu-img", "echo boom >&2\nexit 1\n");
    g_assert_false(purecvisor_vm_provision_zvol_disk("zp/vms/x", 1, e.base, NULL, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_autofree gchar *calls = zvol_env_calls(&e);
    g_assert_null(strstr(calls, "zfs"));
    g_assert_null(strstr(calls, "convert"));
    zvol_env_end(&e);
}

static void
test_zvol_disk_without_base(void)
{
    ZvolEnv e = {0};
    zvol_env_begin(&e);
    GError *error = NULL;
    g_assert_true(purecvisor_vm_provision_zvol_disk("zp/vms/c", 1, NULL, NULL, &error));
    g_assert_no_error(error);
    g_autofree gchar *calls = zvol_env_calls(&e);
    g_assert_cmpstr(calls, ==, "zfs create -V 1G zp/vms/c\n");
    zvol_env_end(&e);
}

static void
test_zvol_disk_bad_base_rejected_before_create(void)
{
    ZvolEnv e = {0};
    zvol_env_begin(&e);
    GError *error = NULL;
    g_autofree gchar *gone = g_build_filename(e.root, "gone.raw", NULL);
    g_assert_false(purecvisor_vm_provision_zvol_disk("zp/vms/d", 1, gone, NULL, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error(&error);

    g_autofree gchar *keep = g_build_filename(e.root, "keep.raw", NULL);
    PcvResPin pin = { 0 };
    file_disk_swap_base(e.base, keep, 4096, &pin);
    g_assert_false(purecvisor_vm_provision_zvol_disk("zp/vms/d", 1, e.base, &pin, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error(&error);

    g_autofree gchar *calls = zvol_env_calls(&e);
    g_assert_cmpstr(calls, ==, "");
    zvol_env_end(&e);
}

static void
test_zvol_disk_create_failure(void)
{
    ZvolEnv e = {0};
    zvol_env_begin(&e);
    zvol_env_mock(&e, "zfs", "echo 'no such pool' >&2\nexit 1\n");
    GError *error = NULL;
    g_assert_false(purecvisor_vm_provision_zvol_disk("zp/vms/e", 1, e.base, NULL, &error));
    g_assert_nonnull(strstr(error->message, "ZFS Provisioning Failed"));
    g_assert_nonnull(strstr(error->message, "no such pool"));
    g_clear_error(&error);
    g_autofree gchar *calls = zvol_env_calls(&e);
    g_autofree gchar *want = g_strdup_printf(
        "qemu-img info --output=json --force-share %s\nzfs create -V 1G zp/vms/e\n", e.base);
    g_assert_cmpstr(calls, ==, want);
    zvol_env_end(&e);
}

static void test_new(void) {
    PureCVisorVmManager *m = purecvisor_vm_manager_new();
    g_assert_nonnull(m);
    g_assert_true(PURECVISOR_IS_VM_MANAGER(m));
    g_object_unref(m);
}

                                                      

static GMainLoop *g_loop = NULL;
static gboolean g_async_done = FALSE;
static GError *g_async_err = NULL;

                                                       

static gboolean g_async_ok = FALSE;

static void on_start_done(GObject *src, GAsyncResult *res, gpointer u) {
    (void)u;
    g_async_ok = purecvisor_vm_manager_start_vm_finish(
        PURECVISOR_VM_MANAGER(src), res, &g_async_err);
    g_async_done = TRUE;
    g_main_loop_quit(g_loop);
}

static gboolean nonexistent_pool_begin(void) {
    g_setenv("PCV_LIBVIRT_URI", "test:///default", TRUE);
    virt_conn_pool_init(1);
    virConnectPtr probe = virt_conn_pool_acquire();
    if (!probe) {
        virt_conn_pool_shutdown();
        g_unsetenv("PCV_LIBVIRT_URI");
        g_test_skip("test:///default 연결 불가");
        return FALSE;
    }
    virt_conn_pool_release(probe);
    return TRUE;
}
static void nonexistent_pool_end(void) {
    virt_conn_pool_shutdown();
    g_unsetenv("PCV_LIBVIRT_URI");
}

static void test_start_vm_nonexistent(void) {
    if (!nonexistent_pool_begin()) return;
    PureCVisorVmManager *m = purecvisor_vm_manager_new();
    g_loop = g_main_loop_new(NULL, FALSE);
    g_async_done = FALSE;
    g_async_err = NULL;
    g_async_ok = TRUE;

    purecvisor_vm_manager_start_vm_async(m, "nonexistent-pcv-vm", on_start_done, NULL);
    g_main_loop_run(g_loop);

    g_assert_true(g_async_done);
    g_assert_false(g_async_ok);                       
    g_assert_error(g_async_err, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error(&g_async_err);

    g_object_unref(m);
    g_main_loop_unref(g_loop);
    g_loop = NULL;
    nonexistent_pool_end();
}

static void on_stop_done(GObject *src, GAsyncResult *res, gpointer u) {
    (void)u;
    g_async_ok = purecvisor_vm_manager_stop_vm_finish(
        PURECVISOR_VM_MANAGER(src), res, &g_async_err);
    g_async_done = TRUE;
    g_main_loop_quit(g_loop);
}

static void test_stop_vm_nonexistent(void) {
    if (!nonexistent_pool_begin()) return;
    PureCVisorVmManager *m = purecvisor_vm_manager_new();
    g_loop = g_main_loop_new(NULL, FALSE);
    g_async_done = FALSE;
    g_async_err = NULL;
    g_async_ok = TRUE;

    purecvisor_vm_manager_stop_vm_async(m, "nonexistent-pcv-vm", on_stop_done, NULL);
    g_main_loop_run(g_loop);

    g_assert_true(g_async_done);
    g_assert_false(g_async_ok);
    g_assert_error(g_async_err, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error(&g_async_err);

    g_object_unref(m);
    g_main_loop_unref(g_loop);
    g_loop = NULL;
    nonexistent_pool_end();
}

                                                         

static void on_set_vcpu_done(GObject *src, GAsyncResult *res, gpointer u) {
    (void)u;
    g_async_ok = purecvisor_vm_manager_set_vcpu_finish(
        PURECVISOR_VM_MANAGER(src), res, &g_async_err);
    g_async_done = TRUE;
    g_main_loop_quit(g_loop);
}

static void test_set_vcpu_nonexistent(void) {
    if (!nonexistent_pool_begin()) return;
    PureCVisorVmManager *m = purecvisor_vm_manager_new();
    g_loop = g_main_loop_new(NULL, FALSE);
    g_async_done = FALSE;
    g_async_err = NULL;

    purecvisor_vm_manager_set_vcpu_async(m, "nonexistent-vm", 4, NULL, on_set_vcpu_done, NULL);
    g_main_loop_run(g_loop);

    g_assert_true(g_async_done);
    g_assert_error(g_async_err, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error(&g_async_err);

    g_object_unref(m);
    g_main_loop_unref(g_loop);
    g_loop = NULL;
    nonexistent_pool_end();
}

static void on_set_mem_done(GObject *src, GAsyncResult *res, gpointer u) {
    (void)u;
    g_async_ok = purecvisor_vm_manager_set_memory_finish(
        PURECVISOR_VM_MANAGER(src), res, &g_async_err);
    g_async_done = TRUE;
    g_main_loop_quit(g_loop);
}

static void test_set_memory_nonexistent(void) {
    if (!nonexistent_pool_begin()) return;
    PureCVisorVmManager *m = purecvisor_vm_manager_new();
    g_loop = g_main_loop_new(NULL, FALSE);
    g_async_done = FALSE;
    g_async_err = NULL;

    purecvisor_vm_manager_set_memory_async(m, "nonexistent-vm", 2048, NULL, on_set_mem_done, NULL);
    g_main_loop_run(g_loop);

    g_assert_true(g_async_done);
    g_assert_error(g_async_err, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error(&g_async_err);

    g_object_unref(m);
    g_main_loop_unref(g_loop);
    g_loop = NULL;
    nonexistent_pool_end();
}

                                                             
static void test_resolve_bridge_null_defaults(void) {
    gchar *r = purecvisor_vm_resolve_network_bridge(NULL);
    g_assert_cmpstr(r, ==, "pcvnat0");                            
    g_free(r);
}
static void test_resolve_bridge_empty_defaults(void) {
    gchar *r = purecvisor_vm_resolve_network_bridge("");
    g_assert_cmpstr(r, ==, "pcvnat0");
    g_free(r);
}
static void test_resolve_bridge_none_is_null(void) {
    g_assert_null(purecvisor_vm_resolve_network_bridge("none"));
}
static void test_resolve_bridge_explicit_passthrough(void) {
    gchar *r = purecvisor_vm_resolve_network_bridge("br-custom");
    g_assert_cmpstr(r, ==, "br-custom");
    g_free(r);
}

                                                                       
                                                                             
                                                                    

static void test_overlay_ethernet_iface_shape(void) {
    gchar *xml = _overlay_ethernet_iface_xml();
    g_assert_nonnull(xml);
    g_assert_nonnull(strstr(xml, "type='ethernet'"));
    g_assert_nonnull(strstr(xml, "<model type='virtio'/>"));
    g_assert_null(strstr(xml, "type='bridge'"));
    g_assert_null(strstr(xml, "<source"));
    g_assert_null(strstr(xml, "virtualport"));
    g_free(xml);
}

                                                              
                                                                     
                                                           

static void test_overlay_metadata_build_parse_roundtrip(void) {
    gchar *xml = _overlay_metadata_xml("tenant-overlay", "acme");
    g_assert_nonnull(xml);
    g_assert_nonnull(strstr(xml, "pcv:overlay"));

    gchar *mode = NULL, *tenant = NULL;
    gboolean ok = _overlay_metadata_parse(xml, &mode, &tenant);
    g_assert_true(ok);
    g_assert_cmpstr(mode, ==, "tenant-overlay");
    g_assert_cmpstr(tenant, ==, "acme");
    g_free(mode);
    g_free(tenant);
    g_free(xml);
}

static void test_overlay_metadata_build_non_overlay_is_empty(void) {
    gchar *xml1 = _overlay_metadata_xml("bridge", "acme");
    g_assert_cmpstr(xml1, ==, "");
    g_free(xml1);

    gchar *xml2 = _overlay_metadata_xml(NULL, NULL);
    g_assert_cmpstr(xml2, ==, "");
    g_free(xml2);

    gchar *xml3 = _overlay_metadata_xml("", "acme");
    g_assert_cmpstr(xml3, ==, "");
    g_free(xml3);
}

static void test_overlay_metadata_build_escapes_values(void) {
                                                                
                                                   
    gchar *xml = _overlay_metadata_xml("tenant-overlay", "a&b<c");
    g_assert_nonnull(xml);
    g_assert_null(strstr(xml, "tenant='a&b<c'"));
    g_assert_nonnull(strstr(xml, "&amp;"));
    g_assert_nonnull(strstr(xml, "&lt;"));
    g_free(xml);
}

static void test_overlay_metadata_parse_absent_returns_false(void) {
    gchar *mode = NULL, *tenant = NULL;
    gboolean ok = _overlay_metadata_parse(
        "<metadata><pcv:owner xmlns:pcv='urn:purecvisor:metadata'>x</pcv:owner></metadata>",
        &mode, &tenant);
    g_assert_false(ok);
    g_assert_null(mode);
    g_assert_null(tenant);
}

static void test_overlay_metadata_parse_malformed_returns_false(void) {
    gchar *mode = NULL, *tenant = NULL;
                                  
    gboolean ok = _overlay_metadata_parse(
        "<pcv:overlay xmlns:pcv='urn:purecvisor:overlay:1' network_mode='tenant-overlay'/>",
        &mode, &tenant);
    g_assert_false(ok);
    g_free(mode);
    g_free(tenant);
}

static void test_overlay_metadata_parse_null_safe(void) {
    gchar *mode = NULL, *tenant = NULL;
    g_assert_false(_overlay_metadata_parse(NULL, &mode, &tenant));
    g_assert_false(_overlay_metadata_parse("<pcv:overlay/>", NULL, &tenant));
}

                                                                            
                                                                             
                                                                                
                                                    
                                                                           
                                                      
                                                           
static void test_overlay_metadata_parse_libvirt_stripped_form(void) {
    gchar *mode = NULL, *tenant = NULL;
    gboolean ok = _overlay_metadata_parse(
        "<overlay network_mode=\"tenant-overlay\" tenant=\"acme\"/>",
        &mode, &tenant);
    g_assert_true(ok);
    g_assert_cmpstr(mode, ==, "tenant-overlay");
    g_assert_cmpstr(tenant, ==, "acme");
    g_free(mode);
    g_free(tenant);
}

                                                                
static void test_overlay_metadata_parse_libvirt_attr_reordered(void) {
    gchar *mode = NULL, *tenant = NULL;
    gboolean ok = _overlay_metadata_parse(
        "<overlay tenant=\"beta\" network_mode=\"tenant-overlay\"/>",
        &mode, &tenant);
    g_assert_true(ok);
    g_assert_cmpstr(mode, ==, "tenant-overlay");
    g_assert_cmpstr(tenant, ==, "beta");
    g_free(mode);
    g_free(tenant);
}

static void test_dpdk_metadata_build_parse_roundtrip(void) {
    gchar *xml = _dpdk_metadata_xml("dpdk", "dpdk-br0");
    g_assert_nonnull(xml);
    g_assert_nonnull(strstr(xml, "pcv:dpdk"));
    gchar *bridge = NULL;
    g_assert_true(_dpdk_metadata_parse(xml, &bridge));
    g_assert_cmpstr(bridge, ==, "dpdk-br0");
    g_free(bridge);
    g_free(xml);
}

static void test_sriov_nic_check_table(void) {
    struct { const gchar *nic, *br, *mode; const gchar *msg; } bad[] = {
        {"sriov", NULL, NULL, PCV_VM_SRIOV_NO_BRIDGE_MSG},
        {"sriov", "", "bridge", PCV_VM_SRIOV_NO_BRIDGE_MSG},
        {"sriov", "pcvnat0", "tenant-overlay", PCV_VM_SRIOV_OVERLAY_MSG},
        {"sriov", NULL, "tenant-overlay", PCV_VM_SRIOV_NO_BRIDGE_MSG},
    };
    for (guint i = 0; i < G_N_ELEMENTS(bad); i++) {
        GError *err = NULL;
        g_assert_false(purecvisor_vm_sriov_nic_check(bad[i].nic, bad[i].br, bad[i].mode, &err));
        g_assert_nonnull(err);
        g_assert_true(g_error_matches(err, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT));
        g_assert_cmpstr(err->message, ==, bad[i].msg);
        g_clear_error(&err);
    }

    struct { const gchar *nic, *br, *mode; } ok[] = {
        {"sriov", "pcvnat0", NULL},
        {NULL, NULL, NULL},
        {"bridge", NULL, "bridge"},
        {NULL, "pcvnat0", "tenant-overlay"},
    };
    for (guint i = 0; i < G_N_ELEMENTS(ok); i++) {
        GError *err = NULL;
        g_assert_true(purecvisor_vm_sriov_nic_check(ok[i].nic, ok[i].br, ok[i].mode, &err));
        g_assert_no_error(err);
    }
}

static void test_sriov_pci_parse_table(void) {
    guint out[4] = {9, 9, 9, 9};
    g_assert_true(purecvisor_vm_sriov_pci_parse("0000:3b:10.1", out));
    g_assert_cmpuint(out[0], ==, 0x0000);
    g_assert_cmpuint(out[1], ==, 0x3b);
    g_assert_cmpuint(out[2], ==, 0x10);
    g_assert_cmpuint(out[3], ==, 0x1);
    g_assert_true(purecvisor_vm_sriov_pci_parse("0000:01:00.0", out));
    g_assert_cmpuint(out[0], ==, 0);
    g_assert_cmpuint(out[1], ==, 1);
    g_assert_cmpuint(out[2], ==, 0);
    g_assert_cmpuint(out[3], ==, 0);

    const gchar *bad[] = {NULL, "", "garbage", "01:00.0", "0000:3B:00.1",
                          "0000:00:20.0", "0000:3b:10.8", "0000:3b:10.1 "};
    for (guint i = 0; i < G_N_ELEMENTS(bad); i++) {
        guint o[4] = {9, 9, 9, 9};
        g_assert_false(purecvisor_vm_sriov_pci_parse(bad[i], o));
        for (guint k = 0; k < 4; k++)
            g_assert_cmpuint(o[k], ==, 0);
    }
}

static void test_dpdk_metadata_non_dpdk_is_empty(void) {
    gchar *xml = _dpdk_metadata_xml("bridge", "dpdk-br0");
    g_assert_cmpstr(xml, ==, "");
    g_free(xml);
}

static void test_dpdk_metadata_parse_libvirt_stripped_form(void) {
    gchar *bridge = NULL;
    g_assert_true(_dpdk_metadata_parse("<dpdk bridge=\"dpdk-br0\"/>", &bridge));
    g_assert_cmpstr(bridge, ==, "dpdk-br0");
    g_free(bridge);
}

static void test_dpdk_metadata_parse_malformed_fails(void) {
    gchar *bridge = NULL;
    g_assert_false(_dpdk_metadata_parse("<dpdk/>", &bridge));
    g_assert_null(bridge);
    g_assert_false(_dpdk_metadata_parse(NULL, &bridge));
}

static gchar *
dpdk_vhost_domain_xml(const gchar *vm_name, const gchar *interfaces)
{
    return g_strdup_printf(
        "<domain type='kvm'><name>%s</name>"
        "<metadata><pcv:dpdk xmlns:pcv='%s' bridge='dpdk-br0'/></metadata>"
        "<devices><disk type='file'/>%s</devices></domain>",
        vm_name, PCV_DPDK_METADATA_URI, interfaces);
}

static void
test_dpdk_vhost_source_classifies_canonical_and_legacy(void)
{
    g_autofree gchar *canonical = dpdk_vhost_domain_xml(
        "vm1", "<interface type='vhostuser'><source mode='server' type='unix' "
               "path='/run/libvirt/qemu/purecvisor-vhost-vm1.sock'/></interface>");
    g_autofree gchar *legacy = dpdk_vhost_domain_xml(
        "vm1", "<interface type='vhostuser'><source type='unix' "
               "path='/var/run/purecvisor/vhost-vm1.sock' mode='server'/></interface>");
    g_autofree gchar *none = dpdk_vhost_domain_xml(
        "vm1", "<interface type='bridge'><source bridge='pcvbr0'/></interface>");

    g_assert_cmpint(pcv_vm_dpdk_vhost_source_classify(canonical, "vm1"), ==,
                    PCV_DPDK_VHOST_SOURCE_CANONICAL);
    g_assert_cmpint(pcv_vm_dpdk_vhost_source_classify(legacy, "vm1"), ==,
                    PCV_DPDK_VHOST_SOURCE_LEGACY);
    g_assert_cmpint(pcv_vm_dpdk_vhost_source_classify(none, "vm1"), ==,
                    PCV_DPDK_VHOST_SOURCE_NONE);
}

static void
test_dpdk_vhost_source_rejects_foreign_or_ambiguous(void)
{
    static const gchar *invalid_interfaces[] = {
        "<interface type='vhostuser'><source type='unix' "
        "path='/run/libvirt/qemu/purecvisor-vhost-vm1.sock' mode='client'/></interface>",
        "<interface type='vhostuser'><source type='unix' "
        "path='/tmp/vhost-vm1.sock' mode='server'/></interface>",
        "<interface type='vhostuser'><source type='unix' "
        "path='/run/libvirt/qemu/purecvisor-vhost-other.sock' mode='server'/></interface>",
        "<interface type='vhostuser'><source type='unix' "
        "path='/run/libvirt/qemu/purecvisor-vhost-vm1.sock' mode='server'/>"
        "<source type='unix' path='/var/run/purecvisor/vhost-vm1.sock' "
        "mode='server'/></interface>",
        "<interface type='vhostuser'><source type='unix' "
        "path='/run/libvirt/qemu/purecvisor-vhost-vm1.sock' mode='server'/></interface>"
        "<interface type='vhostuser'><source type='unix' "
        "path='/var/run/purecvisor/vhost-vm1.sock' mode='server'/></interface>",
        NULL
    };

    for (guint i = 0; invalid_interfaces[i]; i++) {
        g_autofree gchar *xml = dpdk_vhost_domain_xml("vm1", invalid_interfaces[i]);
        g_assert_cmpint(pcv_vm_dpdk_vhost_source_classify(xml, "vm1"), ==,
                        PCV_DPDK_VHOST_SOURCE_INVALID);
    }
    g_assert_cmpint(pcv_vm_dpdk_vhost_source_classify("<domain>", "vm1"), ==,
                    PCV_DPDK_VHOST_SOURCE_INVALID);
    g_assert_cmpint(pcv_vm_dpdk_vhost_source_classify(NULL, "vm1"), ==,
                    PCV_DPDK_VHOST_SOURCE_INVALID);
}

static void
test_dpdk_vhost_legacy_migration_is_exact_and_idempotent(void)
{
    g_autofree gchar *legacy = dpdk_vhost_domain_xml(
        "vm1", "<interface type='vhostuser'><source type='unix' "
               "path='/var/run/purecvisor/vhost-vm1.sock' mode='server'/></interface>");
    GError *error = NULL;
    g_autofree gchar *canonical = pcv_vm_dpdk_vhost_migrate_legacy_xml(
        legacy, "vm1", &error);
    g_assert_no_error(error);
    g_assert_nonnull(canonical);
    g_assert_cmpint(pcv_vm_dpdk_vhost_source_classify(canonical, "vm1"), ==,
                    PCV_DPDK_VHOST_SOURCE_CANONICAL);
    g_assert_nonnull(strstr(canonical, PCV_DPDK_METADATA_URI));
    g_assert_null(strstr(canonical, "/var/run/purecvisor/vhost-vm1.sock"));

    g_autofree gchar *again = pcv_vm_dpdk_vhost_migrate_legacy_xml(
        canonical, "vm1", &error);
    g_assert_no_error(error);
    g_assert_cmpstr(again, ==, canonical);

    g_autofree gchar *unknown = dpdk_vhost_domain_xml(
        "vm1", "<interface type='vhostuser'><source type='unix' "
               "path='/tmp/operator.sock' mode='server'/></interface>");
    g_assert_null(pcv_vm_dpdk_vhost_migrate_legacy_xml(unknown, "vm1", &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
}

static void
test_dpdk_vhost_start_action_matrix(void)
{
    g_assert_cmpint(pcv_vm_dpdk_vhost_start_action(
        FALSE, PCV_DPDK_VHOST_SOURCE_CANONICAL), ==,
        PCV_DPDK_VHOST_START_CANONICAL);
    g_assert_cmpint(pcv_vm_dpdk_vhost_start_action(
        FALSE, PCV_DPDK_VHOST_SOURCE_LEGACY), ==,
        PCV_DPDK_VHOST_START_MIGRATE_LEGACY);
    g_assert_cmpint(pcv_vm_dpdk_vhost_start_action(
        TRUE, PCV_DPDK_VHOST_SOURCE_LEGACY), ==,
        PCV_DPDK_VHOST_START_DEFER_LEGACY);
    g_assert_cmpint(pcv_vm_dpdk_vhost_start_action(
        TRUE, PCV_DPDK_VHOST_SOURCE_CANONICAL), ==,
        PCV_DPDK_VHOST_START_CANONICAL);
    g_assert_cmpint(pcv_vm_dpdk_vhost_start_action(
        FALSE, PCV_DPDK_VHOST_SOURCE_NONE), ==,
        PCV_DPDK_VHOST_START_INVALID);
    g_assert_cmpint(pcv_vm_dpdk_vhost_start_action(
        TRUE, PCV_DPDK_VHOST_SOURCE_INVALID), ==,
        PCV_DPDK_VHOST_START_INVALID);
}




static virDomainPtr define_dpdk_metadata_test_domain(virConnectPtr conn,
                                                     const gchar *name,
                                                     const gchar *metadata)
{
    gchar *xml = g_strdup_printf(
        "<domain type='test'>"
        "<name>%s</name><memory unit='KiB'>65536</memory>"
        "<os><type>hvm</type></os><metadata>%s</metadata>"
        "</domain>", name, metadata);
    virDomainPtr dom = virDomainDefineXML(conn, xml);
    g_free(xml);
    return dom;
}

static void test_dpdk_metadata_read_ok_libvirt(void) {
    virConnectPtr conn = virConnectOpen("test:///default");
    if (!conn) { g_test_skip("libvirt test:/// 드라이버 사용 불가"); return; }
    virDomainPtr dom = define_dpdk_metadata_test_domain(conn, "pcv-dpdk-meta-ok",
        "<pcv:dpdk xmlns:pcv='" PCV_DPDK_METADATA_URI "' bridge='dpdk-br0'/>");
    if (!dom) {
        g_test_skip("test:///default DPDK metadata domain define 실패");
        virConnectClose(conn);
        return;
    }
    gchar *bridge = NULL;
    g_assert_cmpint(pcv_vm_dpdk_metadata_read(dom, &bridge), ==, PCV_DPDK_META_OK);
    g_assert_cmpstr(bridge, ==, "dpdk-br0");
    g_free(bridge);
    virDomainUndefine(dom); virDomainFree(dom); virConnectClose(conn);
}

static void test_dpdk_metadata_read_absent_libvirt(void) {
    virConnectPtr conn = virConnectOpen("test:///default");
    if (!conn) { g_test_skip("libvirt test:/// 드라이버 사용 불가"); return; }
    virDomainPtr dom = virDomainLookupByName(conn, "test");
    if (!dom) {
        g_test_skip("test:///default 기본 도메인 조회 실패");
        virConnectClose(conn);
        return;
    }
    gchar *bridge = NULL;
    g_assert_cmpint(pcv_vm_dpdk_metadata_read(dom, &bridge), ==, PCV_DPDK_META_ABSENT);
    g_assert_null(bridge);
    virDomainFree(dom); virConnectClose(conn);
}

static void test_dpdk_metadata_read_invalid_libvirt(void) {
    virConnectPtr conn = virConnectOpen("test:///default");
    if (!conn) { g_test_skip("libvirt test:/// 드라이버 사용 불가"); return; }
    virDomainPtr dom = define_dpdk_metadata_test_domain(conn, "pcv-dpdk-meta-invalid",
        "<pcv:dpdk xmlns:pcv='" PCV_DPDK_METADATA_URI "'/>");
    if (!dom) {
        g_test_skip("test:///default invalid DPDK metadata domain define 실패");
        virConnectClose(conn);
        return;
    }
    gchar *bridge = NULL;
    g_assert_cmpint(pcv_vm_dpdk_metadata_read(dom, &bridge), ==, PCV_DPDK_META_INVALID);
    g_assert_null(bridge);
    virDomainUndefine(dom); virDomainFree(dom); virConnectClose(conn);
}

static void test_dpdk_metadata_read_legacy_vhost_is_invalid_libvirt(void) {
    virConnectPtr conn = virConnectOpen("test:///default");
    if (!conn) { g_test_skip("libvirt test:/// 드라이버 사용 불가"); return; }
    static const gchar *xml =
        "<domain type='test'><name>pcv-dpdk-meta-legacy</name>"
        "<memory unit='KiB'>65536</memory><os><type>hvm</type></os>"
        "<devices><interface type='vhostuser'><source type='unix' "
        "path='/var/run/purecvisor/vhost-pcv-dpdk-meta-legacy.sock' mode='server'/>"
        "<model type='virtio'/></interface></devices></domain>";
    virDomainPtr dom = virDomainDefineXML(conn, xml);
    if (!dom) {
        g_test_skip("test:///default legacy DPDK domain define 실패");
        virConnectClose(conn);
        return;
    }
    gchar *bridge = NULL;
    g_assert_cmpint(pcv_vm_dpdk_metadata_read(dom, &bridge), ==, PCV_DPDK_META_INVALID);
    g_assert_null(bridge);
    virDomainUndefine(dom); virDomainFree(dom); virConnectClose(conn);
}

static void
test_dpdk_vhost_inactive_migration_libvirt_reread(void)
{
    virConnectPtr conn = virConnectOpen("test:///default");
    if (!conn) {
        g_test_skip("libvirt test:/// 드라이버 사용 불가");
        return;
    }
    static const gchar *legacy_xml =
        "<domain type='test'><name>pcv-dpdk-migrate</name>"
        "<memory unit='KiB'>65536</memory><os><type>hvm</type></os>"
        "<metadata><pcv:dpdk xmlns:pcv='" PCV_DPDK_METADATA_URI
        "' bridge='dpdk-br0'/></metadata><devices>"
        "<interface type='vhostuser'><source type='unix' "
        "path='/var/run/purecvisor/vhost-pcv-dpdk-migrate.sock' mode='server'/>"
        "<model type='virtio'/></interface></devices></domain>";
    virDomainPtr dom = virDomainDefineXML(conn, legacy_xml);
    if (!dom) {
        g_test_skip("test:///default vhostuser domain define 실패");
        virConnectClose(conn);
        return;
    }
    g_assert_cmpint(virDomainIsActive(dom), ==, 0);

    char *stored = virDomainGetXMLDesc(dom, VIR_DOMAIN_XML_INACTIVE);
    g_assert_nonnull(stored);
    g_assert_cmpint(pcv_vm_dpdk_vhost_source_classify(
        stored, "pcv-dpdk-migrate"), ==, PCV_DPDK_VHOST_SOURCE_LEGACY);
    GError *error = NULL;
    g_autofree gchar *migrated = pcv_vm_dpdk_vhost_migrate_legacy_xml(
        stored, "pcv-dpdk-migrate", &error);
    free(stored);
    g_assert_no_error(error);
    g_assert_nonnull(migrated);

    virDomainPtr canonical_dom = virDomainDefineXML(conn, migrated);
    g_assert_nonnull(canonical_dom);
    virDomainFree(dom);
    dom = canonical_dom;
    char *post = virDomainGetXMLDesc(dom, VIR_DOMAIN_XML_INACTIVE);
    g_assert_nonnull(post);
    g_assert_cmpint(pcv_vm_dpdk_vhost_source_classify(
        post, "pcv-dpdk-migrate"), ==, PCV_DPDK_VHOST_SOURCE_CANONICAL);
    free(post);
    gchar *bridge = NULL;
    g_assert_cmpint(pcv_vm_dpdk_metadata_read(dom, &bridge), ==,
                    PCV_DPDK_META_OK);
    g_assert_cmpstr(bridge, ==, "dpdk-br0");
    g_free(bridge);

    virDomainUndefine(dom);
    virDomainFree(dom);
    virConnectClose(conn);
}

                                                                
                                                                        
                                                               
                                           

                                                             
                                                                          
#define OVL_LIVE_XML_TWO_IFACES \
    "<domain type='kvm'><devices>\n" \
    "  <interface type='bridge'>\n" \
    "    <mac address='52:54:00:aa:bb:cc'/>\n" \
    "    <source bridge='pcvnat0'/>\n" \
    "    <target dev='vnet3'/>\n" \
    "    <model type='virtio'/>\n" \
    "  </interface>\n" \
    "  <interface type='ethernet'>\n" \
    "    <mac address='52:54:00:12:34:56'/>\n" \
    "    <target dev='vnet7'/>\n" \
    "    <model type='virtio'/>\n" \
    "  </interface>\n" \
    "</devices></domain>"

static void test_overlay_live_iface_parse_picks_ethernet(void) {
    gchar *tap = NULL, *mac = NULL;
    gboolean ok = _overlay_live_iface_parse(OVL_LIVE_XML_TWO_IFACES, &tap, &mac);
    g_assert_true(ok);
    g_assert_cmpstr(tap, ==, "vnet7");
    g_assert_cmpstr(mac, ==, "52:54:00:12:34:56");
    g_free(tap);
    g_free(mac);
}

static void test_overlay_live_iface_parse_attr_order_independent(void) {
                                                       
    const gchar *xml =
        "<devices><interface managed='no' type='ethernet'>\n"
        "  <target dev='vnet11'/>\n"
        "  <mac address='52:54:00:de:ad:be'/>\n"
        "</interface></devices>";
    gchar *tap = NULL, *mac = NULL;
    gboolean ok = _overlay_live_iface_parse(xml, &tap, &mac);
    g_assert_true(ok);
    g_assert_cmpstr(tap, ==, "vnet11");
    g_assert_cmpstr(mac, ==, "52:54:00:de:ad:be");
    g_free(tap);
    g_free(mac);
}

static void test_overlay_live_iface_parse_absent_returns_false(void) {
                                                            
    const gchar *xml =
        "<devices><interface type='bridge'>\n"
        "  <mac address='52:54:00:aa:bb:cc'/>\n"
        "  <target dev='vnet3'/>\n"
        "</interface></devices>";
    gchar *tap = NULL, *mac = NULL;
    gboolean ok = _overlay_live_iface_parse(xml, &tap, &mac);
    g_assert_false(ok);
    g_assert_null(tap);
    g_assert_null(mac);
}

static void test_overlay_live_iface_parse_missing_target_returns_false(void) {
                                                               
    const gchar *xml =
        "<devices><interface type='ethernet'>\n"
        "  <mac address='52:54:00:12:34:56'/>\n"
        "  <model type='virtio'/>\n"
        "</interface></devices>";
    gchar *tap = NULL, *mac = NULL;
    gboolean ok = _overlay_live_iface_parse(xml, &tap, &mac);
    g_assert_false(ok);
    g_assert_null(tap);
    g_assert_null(mac);
}

static void test_overlay_live_iface_parse_null_safe(void) {
    gchar *tap = NULL, *mac = NULL;
    g_assert_false(_overlay_live_iface_parse(NULL, &tap, &mac));
    g_assert_false(_overlay_live_iface_parse("<devices/>", NULL, &mac));
    g_assert_false(_overlay_live_iface_parse("<devices/>", &tap, NULL));
}

                                                                 
                                                             

static void test_overlay_gw_cidr_basic(void) {
    gchar *gw = _overlay_gw_cidr_from_subnet("10.100.5.0/24");
    g_assert_cmpstr(gw, ==, "10.100.5.1/24");
    g_free(gw);
}

static void test_overlay_gw_cidr_high_index(void) {
    gchar *gw = _overlay_gw_cidr_from_subnet("10.100.200.0/24");
    g_assert_cmpstr(gw, ==, "10.100.200.1/24");
    g_free(gw);
}

static void test_overlay_gw_cidr_malformed_returns_null(void) {
    g_assert_null(_overlay_gw_cidr_from_subnet(NULL));
    g_assert_null(_overlay_gw_cidr_from_subnet("10.100.5.0"));                     
    g_assert_null(_overlay_gw_cidr_from_subnet("10.100.0/24"));                
    g_assert_null(_overlay_gw_cidr_from_subnet("garbage"));
}








static const gchar *RESIZE_FIXTURE_XML =
    "<domain type='kvm'><name>web01</name><devices>"
    "<interface type='direct'><source dev='enp1s0' mode='bridge'/>"
    "<target dev='macvtap0'/></interface>"
    "<disk type='file' device='cdrom'><driver name='qemu' type='raw'/>"
    "<source file='/iso/ubuntu.iso'/><target dev='sda' bus='sata'/></disk>"
    "<disk type='block' device='disk'><driver name='qemu' type='raw'/>"
    "<source dev='/dev/zvol/pcvpool/vms/web01'/><target dev='vda' bus='virtio'/></disk>"
    "<disk type='file' device='disk'><driver name='qemu' type='qcow2'/>"
    "<source file='/var/lib/libvirt/images/web01-data.qcow2'/>"
    "<target dev='vdb' bus='virtio'/></disk>"
    "<disk type='block' device='disk'><driver name='qemu' type='raw'/>"
    "<source dev='/dev/disk/by-path/ip-10.0.0.5:3260-iscsi-iqn.x-lun-1'/>"
    "<target dev='vdc' bus='virtio'/></disk>"
    "<disk type='network' device='disk'><source protocol='rbd' name='pool/img'/>"
    "<target dev='vdd' bus='virtio'/></disk>"
    "</devices></domain>";

static void test_resize_select_picks_requested_file_disk(void) {
    gchar *source = NULL;
    PcvVmDiskSourceKind kind = PCV_VM_DISK_SOURCE_BLOCK;
    GError *error = NULL;
    g_assert_true(pcv_vm_disk_source_for_target(RESIZE_FIXTURE_XML, "vdb",
                                                &source, &kind, &error));
    g_assert_no_error(error);
    g_assert_cmpstr(source, ==, "/var/lib/libvirt/images/web01-data.qcow2");
    g_assert_cmpint(kind, ==, PCV_VM_DISK_SOURCE_FILE);
    g_free(source);
}

static void test_resize_select_picks_requested_zvol_disk(void) {
    gchar *source = NULL;
    PcvVmDiskSourceKind kind = PCV_VM_DISK_SOURCE_FILE;
    GError *error = NULL;
    g_assert_true(pcv_vm_disk_source_for_target(RESIZE_FIXTURE_XML, "vda",
                                                &source, &kind, &error));
    g_assert_no_error(error);

    g_assert_cmpstr(source, ==, "/dev/zvol/pcvpool/vms/web01");
    g_assert_cmpint(kind, ==, PCV_VM_DISK_SOURCE_ZVOL);
    g_free(source);
}

static void test_resize_select_classifies_plain_block_device(void) {
    gchar *source = NULL;
    PcvVmDiskSourceKind kind = PCV_VM_DISK_SOURCE_ZVOL;
    GError *error = NULL;
    g_assert_true(pcv_vm_disk_source_for_target(RESIZE_FIXTURE_XML, "vdc",
                                                &source, &kind, &error));
    g_assert_no_error(error);
    g_assert_cmpint(kind, ==, PCV_VM_DISK_SOURCE_BLOCK);
    g_free(source);
}

static void test_resize_select_rejects_missing_cdrom_and_network(void) {
    const gchar *bad_targets[] = {"vdz", "sda", "vdd", NULL};
    for (gsize i = 0; bad_targets[i]; i++) {
        gchar *source = (gchar *)"sentinel";
        PcvVmDiskSourceKind kind = PCV_VM_DISK_SOURCE_FILE;
        GError *error = NULL;
        g_assert_false(pcv_vm_disk_source_for_target(RESIZE_FIXTURE_XML, bad_targets[i],
                                                     &source, &kind, &error));
        g_assert_nonnull(error);
        g_assert_null(source);
        g_clear_error(&error);
    }
}

static void test_resize_select_rejects_malformed_inputs(void) {
    gchar *source = NULL;
    PcvVmDiskSourceKind kind = PCV_VM_DISK_SOURCE_FILE;
    GError *error = NULL;
    g_assert_false(pcv_vm_disk_source_for_target(NULL, "vda", &source, &kind, &error));
    g_clear_error(&error);
    g_assert_false(pcv_vm_disk_source_for_target("<domain>", "vda", &source, &kind, &error));
    g_clear_error(&error);
    g_assert_false(pcv_vm_disk_source_for_target(RESIZE_FIXTURE_XML, "", &source, &kind, &error));
    g_clear_error(&error);
    g_assert_false(pcv_vm_disk_source_for_target(RESIZE_FIXTURE_XML, NULL, &source, &kind, &error));
    g_clear_error(&error);

    g_assert_false(pcv_vm_disk_source_for_target(
        "<domain><devices><disk type='block' device='disk'><source dev='/dev/zvol/'/>"
        "<target dev='vda'/></disk></devices></domain>", "vda", &source, &kind, &error));
    g_assert_null(source);
    g_clear_error(&error);
}

static void test_resize_check_rejects_shrink(void) {
    const guint64 gib = 1024ULL * 1024ULL * 1024ULL;
    GError *error = NULL;
    g_assert_true(pcv_vm_disk_resize_check(10 * gib, 20, &error));
    g_assert_no_error(error);
    g_assert_true(pcv_vm_disk_resize_check(10 * gib, 10, &error));
    g_assert_no_error(error);
    g_assert_false(pcv_vm_disk_resize_check(10 * gib, 9, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_assert_false(pcv_vm_disk_resize_check(10 * gib + gib / 2, 10, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_assert_false(pcv_vm_disk_resize_check(10 * gib, 0, &error));
    g_clear_error(&error);
    g_assert_false(pcv_vm_disk_resize_check(10 * gib, -5, &error));
    g_clear_error(&error);
}

void test_vm_manager_register(void) {
    g_test_add_func("/vm_manager/file_disk/base_image_populates_qcow2_and_raw",
                    test_file_disk_base_image_populates_qcow2_and_raw);
    g_test_add_func("/vm_manager/file_disk/without_base_creates_requested_shape",
                    test_file_disk_without_base_creates_requested_shape);
    g_test_add_func("/vm_manager/file_disk/missing_base_removes_target",
                    test_file_disk_missing_base_removes_target);
    g_test_add_func("/vm_manager/file_disk/backing_base_is_refused",
                    test_file_disk_backing_base_is_refused);
    g_test_add_func("/vm_manager/file_disk/convert_failure_removes_target",
                    test_file_disk_convert_failure_removes_target);
    g_test_add_func("/vm_manager/file_disk/existing_target_is_preserved",
                    test_file_disk_existing_target_is_preserved);
    g_test_add_func("/vm_manager/file_disk/pinned_base_swap_rejected",
                    test_file_disk_pinned_base_swap_rejected);
    g_test_add_func("/vm_manager/file_disk/unpinned_base_swap_passes",
                    test_file_disk_unpinned_base_swap_passes);
    g_test_add_func("/vm_manager/new", test_new);
    g_test_add_func("/vm_manager/start_vm_nonexistent", test_start_vm_nonexistent);
    g_test_add_func("/vm_manager/zvol_disk/base_format_is_probed", test_zvol_disk_base_format_is_probed);
    g_test_add_func("/vm_manager/zvol_disk/convert_failure_rolls_back", test_zvol_disk_convert_failure_rolls_back);
    g_test_add_func("/vm_manager/zvol_disk/rollback_retries_busy", test_zvol_disk_rollback_retries_busy);
    g_test_add_func("/vm_manager/zvol_disk/rollback_failure_is_reported", test_zvol_disk_rollback_failure_is_reported);
    g_test_add_func("/vm_manager/zvol_disk/refused_base_creates_nothing", test_zvol_disk_refused_base_creates_nothing);
    g_test_add_func("/vm_manager/zvol_disk/recheck_after_lock_wait", test_zvol_disk_recheck_after_lock_wait);
    g_test_add_func("/vm_manager/zvol_disk/pin_recheck_after_lock_wait", test_zvol_disk_pin_recheck_after_lock_wait);
    g_test_add_func("/vm_manager/zvol_disk/without_base", test_zvol_disk_without_base);
    g_test_add_func("/vm_manager/zvol_disk/bad_base_rejected_before_create", test_zvol_disk_bad_base_rejected_before_create);
    g_test_add_func("/vm_manager/zvol_disk/create_failure", test_zvol_disk_create_failure);
    g_test_add_func("/vm_manager/stop_vm_nonexistent", test_stop_vm_nonexistent);
    g_test_add_func("/vm_manager/set_vcpu_nonexistent", test_set_vcpu_nonexistent);
    g_test_add_func("/vm_manager/set_memory_nonexistent", test_set_memory_nonexistent);
    g_test_add_func("/vm_manager/resolve_bridge_null_defaults", test_resolve_bridge_null_defaults);
    g_test_add_func("/vm_manager/resolve_bridge_empty_defaults", test_resolve_bridge_empty_defaults);
    g_test_add_func("/vm_manager/resolve_bridge_none_is_null", test_resolve_bridge_none_is_null);
    g_test_add_func("/vm_manager/resolve_bridge_explicit_passthrough", test_resolve_bridge_explicit_passthrough);
    g_test_add_func("/vm_manager/overlay_ethernet_iface_shape", test_overlay_ethernet_iface_shape);
    g_test_add_func("/vm_manager/overlay_metadata_build_parse_roundtrip", test_overlay_metadata_build_parse_roundtrip);
    g_test_add_func("/vm_manager/overlay_metadata_build_non_overlay_is_empty", test_overlay_metadata_build_non_overlay_is_empty);
    g_test_add_func("/vm_manager/overlay_metadata_build_escapes_values", test_overlay_metadata_build_escapes_values);
    g_test_add_func("/vm_manager/overlay_metadata_parse_absent_returns_false", test_overlay_metadata_parse_absent_returns_false);
    g_test_add_func("/vm_manager/overlay_metadata_parse_malformed_returns_false", test_overlay_metadata_parse_malformed_returns_false);
    g_test_add_func("/vm_manager/overlay_metadata_parse_null_safe", test_overlay_metadata_parse_null_safe);
    g_test_add_func("/vm_manager/overlay_metadata_parse_libvirt_stripped_form", test_overlay_metadata_parse_libvirt_stripped_form);
    g_test_add_func("/vm_manager/overlay_metadata_parse_libvirt_attr_reordered", test_overlay_metadata_parse_libvirt_attr_reordered);
    g_test_add_func("/vm_manager/dpdk_metadata_build_parse_roundtrip", test_dpdk_metadata_build_parse_roundtrip);
    g_test_add_func("/vm_manager/sriov_pci_parse_table", test_sriov_pci_parse_table);
    g_test_add_func("/vm_manager/sriov_nic_check_table", test_sriov_nic_check_table);
    g_test_add_func("/vm_manager/dpdk_metadata_non_dpdk_is_empty", test_dpdk_metadata_non_dpdk_is_empty);
    g_test_add_func("/vm_manager/dpdk_metadata_parse_libvirt_stripped_form", test_dpdk_metadata_parse_libvirt_stripped_form);
    g_test_add_func("/vm_manager/dpdk_metadata_parse_malformed_fails", test_dpdk_metadata_parse_malformed_fails);
    g_test_add_func("/vm_manager/dpdk_vhost/source_classifies_canonical_and_legacy",
                    test_dpdk_vhost_source_classifies_canonical_and_legacy);
    g_test_add_func("/vm_manager/dpdk_vhost/source_rejects_foreign_or_ambiguous",
                    test_dpdk_vhost_source_rejects_foreign_or_ambiguous);
    g_test_add_func("/vm_manager/dpdk_vhost/legacy_migration_exact_idempotent",
                    test_dpdk_vhost_legacy_migration_is_exact_and_idempotent);
    g_test_add_func("/vm_manager/dpdk_vhost/start_action_matrix",
                    test_dpdk_vhost_start_action_matrix);
    g_test_add_func("/vm_manager/dpdk_metadata_read_ok_libvirt", test_dpdk_metadata_read_ok_libvirt);
    g_test_add_func("/vm_manager/dpdk_metadata_read_absent_libvirt", test_dpdk_metadata_read_absent_libvirt);
    g_test_add_func("/vm_manager/dpdk_metadata_read_invalid_libvirt", test_dpdk_metadata_read_invalid_libvirt);
    g_test_add_func("/vm_manager/dpdk_metadata_read_legacy_vhost_is_invalid_libvirt", test_dpdk_metadata_read_legacy_vhost_is_invalid_libvirt);
    g_test_add_func("/vm_manager/dpdk_vhost/inactive_migration_libvirt_reread",
                    test_dpdk_vhost_inactive_migration_libvirt_reread);
    g_test_add_func("/vm_manager/overlay_live_iface_parse_picks_ethernet", test_overlay_live_iface_parse_picks_ethernet);
    g_test_add_func("/vm_manager/overlay_live_iface_parse_attr_order_independent", test_overlay_live_iface_parse_attr_order_independent);
    g_test_add_func("/vm_manager/overlay_live_iface_parse_absent_returns_false", test_overlay_live_iface_parse_absent_returns_false);
    g_test_add_func("/vm_manager/overlay_live_iface_parse_missing_target_returns_false", test_overlay_live_iface_parse_missing_target_returns_false);
    g_test_add_func("/vm_manager/overlay_live_iface_parse_null_safe", test_overlay_live_iface_parse_null_safe);
    g_test_add_func("/vm_manager/overlay_gw_cidr_basic", test_overlay_gw_cidr_basic);
    g_test_add_func("/vm_manager/overlay_gw_cidr_high_index", test_overlay_gw_cidr_high_index);
    g_test_add_func("/vm_manager/overlay_gw_cidr_malformed_returns_null", test_overlay_gw_cidr_malformed_returns_null);
    g_test_add_func("/vm_manager/resize/select_picks_requested_file_disk",
                    test_resize_select_picks_requested_file_disk);
    g_test_add_func("/vm_manager/resize/select_picks_requested_zvol_disk",
                    test_resize_select_picks_requested_zvol_disk);
    g_test_add_func("/vm_manager/resize/select_classifies_plain_block_device",
                    test_resize_select_classifies_plain_block_device);
    g_test_add_func("/vm_manager/resize/select_rejects_missing_cdrom_and_network",
                    test_resize_select_rejects_missing_cdrom_and_network);
    g_test_add_func("/vm_manager/resize/select_rejects_malformed_inputs",
                    test_resize_select_rejects_malformed_inputs);
    g_test_add_func("/vm_manager/resize/check_rejects_shrink",
                    test_resize_check_rejects_shrink);
}
