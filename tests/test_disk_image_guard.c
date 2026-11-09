                                                                                                             
                                                                                                         
                                                                             
                                            
                                                                                       
#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <string.h>
#include <unistd.h>

#include "modules/virt/disk_image_guard.h"
#include "utils/pcv_spawn.h"

#define MARKER "SECRET-BACKING-MARKER-0123456789"

                                                                              
static void
_judge(const gchar *json, PcvDiskImageKind kind, const gchar *needle, const gchar *format)
{
    g_autofree gchar *got = NULL;
    g_autoptr(GError) error = NULL;
    const gboolean ok = pcv_disk_image_judge_info(json, "/img/d", kind, &got, &error);
    if (needle) {
        g_assert_false(ok);
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
        g_assert_nonnull(strstr(error->message, needle));
        g_assert_null(got);
    } else {
        g_assert_no_error(error);
        g_assert_true(ok);
        g_assert_cmpstr(got, ==, format);
    }
}

                                                                           
static void
test_judge_table(void)
{
    _judge("{\"format\":\"raw\"}", PCV_DISK_IMAGE_BASE, NULL, "raw");
    _judge("{\"format\":\"qcow2\",\"format-specific\":{\"type\":\"qcow2\",\"data\":{\"compat\":\"1.1\"}}}",
           PCV_DISK_IMAGE_BASE, NULL, "qcow2");
    _judge("{\"format\":\"qcow\"}", PCV_DISK_IMAGE_BASE, NULL, "qcow");
    _judge("{\"format\":\"qcow2\",\"backing-filename\":\"/etc/shadow\"}", PCV_DISK_IMAGE_BASE, "backing file", NULL);
    _judge("{\"format\":\"qcow2\",\"full-backing-filename\":\"/etc/shadow\"}", PCV_DISK_IMAGE_BASE, "backing file", NULL);
    _judge("{\"format\":\"qcow\",\"backing-filename\":\"x\"}", PCV_DISK_IMAGE_BASE, "backing file", NULL);
    _judge("{\"format\":\"qcow2\",\"format-specific\":{\"data\":{\"data-file\":\"/etc/shadow\"}}}",
           PCV_DISK_IMAGE_BASE, "external data file", NULL);
    _judge("{\"format\":\"vmdk\"}", PCV_DISK_IMAGE_BASE, "raw, qcow2, or qcow", NULL);
    _judge("{\"format\":\"qed\"}", PCV_DISK_IMAGE_BASE, "raw, qcow2, or qcow", NULL);
    _judge("{\"format\":\"luks\"}", PCV_DISK_IMAGE_BASE, "raw, qcow2, or qcow", NULL);
    _judge("{}", PCV_DISK_IMAGE_BASE, "did not report a format", NULL);
    _judge("not json", PCV_DISK_IMAGE_BASE, "not JSON", NULL);
    _judge("[]", PCV_DISK_IMAGE_BASE, "not an object", NULL);
    _judge("", PCV_DISK_IMAGE_BASE, "not", NULL);

    const gchar *sparse = "{\"format\":\"vmdk\",\"format-specific\":{\"data\":{\"create-type\":\"monolithicSparse\","
                          "\"extents\":[{\"filename\":\"/img/d\"}]}}}";
    _judge(sparse, PCV_DISK_IMAGE_OVA_VMDK, NULL, "vmdk");
    _judge("{\"format\":\"vmdk\",\"format-specific\":{\"data\":{\"create-type\":\"streamOptimized\","
           "\"extents\":[{\"filename\":\"/img/d\"}]}}}", PCV_DISK_IMAGE_OVA_VMDK, NULL, "vmdk");
    _judge("{\"format\":\"vmdk\",\"format-specific\":{\"data\":{\"create-type\":\"monolithicFlat\","
           "\"extents\":[{\"filename\":\"/img/d\"}]}}}", PCV_DISK_IMAGE_OVA_VMDK, "create-type", NULL);
    _judge("{\"format\":\"vmdk\",\"format-specific\":{\"data\":{\"create-type\":\"monolithicSparse\","
           "\"extents\":[{\"filename\":\"/img/d\"},{\"filename\":\"/etc/shadow\"}]}}}",
           PCV_DISK_IMAGE_OVA_VMDK, "external extent", NULL);
    _judge("{\"format\":\"vmdk\",\"format-specific\":{\"data\":{\"create-type\":\"monolithicSparse\"}}}",
           PCV_DISK_IMAGE_OVA_VMDK, "extent list", NULL);
    _judge("{\"format\":\"vmdk\"}", PCV_DISK_IMAGE_OVA_VMDK, "details", NULL);
    _judge("{\"format\":\"qcow2\"}", PCV_DISK_IMAGE_OVA_VMDK, "not a vmdk", NULL);
    _judge("{\"format\":\"vmdk\",\"backing-filename\":\"x\",\"format-specific\":{\"data\":{\"create-type\":"
           "\"monolithicSparse\",\"extents\":[{\"filename\":\"/img/d\"}]}}}",
           PCV_DISK_IMAGE_OVA_VMDK, "backing file", NULL);

    g_autoptr(GError) error = NULL;
    g_autofree gchar *got = NULL;
    g_assert_false(pcv_disk_image_judge_info("{\"format\":\"qcow2\",\"backing-filename\":\"/etc/shadow\"}",
                                             "/img/d", PCV_DISK_IMAGE_BASE, &got, &error));
    g_assert_null(strstr(error->message, "shadow"));
}

                                                        
typedef struct { gchar *dir; } QemuFx;

static gboolean
_qemu_img_missing(void)
{
    g_autofree gchar *path = g_find_program_in_path("qemu-img");
    if (!path)
        g_test_skip("qemu-img not available");
    return path == NULL;
}

static gchar *
_p(const QemuFx *f, const gchar *name)
{
    return g_build_filename(f->dir, name, NULL);
}

                                 
static void
_make_secret(const gchar *path)
{
    g_autofree gchar *bytes = g_malloc0(2 * 1024 * 1024);
    memcpy(bytes, MARKER, strlen(MARKER));
    g_assert_true(g_file_set_contents(path, bytes, 2 * 1024 * 1024, NULL));
}

static void
_run(const gchar *const *argv)
{
    GError *error = NULL;
    g_assert_true(pcv_spawn_sync(argv, NULL, NULL, &error));
    g_assert_no_error(error);
}

static void
_fx_begin(QemuFx *f)
{
    f->dir = g_dir_make_tmp("pcv-disk-guard-XXXXXX", NULL);
    g_assert_nonnull(f->dir);
    pcv_spawn_launcher_init();
}

static void
_fx_end(QemuFx *f)
{
    pcv_spawn_launcher_shutdown();
    g_autofree gchar *cmd = g_strdup_printf("rm -rf %s", f->dir);
    g_assert_true(g_spawn_command_line_sync(cmd, NULL, NULL, NULL, NULL));
    g_free(f->dir);
}

                          
static void
_assert_refused(const gchar *path, PcvDiskImageKind kind, const gchar *needle)
{
    g_autofree gchar *format = NULL;
    g_autoptr(GError) error = NULL;
    g_assert_false(pcv_disk_image_inspect(path, kind, &format, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_assert_nonnull(strstr(error->message, needle));
    g_assert_null(format);
}

static void
_assert_allowed(const gchar *path, PcvDiskImageKind kind, const gchar *want)
{
    g_autofree gchar *format = NULL;
    GError *error = NULL;
    g_assert_true(pcv_disk_image_inspect(path, kind, &format, &error));
    g_assert_no_error(error);
    g_assert_cmpstr(format, ==, want);
}

                                                                    
                                                               
static void
test_real_qcow2_cases(void)
{
    if (_qemu_img_missing()) return;
    QemuFx f = {0};
    _fx_begin(&f);
    g_autofree gchar *secret = _p(&f, "secret.raw");
    g_autofree gchar *secret2 = _p(&f, "secret2.raw");
    g_autofree gchar *backed = _p(&f, "backed.qcow2");
    g_autofree gchar *datafile = _p(&f, "datafile.qcow2");
    g_autofree gchar *disguised = _p(&f, "disguised.raw");
    g_autofree gchar *plain_raw = _p(&f, "plain.raw");
    g_autofree gchar *plain_qcow2 = _p(&f, "plain.qcow2");
    g_autofree gchar *qcow2_named_raw = _p(&f, "qcow2-named.raw");
    g_autofree gchar *qed = _p(&f, "other.qed");
    _make_secret(secret);
    const gchar *mk_backed[] = {"qemu-img", "create", "-f", "qcow2", "-b", secret, "-F", "raw", backed, "2M", NULL};
    _run(mk_backed);
                                                                   
    g_autofree gchar *data_opt = g_strdup_printf("data_file=%s,data_file_raw=on", secret2);
    const gchar *mk_data[] = {"qemu-img", "create", "-f", "qcow2", "-o", data_opt, datafile, "2M", NULL};
    _run(mk_data);
    _make_secret(secret2);
    g_autofree gchar *backed_bytes = NULL;
    gsize backed_len = 0;
    g_assert_true(g_file_get_contents(backed, &backed_bytes, &backed_len, NULL));
    g_assert_true(g_file_set_contents(disguised, backed_bytes, (gssize)backed_len, NULL));
    const gchar *mk_raw[] = {"qemu-img", "create", "-f", "raw", plain_raw, "1M", NULL};
    const gchar *mk_q2[] = {"qemu-img", "create", "-f", "qcow2", plain_qcow2, "1M", NULL};
    const gchar *mk_q2r[] = {"qemu-img", "create", "-f", "qcow2", qcow2_named_raw, "1M", NULL};
    const gchar *mk_qed[] = {"qemu-img", "create", "-f", "qed", qed, "1M", NULL};
    _run(mk_raw); _run(mk_q2); _run(mk_q2r); _run(mk_qed);

    _assert_refused(backed, PCV_DISK_IMAGE_BASE, "backing file");
    _assert_refused(datafile, PCV_DISK_IMAGE_BASE, "external data file");
    _assert_refused(disguised, PCV_DISK_IMAGE_BASE, "backing file");
    _assert_refused(qed, PCV_DISK_IMAGE_BASE, "raw, qcow2, or qcow");
    _assert_allowed(plain_raw, PCV_DISK_IMAGE_BASE, "raw");
    _assert_allowed(plain_qcow2, PCV_DISK_IMAGE_BASE, "qcow2");
    _assert_allowed(qcow2_named_raw, PCV_DISK_IMAGE_BASE, "qcow2");                          
    _fx_end(&f);
}

                                                                                                              
                                                            
static void
test_real_vmdk_cases(void)
{
    if (_qemu_img_missing()) return;
    QemuFx f = {0};
    _fx_begin(&f);
    g_autofree gchar *secret = _p(&f, "secret.raw");
    g_autofree gchar *sparse = _p(&f, "sparse.vmdk");
    g_autofree gchar *stream = _p(&f, "stream.vmdk");
    g_autofree gchar *flat = _p(&f, "flat.vmdk");
    g_autofree gchar *lying = _p(&f, "lying.vmdk");
    _make_secret(secret);
    const gchar *mk_sparse[] = {"qemu-img", "create", "-f", "vmdk", "-o", "subformat=monolithicSparse", sparse, "1M", NULL};
    const gchar *mk_stream[] = {"qemu-img", "create", "-f", "vmdk", "-o", "subformat=streamOptimized", stream, "1M", NULL};
    _run(mk_sparse); _run(mk_stream);
    g_autofree gchar *flat_text = g_strdup_printf(
        "# Disk DescriptorFile\nversion=1\nCID=12345678\nparentCID=ffffffff\ncreateType=\"monolithicFlat\"\n"
        "RW 4096 FLAT \"%s\" 0\nddb.adapterType = \"lsilogic\"\n", secret);
    g_assert_true(g_file_set_contents(flat, flat_text, -1, NULL));
    g_autofree gchar *lying_text = g_strdup_printf(
        "# Disk DescriptorFile\nversion=1\nCID=12345678\nparentCID=ffffffff\ncreateType=\"monolithicSparse\"\n"
        "RW 2048 SPARSE \"%s\"\nddb.adapterType = \"lsilogic\"\n", sparse);
    g_assert_true(g_file_set_contents(lying, lying_text, -1, NULL));

    _assert_allowed(sparse, PCV_DISK_IMAGE_OVA_VMDK, "vmdk");
    _assert_allowed(stream, PCV_DISK_IMAGE_OVA_VMDK, "vmdk");
    _assert_refused(flat, PCV_DISK_IMAGE_OVA_VMDK, "create-type");
    g_autofree gchar *lying_format = NULL;
    g_autoptr(GError) lying_error = NULL;
    g_assert_false(pcv_disk_image_inspect(lying, PCV_DISK_IMAGE_OVA_VMDK, &lying_format, &lying_error));
    g_assert_nonnull(lying_error);
    _assert_refused(sparse, PCV_DISK_IMAGE_BASE, "raw, qcow2, or qcow");                                    
    _assert_refused(flat, PCV_DISK_IMAGE_BASE, "raw, qcow2, or qcow");
    _fx_end(&f);
}

                                       
static void
test_real_inspect_failure(void)
{
    if (_qemu_img_missing()) return;
    QemuFx f = {0};
    _fx_begin(&f);
    g_autofree gchar *gone = _p(&f, "gone.raw");
    g_autofree gchar *broken = _p(&f, "broken.qcow2");
    g_assert_true(g_file_set_contents(broken, "QFI\xfb\xff\xff\xff\xff\xff\xff\xff\xff", 12, NULL));
    for (guint i = 0; i < 2; i++) {
        g_autofree gchar *format = NULL;
        g_autoptr(GError) error = NULL;
        g_assert_false(pcv_disk_image_inspect(i ? broken : gone, PCV_DISK_IMAGE_BASE, &format, &error));
        g_assert_nonnull(error);
        g_assert_nonnull(strstr(error->message, "could not be inspected"));
        g_assert_null(format);
    }
    _fx_end(&f);
}

                                                                           
                                      
static void
test_real_convert_fixes_format(void)
{
    if (_qemu_img_missing()) return;
    QemuFx f = {0};
    _fx_begin(&f);
    g_autofree gchar *secret = _p(&f, "secret.raw");
    g_autofree gchar *backed = _p(&f, "backed.qcow2");
    g_autofree gchar *target = _p(&f, "target.raw");
    _make_secret(secret);
    const gchar *mk_backed[] = {"qemu-img", "create", "-f", "qcow2", "-b", secret, "-F", "raw", backed, "2M", NULL};
    const gchar *mk_target[] = {"qemu-img", "create", "-f", "raw", target, "2M", NULL};
    _run(mk_backed); _run(mk_target);
    GError *error = NULL;
    g_assert_true(pcv_disk_image_convert_into(backed, "raw", "raw", target, &error));
    g_assert_no_error(error);
    g_autofree gchar *bytes = NULL;
    gsize len = 0;
    g_assert_true(g_file_get_contents(target, &bytes, &len, NULL));
    g_assert_null(g_strstr_len(bytes, (gssize)len, MARKER));
    g_assert_cmpint(memcmp(bytes, "QFI\xfb", 4), ==, 0);

                                                                   
    const gchar *probed[] = {"qemu-img", "convert", "-n", "-O", "raw", backed, target, NULL};
    _run(probed);
    g_free(g_steal_pointer(&bytes));
    g_assert_true(g_file_get_contents(target, &bytes, &len, NULL));
    g_assert_nonnull(g_strstr_len(bytes, (gssize)len, MARKER));

                                  
    g_autofree gchar *gone = _p(&f, "gone.raw");
    g_assert_false(pcv_disk_image_convert_into(gone, "raw", "raw", target, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_clear_error(&error);
    _fx_end(&f);
}

                                                                                     
static void
test_driver_format_table(void)
{
    const gchar *xml =
        "<domain><devices>"
        "<disk type='file' device='cdrom'><driver name='qemu' type='raw'/><target dev='sda'/></disk>"
        "<disk type='file' device='disk'><driver name='qemu' type='qcow2'/><source file='/a'/><target dev='vda'/></disk>"
        "<disk type='file' device='disk'><driver name='qemu' type='raw'/><source file='/b'/><target dev='vdb'/></disk>"
        "<disk type='file' device='disk'><driver name='qemu'/><source file='/c'/><target dev='vdc'/></disk>"
        "<disk type='file' device='disk'><source file='/d.qcow2'/><target dev='vdd'/></disk>"
        "</devices></domain>";
    const struct { const gchar *dev; const gchar *want; } cases[] = {
        {"vda", "qcow2"}, {"vdb", "raw"}, {"vdc", NULL}, {"vdd", NULL}, {"vdz", NULL}, {"", NULL},
    };
    for (gsize i = 0; i < G_N_ELEMENTS(cases); i++) {
        g_autofree gchar *got = pcv_disk_image_driver_format(xml, cases[i].dev);
        g_assert_cmpstr(got, ==, cases[i].want);
    }
    g_assert_null(pcv_disk_image_driver_format(NULL, "vda"));
    g_assert_null(pcv_disk_image_driver_format("<domain", "vda"));
}

                                                                                  
                                                                                          
               
                                                            
static void
test_real_resize_fixes_format(void)
{
    if (_qemu_img_missing()) return;
    QemuFx f = {0};
    _fx_begin(&f);
    g_autofree gchar *external = _p(&f, "external.raw");
    g_autofree gchar *guest = _p(&f, "guest.img");
    g_autofree gchar *opt = g_strdup_printf("data_file=%s,data_file_raw=on", external);
    const gchar *mk[] = {"qemu-img", "create", "-f", "qcow2", "-o", opt, guest, "2M", NULL};
    _run(mk);
    _make_secret(external);
    g_assert_cmpint(truncate(guest, 8 * 1024 * 1024), ==, 0);
    GStatBuf st = {0};

                                           
    for (guint i = 0; i < 2; i++) {
        g_autoptr(GError) error = NULL;
        g_assert_false(pcv_disk_image_resize_file(guest, i ? "vmdk" : NULL, 1, &error));
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
        g_assert_cmpint(g_stat(guest, &st), ==, 0);
        g_assert_cmpint(st.st_size, ==, 8 * 1024 * 1024);
    }

    GError *error = NULL;
    g_assert_true(pcv_disk_image_resize_file(guest, "raw", 1, &error));
    g_assert_no_error(error);
    g_assert_cmpint(g_stat(guest, &st), ==, 0);
    g_assert_cmpint(st.st_size, ==, 1024 * 1024 * 1024);
    g_assert_cmpint(g_stat(external, &st), ==, 0);
    g_assert_cmpint(st.st_size, ==, 2 * 1024 * 1024);                   

                                          
    g_autofree gchar *real_qcow2 = _p(&f, "real.qcow2");
    const gchar *mk_q2[] = {"qemu-img", "create", "-f", "qcow2", real_qcow2, "1M", NULL};
    _run(mk_q2);
    g_assert_true(pcv_disk_image_resize_file(real_qcow2, "qcow2", 1, &error));
    g_assert_no_error(error);
                          
    g_autofree gchar *gone = _p(&f, "gone.raw");
    g_assert_false(pcv_disk_image_resize_file(gone, "raw", 1, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_clear_error(&error);
    _fx_end(&f);
}

void test_disk_image_guard_register(void);

void
test_disk_image_guard_register(void)
{
    g_test_add_func("/disk_image_guard/judge_table", test_judge_table);
    g_test_add_func("/disk_image_guard/driver_format_table", test_driver_format_table);
    g_test_add_func("/disk_image_guard/real_resize_fixes_format", test_real_resize_fixes_format);
    g_test_add_func("/disk_image_guard/real_qcow2_cases", test_real_qcow2_cases);
    g_test_add_func("/disk_image_guard/real_vmdk_cases", test_real_vmdk_cases);
    g_test_add_func("/disk_image_guard/real_inspect_failure", test_real_inspect_failure);
    g_test_add_func("/disk_image_guard/real_convert_fixes_format", test_real_convert_fixes_format);
}
