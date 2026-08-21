                                                                                          
                                                                                          
                                                                      
                                                              
                                
                    
  
                                                                       
  
                           
                                                                        
                                                                                 
                                                           
                                                                        
                                                          
                                                                
  
                       
                                                     
                                                             
  
                                  
  
                                                        
   

#include <glib.h>
#include <glib/gstdio.h>                                               
#include <json-glib/json-glib.h>
#include <sys/stat.h>
#include <unistd.h>

                             
extern gboolean pcv_dpdk_is_available(void);
extern JsonObject *pcv_dpdk_status(void);
extern JsonObject *pcv_dpdk_hugepage_info(void);
extern JsonArray *pcv_dpdk_list(void);
extern gboolean pcv_dpdk_bind(const gchar *pci_addr, const gchar *driver,
                              GError **error);
extern gboolean pcv_dpdk_unbind(const gchar *pci_addr, GError **error);
extern void pcv_dpdk_test_set_unbind_paths(const gchar *pci_sysfs_root,
                                           const gchar *devbind_path);
extern gboolean pcv_dpdk_bridge_delete(const gchar *name, GError **error);
extern gboolean pcv_dpdk_bridge_create(const gchar *name, const gchar *dpdk_port,
                                       GError **error);

                          
extern gboolean pcv_validate_pci_addr(const gchar *addr);

                        
extern gboolean pcv_dpdk_nic_is_protected(const gchar *pci_addr, gchar **reason);
extern gboolean pcv_dpdk_route_is_default_dev(const gchar *netdev, const gchar *proc_base);

typedef struct {
    gchar *root;
    gchar *pci_root;
    gchar *devices_root;
    gchar *device_path;
    gchar *bin_dir;
    gchar *devbind_path;
    gchar *log_path;
    gchar *driver_path;
    gchar *override_path;
    gchar *probe_path;
    gchar *release_path;
} DpdkUnbindFixture;

static void
dpdk_remove_tree(const gchar *path)
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
        dpdk_remove_tree(child);
        g_free(child);
    }
    g_dir_close(dir);
    g_rmdir(path);
}

static DpdkUnbindFixture *
dpdk_unbind_fixture_new(gboolean device_exists, const gchar *driver_name)
{
    DpdkUnbindFixture *fixture = g_new0(DpdkUnbindFixture, 1);
    fixture->root = g_dir_make_tmp("pcv-dpdk-unbind-XXXXXX", NULL);
    g_assert_nonnull(fixture->root);
    fixture->pci_root = g_build_filename(fixture->root, "pci", NULL);
    fixture->devices_root = g_build_filename(fixture->pci_root, "devices", NULL);
    fixture->device_path = g_build_filename(
        fixture->devices_root, "0000:05:00.0", NULL);
    fixture->bin_dir = g_build_filename(fixture->root, "bin", NULL);
    fixture->devbind_path = g_build_filename(fixture->bin_dir, "dpdk-devbind.py", NULL);
    fixture->log_path = g_build_filename(fixture->root, "devbind.argv", NULL);
    fixture->driver_path = g_build_filename(fixture->device_path, "driver", NULL);
    fixture->override_path = g_build_filename(fixture->device_path, "driver_override", NULL);
    fixture->probe_path = g_build_filename(fixture->pci_root, "drivers_probe", NULL);
    fixture->release_path = g_build_filename(fixture->root, "release", NULL);
    g_assert_cmpint(g_mkdir_with_parents(fixture->devices_root, 0700), ==, 0);
    g_assert_cmpint(g_mkdir_with_parents(fixture->bin_dir, 0700), ==, 0);
    g_assert_true(g_file_set_contents(fixture->probe_path, "", -1, NULL));
    if (device_exists) {
        g_assert_cmpint(g_mkdir_with_parents(fixture->device_path, 0700), ==, 0);
        g_assert_true(g_file_set_contents(fixture->override_path, "vfio-pci", -1, NULL));
        if (driver_name) {
            gchar *target = g_strdup_printf("/sys/bus/pci/drivers/%s", driver_name);
            g_assert_cmpint(symlink(target, fixture->driver_path), ==, 0);
            g_free(target);
        }
    }
    pcv_dpdk_test_set_unbind_paths(fixture->pci_root, fixture->devbind_path);
    return fixture;
}

static void
dpdk_unbind_fixture_free(DpdkUnbindFixture *fixture)
{
    if (!fixture)
        return;
    pcv_dpdk_test_set_unbind_paths(NULL, NULL);
    dpdk_remove_tree(fixture->root);
    g_free(fixture->release_path);
    g_free(fixture->probe_path);
    g_free(fixture->override_path);
    g_free(fixture->driver_path);
    g_free(fixture->log_path);
    g_free(fixture->devbind_path);
    g_free(fixture->bin_dir);
    g_free(fixture->device_path);
    g_free(fixture->devices_root);
    g_free(fixture->pci_root);
    g_free(fixture->root);
    g_free(fixture);
}

static void
dpdk_fixture_set_blocking_devbind(DpdkUnbindFixture *fixture,
                                  const gchar *next_driver)
{
    gchar *script = g_strdup_printf(
        "#!/bin/sh\n"
        "printf '%%s\\n' \"$*\" >> '%s'\n"
        "attempt=0\n"
        "while [ ! -e '%s' ]; do\n"
        "  sleep 0.01\n"
        "  attempt=$((attempt + 1))\n"
        "  [ \"$attempt\" -lt 500 ] || exit 75\n"
        "done\n"
        "rm -f '%s'\n"
        "ln -s '/sys/bus/pci/drivers/%s' '%s'\n"
        "exit 0\n",
        fixture->log_path, fixture->release_path, fixture->driver_path,
        next_driver, fixture->driver_path);
    g_assert_true(g_file_set_contents(fixture->devbind_path, script, -1, NULL));
    g_assert_cmpint(g_chmod(fixture->devbind_path, 0700), ==, 0);
    g_free(script);
}

static guint
dpdk_fixture_log_line_count(const gchar *path)
{
    gchar *contents = NULL;
    if (!g_file_get_contents(path, &contents, NULL, NULL))
        return 0;
    guint count = 0;
    gchar **lines = g_strsplit(contents, "\n", -1);
    for (guint i = 0; lines[i]; i++) {
        if (*lines[i])
            count++;
    }
    g_strfreev(lines);
    g_free(contents);
    return count;
}

typedef struct {
    GMutex mu;
    GCond cond;
    guint ready;
    gboolean start;
    gboolean result[2];
    GError *error[2];
} DpdkUnbindRace;

typedef struct {
    DpdkUnbindRace *race;
    guint index;
} DpdkUnbindRaceWorker;

static gpointer
dpdk_unbind_race_worker(gpointer data)
{
    DpdkUnbindRaceWorker *worker = data;
    DpdkUnbindRace *race = worker->race;
    g_mutex_lock(&race->mu);
    race->ready++;
    g_cond_broadcast(&race->cond);
    while (!race->start)
        g_cond_wait(&race->cond, &race->mu);
    g_mutex_unlock(&race->mu);

    race->result[worker->index] = pcv_dpdk_unbind(
        "0000:05:00.0", &race->error[worker->index]);
    return NULL;
}

static void
dpdk_fixture_set_devbind(DpdkUnbindFixture *fixture,
                         gint exit_status,
                         const gchar *stderr_text,
                         const gchar *next_driver)
{
    gchar *transition = next_driver
        ? g_strdup_printf(
            "rm -f '%s'\nln -s '/sys/bus/pci/drivers/%s' '%s'\n",
            fixture->driver_path, next_driver, fixture->driver_path)
        : g_strdup("");
    gchar *script = g_strdup_printf(
        "#!/bin/sh\n"
        "printf '%%s\\n' \"$*\" > '%s'\n"
        "printf '%%s' '%s' >&2\n"
        "%s"
        "exit %d\n",
        fixture->log_path, stderr_text ? stderr_text : "", transition, exit_status);
    g_assert_true(g_file_set_contents(fixture->devbind_path, script, -1, NULL));
    g_assert_cmpint(g_chmod(fixture->devbind_path, 0700), ==, 0);
    g_free(script);
    g_free(transition);
}

                                                 

static void test_dpdk_status_structure(void) {
    JsonObject *obj = pcv_dpdk_status();
    g_assert_nonnull(obj);
    g_assert_true(json_object_has_member(obj, "available"));
    g_assert_true(json_object_has_member(obj, "vdev_count"));
    json_object_unref(obj);
}

static void test_dpdk_hugepage_structure(void) {
    JsonObject *obj = pcv_dpdk_hugepage_info();
    g_assert_nonnull(obj);
    g_assert_true(json_object_has_member(obj, "total_mb"));
    g_assert_true(json_object_has_member(obj, "free_mb"));
    g_assert_true(json_object_has_member(obj, "hugepage_1g_total"));
    g_assert_true(json_object_has_member(obj, "hugepage_2m_total"));
    json_object_unref(obj);
}

static void test_dpdk_list_empty(void) {
    JsonArray *arr = pcv_dpdk_list();
    g_assert_nonnull(arr);
                         
    json_array_unref(arr);
}

                 

static void test_dpdk_unbind_idempotent(void) {
                                 
    g_assert_true(pcv_dpdk_unbind("0000:ff:1f.7", NULL));
}

                           
                                                                   
                                                                   
                                                                        
                                                          
  
                       
                                                        
                                       
static void
test_dpdk_unbind_actual_state_and_exact_argv(void)
{
    DpdkUnbindFixture *absent = dpdk_unbind_fixture_new(FALSE, NULL);
    dpdk_fixture_set_devbind(absent, 1, "must not run", NULL);
    GError *error = NULL;
    g_assert_true(pcv_dpdk_unbind("0000:05:00.0", &error));
    g_assert_no_error(error);
    g_assert_false(g_file_test(absent->log_path, G_FILE_TEST_EXISTS));
    dpdk_unbind_fixture_free(absent);

    DpdkUnbindFixture *restored = dpdk_unbind_fixture_new(TRUE, "ixgbe");
    dpdk_fixture_set_devbind(restored, 1, "must not run", NULL);
    g_assert_true(pcv_dpdk_unbind("0000:05:00.0", &error));
    g_assert_no_error(error);
    g_assert_false(g_file_test(restored->log_path, G_FILE_TEST_EXISTS));
    gchar *restored_override = NULL;
    g_assert_true(g_file_get_contents(
        restored->override_path, &restored_override, NULL, NULL));
    g_assert_cmpstr(g_strstrip(restored_override), ==, "");
    gchar *restored_probe = NULL;
    g_assert_true(g_file_get_contents(
        restored->probe_path, &restored_probe, NULL, NULL));
    g_assert_cmpstr(g_strstrip(restored_probe), ==, "");
    gchar *restored_driver = g_file_read_link(restored->driver_path, NULL);
    g_assert_nonnull(restored_driver);
    g_assert_true(g_str_has_suffix(restored_driver, "/ixgbe"));
    g_free(restored_driver);
    g_free(restored_probe);
    g_free(restored_override);
    dpdk_unbind_fixture_free(restored);

    DpdkUnbindFixture *transition = dpdk_unbind_fixture_new(TRUE, "vfio-pci");
    dpdk_fixture_set_devbind(transition, 0, "", "ixgbe");
    g_assert_true(pcv_dpdk_unbind("0000:05:00.0", &error));
    g_assert_no_error(error);
    gchar *argv_text = NULL;
    g_assert_true(g_file_get_contents(transition->log_path, &argv_text, NULL, NULL));
    g_assert_cmpstr(g_strstrip(argv_text), ==, "--unbind 0000:05:00.0");
    gchar *driver_target = g_file_read_link(transition->driver_path, NULL);
    g_assert_nonnull(driver_target);
    g_assert_true(g_str_has_suffix(driver_target, "/ixgbe"));
    gchar *override_text = NULL;
    g_assert_true(g_file_get_contents(
        transition->override_path, &override_text, NULL, NULL));
    g_assert_cmpstr(g_strstrip(override_text), ==, "");
    gchar *probe_text = NULL;
    g_assert_true(g_file_get_contents(transition->probe_path, &probe_text, NULL, NULL));
    g_assert_cmpstr(g_strstrip(probe_text), ==, "0000:05:00.0");
    g_free(probe_text);
    g_free(override_text);
    g_free(driver_target);
    g_free(argv_text);
    dpdk_unbind_fixture_free(transition);
}

                                                     
                                                               
                                                          
                                               
static void
test_dpdk_unbind_serializes_state_through_postcondition(void)
{
    DpdkUnbindFixture *fixture = dpdk_unbind_fixture_new(TRUE, "vfio-pci");
    dpdk_fixture_set_blocking_devbind(fixture, "ixgbe");

    DpdkUnbindRace race = {0};
    DpdkUnbindRaceWorker workers[] = {
        { .race = &race, .index = 0 },
        { .race = &race, .index = 1 },
    };
    g_mutex_init(&race.mu);
    g_cond_init(&race.cond);
    GThread *threads[] = {
        g_thread_new("dpdk-unbind-0", dpdk_unbind_race_worker, &workers[0]),
        g_thread_new("dpdk-unbind-1", dpdk_unbind_race_worker, &workers[1]),
    };

    g_mutex_lock(&race.mu);
    while (race.ready < G_N_ELEMENTS(workers))
        g_cond_wait(&race.cond, &race.mu);
    race.start = TRUE;
    g_cond_broadcast(&race.cond);
    g_mutex_unlock(&race.mu);

    guint before_release = 0;
    const gint64 deadline = g_get_monotonic_time() + (5 * G_TIME_SPAN_SECOND);
    while (g_get_monotonic_time() < deadline) {
        before_release = dpdk_fixture_log_line_count(fixture->log_path);
        if (before_release > 0)
            break;
        g_usleep(10 * 1000);
    }
    if (before_release > 0) {
        g_usleep(300 * 1000);
        before_release = dpdk_fixture_log_line_count(fixture->log_path);
    }

    gboolean release_ok = g_file_set_contents(fixture->release_path, "go", -1, NULL);
    for (guint i = 0; i < G_N_ELEMENTS(threads); i++)
        g_thread_join(threads[i]);

    g_assert_true(release_ok);
    g_assert_cmpuint(before_release, ==, 1);
    g_assert_cmpuint(dpdk_fixture_log_line_count(fixture->log_path), ==, 1);
    for (guint i = 0; i < G_N_ELEMENTS(workers); i++) {
        g_assert_true(race.result[i]);
        g_assert_no_error(race.error[i]);
        g_clear_error(&race.error[i]);
    }
    gchar *driver_target = g_file_read_link(fixture->driver_path, NULL);
    g_assert_nonnull(driver_target);
    g_assert_true(g_str_has_suffix(driver_target, "/ixgbe"));
    g_free(driver_target);
    g_cond_clear(&race.cond);
    g_mutex_clear(&race.mu);
    dpdk_unbind_fixture_free(fixture);
}

                           
                                                                                          
                                                                           
                                                                         
                       
  
                       
                                                          
static void
test_dpdk_unbind_failures_are_not_swallowed(void)
{
    DpdkUnbindFixture *failed = dpdk_unbind_fixture_new(TRUE, "vfio-pci");
    dpdk_fixture_set_devbind(failed, 17, "unbind denied", NULL);
    GError *error = NULL;
    g_assert_false(pcv_dpdk_unbind("0000:05:00.0", &error));
    g_assert_nonnull(error);
    gchar *argv_text = NULL;
    g_assert_true(g_file_get_contents(failed->log_path, &argv_text, NULL, NULL));
    g_assert_cmpstr(g_strstrip(argv_text), ==, "--unbind 0000:05:00.0");
    gchar *driver_target = g_file_read_link(failed->driver_path, NULL);
    g_assert_nonnull(driver_target);
    g_assert_true(g_str_has_suffix(driver_target, "/vfio-pci"));
    gchar *override_text = NULL;
    g_assert_true(g_file_get_contents(
        failed->override_path, &override_text, NULL, NULL));
    g_assert_cmpstr(g_strstrip(override_text), ==, "vfio-pci");
    gchar *probe_text = NULL;
    g_assert_true(g_file_get_contents(failed->probe_path, &probe_text, NULL, NULL));
    g_assert_cmpstr(g_strstrip(probe_text), ==, "");
    g_free(probe_text);
    g_free(override_text);
    g_free(driver_target);
    g_free(argv_text);
    g_clear_error(&error);
    dpdk_unbind_fixture_free(failed);

    DpdkUnbindFixture *unchanged = dpdk_unbind_fixture_new(TRUE, "vfio-pci");
    dpdk_fixture_set_devbind(unchanged, 0, "", NULL);
    g_assert_false(pcv_dpdk_unbind("0000:05:00.0", &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    dpdk_unbind_fixture_free(unchanged);

    DpdkUnbindFixture *missing = dpdk_unbind_fixture_new(TRUE, "vfio-pci");
    dpdk_remove_tree(missing->devbind_path);
    g_assert_false(pcv_dpdk_unbind("0000:05:00.0", &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    dpdk_unbind_fixture_free(missing);

    DpdkUnbindFixture *denied = dpdk_unbind_fixture_new(TRUE, "vfio-pci");
    dpdk_fixture_set_devbind(denied, 0, "", "ixgbe");
    g_assert_cmpint(g_chmod(denied->devbind_path, 0600), ==, 0);
    g_assert_false(pcv_dpdk_unbind("0000:05:00.0", &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    dpdk_unbind_fixture_free(denied);
}

static void test_dpdk_bridge_delete_idempotent(void) {
    g_assert_true(pcv_dpdk_bridge_delete("nonexist-dpdk-br", NULL));
}

                     

static void test_pci_addr_valid(void) {
    g_assert_true(pcv_validate_pci_addr("0000:01:00.0"));
    g_assert_true(pcv_validate_pci_addr("0000:3b:10.1"));
    g_assert_true(pcv_validate_pci_addr("ffff:ff:1f.7"));
}

static void test_pci_addr_invalid(void) {
    g_assert_false(pcv_validate_pci_addr(NULL));
    g_assert_false(pcv_validate_pci_addr(""));
    g_assert_false(pcv_validate_pci_addr("../../etc"));
    g_assert_false(pcv_validate_pci_addr("00:3b:00.0"));                                
    g_assert_false(pcv_validate_pci_addr("0000:3B:00.0"));                               
    g_assert_false(pcv_validate_pci_addr("01:00.0"));                    
    g_assert_false(pcv_validate_pci_addr("0000:01:00"));                      
    g_assert_false(pcv_validate_pci_addr("0000:01:20.0"));                    
    g_assert_false(pcv_validate_pci_addr("0000:01:00.8"));                       
    g_assert_false(pcv_validate_pci_addr("0000:01:00.0 ; rm -rf /"));          

                                                           
    GError *error = NULL;
    g_assert_false(pcv_dpdk_unbind("00:3b:00.0", &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
}

static void test_dpdk_bind_rejects_unrecoverable_driver(void) {
    GError *error = NULL;
    g_assert_false(pcv_dpdk_bind("0000:05:00.0", "custom_uio", &error));
    g_assert_nonnull(error);
    g_assert_nonnull(g_strstr_len(error->message, -1, "Unsupported DPDK driver"));
    g_clear_error(&error);
}

                                            

static void test_dpdk_bridge_create_reject_injection(void) {
    GError *err = NULL;
      
                                                     
                                                                    
                                                             
       
    gboolean ok = pcv_dpdk_bridge_create("br0", "x; touch /tmp/pcv_dpdk_pwn", &err);
    g_assert_false(ok);
    g_assert_nonnull(err);
    g_clear_error(&err);
                       
    g_assert_false(g_file_test("/tmp/pcv_dpdk_pwn", G_FILE_TEST_EXISTS));
}

                               

                                                                  
                                                                      
                                                       
                                                                     
                                                    
                                                       
                                               
                                                              
                                                       
                                                    
                               

                                                          
static void test_dpdk_nic_route_default(void) {
    gchar *base = g_dir_make_tmp("pcvdpdk_XXXXXX", NULL);
    g_assert_nonnull(base);
    gchar *pd = g_build_filename(base, "proc", "net", NULL);
    g_assert_cmpint(g_mkdir_with_parents(pd, 0700), ==, 0);
    gchar *route = g_build_filename(pd, "route", NULL);
                                                               
    g_assert_true(g_file_set_contents(route,
        "Iface\tDestination\tGateway\tFlags\n"
        "eth0\t00000000\t0102A8C0\t0003\n"
        "eth1\t0000A8C0\t00000000\t0001\n", -1, NULL));
    g_assert_true (pcv_dpdk_route_is_default_dev("eth0", base));
    g_assert_false(pcv_dpdk_route_is_default_dev("eth1", base));
    g_assert_false(pcv_dpdk_route_is_default_dev("ethX", base));
    g_assert_false(pcv_dpdk_route_is_default_dev(NULL,   base));
    g_unlink(route); g_rmdir(pd);
    gchar *pdir = g_build_filename(base, "proc", NULL);
    g_rmdir(pdir); g_rmdir(base);
    g_free(route); g_free(pd); g_free(pdir); g_free(base);
}

                                              
static void test_dpdk_nic_null_failsecure(void) {
    gchar *reason = NULL;
    g_assert_true(pcv_dpdk_nic_is_protected(NULL, &reason));
    g_free(reason); reason = NULL;
    g_assert_true(pcv_dpdk_nic_is_protected("", &reason));
    g_free(reason);
}

                                          
static void test_dpdk_nic_absent_netdir_passes(void) {
    gchar *reason = NULL;
                                                                 
    g_assert_false(pcv_dpdk_nic_is_protected("ffff:ff:1f.7", &reason));
    g_assert_null(reason);
    g_free(reason);
}

                                                                
static void test_dpdk_nic_malformed_failsecure(void) {
    gchar *reason = NULL;
    g_assert_true(pcv_dpdk_nic_is_protected("../../../etc", &reason));
    g_assert_nonnull(reason);                               
    g_free(reason); reason = NULL;
    g_assert_true(pcv_dpdk_nic_is_protected("not-a-bdf", &reason));
    g_free(reason);
}

              

void test_dpdk_register(void) {
    g_test_add_func("/dpdk/status/structure",          test_dpdk_status_structure);
    g_test_add_func("/dpdk/hugepage/structure",        test_dpdk_hugepage_structure);
    g_test_add_func("/dpdk/list/empty",                test_dpdk_list_empty);
    g_test_add_func("/dpdk/unbind/idempotent",         test_dpdk_unbind_idempotent);
    g_test_add_func("/dpdk/unbind/actual_state_and_exact_argv",
                    test_dpdk_unbind_actual_state_and_exact_argv);
    g_test_add_func("/dpdk/unbind/transaction_serialized",
                    test_dpdk_unbind_serializes_state_through_postcondition);
    g_test_add_func("/dpdk/unbind/failures_not_swallowed",
                    test_dpdk_unbind_failures_are_not_swallowed);
    g_test_add_func("/dpdk/bridge_delete/idempotent",  test_dpdk_bridge_delete_idempotent);
    g_test_add_func("/dpdk/pci_addr/valid",            test_pci_addr_valid);
    g_test_add_func("/dpdk/pci_addr/invalid",          test_pci_addr_invalid);
    g_test_add_func("/dpdk/bind/rejects_unrecoverable_driver",
                    test_dpdk_bind_rejects_unrecoverable_driver);
    g_test_add_func("/dpdk/bridge_create/reject_injection",
                    test_dpdk_bridge_create_reject_injection);
    g_test_add_func("/dpdk/nic_protected/route_default",  test_dpdk_nic_route_default);
    g_test_add_func("/dpdk/nic_protected/null_failsecure", test_dpdk_nic_null_failsecure);
    g_test_add_func("/dpdk/nic_protected/absent_netdir",  test_dpdk_nic_absent_netdir_passes);
    g_test_add_func("/dpdk/nic_protected/malformed",      test_dpdk_nic_malformed_failsecure);
}
