
#include <glib.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>

#include "modules/virt/vm_clone_plan.h"
#include "utils/pcv_spawn.h"

static const gchar *ZVOL_WITH_CDROM_XML =
    "<domain type='kvm'>"
    "  <name>leesieun</name>"
    "  <devices>"
    "    <disk type='block' device='disk'>"
    "      <driver name='qemu' type='raw'/>"
    "      <source dev='/dev/zvol/pcvtntank/vms/leesieun'/>"
    "      <target dev='vda' bus='virtio'/>"
    "    </disk>"
    "    <disk type='file' device='cdrom'>"
    "      <driver name='qemu' type='raw'/>"
    "      <source file='/iso/ubuntu-24.04.4-live-server-amd64.iso'/>"
    "      <target dev='sda' bus='sata'/>"
    "      <readonly/>"
    "    </disk>"
    "  </devices>"
    "</domain>";

static void
test_zvol_with_cdrom_extracts_single_data_disk(void)
{
    PcvVmCloneDiskInfo info = {0};
    gchar *error_msg = NULL;

    g_assert_true(pcv_vm_clone_extract_disk_info(ZVOL_WITH_CDROM_XML,
                                                &info,
                                                &error_msg));
    g_assert_null(error_msg);
    g_assert_cmpuint(info.disk_count, ==, 1);
    g_assert_cmpuint(info.kind, ==, PCV_VM_CLONE_DISK_ZVOL);
    g_assert_cmpstr(info.source_attr, ==, "dev");
    g_assert_cmpstr(info.source_path, ==, "/dev/zvol/pcvtntank/vms/leesieun");
    g_assert_cmpstr(info.driver_type, ==, "raw");

    pcv_vm_clone_disk_info_clear(&info);
}

static void
test_zvol_plan_uses_source_pool_path(void)
{
    PcvVmCloneDiskInfo info = {0};
    PcvVmCloneDiskPlan plan = {0};
    gchar *error_msg = NULL;

    g_assert_true(pcv_vm_clone_extract_disk_info(ZVOL_WITH_CDROM_XML,
                                                &info,
                                                &error_msg));
    g_assert_null(error_msg);
    g_assert_true(pcv_vm_clone_build_zvol_plan("naverblog",
                                              &info,
                                              &plan,
                                              &error_msg));
    g_assert_null(error_msg);
    g_assert_cmpuint(plan.kind, ==, PCV_VM_CLONE_DISK_ZVOL);
    g_assert_cmpstr(plan.source_disk_path, ==, "/dev/zvol/pcvtntank/vms/leesieun");
    g_assert_cmpstr(plan.target_disk_path, ==, "/dev/zvol/pcvtntank/vms/naverblog");
    g_assert_cmpstr(plan.source_dataset, ==, "pcvtntank/vms/leesieun");
    g_assert_cmpstr(plan.target_dataset, ==, "pcvtntank/vms/naverblog");
    g_assert_cmpstr(plan.zfs_pool, ==, "pcvtntank/vms");
    g_assert_cmpstr(plan.source_zvol_name, ==, "leesieun");

    pcv_vm_clone_disk_plan_clear(&plan);
    pcv_vm_clone_disk_info_clear(&info);
}

static void
test_qcow2_is_detected_but_zvol_plan_is_blocked(void)
{
    const gchar *xml =
        "<domain>"
        "  <devices>"
        "    <disk type='file' device='disk'>"
        "      <driver name='qemu' type='qcow2'/>"
        "      <source file='/var/lib/libvirt/images/source.qcow2'/>"
        "      <target dev='vda' bus='virtio'/>"
        "    </disk>"
        "  </devices>"
        "</domain>";
    PcvVmCloneDiskInfo info = {0};
    PcvVmCloneDiskPlan plan = {0};
    gchar *error_msg = NULL;

    g_assert_true(pcv_vm_clone_extract_disk_info(xml, &info, &error_msg));
    g_assert_null(error_msg);
    g_assert_cmpuint(info.disk_count, ==, 1);
    g_assert_cmpuint(info.kind, ==, PCV_VM_CLONE_DISK_QCOW2);

    g_assert_false(pcv_vm_clone_build_zvol_plan("clone", &info, &plan, &error_msg));
    g_assert_nonnull(error_msg);
    g_assert_nonnull(strstr(error_msg, "zvol"));

    g_free(error_msg);
    pcv_vm_clone_disk_plan_clear(&plan);
    pcv_vm_clone_disk_info_clear(&info);
}

static void
test_qcow2_file_plan_calculates_target_path(void)
{
    const gchar *xml =
        "<domain>"
        "  <devices>"
        "    <disk type='file' device='disk'>"
        "      <driver name='qemu' type='qcow2'/>"
        "      <source file='/var/lib/libvirt/images/source.qcow2'/>"
        "      <target dev='vda' bus='virtio'/>"
        "    </disk>"
        "  </devices>"
        "</domain>";
    PcvVmCloneDiskInfo info = {0};
    PcvVmCloneDiskPlan plan = {0};
    gchar *error_msg = NULL;

    g_assert_true(pcv_vm_clone_extract_disk_info(xml, &info, &error_msg));
    g_assert_null(error_msg);
    g_assert_true(pcv_vm_clone_build_disk_plan("clone", &info, &plan, &error_msg));
    g_assert_null(error_msg);
    g_assert_cmpuint(plan.kind, ==, PCV_VM_CLONE_DISK_QCOW2);
    g_assert_cmpstr(plan.source_disk_path, ==, "/var/lib/libvirt/images/source.qcow2");
    g_assert_cmpstr(plan.target_disk_path, ==, "/var/lib/libvirt/images/clone.qcow2");
    g_assert_null(plan.source_dataset);
    g_assert_null(plan.target_dataset);

    g_assert_true(pcv_vm_clone_disk_plan_beta_allowed(&plan, &error_msg));
    g_assert_null(error_msg);

    g_free(error_msg);
    pcv_vm_clone_disk_plan_clear(&plan);
    pcv_vm_clone_disk_info_clear(&info);
}

static void
test_raw_is_detected_but_zvol_plan_is_blocked(void)
{
    const gchar *xml =
        "<domain>"
        "  <devices>"
        "    <disk type='file' device='disk'>"
        "      <driver name='qemu' type='raw'/>"
        "      <source file='/var/lib/libvirt/images/source.img'/>"
        "      <target dev='vda' bus='virtio'/>"
        "    </disk>"
        "  </devices>"
        "</domain>";
    PcvVmCloneDiskInfo info = {0};
    PcvVmCloneDiskPlan plan = {0};
    gchar *error_msg = NULL;

    g_assert_true(pcv_vm_clone_extract_disk_info(xml, &info, &error_msg));
    g_assert_null(error_msg);
    g_assert_cmpuint(info.kind, ==, PCV_VM_CLONE_DISK_RAW);

    g_assert_false(pcv_vm_clone_build_zvol_plan("clone", &info, &plan, &error_msg));
    g_assert_nonnull(error_msg);
    g_assert_nonnull(strstr(error_msg, "zvol"));

    g_free(error_msg);
    pcv_vm_clone_disk_plan_clear(&plan);
    pcv_vm_clone_disk_info_clear(&info);
}

static void
test_raw_img_file_plan_keeps_img_extension(void)
{
    const gchar *xml =
        "<domain>"
        "  <devices>"
        "    <disk type='file' device='disk'>"
        "      <driver name='qemu' type='raw'/>"
        "      <source file='/var/lib/libvirt/images/source.img'/>"
        "      <target dev='vda' bus='virtio'/>"
        "    </disk>"
        "  </devices>"
        "</domain>";
    PcvVmCloneDiskInfo info = {0};
    PcvVmCloneDiskPlan plan = {0};
    gchar *error_msg = NULL;

    g_assert_true(pcv_vm_clone_extract_disk_info(xml, &info, &error_msg));
    g_assert_null(error_msg);
    g_assert_true(pcv_vm_clone_build_disk_plan("clone", &info, &plan, &error_msg));
    g_assert_null(error_msg);
    g_assert_cmpuint(plan.kind, ==, PCV_VM_CLONE_DISK_RAW);
    g_assert_cmpstr(plan.target_disk_path, ==, "/var/lib/libvirt/images/clone.img");

    pcv_vm_clone_disk_plan_clear(&plan);
    pcv_vm_clone_disk_info_clear(&info);
}

static void
test_raw_file_plan_defaults_to_img_extension(void)
{
    const gchar *xml =
        "<domain>"
        "  <devices>"
        "    <disk type='file' device='disk'>"
        "      <driver name='qemu' type='raw'/>"
        "      <source file='/var/lib/libvirt/images/source-disk'/>"
        "      <target dev='vda' bus='virtio'/>"
        "    </disk>"
        "  </devices>"
        "</domain>";
    PcvVmCloneDiskInfo info = {0};
    PcvVmCloneDiskPlan plan = {0};
    gchar *error_msg = NULL;

    g_assert_true(pcv_vm_clone_extract_disk_info(xml, &info, &error_msg));
    g_assert_null(error_msg);
    g_assert_true(pcv_vm_clone_build_disk_plan("clone", &info, &plan, &error_msg));
    g_assert_null(error_msg);
    g_assert_cmpuint(plan.kind, ==, PCV_VM_CLONE_DISK_RAW);
    g_assert_cmpstr(plan.source_disk_path, ==, "/var/lib/libvirt/images/source-disk");
    g_assert_cmpstr(plan.target_disk_path, ==, "/var/lib/libvirt/images/clone.img");

    pcv_vm_clone_disk_plan_clear(&plan);
    pcv_vm_clone_disk_info_clear(&info);
}

static void
test_raw_file_plan_is_allowed_for_full_clone_worker(void)
{
    const gchar *xml =
        "<domain>"
        "  <devices>"
        "    <disk type='file' device='disk'>"
        "      <driver name='qemu' type='raw'/>"
        "      <source file='/var/lib/libvirt/images/source.raw'/>"
        "      <target dev='vda' bus='virtio'/>"
        "    </disk>"
        "  </devices>"
        "</domain>";
    PcvVmCloneDiskInfo info = {0};
    PcvVmCloneDiskPlan plan = {0};
    gchar *error_msg = NULL;

    g_assert_true(pcv_vm_clone_extract_disk_info(xml, &info, &error_msg));
    g_assert_null(error_msg);
    g_assert_true(pcv_vm_clone_build_disk_plan("clone", &info, &plan, &error_msg));
    g_assert_null(error_msg);
    g_assert_true(pcv_vm_clone_disk_plan_beta_allowed(&plan, &error_msg));
    g_assert_null(error_msg);

    pcv_vm_clone_disk_plan_clear(&plan);
    pcv_vm_clone_disk_info_clear(&info);
}

static void
test_file_plan_rejects_unsafe_target_name(void)
{
    const gchar *xml =
        "<domain>"
        "  <devices>"
        "    <disk type='file' device='disk'>"
        "      <driver name='qemu' type='qcow2'/>"
        "      <source file='/var/lib/libvirt/images/source.qcow2'/>"
        "      <target dev='vda' bus='virtio'/>"
        "    </disk>"
        "  </devices>"
        "</domain>";
    PcvVmCloneDiskInfo info = {0};
    PcvVmCloneDiskPlan plan = {0};
    gchar *error_msg = NULL;

    g_assert_true(pcv_vm_clone_extract_disk_info(xml, &info, &error_msg));
    g_assert_null(error_msg);
    g_assert_false(pcv_vm_clone_build_disk_plan("../clone", &info, &plan, &error_msg));
    g_assert_nonnull(error_msg);
    g_assert_nonnull(strstr(error_msg, "invalid clone disk plan"));

    g_free(error_msg);
    pcv_vm_clone_disk_plan_clear(&plan);
    pcv_vm_clone_disk_info_clear(&info);
}

static gboolean
strv_contains(GStrv values, const gchar *needle)
{
    if (!values || !needle)
        return FALSE;
    for (gsize i = 0; values[i]; i++) {
        if (g_strcmp0(values[i], needle) == 0)
            return TRUE;
    }
    return FALSE;
}

static gboolean
strv_contains_substring(GStrv values, const gchar *needle)
{
    if (!values || !needle)
        return FALSE;
    for (gsize i = 0; values[i]; i++) {
        if (strstr(values[i], needle))
            return TRUE;
    }
    return FALSE;
}

static void
test_guest_reset_argv_covers_identity_reset(void)
{
    PcvVmCloneDiskPlan plan = {
        .kind = PCV_VM_CLONE_DISK_QCOW2,
        .target_disk_path = g_strdup("/var/lib/libvirt/images/clone.qcow2"),
    };
    gchar *error_msg = NULL;
    GStrv argv = pcv_vm_clone_build_guest_reset_argv(&plan,
                                                     "clone-vm",
                                                     &error_msg);

    g_assert_null(error_msg);
    g_assert_nonnull(argv);
    g_assert_cmpstr(argv[0], ==, "virt-sysprep");
    g_assert_true(strv_contains(argv, "--format"));
    g_assert_true(strv_contains(argv, "qcow2"));
    g_assert_true(strv_contains(argv, "-a"));
    g_assert_true(strv_contains(argv, "/var/lib/libvirt/images/clone.qcow2"));
    g_assert_true(strv_contains(argv, "--no-network"));
    g_assert_true(strv_contains_substring(argv, "fs-uuids"));
    g_assert_true(strv_contains_substring(argv, "lvm-uuids"));
    g_assert_true(strv_contains_substring(argv, "lvm-system-devices"));
    g_assert_true(strv_contains_substring(argv, "net-hostname"));
    g_assert_true(strv_contains(argv, "--hostname"));
    g_assert_true(strv_contains(argv, "clone-vm"));
    g_assert_true(strv_contains_substring(argv, "cloud-init clean"));
    g_assert_false(strv_contains_substring(argv, "update-initramfs"));
    g_assert_false(strv_contains_substring(argv, "dracut"));
    g_assert_false(strv_contains_substring(argv, "grub2-mkconfig"));
    g_assert_false(strv_contains_substring(argv, "/.autorelabel"));

    g_strfreev(argv);
    pcv_vm_clone_disk_plan_clear(&plan);
}

static void
test_guest_boot_rebuild_argv_runs_after_sysprep_contract(void)
{
    PcvVmCloneDiskPlan plan = {
        .kind = PCV_VM_CLONE_DISK_QCOW2,
        .target_disk_path = g_strdup("/var/lib/libvirt/images/clone.qcow2"),
    };
    gchar *error_msg = NULL;
    GStrv argv = pcv_vm_clone_build_guest_boot_rebuild_argv(&plan, &error_msg);

    g_assert_null(error_msg);
    g_assert_nonnull(argv);
    g_assert_cmpstr(argv[0], ==, "virt-customize");
    g_assert_true(strv_contains(argv, "--format"));
    g_assert_true(strv_contains(argv, "qcow2"));
    g_assert_true(strv_contains(argv, "-a"));
    g_assert_true(strv_contains(argv, "/var/lib/libvirt/images/clone.qcow2"));
    g_assert_true(strv_contains(argv, "--no-network"));
    g_assert_true(strv_contains_substring(argv, "update-initramfs"));
    g_assert_true(strv_contains_substring(argv, "dracut"));
    g_assert_true(strv_contains_substring(argv, "grub2-mkconfig"));
    g_assert_true(strv_contains_substring(argv, "/.autorelabel"));

    g_strfreev(argv);
    pcv_vm_clone_disk_plan_clear(&plan);
}

static void
test_guest_reset_argv_uses_raw_format_for_zvol(void)
{
    PcvVmCloneDiskPlan plan = {
        .kind = PCV_VM_CLONE_DISK_ZVOL,
        .target_disk_path = g_strdup("/dev/zvol/rpool/clone"),
    };
    gchar *error_msg = NULL;
    GStrv argv = pcv_vm_clone_build_guest_reset_argv(&plan,
                                                     "clone",
                                                     &error_msg);

    g_assert_null(error_msg);
    g_assert_nonnull(argv);
    g_assert_true(strv_contains(argv, "--format"));
    g_assert_true(strv_contains(argv, "raw"));
    g_assert_true(strv_contains(argv, "/dev/zvol/rpool/clone"));

    g_strfreev(argv);
    pcv_vm_clone_disk_plan_clear(&plan);
}

static void
test_guest_boot_rebuild_argv_uses_raw_format_for_zvol(void)
{
    PcvVmCloneDiskPlan plan = {
        .kind = PCV_VM_CLONE_DISK_ZVOL,
        .target_disk_path = g_strdup("/dev/zvol/rpool/clone"),
    };
    gchar *error_msg = NULL;
    GStrv argv = pcv_vm_clone_build_guest_boot_rebuild_argv(&plan, &error_msg);

    g_assert_null(error_msg);
    g_assert_nonnull(argv);
    g_assert_cmpstr(argv[0], ==, "virt-customize");
    g_assert_true(strv_contains(argv, "--format"));
    g_assert_true(strv_contains(argv, "raw"));
    g_assert_true(strv_contains(argv, "/dev/zvol/rpool/clone"));

    g_strfreev(argv);
    pcv_vm_clone_disk_plan_clear(&plan);
}

static void
test_multiple_data_disks_are_rejected(void)
{
    const gchar *xml =
        "<domain>"
        "  <devices>"
        "    <disk type='block' device='disk'>"
        "      <source dev='/dev/zvol/pool/vms/a'/>"
        "    </disk>"
        "    <disk type='block' device='disk'>"
        "      <source dev='/dev/zvol/pool/vms/a-data'/>"
        "    </disk>"
        "  </devices>"
        "</domain>";
    PcvVmCloneDiskInfo info = {0};
    gchar *error_msg = NULL;

    g_assert_false(pcv_vm_clone_extract_disk_info(xml, &info, &error_msg));
    g_assert_cmpuint(info.disk_count, ==, 2);
    g_assert_nonnull(error_msg);
    g_assert_nonnull(strstr(error_msg, "found 2"));

    g_free(error_msg);
    pcv_vm_clone_disk_info_clear(&info);
}

static void
test_cdrom_only_is_rejected(void)
{
    const gchar *xml =
        "<domain>"
        "  <devices>"
        "    <disk type='file' device='cdrom'>"
        "      <source file='/iso/ubuntu.iso'/>"
        "      <readonly/>"
        "    </disk>"
        "  </devices>"
        "</domain>";
    PcvVmCloneDiskInfo info = {0};
    gchar *error_msg = NULL;

    g_assert_false(pcv_vm_clone_extract_disk_info(xml, &info, &error_msg));
    g_assert_cmpuint(info.disk_count, ==, 0);
    g_assert_nonnull(error_msg);
    g_assert_nonnull(strstr(error_msg, "found 0"));

    g_free(error_msg);
    pcv_vm_clone_disk_info_clear(&info);
}

static void
test_invalid_zvol_dataset_is_rejected(void)
{
    const gchar *xml =
        "<domain>"
        "  <devices>"
        "    <disk type='block' device='disk'>"
        "      <source dev='/dev/zvol/leafonly'/>"
        "    </disk>"
        "  </devices>"
        "</domain>";
    PcvVmCloneDiskInfo info = {0};
    PcvVmCloneDiskPlan plan = {0};
    gchar *error_msg = NULL;

    g_assert_true(pcv_vm_clone_extract_disk_info(xml, &info, &error_msg));
    g_assert_null(error_msg);
    g_assert_cmpuint(info.kind, ==, PCV_VM_CLONE_DISK_ZVOL);

    g_assert_false(pcv_vm_clone_build_zvol_plan("clone", &info, &plan, &error_msg));
    g_assert_nonnull(error_msg);
    g_assert_nonnull(strstr(error_msg, "invalid zvol"));

    g_free(error_msg);
    pcv_vm_clone_disk_plan_clear(&plan);
    pcv_vm_clone_disk_info_clear(&info);
}

static void
test_disk_kind_strings_are_stable(void)
{
    g_assert_cmpstr(pcv_vm_clone_disk_kind_to_string(PCV_VM_CLONE_DISK_ZVOL),
                    ==,
                    "zvol");
    g_assert_cmpstr(pcv_vm_clone_disk_kind_to_string(PCV_VM_CLONE_DISK_QCOW2),
                    ==,
                    "qcow2");
    g_assert_cmpstr(pcv_vm_clone_disk_kind_to_string(PCV_VM_CLONE_DISK_RAW),
                    ==,
                    "raw");
    g_assert_cmpstr(pcv_vm_clone_disk_kind_to_string(PCV_VM_CLONE_DISK_UNSUPPORTED),
                    ==,
                    "unsupported");
}

static gboolean
qemu_img_available(void)
{
    gchar *path = g_find_program_in_path("qemu-img");
    if (!path)
        return FALSE;
    g_free(path);
    return TRUE;
}

static gchar *
make_clone_copy_tmpdir(void)
{
    GError *error = NULL;
    gchar *dir = g_dir_make_tmp("pcv-vm-clone-file-XXXXXX", &error);
    g_assert_no_error(error);
    g_assert_nonnull(dir);
    return dir;
}

static void
test_qcow2_file_copy_uses_qemu_img_convert(void)
{
    if (!qemu_img_available()) {
        g_test_skip("qemu-img not available");
        return;
    }

    gchar *dir = make_clone_copy_tmpdir();
    gchar *source = g_build_filename(dir, "source.qcow2", NULL);
    gchar *target = g_build_filename(dir, "target.qcow2", NULL);
    GError *error = NULL;

    pcv_spawn_launcher_init();

    const gchar *create_argv[] = {
        "qemu-img", "create", "-f", "qcow2", source, "1M", NULL
    };
    g_assert_true(pcv_spawn_sync(create_argv, NULL, NULL, &error));
    g_assert_no_error(error);

    PcvVmCloneDiskPlan plan = {
        .kind = PCV_VM_CLONE_DISK_QCOW2,
        .source_disk_path = g_strdup(source),
        .target_disk_path = g_strdup(target),
    };

    g_assert_true(pcv_vm_clone_copy_file_disk(&plan, &error));
    g_assert_no_error(error);
    g_assert_true(g_file_test(target, G_FILE_TEST_EXISTS));

    gchar *info_out = NULL;
    const gchar *info_argv[] = {
        "qemu-img", "info", "--output=json", target, NULL
    };
    g_assert_true(pcv_spawn_sync(info_argv, &info_out, NULL, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(info_out, "\"format\": \"qcow2\""));

    g_free(info_out);
    pcv_vm_clone_disk_plan_clear(&plan);
    g_remove(target);
    g_remove(source);
    g_rmdir(dir);
    g_free(target);
    g_free(source);
    g_free(dir);
    pcv_spawn_launcher_shutdown();
}

static void
test_raw_file_copy_creates_separate_target(void)
{
    if (!qemu_img_available()) {
        g_test_skip("qemu-img not available");
        return;
    }

    gchar *dir = make_clone_copy_tmpdir();
    gchar *source = g_build_filename(dir, "source.raw", NULL);
    gchar *target = g_build_filename(dir, "target.raw", NULL);
    const gchar *payload = "purecvisor raw clone source\n";
    GError *error = NULL;

    pcv_spawn_launcher_init();

    g_assert_true(g_file_set_contents(source, payload, -1, &error));
    g_assert_no_error(error);

    PcvVmCloneDiskPlan plan = {
        .kind = PCV_VM_CLONE_DISK_RAW,
        .source_disk_path = g_strdup(source),
        .target_disk_path = g_strdup(target),
    };

    g_assert_true(pcv_vm_clone_copy_file_disk(&plan, &error));
    g_assert_no_error(error);
    g_assert_true(g_file_test(target, G_FILE_TEST_EXISTS));
    g_assert_cmpstr(plan.source_disk_path, !=, plan.target_disk_path);

    gchar *target_bytes = NULL;
    gsize target_len = 0;
    g_assert_true(g_file_get_contents(target, &target_bytes, &target_len, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(target_len, >=, strlen(payload));
    g_assert_true(memcmp(target_bytes, payload, strlen(payload)) == 0);

    g_free(target_bytes);
    pcv_vm_clone_disk_plan_clear(&plan);
    g_remove(target);
    g_remove(source);
    g_rmdir(dir);
    g_free(target);
    g_free(source);
    g_free(dir);
    pcv_spawn_launcher_shutdown();
}

static void
test_file_copy_rejects_existing_target(void)
{
    gchar *dir = make_clone_copy_tmpdir();
    gchar *source = g_build_filename(dir, "source.raw", NULL);
    gchar *target = g_build_filename(dir, "target.raw", NULL);
    GError *error = NULL;

    g_assert_true(g_file_set_contents(source, "source", -1, &error));
    g_assert_no_error(error);
    g_assert_true(g_file_set_contents(target, "already here", -1, &error));
    g_assert_no_error(error);

    PcvVmCloneDiskPlan plan = {
        .kind = PCV_VM_CLONE_DISK_RAW,
        .source_disk_path = g_strdup(source),
        .target_disk_path = g_strdup(target),
    };

    g_assert_false(pcv_vm_clone_copy_file_disk(&plan, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS);
    g_clear_error(&error);

    gchar *target_contents = NULL;
    g_assert_true(g_file_get_contents(target, &target_contents, NULL, &error));
    g_assert_no_error(error);
    g_assert_cmpstr(target_contents, ==, "already here");

    g_free(target_contents);
    pcv_vm_clone_disk_plan_clear(&plan);
    g_remove(target);
    g_remove(source);
    g_rmdir(dir);
    g_free(target);
    g_free(source);
    g_free(dir);
}

static void
test_file_copy_rejects_zvol_plan(void)
{
    PcvVmCloneDiskPlan plan = {
        .kind = PCV_VM_CLONE_DISK_ZVOL,
        .source_disk_path = g_strdup("/dev/zvol/rpool/source"),
        .target_disk_path = g_strdup("/dev/zvol/rpool/target"),
    };
    GError *error = NULL;

    g_assert_false(pcv_vm_clone_copy_file_disk(&plan, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);

    g_clear_error(&error);
    pcv_vm_clone_disk_plan_clear(&plan);
}

void
test_vm_clone_plan_register(void)
{

    g_test_add_func("/vm_clone_plan/zvol_with_cdrom_extracts_single_data_disk",
                    test_zvol_with_cdrom_extracts_single_data_disk);
    g_test_add_func("/vm_clone_plan/zvol_plan_uses_source_pool_path",
                    test_zvol_plan_uses_source_pool_path);
    g_test_add_func("/vm_clone_plan/qcow2_detected_but_zvol_plan_blocked",
                    test_qcow2_is_detected_but_zvol_plan_is_blocked);
    g_test_add_func("/vm_clone_plan/qcow2_file_plan_calculates_target_path",
                    test_qcow2_file_plan_calculates_target_path);
    g_test_add_func("/vm_clone_plan/raw_detected_but_zvol_plan_blocked",
                    test_raw_is_detected_but_zvol_plan_is_blocked);
    g_test_add_func("/vm_clone_plan/raw_img_file_plan_keeps_img_extension",
                    test_raw_img_file_plan_keeps_img_extension);
    g_test_add_func("/vm_clone_plan/raw_file_plan_defaults_to_img_extension",
                    test_raw_file_plan_defaults_to_img_extension);
    g_test_add_func("/vm_clone_plan/raw_file_plan_is_allowed_for_full_clone_worker",
                    test_raw_file_plan_is_allowed_for_full_clone_worker);
    g_test_add_func("/vm_clone_plan/file_plan_rejects_unsafe_target_name",
                    test_file_plan_rejects_unsafe_target_name);
    g_test_add_func("/vm_clone_plan/guest_reset_argv_covers_identity_reset",
                    test_guest_reset_argv_covers_identity_reset);
    g_test_add_func("/vm_clone_plan/guest_boot_rebuild_argv_runs_after_sysprep_contract",
                    test_guest_boot_rebuild_argv_runs_after_sysprep_contract);
    g_test_add_func("/vm_clone_plan/guest_reset_argv_uses_raw_format_for_zvol",
                    test_guest_reset_argv_uses_raw_format_for_zvol);
    g_test_add_func("/vm_clone_plan/guest_boot_rebuild_argv_uses_raw_format_for_zvol",
                    test_guest_boot_rebuild_argv_uses_raw_format_for_zvol);
    g_test_add_func("/vm_clone_plan/multiple_data_disks_rejected",
                    test_multiple_data_disks_are_rejected);
    g_test_add_func("/vm_clone_plan/cdrom_only_rejected",
                    test_cdrom_only_is_rejected);
    g_test_add_func("/vm_clone_plan/invalid_zvol_dataset_rejected",
                    test_invalid_zvol_dataset_is_rejected);
    g_test_add_func("/vm_clone_plan/disk_kind_strings",
                    test_disk_kind_strings_are_stable);
    g_test_add_func("/vm_clone_plan/qcow2_file_copy_uses_qemu_img_convert",
                    test_qcow2_file_copy_uses_qemu_img_convert);
    g_test_add_func("/vm_clone_plan/raw_file_copy_creates_separate_target",
                    test_raw_file_copy_creates_separate_target);
    g_test_add_func("/vm_clone_plan/file_copy_rejects_existing_target",
                    test_file_copy_rejects_existing_target);
    g_test_add_func("/vm_clone_plan/file_copy_rejects_zvol_plan",
                    test_file_copy_rejects_zvol_plan);
}
