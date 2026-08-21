                                                                                        
                                                                                          
                                                                    
                                                                 
                         
                     
  
                                                            
  
                           
                                                                
                                                     
                                                                   
                                                          
                                                                    
                                                                    
                                           
  
                       
                                                          
                                                      
  
                                   
  
                                                                  
   

#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <sys/stat.h>
#include <unistd.h>

                              
extern JsonObject *pcv_sriov_status(void);
extern JsonArray *pcv_sriov_list(const gchar *pf);
extern gboolean pcv_sriov_disable(const gchar *pf, GError **error);
extern gchar *pcv_sriov_vf_pci_addr(const gchar *pf, gint vf_index);
extern gboolean pcv_sriov_set(const gchar *pf, gint vf_index,
                              const gchar *mac, gint vlan,
                              gint spoofchk, GError **error);
extern gboolean pcv_sriov_attach_vm(const gchar *vm_name,
                                    const gchar *pf, gint vf_index,
                                    GError **error);
extern gboolean pcv_sriov_detach_vm(const gchar *vm_name,
                                    const gchar *pci_addr, GError **error);
                                                    
extern void pcv_sriov_set_paths_for_test(const gchar *sysfs_net_root,
                                         const gchar *pci_devices_root,
                                         const gchar *virsh_path);

typedef struct {
    gchar *root;
    gchar *net_root;
    gchar *pci_root;
    gchar *drivers_root;
    gchar *iommu_root;
    gchar *bin_dir;
    gchar *log_path;
    gchar *virsh_path;
} SriovExecFixture;

static void
remove_tree(const gchar *path)
{
    GStatBuf stat_buf;
    if (!path || g_lstat(path, &stat_buf) != 0)
        return;
    if (!S_ISDIR(stat_buf.st_mode) || S_ISLNK(stat_buf.st_mode)) {
        g_unlink(path);
        return;
    }
    GDir *dir = g_dir_open(path, 0, NULL);
    if (!dir)
        return;
    const gchar *name = NULL;
    while ((name = g_dir_read_name(dir)) != NULL) {
        gchar *child = g_build_filename(path, name, NULL);
        remove_tree(child);
        g_free(child);
    }
    g_dir_close(dir);
    g_rmdir(path);
}

static SriovExecFixture *
sriov_exec_fixture_new(void)
{
    SriovExecFixture *fixture = g_new0(SriovExecFixture, 1);
    fixture->root = g_dir_make_tmp("pcv-sriov-exec-XXXXXX", NULL);
    g_assert_nonnull(fixture->root);
    fixture->net_root = g_build_filename(fixture->root, "net", NULL);
    fixture->pci_root = g_build_filename(fixture->root, "pci-devices", NULL);
    fixture->drivers_root = g_build_filename(fixture->root, "drivers", NULL);
    fixture->iommu_root = g_build_filename(fixture->root, "iommu-groups", NULL);
    fixture->bin_dir = g_build_filename(fixture->root, "bin", NULL);
    fixture->log_path = g_build_filename(fixture->root, "virsh.argv", NULL);
    fixture->virsh_path = g_build_filename(fixture->bin_dir, "virsh", NULL);
    g_assert_cmpint(g_mkdir_with_parents(fixture->net_root, 0700), ==, 0);
    g_assert_cmpint(g_mkdir_with_parents(fixture->pci_root, 0700), ==, 0);
    g_assert_cmpint(g_mkdir_with_parents(fixture->drivers_root, 0700), ==, 0);
    g_assert_cmpint(g_mkdir_with_parents(fixture->iommu_root, 0700), ==, 0);
    g_assert_cmpint(g_mkdir_with_parents(fixture->bin_dir, 0700), ==, 0);
    pcv_sriov_set_paths_for_test(fixture->net_root, fixture->pci_root,
                                 fixture->virsh_path);
    return fixture;
}

static void
sriov_exec_fixture_free(SriovExecFixture *fixture)
{
    if (!fixture)
        return;
    pcv_sriov_set_paths_for_test(NULL, NULL, NULL);
    remove_tree(fixture->root);
    g_free(fixture->virsh_path);
    g_free(fixture->log_path);
    g_free(fixture->bin_dir);
    g_free(fixture->iommu_root);
    g_free(fixture->drivers_root);
    g_free(fixture->pci_root);
    g_free(fixture->net_root);
    g_free(fixture->root);
    g_free(fixture);
}

static void
sriov_fixture_set_virsh(SriovExecFixture *fixture,
                        gint exit_status,
                        const gchar *stderr_text)
{
    gchar *script = g_strdup_printf(
        "#!/bin/sh\n"
        "printf '%%s\\n' \"$*\" > '%s'\n"
        "printf '%%s' '%s' >&2\n"
        "exit %d\n",
        fixture->log_path, stderr_text ? stderr_text : "", exit_status);
    g_assert_true(g_file_set_contents(fixture->virsh_path, script, -1, NULL));
    g_assert_cmpint(g_chmod(fixture->virsh_path, 0700), ==, 0);
    g_free(script);
}

typedef struct {
    gboolean initially_present;
    gboolean absent_after_detach_attempt;
    gboolean malformed_dumpxml;
    gint dumpxml_exit_status;
    const gchar *dumpxml_stderr;
    gint detach_exit_status;
    const gchar *detach_stderr;
} SriovDetachVirshSpec;

                                                            
                                                              
                                                    
static void
sriov_fixture_set_detach_virsh(SriovExecFixture *fixture,
                                const SriovDetachVirshSpec *spec)
{
    static const gchar absent_xml[] =
        "<domain><devices>"
        "<hostdev type='usb'><source><address domain='0x0000' bus='0x03' "
        "slot='0x10' function='0x0'/></source></hostdev>"
        "<hostdev type='pci'><source><address domain='0x0000' bus='0x03' "
        "slot='0x11' function='0x0'/></source>"
        "<address type='pci' domain='0x0000' bus='0x03' slot='0x10' "
        "function='0x0'/></hostdev>"
        "</devices></domain>";
    static const gchar present_xml[] =
        "<domain><devices>"
        "<hostdev mode='subsystem' type='pci' managed='yes'><source>"
        "<address domain='0x0000' bus='0x03' slot='0x10' function='0x0'/>"
        "</source></hostdev>"
        "</devices></domain>";

    gchar *state_path = g_build_filename(fixture->root, "device.state", NULL);
    gchar *present_path = g_build_filename(fixture->root, "domain-present.xml", NULL);
    gchar *absent_path = g_build_filename(fixture->root, "domain-absent.xml", NULL);
    gchar *malformed_path = g_build_filename(fixture->root, "domain-malformed.xml", NULL);
    gchar *dumpxml_stderr_path = g_build_filename(
        fixture->root, "dumpxml.stderr", NULL);
    gchar *detach_stderr_path = g_build_filename(
        fixture->root, "detach.stderr", NULL);

    g_assert_true(g_file_set_contents(
        state_path, spec->initially_present ? "present\n" : "absent\n",
        -1, NULL));
    g_assert_true(g_file_set_contents(present_path, present_xml, -1, NULL));
    g_assert_true(g_file_set_contents(absent_path, absent_xml, -1, NULL));
    g_assert_true(g_file_set_contents(malformed_path, "<domain><devices>",
                                      -1, NULL));
    g_assert_true(g_file_set_contents(
        dumpxml_stderr_path,
        spec->dumpxml_stderr ? spec->dumpxml_stderr : "", -1, NULL));
    g_assert_true(g_file_set_contents(
        detach_stderr_path,
        spec->detach_stderr ? spec->detach_stderr : "", -1, NULL));

    gchar *script = g_strdup_printf(
        "#!/bin/sh\n"
        "printf '%%s\\n' \"$*\" >> '%s'\n"
        "case \"$1\" in\n"
        "  dumpxml)\n"
        "    cat '%s' >&2\n"
        "    [ %d -eq 0 ] || exit %d\n"
        "    if [ %d -eq 1 ]; then\n"
        "      cat '%s'\n"
        "    elif [ \"$(cat '%s')\" = present ]; then\n"
        "      cat '%s'\n"
        "    else\n"
        "      cat '%s'\n"
        "    fi\n"
        "    exit 0\n"
        "    ;;\n"
        "  detach-device)\n"
        "    cat '%s' >&2\n"
        "    [ %d -eq 0 ] || printf '%%s\\n' absent > '%s'\n"
        "    exit %d\n"
        "    ;;\n"
        "  *) exit 90 ;;\n"
        "esac\n",
        fixture->log_path,
        dumpxml_stderr_path,
        spec->dumpxml_exit_status, spec->dumpxml_exit_status,
        spec->malformed_dumpxml ? 1 : 0, malformed_path,
        state_path, present_path, absent_path,
        detach_stderr_path,
        spec->absent_after_detach_attempt ? 1 : 0, state_path,
        spec->detach_exit_status);
    g_assert_true(g_file_set_contents(fixture->virsh_path, script, -1, NULL));
    g_assert_cmpint(g_chmod(fixture->virsh_path, 0700), ==, 0);

    g_free(script);
    g_free(detach_stderr_path);
    g_free(dumpxml_stderr_path);
    g_free(malformed_path);
    g_free(absent_path);
    g_free(present_path);
    g_free(state_path);
}

static void
sriov_fixture_set_blocking_attach_virsh(SriovExecFixture *fixture,
                                        const gchar *attach_ready,
                                        const gchar *attach_release,
                                        const gchar *detach_invoked)
{
    gchar *script = g_strdup_printf(
        "#!/bin/sh\n"
        "printf '%%s\\n' \"$*\" >> '%s'\n"
        "if [ \"$1\" = dumpxml ]; then\n"
        "  printf '%%s\\n' \"<domain><devices><hostdev type='pci'>"
        "<source><address domain='0xffff' bus='0xff' slot='0x1f' "
        "function='0x7'/></source></hostdev></devices></domain>\"\n"
        "elif [ \"$1\" = attach-device ]; then\n"
        "  : > '%s'\n"
        "  attempts=0\n"
        "  while [ ! -e '%s' ]; do\n"
        "    sleep 0.01\n"
        "    attempts=$((attempts + 1))\n"
        "    [ \"$attempts\" -lt 500 ] || exit 91\n"
        "  done\n"
        "elif [ \"$1\" = detach-device ]; then\n"
        "  : > '%s'\n"
        "fi\n"
        "exit 0\n",
        fixture->log_path, attach_ready, attach_release, detach_invoked);
    g_assert_true(g_file_set_contents(fixture->virsh_path, script, -1, NULL));
    g_assert_cmpint(g_chmod(fixture->virsh_path, 0700), ==, 0);
    g_free(script);
}

typedef struct {
    gboolean attach;
    gboolean started;
    gboolean ok;
    GError *error;
    GMutex start_mu;
    GCond start_cond;
} SriovConcurrentCall;

static gpointer
sriov_concurrent_call_thread(gpointer user_data)
{
    SriovConcurrentCall *call = user_data;

    g_mutex_lock(&call->start_mu);
    call->started = TRUE;
    g_cond_signal(&call->start_cond);
    g_mutex_unlock(&call->start_mu);

    if (call->attach)
        call->ok = pcv_sriov_attach_vm("vm-safe", "pf0", 0, &call->error);
    else
        call->ok = pcv_sriov_detach_vm("vm-safe", "ffff:ff:1f.7", &call->error);
    return NULL;
}

static void
sriov_concurrent_call_init(SriovConcurrentCall *call, gboolean attach)
{
    *call = (SriovConcurrentCall){.attach = attach};
    g_mutex_init(&call->start_mu);
    g_cond_init(&call->start_cond);
}

static void
sriov_concurrent_call_wait_started(SriovConcurrentCall *call)
{
    g_mutex_lock(&call->start_mu);
    while (!call->started)
        g_cond_wait(&call->start_cond, &call->start_mu);
    g_mutex_unlock(&call->start_mu);
}

static void
sriov_concurrent_call_clear(SriovConcurrentCall *call)
{
    g_clear_error(&call->error);
    g_cond_clear(&call->start_cond);
    g_mutex_clear(&call->start_mu);
}

static void
sriov_fixture_add_vf(SriovExecFixture *fixture,
                     const gchar *pf,
                     gint vf_index,
                     const gchar *pci_addr,
                     gboolean with_pf_iommu_group,
                     gboolean with_vf_iommu_group)
{
    gchar *pf_device = g_build_filename(fixture->net_root, pf, "device", NULL);
    gchar *pf_iommu_group = g_build_filename(pf_device, "iommu_group", NULL);
    gchar *vf_link = g_strdup_printf("%s/virtfn%d", pf_device, vf_index);
    gchar *pci_device = g_build_filename(fixture->pci_root, pci_addr, NULL);
    gchar *driver = g_build_filename(pci_device, "driver", NULL);
    gchar *host_driver = g_build_filename(fixture->drivers_root, "ixgbevf", NULL);
    gchar *vf_iommu_group = g_build_filename(pci_device, "iommu_group", NULL);
    gchar *pf_group_target = g_build_filename(fixture->iommu_root, "6", NULL);
    gchar *vf_group_target = g_build_filename(fixture->iommu_root, "7", NULL);
    g_assert_cmpint(g_mkdir_with_parents(pf_device, 0700), ==, 0);
    g_assert_cmpint(g_mkdir_with_parents(pci_device, 0700), ==, 0);
    g_assert_cmpint(g_mkdir_with_parents(host_driver, 0700), ==, 0);
    g_assert_cmpint(symlink(pci_device, vf_link), ==, 0);
    g_assert_cmpint(symlink(host_driver, driver), ==, 0);
    if (with_pf_iommu_group) {
        g_assert_cmpint(g_mkdir_with_parents(pf_group_target, 0700), ==, 0);
        g_assert_cmpint(symlink(pf_group_target, pf_iommu_group), ==, 0);
    }
    if (with_vf_iommu_group) {
        g_assert_cmpint(g_mkdir_with_parents(vf_group_target, 0700), ==, 0);
        g_assert_cmpint(symlink(vf_group_target, vf_iommu_group), ==, 0);
    }
    g_free(vf_group_target);
    g_free(pf_group_target);
    g_free(vf_iommu_group);
    g_free(host_driver);
    g_free(driver);
    g_free(pci_device);
    g_free(vf_link);
    g_free(pf_iommu_group);
    g_free(pf_device);
}

static gchar *
sriov_fixture_pci_attr(SriovExecFixture *fixture, const gchar *pci_addr,
                       const gchar *attribute)
{
    return g_build_filename(fixture->pci_root, pci_addr, attribute, NULL);
}

static gchar *
sriov_fixture_driver_attr(SriovExecFixture *fixture, const gchar *driver,
                          const gchar *attribute)
{
    return g_build_filename(fixture->drivers_root, driver, attribute, NULL);
}

static void
sriov_fixture_add_driver_controls(SriovExecFixture *fixture,
                                  const gchar *pci_addr)
{
    gchar *host_driver = g_build_filename(fixture->drivers_root, "ixgbevf", NULL);
    gchar *vfio_driver = g_build_filename(fixture->drivers_root, "vfio-pci", NULL);
    g_assert_cmpint(g_mkdir_with_parents(host_driver, 0700), ==, 0);
    g_assert_cmpint(g_mkdir_with_parents(vfio_driver, 0700), ==, 0);

    gchar *override = sriov_fixture_pci_attr(
        fixture, pci_addr, "driver_override");
    gchar *host_unbind = sriov_fixture_driver_attr(fixture, "ixgbevf", "unbind");
    gchar *host_bind = sriov_fixture_driver_attr(fixture, "ixgbevf", "bind");
    gchar *vfio_unbind = sriov_fixture_driver_attr(fixture, "vfio-pci", "unbind");
    gchar *vfio_bind = sriov_fixture_driver_attr(fixture, "vfio-pci", "bind");
    g_assert_true(g_file_set_contents(override, "", -1, NULL));
    g_assert_true(g_file_set_contents(host_unbind, "", -1, NULL));
    g_assert_true(g_file_set_contents(host_bind, "", -1, NULL));
    g_assert_true(g_file_set_contents(vfio_unbind, "", -1, NULL));
    g_assert_true(g_file_set_contents(vfio_bind, "", -1, NULL));
    g_free(vfio_bind);
    g_free(vfio_unbind);
    g_free(host_bind);
    g_free(host_unbind);
    g_free(override);
    g_free(vfio_driver);
    g_free(host_driver);
}

static void
sriov_assert_file_value(const gchar *path, const gchar *expected)
{
    gchar *contents = NULL;
    g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));
    g_assert_cmpstr(g_strstrip(contents), ==, expected);
    g_free(contents);
}

                                              

static void test_sriov_status_structure(void) {
    JsonObject *obj = pcv_sriov_status();
    g_assert_nonnull(obj);
    g_assert_true(json_object_has_member(obj, "available"));
    g_assert_true(json_object_has_member(obj, "physical_functions"));
    JsonArray *pfs = json_object_get_array_member(obj, "physical_functions");
    g_assert_nonnull(pfs);
    json_object_unref(obj);
}

static void test_sriov_list_empty(void) {
    JsonArray *arr = pcv_sriov_list(NULL);
    g_assert_nonnull(arr);
                           
    json_array_unref(arr);
}

static void test_sriov_list_nonexist_pf(void) {
    JsonArray *arr = pcv_sriov_list("nonexist99");
    g_assert_nonnull(arr);
    g_assert_cmpuint(json_array_get_length(arr), ==, 0);
    json_array_unref(arr);
}

                 

static void test_sriov_disable_idempotent(void) {
                                    
    gboolean ok = pcv_sriov_disable("nonexist99", NULL);
                                            
    g_assert_true(ok);
}

                                                                    
                                                                     
static void test_sriov_disable_write_failure_propagates(void) {
    if (!g_file_test("/dev/full", G_FILE_TEST_EXISTS)) { g_test_skip("no /dev/full"); return; }
    gchar *root = g_dir_make_tmp("pcv-sriov-XXXXXX", NULL); g_assert_nonnull(root);
    gchar *devdir = g_build_filename(root, "testpf0", "device", NULL);
    g_assert_cmpint(g_mkdir_with_parents(devdir, 0700), ==, 0);
    gchar *numvfs = g_build_filename(devdir, "sriov_numvfs", NULL);
    g_assert_cmpint(symlink("/dev/full", numvfs), ==, 0);
    pcv_sriov_set_paths_for_test(root, NULL, NULL);
    GError *err = NULL;
    gboolean ok = pcv_sriov_disable("testpf0", &err);
    pcv_sriov_set_paths_for_test(NULL, NULL, NULL);
    g_assert_false(ok);                                 
    g_assert_nonnull(err); g_clear_error(&err);
    g_unlink(numvfs); g_rmdir(devdir);
    gchar *pfdir = g_build_filename(root, "testpf0", NULL); g_rmdir(pfdir); g_rmdir(root);
    g_free(numvfs); g_free(devdir); g_free(pfdir); g_free(root);
}

                                     

static void test_sriov_vf_pci_null(void) {
    gchar *pci = pcv_sriov_vf_pci_addr("nonexist99", 0);
    g_assert_null(pci);
}

                                         
  
                                               
                                                       
   

static void test_sriov_disable_reject_injection(void) {
    GError *err = NULL;
                                                           
    gboolean ok = pcv_sriov_disable("x; touch /tmp/pwn", &err);
    g_assert_false(ok);
    g_assert_nonnull(err);                               
    g_clear_error(&err);
}

static void test_sriov_set_reject_bad_mac(void) {
    GError *err = NULL;
                                                            
    gboolean ok = pcv_sriov_set("eth0", 0, "52:54:00 vlan 4095", -1, -1, &err);
    g_assert_false(ok);
    g_assert_nonnull(err);
    g_clear_error(&err);
}

static void test_sriov_detach_reject_bad_vm(void) {
    GError *err = NULL;
                                                                          
    gboolean ok = pcv_sriov_detach_vm("vm; reboot", "0000:01:00.0", &err);
    g_assert_false(ok);
    g_assert_nonnull(err);
    g_clear_error(&err);
}

                           
                                                                    
                                                                            
                                                       
  
                       
                                                          
                                                     
static void
test_sriov_attach_without_iommu_fails_before_driver_change(void)
{
    static const struct {
        const gchar *name;
        gboolean with_pf_iommu_group;
        gboolean with_vf_iommu_group;
    } cases[] = {
        { "pf-group-missing", FALSE, TRUE },
        { "vf-group-missing", TRUE, FALSE },
    };

    for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
        g_test_message("attach preflight fixture: %s", cases[i].name);
        SriovExecFixture *fixture = sriov_exec_fixture_new();
        sriov_fixture_add_vf(fixture, "pf0", 0, "ffff:ff:1f.7",
                             cases[i].with_pf_iommu_group,
                             cases[i].with_vf_iommu_group);
        sriov_fixture_set_virsh(fixture, 0, "");
        gchar *pci_device = g_build_filename(
            fixture->pci_root, "ffff:ff:1f.7", NULL);
        gchar *driver = g_build_filename(pci_device, "driver", NULL);
        gchar *override = g_build_filename(pci_device, "driver_override", NULL);
        gchar *pf_group = g_build_filename(
            fixture->net_root, "pf0", "device", "iommu_group", NULL);
        gchar *vf_group = g_build_filename(pci_device, "iommu_group", NULL);
        gchar *sentinel = g_build_filename(
            pci_device, "host-driver-intact", NULL);
        g_assert_true(g_file_set_contents(sentinel, "intact", -1, NULL));
        g_assert_cmpint(g_file_test(pf_group, G_FILE_TEST_EXISTS), ==,
                        cases[i].with_pf_iommu_group);
        g_assert_cmpint(g_file_test(vf_group, G_FILE_TEST_EXISTS), ==,
                        cases[i].with_vf_iommu_group);

        GError *error = NULL;
        gboolean ok = pcv_sriov_attach_vm("vm-safe", "pf0", 0, &error);

        g_assert_false(ok);
        g_assert_nonnull(error);
        g_assert_nonnull(g_strstr_len(error->message, -1, "IOMMU"));
        g_assert_true(g_file_test(driver, G_FILE_TEST_IS_SYMLINK));
        g_assert_false(g_file_test(override, G_FILE_TEST_EXISTS));
        g_assert_true(g_file_test(sentinel, G_FILE_TEST_EXISTS));
        g_assert_false(g_file_test(fixture->log_path, G_FILE_TEST_EXISTS));
        g_clear_error(&error);
        g_free(sentinel);
        g_free(vf_group);
        g_free(pf_group);
        g_free(override);
        g_free(driver);
        g_free(pci_device);
        sriov_exec_fixture_free(fixture);
    }
}

                                                                    
                                                            
                                                   
static void
test_sriov_attach_writes_seamed_sysfs_controls(void)
{
    const gchar *pci = "ffff:ff:1f.7";
    SriovExecFixture *fixture = sriov_exec_fixture_new();
    sriov_fixture_add_vf(fixture, "pf0", 0, pci, TRUE, TRUE);
    sriov_fixture_add_driver_controls(fixture, pci);
    sriov_fixture_set_virsh(fixture, 0, "");

    GError *error = NULL;
    g_assert_true(pcv_sriov_attach_vm("vm-safe", "pf0", 0, &error));
    g_assert_no_error(error);

    gchar *host_unbind = sriov_fixture_driver_attr(fixture, "ixgbevf", "unbind");
    gchar *host_bind = sriov_fixture_driver_attr(fixture, "ixgbevf", "bind");
    gchar *override = sriov_fixture_pci_attr(fixture, pci, "driver_override");
    gchar *vfio_bind = sriov_fixture_driver_attr(fixture, "vfio-pci", "bind");
    gchar *vfio_unbind = sriov_fixture_driver_attr(fixture, "vfio-pci", "unbind");
    sriov_assert_file_value(host_unbind, pci);
    sriov_assert_file_value(override, "vfio-pci");
    sriov_assert_file_value(vfio_bind, pci);
    sriov_assert_file_value(vfio_unbind, "");
    sriov_assert_file_value(host_bind, "");

    gchar *argv_text = NULL;
    g_assert_true(g_file_get_contents(fixture->log_path, &argv_text, NULL, NULL));
    g_assert_true(g_str_has_prefix(argv_text, "attach-device vm-safe "));
    g_assert_nonnull(g_strstr_len(argv_text, -1, "pcv-sriov-attach-"));
    g_assert_true(g_str_has_suffix(g_strstrip(argv_text), "--live"));
    g_free(argv_text);
    g_free(vfio_unbind);
    g_free(vfio_bind);
    g_free(override);
    g_free(host_bind);
    g_free(host_unbind);
    sriov_exec_fixture_free(fixture);
}

                                                                  
                                                                    
static void
test_sriov_attach_bind_failure_rolls_back_original_driver(void)
{
    if (!g_file_test("/dev/full", G_FILE_TEST_EXISTS)) {
        g_test_skip("no /dev/full");
        return;
    }

    const gchar *pci = "ffff:ff:1f.7";
    SriovExecFixture *fixture = sriov_exec_fixture_new();
    sriov_fixture_add_vf(fixture, "pf0", 0, pci, TRUE, TRUE);
    sriov_fixture_add_driver_controls(fixture, pci);
    sriov_fixture_set_virsh(fixture, 0, "must not run");
    gchar *vfio_bind = sriov_fixture_driver_attr(fixture, "vfio-pci", "bind");
    g_assert_cmpint(g_unlink(vfio_bind), ==, 0);
    g_assert_cmpint(symlink("/dev/full", vfio_bind), ==, 0);

    GError *error = NULL;
    g_assert_false(pcv_sriov_attach_vm("vm-safe", "pf0", 0, &error));
    g_assert_nonnull(error);
    g_assert_nonnull(g_strstr_len(error->message, -1, "vfio-pci/bind"));
    g_assert_false(g_file_test(fixture->log_path, G_FILE_TEST_EXISTS));

    gchar *host_unbind = sriov_fixture_driver_attr(fixture, "ixgbevf", "unbind");
    gchar *host_bind = sriov_fixture_driver_attr(fixture, "ixgbevf", "bind");
    gchar *override = sriov_fixture_pci_attr(fixture, pci, "driver_override");
    gchar *vfio_unbind = sriov_fixture_driver_attr(fixture, "vfio-pci", "unbind");
    sriov_assert_file_value(host_unbind, pci);
    sriov_assert_file_value(vfio_unbind, pci);
    sriov_assert_file_value(override, "");
    sriov_assert_file_value(host_bind, pci);

    g_free(vfio_unbind);
    g_free(override);
    g_free(host_bind);
    g_free(host_unbind);
    g_free(vfio_bind);
    g_clear_error(&error);
    sriov_exec_fixture_free(fixture);
}

                                                          
                                                             
static void
test_sriov_attach_virsh_failure_rolls_back_original_driver(void)
{
    const gchar *pci = "ffff:ff:1f.7";
    SriovExecFixture *fixture = sriov_exec_fixture_new();
    sriov_fixture_add_vf(fixture, "pf0", 0, pci, TRUE, TRUE);
    sriov_fixture_add_driver_controls(fixture, pci);
    sriov_fixture_set_virsh(fixture, 17, "attach rejected by libvirt");

    GError *error = NULL;
    g_assert_false(pcv_sriov_attach_vm("vm-safe", "pf0", 0, &error));
    g_assert_nonnull(error);

    gchar *host_unbind = sriov_fixture_driver_attr(fixture, "ixgbevf", "unbind");
    gchar *host_bind = sriov_fixture_driver_attr(fixture, "ixgbevf", "bind");
    gchar *override = sriov_fixture_pci_attr(fixture, pci, "driver_override");
    gchar *vfio_bind = sriov_fixture_driver_attr(fixture, "vfio-pci", "bind");
    gchar *vfio_unbind = sriov_fixture_driver_attr(fixture, "vfio-pci", "unbind");
    sriov_assert_file_value(host_unbind, pci);
    sriov_assert_file_value(vfio_bind, pci);
    sriov_assert_file_value(vfio_unbind, pci);
    sriov_assert_file_value(override, "");
    sriov_assert_file_value(host_bind, pci);

    gchar *argv_text = NULL;
    g_assert_true(g_file_get_contents(fixture->log_path, &argv_text, NULL, NULL));
    g_assert_true(g_str_has_prefix(argv_text, "attach-device vm-safe "));
    g_assert_true(g_str_has_suffix(g_strstrip(argv_text), "--live"));
    g_free(argv_text);
    g_free(vfio_unbind);
    g_free(vfio_bind);
    g_free(override);
    g_free(host_bind);
    g_free(host_unbind);
    g_clear_error(&error);
    sriov_exec_fixture_free(fixture);
}

                                                            
                                                               
                                                                
static void
test_sriov_attach_detach_are_serialized(void)
{
    const gchar *pci = "ffff:ff:1f.7";
    SriovExecFixture *fixture = sriov_exec_fixture_new();
    sriov_fixture_add_vf(fixture, "pf0", 0, pci, TRUE, TRUE);
    sriov_fixture_add_driver_controls(fixture, pci);
    gchar *attach_ready = g_build_filename(fixture->root, "attach-ready", NULL);
    gchar *attach_release = g_build_filename(fixture->root, "attach-release", NULL);
    gchar *detach_invoked = g_build_filename(fixture->root, "detach-invoked", NULL);
    sriov_fixture_set_blocking_attach_virsh(
        fixture, attach_ready, attach_release, detach_invoked);

    SriovConcurrentCall attach_call;
    SriovConcurrentCall detach_call;
    sriov_concurrent_call_init(&attach_call, TRUE);
    sriov_concurrent_call_init(&detach_call, FALSE);
    GThread *attach_thread = g_thread_new(
        "sriov-attach", sriov_concurrent_call_thread, &attach_call);

    gboolean ready = FALSE;
    for (guint attempt = 0; attempt < 500; attempt++) {
        if (g_file_test(attach_ready, G_FILE_TEST_EXISTS)) {
            ready = TRUE;
            break;
        }
        g_usleep(10000);
    }
    g_assert_true(ready);

    GThread *detach_thread = g_thread_new(
        "sriov-detach", sriov_concurrent_call_thread, &detach_call);
    sriov_concurrent_call_wait_started(&detach_call);
    g_usleep(150000);
    g_assert_false(g_file_test(detach_invoked, G_FILE_TEST_EXISTS));

    g_assert_true(g_file_set_contents(attach_release, "go", -1, NULL));
    g_thread_join(attach_thread);
    g_thread_join(detach_thread);
    g_assert_true(attach_call.ok);
    g_assert_no_error(attach_call.error);
    g_assert_true(detach_call.ok);
    g_assert_no_error(detach_call.error);
    g_assert_true(g_file_test(detach_invoked, G_FILE_TEST_EXISTS));

    gchar *argv_text = NULL;
    g_assert_true(g_file_get_contents(fixture->log_path, &argv_text, NULL, NULL));
    gchar **lines = g_strsplit(g_strstrip(argv_text), "\n", -1);
    g_assert_cmpstr(lines[0], !=, NULL);
    g_assert_true(g_str_has_prefix(lines[0], "attach-device vm-safe "));
    g_assert_cmpstr(lines[1], !=, NULL);
    g_assert_cmpstr(lines[1], ==, "dumpxml vm-safe");
    g_assert_cmpstr(lines[2], !=, NULL);
    g_assert_true(g_str_has_prefix(lines[2], "detach-device vm-safe "));
    g_assert_null(lines[3]);

    g_strfreev(lines);
    g_free(argv_text);
    sriov_concurrent_call_clear(&detach_call);
    sriov_concurrent_call_clear(&attach_call);
    g_free(detach_invoked);
    g_free(attach_release);
    g_free(attach_ready);
    sriov_exec_fixture_free(fixture);
}

                           
                                                                        
                                                              
                                                               
                                                        
  
                       
                                                       
                                              
static void
test_sriov_detach_exact_idempotency_and_failure_classes(void)
{
    static const struct {
        const gchar *name;
        SriovDetachVirshSpec spec;
        gboolean expected_ok;
        gboolean expected_detach;
        gboolean expected_post_query;
    } cases[] = {
        {
            "success",
            {TRUE, TRUE, FALSE, 0, "", 0, ""},
            TRUE, TRUE, FALSE,
        },
        {
            "already-absent-structured",
            {FALSE, FALSE, FALSE, 0, "", 77,
             "error: host pci device 0000:03:10.0 not found"},
            TRUE, FALSE, FALSE,
        },
        {
            "removed-after-host-pci-diagnostic",
            {TRUE, TRUE, FALSE, 0, "", 1,
             "error: host pci device 0000:03:10.0 not found"},
            TRUE, TRUE, TRUE,
        },
        {
            "removed-after-localized-diagnostic",
            {TRUE, TRUE, FALSE, 0, "", 1,
             "erreur: périphérique PCI hôte introuvable"},
            TRUE, TRUE, TRUE,
        },
        {
            "not-found-words-but-device-remains",
            {TRUE, FALSE, FALSE, 0, "", 1,
             "error: host pci device 0000:03:10.0 not found"},
            FALSE, TRUE, TRUE,
        },
        {
            "missing-vm",
            {TRUE, FALSE, FALSE, 1,
             "error: failed to get domain vm-safe", 0, ""},
            FALSE, FALSE, FALSE,
        },
        {
            "dumpxml-not-found-words",
            {TRUE, FALSE, FALSE, 1,
             "error: host pci device 0000:03:10.0 not found", 0, ""},
            FALSE, FALSE, FALSE,
        },
        {
            "permission",
            {TRUE, FALSE, FALSE, 1,
             "error: authentication failed: permission denied", 0, ""},
            FALSE, FALSE, FALSE,
        },
        {
            "libvirt-detach-failure",
            {TRUE, FALSE, FALSE, 0, "", 1,
             "error: internal error: monitor closed"},
            FALSE, TRUE, TRUE,
        },
        {
            "malformed-domain-xml",
            {TRUE, FALSE, TRUE, 0, "", 0, ""},
            FALSE, FALSE, FALSE,
        },
    };

    for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
        g_test_message("detach fixture: %s", cases[i].name);
        SriovExecFixture *fixture = sriov_exec_fixture_new();
        sriov_fixture_set_detach_virsh(fixture, &cases[i].spec);
        GError *error = NULL;
        gboolean ok = pcv_sriov_detach_vm("vm-safe", "0000:03:10.0", &error);
        g_assert_cmpint(ok, ==, cases[i].expected_ok);
        if (cases[i].expected_ok)
            g_assert_no_error(error);
        else
            g_assert_nonnull(error);
        gchar *argv_text = NULL;
        g_assert_true(g_file_get_contents(fixture->log_path, &argv_text, NULL, NULL));
        gchar **lines = g_strsplit(g_strchomp(argv_text), "\n", -1);
        g_assert_cmpstr(lines[0], ==, "dumpxml vm-safe");
        if (cases[i].expected_detach) {
            g_assert_cmpstr(lines[1], !=, NULL);
            g_assert_true(g_str_has_prefix(lines[1], "detach-device vm-safe "));
            g_assert_nonnull(g_strstr_len(lines[1], -1,
                                          "pcv-sriov-detach-"));
            g_assert_true(g_str_has_suffix(lines[1], "--live"));
            if (cases[i].expected_post_query) {
                g_assert_cmpstr(lines[2], ==, "dumpxml vm-safe");
                g_assert_null(lines[3]);
            } else {
                g_assert_null(lines[2]);
            }
        } else {
            g_assert_null(lines[1]);
        }
        g_strfreev(lines);
        g_free(argv_text);
        g_clear_error(&error);
        sriov_exec_fixture_free(fixture);
    }

    SriovExecFixture *missing = sriov_exec_fixture_new();
    remove_tree(missing->virsh_path);
    GError *error = NULL;
    g_assert_false(pcv_sriov_detach_vm("vm-safe", "0000:03:10.0", &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    sriov_exec_fixture_free(missing);
}

              

void test_sriov_register(void) {
    g_test_add_func("/sriov/status/structure",       test_sriov_status_structure);
    g_test_add_func("/sriov/list/empty",             test_sriov_list_empty);
    g_test_add_func("/sriov/list/nonexist_pf",       test_sriov_list_nonexist_pf);
    g_test_add_func("/sriov/disable/idempotent",     test_sriov_disable_idempotent);
    g_test_add_func("/sriov/disable/write_failure_propagates", test_sriov_disable_write_failure_propagates);
    g_test_add_func("/sriov/vf_pci/null",            test_sriov_vf_pci_null);
    g_test_add_func("/sriov/disable/reject_injection", test_sriov_disable_reject_injection);
    g_test_add_func("/sriov/set/reject_bad_mac",       test_sriov_set_reject_bad_mac);
    g_test_add_func("/sriov/detach/reject_bad_vm",     test_sriov_detach_reject_bad_vm);
    g_test_add_func("/sriov/attach/no_iommu_before_driver_change",
                    test_sriov_attach_without_iommu_fails_before_driver_change);
    g_test_add_func("/sriov/attach/seamed_sysfs_controls",
                    test_sriov_attach_writes_seamed_sysfs_controls);
    g_test_add_func("/sriov/attach/bind_failure_rolls_back",
                    test_sriov_attach_bind_failure_rolls_back_original_driver);
    g_test_add_func("/sriov/attach/virsh_failure_rolls_back",
                    test_sriov_attach_virsh_failure_rolls_back_original_driver);
    g_test_add_func("/sriov/attach_detach/serialized",
                    test_sriov_attach_detach_are_serialized);
    g_test_add_func("/sriov/detach/exact_idempotency_and_failures",
                    test_sriov_detach_exact_idempotency_and_failure_classes);
}
