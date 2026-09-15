                                                                                          
                                                                                          
                                                                      
                                                              
                                
                    
  
                                                                       
  
                           
                                                                        
                                                                                 
                                                           
                                                                        
                                                          
                                                                
  
                       
                                                     
                                                             
  
                                  
  
                                                        
   

#include <glib.h>
#include <glib/gstdio.h>                                               
#include <json-glib/json-glib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include "../src/modules/network/dpdk_manager.h"

                             
extern gboolean pcv_dpdk_is_available(void);
extern JsonObject *pcv_dpdk_status(void);
extern JsonObject *pcv_dpdk_hugepage_info(void);
extern JsonArray *pcv_dpdk_list(void);
extern gboolean pcv_dpdk_bind(const gchar *pci_addr, const gchar *driver,
                              GError **error);
extern gboolean pcv_dpdk_unbind(const gchar *pci_addr, GError **error);
extern void pcv_dpdk_test_set_unbind_paths(const gchar *pci_sysfs_root,
                                           const gchar *devbind_path);
extern void pcv_dpdk_test_set_vswitchd_running(gint state);
typedef gboolean (*PcvDpdkTestOvsRunner)(const gchar * const *argv,
                                         gchar **stdout_out,
                                         gchar **stderr_out,
                                         GError **error,
                                         gpointer user_data);
extern void pcv_dpdk_test_set_ovs_runner(PcvDpdkTestOvsRunner runner,
                                         gpointer user_data,
                                         gint available_override);
extern gboolean pcv_dpdk_bridge_delete(const gchar *name, GError **error);
extern gboolean pcv_dpdk_bridge_create(const gchar *name, const gchar *dpdk_port,
                                       guint mtu, GError **error);
extern gboolean pcv_dpdk_vm_port_ensure(const gchar *bridge_name,
                                        const gchar *vm_name,
                                        GError **error);
extern gboolean pcv_dpdk_test_vhost_runtime_preflight_at(
    const gchar *runtime_dir, GError **error);
extern gboolean pcv_dpdk_vm_port_delete(const gchar *vm_name, GError **error);

                          
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

typedef struct {
    gboolean bridge_exists;
    gboolean foreign_bridge;
    gboolean bridge_netdev;
    gboolean physical_exists;
    gboolean foreign_physical;
    gboolean invalid_physical_pci;
    gboolean vhost_exists;
    gboolean foreign_vhost;
    gboolean foreign_port_exists;
    gboolean fail_physical_add;
    gboolean fail_mtu_wait;
    gboolean ignore_vhost_config;
    guint mtu;
    gchar *vhost_type;
    gchar *vhost_path;
    GString *commands;
} DpdkOvsFixture;

static gboolean
dpdk_argv_has(const gchar * const *argv, const gchar *token)
{
    for (guint i = 0; argv && argv[i]; i++) {
        if (g_strcmp0(argv[i], token) == 0)
            return TRUE;
    }
    return FALSE;
}

static const gchar *
dpdk_argv_after(const gchar * const *argv, const gchar *token)
{
    for (guint i = 0; argv && argv[i]; i++) {
        if (g_strcmp0(argv[i], token) == 0)
            return argv[i + 1];
    }
    return NULL;
}

static void
dpdk_fixture_set_out(gchar **stdout_out, const gchar *value)
{
    if (stdout_out)
        *stdout_out = g_strdup(value ? value : "");
}

static void
dpdk_fixture_record_vhost_config(DpdkOvsFixture *fixture,
                                 const gchar * const *argv)
{
    for (guint i = 0; argv && argv[i]; i++) {
        if (g_str_has_prefix(argv[i], "type=")) {
            g_free(fixture->vhost_type);
            fixture->vhost_type = g_strdup(argv[i] + strlen("type="));
        } else if (g_str_has_prefix(argv[i], "options:vhost-server-path=")) {
            g_free(fixture->vhost_path);
            fixture->vhost_path = g_strdup(
                argv[i] + strlen("options:vhost-server-path="));
        }
    }
}



static gboolean
dpdk_ovs_fixture_run(const gchar * const *argv, gchar **stdout_out,
                     gchar **stderr_out, GError **error, gpointer user_data)
{
    DpdkOvsFixture *f = user_data;
    gchar *joined = g_strjoinv(" ", (gchar **)argv);
    g_string_append_printf(f->commands, "%s\n", joined);

    if (stderr_out)
        *stderr_out = g_strdup("");

    if (dpdk_argv_has(argv, "find") && dpdk_argv_has(argv, "Bridge")) {
        dpdk_fixture_set_out(stdout_out, f->bridge_exists ? "dpdk-br0\n" : "");
    } else if (dpdk_argv_has(argv, "list-ports")) {
        GString *ports = g_string_new(NULL);
        if (f->physical_exists)
            g_string_append(ports, "dpdk-p-dpdk-br0\n");
        if (f->vhost_exists)
            g_string_append(ports, "dpdk-v-vm1\n");
        if (f->foreign_port_exists)
            g_string_append(ports, "operator-port\n");
        dpdk_fixture_set_out(stdout_out, ports->str);
        g_string_free(ports, TRUE);
    } else if (dpdk_argv_has(argv, "get") && dpdk_argv_has(argv, "Bridge")) {
        dpdk_fixture_set_out(stdout_out, f->bridge_netdev ? "netdev\n" : "system\n");
    } else if (dpdk_argv_has(argv, "br-get-external-id")) {
        const gchar *key = argv[g_strv_length((gchar **)argv) - 1];
        if (g_strcmp0(key, "purecvisor-owner") == 0)
            dpdk_fixture_set_out(stdout_out, f->foreign_bridge ? "operator\n" : "dpdk-bridge\n");
        else if (g_strcmp0(key, "purecvisor-mtu") == 0) {
            gchar *mtu = g_strdup_printf("%u\n", f->mtu ? f->mtu : 1500);
            dpdk_fixture_set_out(stdout_out, mtu);
            g_free(mtu);
        } else
            dpdk_fixture_set_out(stdout_out, "");
    } else if (dpdk_argv_has(argv, "find") && dpdk_argv_has(argv, "Interface")) {
        gboolean vhost_query = joined && strstr(joined, "purecvisor-owner=dpdk-vhost");
        const gchar *condition = dpdk_argv_after(argv, "Interface");
        gboolean physical_name = condition && strstr(condition, "name=dpdk-p-");
        gboolean vhost_name = condition && strstr(condition, "name=dpdk-v-");
        dpdk_fixture_set_out(stdout_out,
            (vhost_query || vhost_name) ? (f->vhost_exists ? "dpdk-v-vm1\n" : "") :
            physical_name ? (f->physical_exists ? "dpdk-p-dpdk-br0\n" : "") : "");
    } else if (dpdk_argv_has(argv, "get") && dpdk_argv_has(argv, "Interface")) {
        const gchar *iface = dpdk_argv_after(argv, "Interface");
        const gchar *column = argv[g_strv_length((gchar **)argv) - 1];
        gboolean vhost = iface && g_str_has_prefix(iface, "dpdk-v-");
        if (g_strcmp0(column, "external_ids:purecvisor-owner") == 0)
            dpdk_fixture_set_out(stdout_out,
                vhost ? (f->foreign_vhost ? "operator\n" : "dpdk-vhost\n") :
                        (f->foreign_physical ? "operator\n" : "dpdk-physical\n"));
        else if (g_strcmp0(column, "external_ids:purecvisor-bridge") == 0)
            dpdk_fixture_set_out(stdout_out, "dpdk-br0\n");
        else if (g_strcmp0(column, "external_ids:purecvisor-vm") == 0)
            dpdk_fixture_set_out(stdout_out, "vm1\n");
        else if (g_strcmp0(column, "external_ids:purecvisor-pci") == 0)
            dpdk_fixture_set_out(stdout_out,
                f->invalid_physical_pci ? "invalid\n" : "0000:82:00.1\n");
        else if (vhost && g_strcmp0(column, "type") == 0)
            dpdk_fixture_set_out(stdout_out, f->vhost_type);
        else if (vhost && g_strcmp0(column, "options:vhost-server-path") == 0)
            dpdk_fixture_set_out(stdout_out, f->vhost_path);
        else
            dpdk_fixture_set_out(stdout_out, "");
    } else if (dpdk_argv_has(argv, "iface-to-br")) {
        dpdk_fixture_set_out(stdout_out, "dpdk-br0\n");
    } else if (dpdk_argv_has(argv, "add-br")) {
        f->bridge_exists = TRUE;
        f->mtu = strstr(joined, "purecvisor-mtu=9000") ? 9000 : 1500;
        dpdk_fixture_set_out(stdout_out, "");
    } else if (dpdk_argv_has(argv, "add-port")) {
        const gchar *port = dpdk_argv_after(argv, "add-port");
        port = port ? dpdk_argv_after(argv, port) : NULL;
        gboolean vhost = port && g_str_has_prefix(port, "dpdk-v-");
        if (!vhost && f->fail_physical_add) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "injected physical add failure");
            g_free(joined);
            return FALSE;
        }
        if (vhost) {
            f->vhost_exists = TRUE;
            if (!f->ignore_vhost_config)
                dpdk_fixture_record_vhost_config(f, argv);
        } else {
            f->physical_exists = TRUE;
        }
        dpdk_fixture_set_out(stdout_out, "");
    } else if (dpdk_argv_has(argv, "set") &&
               dpdk_argv_has(argv, "Interface")) {
        const gchar *iface = dpdk_argv_after(argv, "Interface");
        if (!f->ignore_vhost_config && iface && g_str_has_prefix(iface, "dpdk-v-"))
            dpdk_fixture_record_vhost_config(f, argv);
        dpdk_fixture_set_out(stdout_out, "");
    } else if (dpdk_argv_has(argv, "wait-until")) {
        if (f->fail_mtu_wait) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "injected MTU timeout");
            g_free(joined);
            return FALSE;
        }
        dpdk_fixture_set_out(stdout_out, "");
    } else if (dpdk_argv_has(argv, "del-port")) {
        const gchar *port = argv[g_strv_length((gchar **)argv) - 1];
        if (port && g_str_has_prefix(port, "dpdk-v-"))
            f->vhost_exists = FALSE;
        else
            f->physical_exists = FALSE;
        dpdk_fixture_set_out(stdout_out, "");
    } else if (dpdk_argv_has(argv, "del-br")) {
        f->bridge_exists = FALSE;
        f->physical_exists = FALSE;
        dpdk_fixture_set_out(stdout_out, "");
    } else {
        dpdk_fixture_set_out(stdout_out, "");
    }

    g_free(joined);
    return TRUE;
}

static void
dpdk_ovs_fixture_begin(DpdkOvsFixture *fixture)
{
    memset(fixture, 0, sizeof *fixture);
    fixture->bridge_netdev = TRUE;
    fixture->mtu = 1500;
    fixture->commands = g_string_new(NULL);
    pcv_dpdk_test_set_ovs_runner(dpdk_ovs_fixture_run, fixture, 1);
}

static void
dpdk_ovs_fixture_end(DpdkOvsFixture *fixture)
{
    pcv_dpdk_test_set_ovs_runner(NULL, NULL, -1);
    g_free(fixture->vhost_type);
    g_free(fixture->vhost_path);
    g_string_free(fixture->commands, TRUE);
}

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
    pcv_dpdk_test_set_vswitchd_running(0);
    pcv_dpdk_test_set_unbind_paths(fixture->pci_root, fixture->devbind_path);
    return fixture;
}

static void
dpdk_unbind_fixture_free(DpdkUnbindFixture *fixture)
{
    if (!fixture)
        return;
    pcv_dpdk_test_set_unbind_paths(NULL, NULL);
    pcv_dpdk_test_set_vswitchd_running(-1);
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




static void
test_dpdk_unbind_rejects_running_ovs_dpdk_owner(void)
{
    DpdkUnbindFixture *fixture = dpdk_unbind_fixture_new(TRUE, "vfio-pci");
    dpdk_fixture_set_devbind(fixture, 0, "", "ixgbe");
    pcv_dpdk_test_set_vswitchd_running(1);

    GError *error = NULL;
    g_assert_false(pcv_dpdk_unbind("0000:05:00.0", &error));
    g_assert_nonnull(error);
    g_assert_nonnull(g_strstr_len(error->message, -1,
                                  "stop openvswitch-switch"));
    g_assert_false(g_file_test(fixture->log_path, G_FILE_TEST_EXISTS));
    gchar *driver_target = g_file_read_link(fixture->driver_path, NULL);
    g_assert_nonnull(driver_target);
    g_assert_true(g_str_has_suffix(driver_target, "/vfio-pci"));

    g_free(driver_target);
    g_clear_error(&error);
    dpdk_unbind_fixture_free(fixture);
}

static void test_dpdk_bridge_delete_idempotent(void) {
    DpdkOvsFixture fixture;
    dpdk_ovs_fixture_begin(&fixture);
    g_assert_true(pcv_dpdk_bridge_delete("nonexist-dpdk-br", NULL));
    g_assert_false(fixture.bridge_exists);
    dpdk_ovs_fixture_end(&fixture);
}

static void test_dpdk_bridge_delete_owned_physical_port(void) {
    DpdkOvsFixture fixture;
    dpdk_ovs_fixture_begin(&fixture);
    fixture.bridge_exists = TRUE;
    fixture.physical_exists = TRUE;
    g_assert_true(pcv_dpdk_bridge_delete("dpdk-br0", NULL));
    g_assert_false(fixture.bridge_exists);
    g_assert_false(fixture.physical_exists);
    dpdk_ovs_fixture_end(&fixture);
}

static void test_dpdk_bridge_delete_rejects_managed_vm_port(void) {
    DpdkOvsFixture fixture;
    dpdk_ovs_fixture_begin(&fixture);
    fixture.bridge_exists = TRUE;
    fixture.vhost_exists = TRUE;
    GError *error = NULL;
    g_assert_false(pcv_dpdk_bridge_delete("dpdk-br0", &error));
    g_assert_nonnull(error);
    g_assert_true(fixture.bridge_exists);
    g_assert_true(fixture.vhost_exists);
    g_assert_null(strstr(fixture.commands->str, " del-br "));
    g_clear_error(&error);
    dpdk_ovs_fixture_end(&fixture);
}

static void test_dpdk_bridge_delete_rejects_foreign_port(void) {
    DpdkOvsFixture fixture;
    dpdk_ovs_fixture_begin(&fixture);
    fixture.bridge_exists = TRUE;
    fixture.foreign_port_exists = TRUE;
    GError *error = NULL;
    g_assert_false(pcv_dpdk_bridge_delete("dpdk-br0", &error));
    g_assert_nonnull(error);
    g_assert_true(fixture.bridge_exists);
    g_assert_true(fixture.foreign_port_exists);
    g_assert_null(strstr(fixture.commands->str, " del-br "));
    g_clear_error(&error);
    dpdk_ovs_fixture_end(&fixture);
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
      
                                                     
                                                                    
                                                             
       
    gboolean ok = pcv_dpdk_bridge_create("br0", "x; touch /tmp/pcv_dpdk_pwn", 1500, &err);
    g_assert_false(ok);
    g_assert_nonnull(err);
    g_clear_error(&err);
                       
    g_assert_false(g_file_test("/tmp/pcv_dpdk_pwn", G_FILE_TEST_EXISTS));
}

static void test_dpdk_bridge_create_mtu_owned_exact_argv(void) {
    DpdkOvsFixture fixture;
    dpdk_ovs_fixture_begin(&fixture);
    GError *error = NULL;
    g_assert_true(pcv_dpdk_bridge_create("dpdk-br0", "0000:82:00.1", 9000, &error));
    g_assert_no_error(error);
    g_assert_true(fixture.bridge_exists);
    g_assert_true(fixture.physical_exists);
    g_assert_nonnull(strstr(fixture.commands->str, "datapath_type=netdev"));
    g_assert_nonnull(strstr(fixture.commands->str, "external_ids:purecvisor-owner=dpdk-bridge"));
    g_assert_nonnull(strstr(fixture.commands->str, "external_ids:purecvisor-mtu=9000"));
    g_assert_nonnull(strstr(fixture.commands->str, "type=dpdk"));
    g_assert_nonnull(strstr(fixture.commands->str, "options:dpdk-devargs=0000:82:00.1"));
    g_assert_nonnull(strstr(fixture.commands->str, "mtu_request=9000"));
    g_assert_nonnull(strstr(fixture.commands->str, "wait-until Interface dpdk-p-dpdk-br0 mtu=9000"));
    dpdk_ovs_fixture_end(&fixture);
}

static void test_dpdk_bridge_create_rejects_foreign_without_mutation(void) {
    DpdkOvsFixture fixture;
    dpdk_ovs_fixture_begin(&fixture);
    fixture.bridge_exists = TRUE;
    fixture.foreign_bridge = TRUE;
    GError *error = NULL;
    g_assert_false(pcv_dpdk_bridge_create("dpdk-br0", "0000:82:00.1", 9000, &error));
    g_assert_nonnull(error);
    g_assert_null(strstr(fixture.commands->str, " add-br "));
    g_assert_null(strstr(fixture.commands->str, " set Bridge "));
    g_assert_null(strstr(fixture.commands->str, " del-br "));
    g_clear_error(&error);
    dpdk_ovs_fixture_end(&fixture);
}

static void test_dpdk_bridge_create_mtu_failure_rolls_back_new_resources(void) {
    DpdkOvsFixture fixture;
    dpdk_ovs_fixture_begin(&fixture);
    fixture.fail_mtu_wait = TRUE;
    GError *error = NULL;
    g_assert_false(pcv_dpdk_bridge_create("dpdk-br0", "0000:82:00.1", 9000, &error));
    g_assert_nonnull(error);
    g_assert_false(fixture.bridge_exists);
    g_assert_false(fixture.physical_exists);
    g_assert_nonnull(strstr(fixture.commands->str, " del-port dpdk-br0 dpdk-p-dpdk-br0"));
    g_assert_nonnull(strstr(fixture.commands->str, " del-br dpdk-br0"));
    g_clear_error(&error);
    dpdk_ovs_fixture_end(&fixture);
}

static void test_dpdk_bridge_create_rejects_invalid_mtu_before_ovs(void) {
    DpdkOvsFixture fixture;
    dpdk_ovs_fixture_begin(&fixture);
    GError *error = NULL;
    g_assert_false(pcv_dpdk_bridge_create("dpdk-br0", NULL, 67, &error));
    g_assert_nonnull(error);
    g_assert_cmpuint(fixture.commands->len, ==, 0);
    g_clear_error(&error);
    dpdk_ovs_fixture_end(&fixture);
}

static void test_dpdk_bridge_create_rejects_corrupt_owned_physical_pci(void) {
    DpdkOvsFixture fixture;
    dpdk_ovs_fixture_begin(&fixture);
    fixture.bridge_exists = TRUE;
    fixture.physical_exists = TRUE;
    fixture.invalid_physical_pci = TRUE;
    GError *error = NULL;
    g_assert_false(pcv_dpdk_bridge_create("dpdk-br0", NULL, 9000, &error));
    g_assert_nonnull(error);
    g_assert_null(strstr(fixture.commands->str, " set Bridge "));
    g_clear_error(&error);
    dpdk_ovs_fixture_end(&fixture);
}

static void test_dpdk_vm_port_ensure_delete_owned_lifecycle(void) {
    DpdkOvsFixture fixture;
    dpdk_ovs_fixture_begin(&fixture);
    fixture.bridge_exists = TRUE;
    fixture.mtu = 9000;
    GError *error = NULL;
    g_assert_true(pcv_dpdk_vm_port_ensure("dpdk-br0", "vm1", &error));
    g_assert_no_error(error);
    g_assert_true(fixture.vhost_exists);
    g_assert_nonnull(strstr(fixture.commands->str, "type=dpdkvhostuserclient"));
    g_assert_nonnull(strstr(fixture.commands->str,
        "options:vhost-server-path=/run/libvirt/qemu/purecvisor-vhost-vm1.sock"));
    g_assert_nonnull(strstr(fixture.commands->str, "mtu_request=9000"));
    g_assert_nonnull(strstr(fixture.commands->str, "external_ids:purecvisor-vm=vm1"));

    g_assert_true(pcv_dpdk_vm_port_delete("vm1", &error));
    g_assert_no_error(error);
    g_assert_false(fixture.vhost_exists);
    dpdk_ovs_fixture_end(&fixture);
}

static void
test_dpdk_vhost_path_and_sun_path_contract(void)
{
    g_autofree gchar *max_name = g_strnfill(64, 'v');
    g_autofree gchar *path = pcv_dpdk_vhost_socket_path(max_name);
    g_assert_nonnull(path);
    g_assert_true(g_str_has_prefix(
        path, "/run/libvirt/qemu/purecvisor-vhost-"));
    g_assert_true(strlen(path) + 1 <= sizeof(((struct sockaddr_un *)0)->sun_path));

    g_autofree gchar *too_long_name = g_strnfill(65, 'v');
    g_assert_null(pcv_dpdk_vhost_socket_path(too_long_name));
    g_assert_null(pcv_dpdk_vhost_socket_path(NULL));
}

static void
test_dpdk_vhost_runtime_preflight_rejects_missing_and_symlink(void)
{
    GError *error = NULL;
    g_autofree gchar *root = g_dir_make_tmp("pcv-dpdk-runtime-XXXXXX", &error);
    g_assert_no_error(error);
    g_assert_nonnull(root);
    g_autofree gchar *real_dir = g_build_filename(root, "real", NULL);
    g_autofree gchar *link_dir = g_build_filename(root, "link", NULL);
    g_autofree gchar *missing_dir = g_build_filename(root, "missing", NULL);
    g_assert_cmpint(g_mkdir(real_dir, 0755), ==, 0);
    g_assert_cmpint(symlink(real_dir, link_dir), ==, 0);

    g_assert_true(pcv_dpdk_test_vhost_runtime_preflight_at(real_dir, &error));
    g_assert_no_error(error);
    g_assert_false(pcv_dpdk_test_vhost_runtime_preflight_at(link_dir, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_assert_false(pcv_dpdk_test_vhost_runtime_preflight_at(missing_dir, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);

    g_assert_cmpint(g_unlink(link_dir), ==, 0);
    g_assert_cmpint(g_rmdir(real_dir), ==, 0);
    g_assert_cmpint(g_rmdir(root), ==, 0);
}

static void
test_dpdk_active_legacy_endpoint_is_closed_and_exact(void)
{
    DpdkOvsFixture fixture;
    dpdk_ovs_fixture_begin(&fixture);
    fixture.bridge_exists = TRUE;
    GError *error = NULL;
    g_assert_true(pcv_dpdk_vm_port_ensure_endpoint(
        "dpdk-br0", "vm1", PCV_DPDK_VHOST_ENDPOINT_ACTIVE_LEGACY, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(fixture.commands->str,
        "options:vhost-server-path=/var/run/purecvisor/vhost-vm1.sock"));
    g_assert_null(strstr(fixture.commands->str,
        "options:vhost-server-path=/tmp/"));
    dpdk_ovs_fixture_end(&fixture);
}

static void
test_dpdk_owned_vhost_endpoint_converges_and_rechecks(void)
{
    DpdkOvsFixture fixture;
    dpdk_ovs_fixture_begin(&fixture);
    fixture.bridge_exists = TRUE;
    fixture.vhost_exists = TRUE;
    fixture.vhost_type = g_strdup("dpdkvhostuserclient");
    fixture.vhost_path = g_strdup("/var/run/purecvisor/vhost-vm1.sock");
    GError *error = NULL;

    g_assert_true(pcv_dpdk_vm_port_ensure("dpdk-br0", "vm1", &error));
    g_assert_no_error(error);
    g_assert_cmpstr(fixture.vhost_type, ==, "dpdkvhostuserclient");
    g_assert_cmpstr(fixture.vhost_path, ==,
                    "/run/libvirt/qemu/purecvisor-vhost-vm1.sock");
    g_assert_nonnull(strstr(fixture.commands->str,
                           " get Interface dpdk-v-vm1 type"));
    g_assert_nonnull(strstr(
        fixture.commands->str,
        " get Interface dpdk-v-vm1 options:vhost-server-path"));
    dpdk_ovs_fixture_end(&fixture);
}

static void
test_dpdk_new_vhost_postcondition_failure_rolls_back(void)
{
    DpdkOvsFixture fixture;
    dpdk_ovs_fixture_begin(&fixture);
    fixture.bridge_exists = TRUE;
    fixture.ignore_vhost_config = TRUE;
    GError *error = NULL;

    g_assert_false(pcv_dpdk_vm_port_ensure("dpdk-br0", "vm1", &error));
    g_assert_nonnull(error);
    g_assert_false(fixture.vhost_exists);
    g_assert_nonnull(strstr(fixture.commands->str,
                           " --if-exists del-port dpdk-v-vm1"));
    g_clear_error(&error);
    dpdk_ovs_fixture_end(&fixture);
}

static void
test_dpdk_vhost_endpoint_enum_rejects_unknown_before_ovs(void)
{
    DpdkOvsFixture fixture;
    dpdk_ovs_fixture_begin(&fixture);
    GError *error = NULL;
    g_assert_false(pcv_dpdk_vm_port_ensure_endpoint(
        "dpdk-br0", "vm1", (PcvDpdkVhostEndpoint)99, &error));
    g_assert_nonnull(error);
    g_assert_cmpuint(fixture.commands->len, ==, 0);
    g_clear_error(&error);
    dpdk_ovs_fixture_end(&fixture);
}

static void test_dpdk_vm_port_delete_rejects_foreign(void) {
    DpdkOvsFixture fixture;
    dpdk_ovs_fixture_begin(&fixture);
    fixture.bridge_exists = TRUE;
    fixture.vhost_exists = TRUE;
    fixture.foreign_vhost = TRUE;
    GError *error = NULL;
    g_assert_false(pcv_dpdk_vm_port_delete("vm1", &error));
    g_assert_nonnull(error);
    g_assert_true(fixture.vhost_exists);
    g_assert_null(strstr(fixture.commands->str, " del-port "));
    g_clear_error(&error);
    dpdk_ovs_fixture_end(&fixture);
}

static void test_dpdk_vm_port_ensure_rejects_non_dpdk_bridge(void) {
    DpdkOvsFixture fixture;
    dpdk_ovs_fixture_begin(&fixture);
    fixture.bridge_exists = TRUE;
    fixture.bridge_netdev = FALSE;
    GError *error = NULL;
    g_assert_false(pcv_dpdk_vm_port_ensure("dpdk-br0", "vm1", &error));
    g_assert_nonnull(error);
    g_assert_false(fixture.vhost_exists);
    g_assert_null(strstr(fixture.commands->str, " add-port "));
    g_clear_error(&error);
    dpdk_ovs_fixture_end(&fixture);
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
    g_test_add_func("/dpdk/unbind/rejects_running_ovs_dpdk_owner",
                    test_dpdk_unbind_rejects_running_ovs_dpdk_owner);
    g_test_add_func("/dpdk/bridge_delete/idempotent",  test_dpdk_bridge_delete_idempotent);
    g_test_add_func("/dpdk/bridge_delete/owned_physical_port",
                    test_dpdk_bridge_delete_owned_physical_port);
    g_test_add_func("/dpdk/bridge_delete/rejects_managed_vm_port",
                    test_dpdk_bridge_delete_rejects_managed_vm_port);
    g_test_add_func("/dpdk/bridge_delete/rejects_foreign_port",
                    test_dpdk_bridge_delete_rejects_foreign_port);
    g_test_add_func("/dpdk/pci_addr/valid",            test_pci_addr_valid);
    g_test_add_func("/dpdk/pci_addr/invalid",          test_pci_addr_invalid);
    g_test_add_func("/dpdk/bind/rejects_unrecoverable_driver",
                    test_dpdk_bind_rejects_unrecoverable_driver);
    g_test_add_func("/dpdk/bridge_create/reject_injection",
                    test_dpdk_bridge_create_reject_injection);
    g_test_add_func("/dpdk/bridge_create/mtu_owned_exact_argv",
                    test_dpdk_bridge_create_mtu_owned_exact_argv);
    g_test_add_func("/dpdk/bridge_create/rejects_foreign",
                    test_dpdk_bridge_create_rejects_foreign_without_mutation);
    g_test_add_func("/dpdk/bridge_create/mtu_failure_rollback",
                    test_dpdk_bridge_create_mtu_failure_rolls_back_new_resources);
    g_test_add_func("/dpdk/bridge_create/rejects_invalid_mtu",
                    test_dpdk_bridge_create_rejects_invalid_mtu_before_ovs);
    g_test_add_func("/dpdk/bridge_create/rejects_corrupt_physical_pci",
                    test_dpdk_bridge_create_rejects_corrupt_owned_physical_pci);
    g_test_add_func("/dpdk/vhost/owned_lifecycle",
                    test_dpdk_vm_port_ensure_delete_owned_lifecycle);
    g_test_add_func("/dpdk/vhost/path_and_sun_path_contract",
                    test_dpdk_vhost_path_and_sun_path_contract);
    g_test_add_func("/dpdk/vhost/runtime_preflight",
                    test_dpdk_vhost_runtime_preflight_rejects_missing_and_symlink);
    g_test_add_func("/dpdk/vhost/active_legacy_endpoint_closed_exact",
                    test_dpdk_active_legacy_endpoint_is_closed_and_exact);
    g_test_add_func("/dpdk/vhost/owned_endpoint_converges_rechecks",
                    test_dpdk_owned_vhost_endpoint_converges_and_rechecks);
    g_test_add_func("/dpdk/vhost/new_postcondition_failure_rollback",
                    test_dpdk_new_vhost_postcondition_failure_rolls_back);
    g_test_add_func("/dpdk/vhost/unknown_endpoint_rejected_before_ovs",
                    test_dpdk_vhost_endpoint_enum_rejects_unknown_before_ovs);
    g_test_add_func("/dpdk/vhost/rejects_foreign_delete",
                    test_dpdk_vm_port_delete_rejects_foreign);
    g_test_add_func("/dpdk/vhost/rejects_non_dpdk_bridge",
                    test_dpdk_vm_port_ensure_rejects_non_dpdk_bridge);
    g_test_add_func("/dpdk/nic_protected/route_default",  test_dpdk_nic_route_default);
    g_test_add_func("/dpdk/nic_protected/null_failsecure", test_dpdk_nic_null_failsecure);
    g_test_add_func("/dpdk/nic_protected/absent_netdir",  test_dpdk_nic_absent_netdir_passes);
    g_test_add_func("/dpdk/nic_protected/malformed",      test_dpdk_nic_malformed_failsecure);
}
