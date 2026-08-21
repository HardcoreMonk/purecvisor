                                                                                       
                                                                                  
                                                                        
                                                            
                                
                           
  
                                                         
                                                          
   

#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <unistd.h>

#include "modules/daemons/alert_engine.h"
#include "modules/auth/pcv_rbac.h"                                               
#include "api/daemon_config_policy.h"
#include "utils/pcv_config.h"

                                                                      
                                                                             
                                                             
void     pcv_dispatcher_init_policy_map(void);
gboolean pcv_dispatcher_check_rbac(const gchar *method, gint caller_role);

                                                                         
                                    
gchar *pcv_alert_engine_test_dup_vm_webhook(const gchar *vm_name);
void pcv_test_alert_telemetry_set(gboolean enabled, gdouble cpu_percent,
                                  gdouble mem_percent);
gint pcv_test_alert_telemetry_call_count(void);
gint pcv_alert_engine_test_worker_start_count(void);
void pcv_alert_engine_test_seed_cpu_warn_elapsed(gint64 elapsed_sec);
void pcv_alert_engine_test_eval_cpu_once(gdouble current_pct);
gboolean pcv_alert_engine_test_secret_buffer_tail_zero(void);
static gint alert_history_count(void);

static GMutex g_config_commit_hook_mu;
static GCond g_config_commit_hook_cond;
static gsize g_config_commit_hook_sync_initialized;
static gboolean g_config_commit_hook_enabled;
static gboolean g_config_commit_hook_entered;
static gboolean g_config_commit_hook_release;
static volatile gint g_snapshot_wipe_count;
static volatile gint g_daemon_config_publish_count;
static GMutex g_daemon_publish_hook_mu;
static GCond g_daemon_publish_hook_cond;
static gsize g_daemon_publish_hook_sync_initialized;
static gboolean g_daemon_publish_hook_enabled;
static gboolean g_daemon_publish_hook_entered;
static gboolean g_daemon_publish_hook_release;

void
pcv_test_alert_snapshot_wipe_hook(const gchar *secret, gsize len)
{
    for (gsize i = 0; i < len; i++)
        g_assert_cmpuint((guchar)secret[i], ==, 0);
    g_atomic_int_inc(&g_snapshot_wipe_count);
}

void
pcv_test_daemon_config_publish_hook(void)
{
    g_atomic_int_inc(&g_daemon_config_publish_count);
    if (g_once_init_enter(&g_daemon_publish_hook_sync_initialized)) {
        g_mutex_init(&g_daemon_publish_hook_mu);
        g_cond_init(&g_daemon_publish_hook_cond);
        g_once_init_leave(&g_daemon_publish_hook_sync_initialized, 1);
    }
    g_mutex_lock(&g_daemon_publish_hook_mu);
    if (g_daemon_publish_hook_enabled) {
        g_daemon_publish_hook_entered = TRUE;
        g_cond_broadcast(&g_daemon_publish_hook_cond);
        while (!g_daemon_publish_hook_release) {
            g_cond_wait(
                &g_daemon_publish_hook_cond,
                &g_daemon_publish_hook_mu);
        }
    }
    g_mutex_unlock(&g_daemon_publish_hook_mu);
}

static void
ensure_config_commit_hook_sync(void)
{
    if (g_once_init_enter(&g_config_commit_hook_sync_initialized)) {
        g_mutex_init(&g_config_commit_hook_mu);
        g_cond_init(&g_config_commit_hook_cond);
        g_once_init_leave(&g_config_commit_hook_sync_initialized, 1);
    }
}

void
pcv_test_alert_config_commit_hook(void)
{
    ensure_config_commit_hook_sync();
    g_mutex_lock(&g_config_commit_hook_mu);
    if (g_config_commit_hook_enabled) {
        g_config_commit_hook_entered = TRUE;
        g_cond_broadcast(&g_config_commit_hook_cond);
        while (!g_config_commit_hook_release) {
            g_cond_wait(&g_config_commit_hook_cond, &g_config_commit_hook_mu);
        }
    }
    g_mutex_unlock(&g_config_commit_hook_mu);
}

static JsonObject *
complete_daemon_config(gboolean enabled)
{
    JsonObject *cfg = json_object_new();
    json_object_set_boolean_member(cfg, "enabled", enabled);
    json_object_set_int_member(cfg, "cpu_warn", 80);
    json_object_set_int_member(cfg, "cpu_crit", 95);
    json_object_set_int_member(cfg, "mem_warn", 85);
    json_object_set_int_member(cfg, "mem_crit", 95);
    json_object_set_int_member(cfg, "disk_warn", 80);
    json_object_set_int_member(cfg, "disk_crit", 90);
    json_object_set_int_member(cfg, "data_pool_warn", 80);
    json_object_set_int_member(cfg, "data_pool_crit", 90);
    json_object_set_int_member(cfg, "eval_period", 30);
    json_object_set_int_member(cfg, "dedup_window", 300);
    json_object_set_string_member(cfg, "webhook_url", "");
    json_object_set_string_member(cfg, "webhook_secret", "");
    json_object_set_string_member(cfg, "webhook_crit_url", "");
    json_object_set_string_member(cfg, "webhook_format", "generic");
    json_object_set_string_member(cfg, "telegram_chat_id", "");
    return cfg;
}

static gint64
config_revision(void)
{
    JsonObject *cfg = pcv_alert_engine_get_config();
    gint64 revision = json_object_get_int_member(cfg, "config_revision");
    json_object_unref(cfg);
    return revision;
}

static void
engine_setup(void)
{
    pcv_alert_engine_init();
    JsonObject *baseline = complete_daemon_config(FALSE);
    g_assert_cmpint(
        pcv_alert_engine_apply_daemon_config(baseline, PCV_ALERT_CONFIG_SOURCE_RELOAD),
        ==, PCV_ALERT_CONFIG_SET_OK);
    json_object_unref(baseline);
}

static void
engine_teardown(void)
{
    pcv_alert_engine_shutdown();
}

static void
test_alert_config_valid(void)
{
    engine_setup();
    gint64 revision = config_revision();

    JsonObject *patch = json_object_new();
    json_object_set_int_member(patch, "cpu_warn", 81);
    g_assert_cmpint(pcv_alert_engine_set_config(patch, revision),
                    ==, PCV_ALERT_CONFIG_SET_OK);
    json_object_unref(patch);

    JsonObject *actual = pcv_alert_engine_get_config();
    g_assert_cmpint(json_object_get_int_member(actual, "cpu_warn"), ==, 81);
    g_assert_cmpint(json_object_get_int_member(actual, "config_revision"),
                    ==, revision + 1);
    json_object_unref(actual);
    engine_teardown();
}

static void
test_alert_config_revision_conflict(void)
{
    engine_setup();
    gint64 revision = config_revision();

    JsonObject *client_a = json_object_new();
    json_object_set_int_member(client_a, "cpu_warn", 82);
    g_assert_cmpint(pcv_alert_engine_set_config(client_a, revision),
                    ==, PCV_ALERT_CONFIG_SET_OK);
    json_object_unref(client_a);

    JsonObject *client_b = json_object_new();
    json_object_set_int_member(client_b, "cpu_warn", 83);
    g_assert_cmpint(pcv_alert_engine_set_config(client_b, revision),
                    ==, PCV_ALERT_CONFIG_SET_CONFLICT);
    json_object_unref(client_b);

    JsonObject *actual = pcv_alert_engine_get_config();
    g_assert_cmpint(json_object_get_int_member(actual, "cpu_warn"), ==, 82);
    g_assert_cmpint(json_object_get_int_member(actual, "config_revision"),
                    ==, revision + 1);
    json_object_unref(actual);
    engine_teardown();
}

static void
test_alert_config_revision_increment_rules(void)
{
    engine_setup();
    gint64 revision = config_revision();

    JsonObject *invalid = json_object_new();
    json_object_set_int_member(invalid, "cpu_warn", 95);
    g_assert_cmpint(pcv_alert_engine_set_config(invalid, revision),
                    ==, PCV_ALERT_CONFIG_SET_INVALID);
    json_object_unref(invalid);
    g_assert_cmpint(config_revision(), ==, revision);

    JsonObject *valid = json_object_new();
    json_object_set_int_member(valid, "cpu_warn", 81);
    g_assert_cmpint(pcv_alert_engine_set_config(valid, revision),
                    ==, PCV_ALERT_CONFIG_SET_OK);
    json_object_unref(valid);
    revision++;
    g_assert_cmpint(config_revision(), ==, revision);

    JsonObject *stale = json_object_new();
    json_object_set_int_member(stale, "cpu_warn", 82);
    g_assert_cmpint(pcv_alert_engine_set_config(stale, revision - 1),
                    ==, PCV_ALERT_CONFIG_SET_CONFLICT);
    json_object_unref(stale);
    g_assert_cmpint(config_revision(), ==, revision);

    JsonObject *invalid_reload = complete_daemon_config(FALSE);
    json_object_set_int_member(invalid_reload, "mem_warn", 95);
    g_assert_cmpint(
        pcv_alert_engine_apply_daemon_config(
            invalid_reload, PCV_ALERT_CONFIG_SOURCE_RELOAD),
        ==, PCV_ALERT_CONFIG_SET_INVALID);
    json_object_unref(invalid_reload);
    g_assert_cmpint(config_revision(), ==, revision);

    JsonObject *valid_reload = complete_daemon_config(FALSE);
    g_assert_cmpint(
        pcv_alert_engine_apply_daemon_config(
            valid_reload, PCV_ALERT_CONFIG_SOURCE_RELOAD),
        ==, PCV_ALERT_CONFIG_SET_OK);
    json_object_unref(valid_reload);
    g_assert_cmpint(config_revision(), ==, revision + 1);
    engine_teardown();
}

static void
test_alert_config_rejects_pair_without_partial_mutation(void)
{
    engine_setup();
    gint64 revision = config_revision();

    JsonObject *patch = json_object_new();
    json_object_set_int_member(patch, "mem_warn", 86);
    json_object_set_int_member(patch, "cpu_warn", 96);
    g_assert_cmpint(pcv_alert_engine_set_config(patch, revision),
                    ==, PCV_ALERT_CONFIG_SET_INVALID);
    json_object_unref(patch);

    JsonObject *actual = pcv_alert_engine_get_config();
    g_assert_cmpint(json_object_get_int_member(actual, "cpu_warn"), ==, 80);
    g_assert_cmpint(json_object_get_int_member(actual, "mem_warn"), ==, 85);
    g_assert_cmpint(json_object_get_int_member(actual, "config_revision"),
                    ==, revision);
    json_object_unref(actual);
    engine_teardown();
}

static void
assert_invalid_patch_unchanged(JsonObject *patch)
{
    gint64 revision = config_revision();
    JsonObject *before = pcv_alert_engine_get_config();
    gint64 before_cpu = json_object_get_int_member(before, "cpu_warn");
    gint64 before_eval = json_object_get_int_member(before, "eval_period");
    const gchar *before_url = json_object_get_string_member(before, "webhook_url");
    gchar *url_copy = g_strdup(before_url);
    json_object_unref(before);

    g_assert_cmpint(pcv_alert_engine_set_config(patch, revision),
                    ==, PCV_ALERT_CONFIG_SET_INVALID);

    JsonObject *after = pcv_alert_engine_get_config();
    g_assert_cmpint(json_object_get_int_member(after, "cpu_warn"), ==,
                    before_cpu);
    g_assert_cmpint(json_object_get_int_member(after, "eval_period"), ==,
                    before_eval);
    g_assert_cmpstr(json_object_get_string_member(after, "webhook_url"), ==,
                    url_copy);
    g_assert_cmpint(json_object_get_int_member(after, "config_revision"), ==,
                    revision);
    json_object_unref(after);
    g_free(url_copy);
}

static void
test_alert_config_rejects_invalid_type(void)
{
    engine_setup();
    JsonObject *patch = json_object_new();
    json_object_set_string_member(patch, "cpu_warn", "81");
    assert_invalid_patch_unchanged(patch);
    json_object_unref(patch);
    engine_teardown();
}

static void
test_alert_config_rejects_empty_patch(void)
{
    engine_setup();
    JsonObject *patch = json_object_new();
    assert_invalid_patch_unchanged(patch);
    json_object_unref(patch);
    engine_teardown();
}

static void
test_alert_config_boundaries(void)
{
    engine_setup();
    gint64 revision = config_revision();
    JsonObject *patch = json_object_new();
    json_object_set_int_member(patch, "cpu_warn", 0);
    json_object_set_int_member(patch, "cpu_crit", 100);
    json_object_set_int_member(patch, "mem_warn", 0);
    json_object_set_int_member(patch, "mem_crit", 100);
    json_object_set_int_member(patch, "disk_warn", 0);
    json_object_set_int_member(patch, "disk_crit", 100);
    json_object_set_int_member(patch, "eval_period", 5);
    g_assert_cmpint(pcv_alert_engine_set_config(patch, revision),
                    ==, PCV_ALERT_CONFIG_SET_OK);
    json_object_unref(patch);

    revision++;
    patch = json_object_new();
    json_object_set_int_member(patch, "eval_period", 600);
    g_assert_cmpint(pcv_alert_engine_set_config(patch, revision),
                    ==, PCV_ALERT_CONFIG_SET_OK);
    json_object_unref(patch);

    const gint invalid_thresholds[] = {-1, 101};
    for (guint i = 0; i < G_N_ELEMENTS(invalid_thresholds); i++) {
        patch = json_object_new();
        json_object_set_int_member(patch, "cpu_warn", invalid_thresholds[i]);
        assert_invalid_patch_unchanged(patch);
        json_object_unref(patch);
    }
    const gint invalid_periods[] = {4, 601};
    for (guint i = 0; i < G_N_ELEMENTS(invalid_periods); i++) {
        patch = json_object_new();
        json_object_set_int_member(patch, "eval_period", invalid_periods[i]);
        assert_invalid_patch_unchanged(patch);
        json_object_unref(patch);
    }
    engine_teardown();
}

static void
test_alert_config_rejects_equal_or_reversed_pairs(void)
{
    engine_setup();
    JsonObject *equal = json_object_new();
    json_object_set_int_member(equal, "mem_warn", 90);
    json_object_set_int_member(equal, "mem_crit", 90);
    assert_invalid_patch_unchanged(equal);
    json_object_unref(equal);

    JsonObject *reversed = json_object_new();
    json_object_set_int_member(reversed, "disk_warn", 91);
    json_object_set_int_member(reversed, "disk_crit", 90);
    assert_invalid_patch_unchanged(reversed);
    json_object_unref(reversed);
    engine_teardown();
}

static void
test_alert_config_rejects_format_url_and_overlong_strings(void)
{
    engine_setup();
    JsonObject *patch = json_object_new();
    json_object_set_string_member(patch, "webhook_format", "email");
    assert_invalid_patch_unchanged(patch);
    json_object_unref(patch);

    patch = json_object_new();
    json_object_set_string_member(patch, "webhook_url", "ftp://example.com/hook");
    assert_invalid_patch_unchanged(patch);
    json_object_unref(patch);

    patch = json_object_new();
    json_object_set_string_member(patch, "webhook_crit_url",
                                  "https:///missing-host");
    assert_invalid_patch_unchanged(patch);
    json_object_unref(patch);

    gchar overlong[512];
    memset(overlong, 'x', sizeof(overlong));
    overlong[sizeof(overlong) - 1] = '\0';
    patch = json_object_new();
    json_object_set_string_member(patch, "webhook_url", overlong);
    assert_invalid_patch_unchanged(patch);
    json_object_unref(patch);
    engine_teardown();
}

static void
test_alert_config_canonicalizes_case_insensitive_webhook_scheme(void)
{
    engine_setup();
    JsonObject *patch = json_object_new();
    json_object_set_string_member(
        patch, "webhook_url", "HTTPS://alerts.example.com/hook");
    g_assert_cmpint(
        pcv_alert_engine_set_config(patch, config_revision()),
        ==, PCV_ALERT_CONFIG_SET_OK);
    json_object_unref(patch);

    JsonObject *actual = pcv_alert_engine_get_config();
    g_assert_cmpstr(
        json_object_get_string_member(actual, "webhook_url"),
        ==, "https://alerts.example.com/hook");
    json_object_unref(actual);
    engine_teardown();
}

static void
test_alert_config_rejects_unknown_and_read_only_keys(void)
{
    engine_setup();
    JsonObject *patch = json_object_new();
    json_object_set_int_member(patch, "cpu_wran", 81);
    assert_invalid_patch_unchanged(patch);
    json_object_unref(patch);

    patch = pcv_alert_engine_get_config();
    assert_invalid_patch_unchanged(patch);
    json_object_unref(patch);
    engine_teardown();
}

static void
test_alert_config_valid_partial(void)
{
    engine_setup();
    gint64 revision = config_revision();
    JsonObject *patch = json_object_new();
    json_object_set_boolean_member(patch, "enabled", FALSE);
    json_object_set_int_member(patch, "dedup_window", 0);
    json_object_set_string_member(patch, "webhook_url",
                                  "https://alerts.example.com/hook");
    json_object_set_string_member(patch, "webhook_crit_url",
                                  "http://critical.example.com/hook");
    json_object_set_string_member(patch, "webhook_format", "slack");
    g_assert_cmpint(pcv_alert_engine_set_config(patch, revision),
                    ==, PCV_ALERT_CONFIG_SET_OK);
    json_object_unref(patch);

    JsonObject *actual = pcv_alert_engine_get_config();
    g_assert_cmpint(json_object_get_int_member(actual, "dedup_window"), ==, 0);
    g_assert_cmpstr(json_object_get_string_member(actual, "webhook_format"),
                    ==, "slack");
    json_object_unref(actual);
    engine_teardown();
}

static void
test_alert_config_disabled_loads_values(void)
{
    pcv_alert_engine_init();
    gint64 revision = config_revision();
    JsonObject *source = complete_daemon_config(FALSE);
    json_object_set_int_member(source, "cpu_warn", 70);
    json_object_set_int_member(source, "cpu_crit", 90);
    json_object_set_int_member(source, "data_pool_warn", 60);
    json_object_set_int_member(source, "data_pool_crit", 85);
    json_object_set_int_member(source, "eval_period", 45);
    g_assert_cmpint(
        pcv_alert_engine_apply_daemon_config(
            source, PCV_ALERT_CONFIG_SOURCE_RELOAD),
        ==, PCV_ALERT_CONFIG_SET_OK);
    json_object_unref(source);

    JsonObject *actual = pcv_alert_engine_get_config();
    g_assert_false(json_object_get_boolean_member(actual, "enabled"));
    g_assert_cmpint(json_object_get_int_member(actual, "cpu_warn"), ==, 70);
    g_assert_cmpint(json_object_get_int_member(actual, "data_pool_warn"), ==,
                    60);
    g_assert_cmpint(json_object_get_int_member(actual, "eval_period"), ==, 45);
    g_assert_true(json_object_get_boolean_member(actual,
                                                 "daemon_config_valid"));
    g_assert_cmpint(json_object_get_int_member(actual, "config_revision"), ==,
                    revision + 1);
    json_object_unref(actual);
    pcv_alert_engine_shutdown();
}

static void
test_alert_config_invalid_source_uses_safe_defaults(void)
{
    pcv_alert_engine_init();
    JsonObject *source = complete_daemon_config(TRUE);
    json_object_set_int_member(source, "cpu_warn", 99);
    json_object_set_int_member(source, "cpu_crit", 90);
    json_object_set_string_member(source, "webhook_secret",
                                  "must-never-appear-in-error");
    g_assert_cmpint(
        pcv_alert_engine_apply_daemon_config(
            source, PCV_ALERT_CONFIG_SOURCE_STARTUP),
        ==, PCV_ALERT_CONFIG_SET_INVALID);
    json_object_unref(source);

    JsonObject *actual = pcv_alert_engine_get_config();
    g_assert_false(json_object_get_boolean_member(actual, "enabled"));
    g_assert_cmpint(json_object_get_int_member(actual, "cpu_warn"), ==, 80);
    g_assert_cmpint(json_object_get_int_member(actual, "cpu_crit"), ==, 95);
    g_assert_cmpint(json_object_get_int_member(actual, "mem_warn"), ==, 85);
    g_assert_cmpint(json_object_get_int_member(actual, "disk_crit"), ==, 90);
    g_assert_cmpint(json_object_get_int_member(actual, "data_pool_warn"), ==,
                    80);
    g_assert_cmpint(json_object_get_int_member(actual, "eval_period"), ==, 30);
    g_assert_cmpint(json_object_get_int_member(actual, "dedup_window"), ==,
                    300);
    g_assert_cmpstr(json_object_get_string_member(actual, "webhook_url"), ==,
                    "");
    g_assert_false(json_object_has_member(actual, "webhook_secret"));
    g_assert_false(json_object_get_boolean_member(
        actual, "webhook_secret_configured"));
    g_assert_false(json_object_get_boolean_member(actual,
                                                  "daemon_config_valid"));
    const gchar *error =
        json_object_get_string_member(actual, "daemon_config_error");
    g_assert_cmpstr(error, ==, "invalid_alert_config");
    g_assert_null(strstr(error, "must-never-appear"));
    g_assert_cmpint(json_object_get_int_member(actual, "config_revision"), ==,
                    1);
    json_object_unref(actual);
    pcv_alert_engine_shutdown();
}

static void
assert_invalid_numeric_source_uses_safe_defaults(const gchar *raw_cpu_warn)
{
    gchar *path = NULL;
    GError *error = NULL;
    gint fd = g_file_open_tmp("pcv-alert-source-XXXXXX", &path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    close(fd);

    gchar *source = g_strdup_printf(
        "[alert]\n"
        "enabled=true\n"
        "cpu_warn=%s\n"
        "webhook_secret=must-never-appear-from-source\n",
        raw_cpu_warn);
    g_assert_true(g_file_set_contents(path, source, -1, &error));
    g_assert_no_error(error);
    g_free(source);

    const gchar *prior_path = g_getenv("PCV_CONFIG_PATH");
    gchar *prior_path_copy = g_strdup(prior_path);
    g_setenv("PCV_CONFIG_PATH", path, TRUE);
    pcv_config_init();
    if (prior_path_copy) {
        g_setenv("PCV_CONFIG_PATH", prior_path_copy, TRUE);
    } else {
        g_unsetenv("PCV_CONFIG_PATH");
    }

    pcv_alert_engine_init();
    JsonObject *actual = pcv_alert_engine_get_config();
    g_assert_false(json_object_get_boolean_member(actual, "enabled"));
    g_assert_cmpint(json_object_get_int_member(actual, "cpu_warn"), ==, 80);
    g_assert_false(json_object_get_boolean_member(actual,
                                                  "daemon_config_valid"));
    g_assert_cmpstr(json_object_get_string_member(actual,
                                                  "daemon_config_error"),
                    ==, "invalid_alert_config");
    g_assert_false(json_object_has_member(actual, "webhook_secret"));
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(node, actual);
    gchar *serialized = json_to_string(node, FALSE);
    g_assert_null(strstr(serialized, "must-never-appear-from-source"));
    g_free(serialized);
    json_node_free(node);
    json_object_unref(actual);

    JsonObject *runtime = json_object_new();
    json_object_set_int_member(runtime, "cpu_warn", 81);
    g_assert_cmpint(pcv_alert_engine_set_config(runtime, 1),
                    ==, PCV_ALERT_CONFIG_SET_OK);
    json_object_unref(runtime);
    g_assert_cmpint(
        pcv_alert_engine_load_daemon_config(PCV_ALERT_CONFIG_SOURCE_RELOAD),
        ==, PCV_ALERT_CONFIG_SET_INVALID);

    actual = pcv_alert_engine_get_config();
    g_assert_cmpint(json_object_get_int_member(actual, "cpu_warn"), ==, 81);
    g_assert_cmpint(json_object_get_int_member(actual, "config_revision"),
                    ==, 2);
    g_assert_false(json_object_get_boolean_member(actual,
                                                  "daemon_config_valid"));
    g_assert_false(json_object_has_member(actual, "webhook_secret"));
    json_object_unref(actual);
    pcv_alert_engine_shutdown();

    pcv_config_shutdown();
    g_free(prior_path_copy);
    g_assert_cmpint(g_remove(path), ==, 0);
    g_free(path);
}

static void
test_alert_config_invalid_numeric_source_uses_safe_defaults(void)
{
    assert_invalid_numeric_source_uses_safe_defaults("abc");
}

static void
test_alert_config_empty_numeric_source_uses_safe_defaults(void)
{
    assert_invalid_numeric_source_uses_safe_defaults("");
}

static gchar *write_alert_reload_fixture(const gchar *source);

static void
assert_invalid_raw_source_uses_safe_defaults(const gchar *key,
                                             const gchar *raw_value)
{
    gchar *source = g_strdup_printf(
        "[alert]\n"
        "enabled=%s\n"
        "webhook_format=%s\n"
        "cpu_warn=80\n",
        g_strcmp0(key, "enabled") == 0 ? raw_value : "false",
        g_strcmp0(key, "webhook_format") == 0 ? raw_value : "generic");
    gchar *path = write_alert_reload_fixture(source);
    g_free(source);

    const gchar *prior_path = g_getenv("PCV_CONFIG_PATH");
    gchar *prior_path_copy = g_strdup(prior_path);
    g_setenv("PCV_CONFIG_PATH", path, TRUE);
    pcv_config_init();
    pcv_alert_engine_init();

    JsonObject *actual = pcv_alert_engine_get_config();
    g_assert_false(json_object_get_boolean_member(actual, "enabled"));
    g_assert_cmpstr(json_object_get_string_member(actual, "webhook_format"),
                    ==, "generic");
    g_assert_false(json_object_get_boolean_member(actual,
                                                  "daemon_config_valid"));
    g_assert_cmpstr(json_object_get_string_member(actual,
                                                  "daemon_config_error"),
                    ==, "invalid_alert_config");
    g_assert_cmpint(json_object_get_int_member(actual, "config_revision"), ==,
                    1);
    json_object_unref(actual);

    JsonObject *runtime = json_object_new();
    json_object_set_int_member(runtime, "cpu_warn", 81);
    g_assert_cmpint(pcv_alert_engine_set_config(runtime, 1),
                    ==, PCV_ALERT_CONFIG_SET_OK);
    json_object_unref(runtime);
    g_assert_cmpint(pcv_alert_engine_reload_daemon_config(), ==,
                    PCV_ALERT_CONFIG_SET_INVALID);

    actual = pcv_alert_engine_get_config();
    g_assert_cmpint(json_object_get_int_member(actual, "cpu_warn"), ==, 81);
    g_assert_cmpint(json_object_get_int_member(actual, "config_revision"), ==,
                    2);
    g_assert_false(json_object_get_boolean_member(actual,
                                                  "daemon_config_valid"));
    g_assert_cmpstr(json_object_get_string_member(actual,
                                                  "daemon_config_error"),
                    ==, "invalid_alert_config");
    json_object_unref(actual);

    pcv_alert_engine_shutdown();
    pcv_config_shutdown();
    if (prior_path_copy) {
        g_setenv("PCV_CONFIG_PATH", prior_path_copy, TRUE);
    } else {
        g_unsetenv("PCV_CONFIG_PATH");
    }
    g_free(prior_path_copy);
    g_assert_cmpint(g_remove(path), ==, 0);
    g_free(path);
}

static void
test_alert_config_empty_enabled_source_uses_safe_defaults(void)
{
    assert_invalid_raw_source_uses_safe_defaults("enabled", "");
}

static void
test_alert_config_empty_format_source_uses_safe_defaults(void)
{
    assert_invalid_raw_source_uses_safe_defaults("webhook_format", "");
}

static void
test_alert_config_whitespace_enabled_source_uses_safe_defaults(void)
{
    assert_invalid_raw_source_uses_safe_defaults("enabled", "   ");
}

static void
test_alert_config_whitespace_format_source_uses_safe_defaults(void)
{
    assert_invalid_raw_source_uses_safe_defaults("webhook_format", "   ");
}

static gchar *
write_alert_reload_fixture(const gchar *source)
{
    gchar *path = NULL;
    GError *error = NULL;
    gint fd = g_file_open_tmp("pcv-alert-reload-XXXXXX", &path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    g_assert_true(g_file_set_contents(path, source, -1, &error));
    g_assert_no_error(error);
    return path;
}

static void
test_alert_config_disk_reload_updates_once(void)
{
    gchar *path = write_alert_reload_fixture(
        "[alert]\n"
        "enabled=false\n"
        "cpu_warn=80\n");
    const gchar *prior_path = g_getenv("PCV_CONFIG_PATH");
    gchar *prior_path_copy = g_strdup(prior_path);
    g_setenv("PCV_CONFIG_PATH", path, TRUE);
    pcv_config_init();
    pcv_alert_engine_init();

    JsonObject *actual = pcv_alert_engine_get_config();
    g_assert_cmpint(json_object_get_int_member(actual, "cpu_warn"), ==, 80);
    g_assert_cmpint(json_object_get_int_member(actual, "config_revision"), ==,
                    1);
    json_object_unref(actual);

    g_assert_true(g_file_set_contents(
        path,
        "[alert]\n"
        "enabled=false\n"
        "cpu_warn=82\n",
        -1, NULL));
    g_assert_cmpint(pcv_alert_engine_reload_daemon_config(), ==,
                    PCV_ALERT_CONFIG_SET_OK);

    actual = pcv_alert_engine_get_config();
    g_assert_cmpint(json_object_get_int_member(actual, "cpu_warn"), ==, 82);
    g_assert_cmpint(json_object_get_int_member(actual, "config_revision"), ==,
                    2);
    g_assert_true(json_object_get_boolean_member(actual,
                                                 "daemon_config_valid"));
    json_object_unref(actual);

    pcv_alert_engine_shutdown();
    pcv_config_shutdown();
    if (prior_path_copy) {
        g_setenv("PCV_CONFIG_PATH", prior_path_copy, TRUE);
    } else {
        g_unsetenv("PCV_CONFIG_PATH");
    }
    g_free(prior_path_copy);
    g_assert_cmpint(g_remove(path), ==, 0);
    g_free(path);
}

static void
test_alert_config_disk_reload_failure_preserves_runtime(void)
{
    gchar *path = write_alert_reload_fixture(
        "[alert]\n"
        "enabled=false\n"
        "cpu_warn=81\n");
    const gchar *prior_path = g_getenv("PCV_CONFIG_PATH");
    gchar *prior_path_copy = g_strdup(prior_path);
    g_setenv("PCV_CONFIG_PATH", path, TRUE);
    pcv_config_init();
    pcv_alert_engine_init();

    g_assert_true(g_file_set_contents(path, "[alert\ncpu_warn=82\n", -1,
                                      NULL));
    g_assert_cmpint(pcv_alert_engine_reload_daemon_config(), ==,
                    PCV_ALERT_CONFIG_SET_INVALID);

    JsonObject *actual = pcv_alert_engine_get_config();
    g_assert_cmpint(json_object_get_int_member(actual, "cpu_warn"), ==, 81);
    g_assert_cmpint(json_object_get_int_member(actual, "config_revision"), ==,
                    1);
    g_assert_false(json_object_get_boolean_member(actual,
                                                  "daemon_config_valid"));
    g_assert_cmpstr(json_object_get_string_member(actual,
                                                  "daemon_config_error"),
                    ==, "invalid_alert_config");
    json_object_unref(actual);

    pcv_alert_engine_shutdown();
    pcv_config_shutdown();
    if (prior_path_copy) {
        g_setenv("PCV_CONFIG_PATH", prior_path_copy, TRUE);
    } else {
        g_unsetenv("PCV_CONFIG_PATH");
    }
    g_free(prior_path_copy);
    g_assert_cmpint(g_remove(path), ==, 0);
    g_free(path);
}

static void
test_alert_daemon_set_invalid_value_is_never_published(void)
{
    gchar *path = write_alert_reload_fixture(
        "[alert]\n"
        "enabled=false\n"
        "cpu_warn=80\n");
    const gchar *prior_path = g_getenv("PCV_CONFIG_PATH");
    gchar *prior_path_copy = g_strdup(prior_path);
    g_setenv("PCV_CONFIG_PATH", path, TRUE);
    pcv_config_init();
    pcv_alert_engine_init();

    gchar *before_file = NULL;
    gsize before_file_len = 0;
    g_assert_true(g_file_get_contents(
        path, &before_file, &before_file_len, NULL));
    JsonObject *before = pcv_alert_engine_get_config();
    gint64 before_revision =
        json_object_get_int_member(before, "config_revision");
    g_assert_true(json_object_get_boolean_member(
        before, "daemon_config_valid"));
    json_object_unref(before);

    GError *error = NULL;
    g_atomic_int_set(&g_daemon_config_publish_count, 0);
    g_assert_cmpint(
        pcv_daemon_config_set_value(
            path, "alert", "cpu_warn", "invalid", &error),
        ==, PCV_ALERT_CONFIG_SET_INVALID);
    g_assert_no_error(error);
    g_assert_cmpint(
        g_atomic_int_get(&g_daemon_config_publish_count), ==, 0);

    gchar *after_file = NULL;
    gsize after_file_len = 0;
    g_assert_true(g_file_get_contents(
        path, &after_file, &after_file_len, NULL));
    g_assert_cmpuint(after_file_len, ==, before_file_len);
    g_assert_cmpmem(
        after_file, after_file_len, before_file, before_file_len);
    g_assert_cmpint(
        pcv_config_get_int("alert", "cpu_warn", -1), ==, 80);

    JsonObject *after = pcv_alert_engine_get_config();
    g_assert_cmpint(
        json_object_get_int_member(after, "config_revision"),
        ==, before_revision);
    g_assert_cmpint(
        json_object_get_int_member(after, "cpu_warn"), ==, 80);
    g_assert_true(json_object_get_boolean_member(
        after, "daemon_config_valid"));
    json_object_unref(after);

    g_free(after_file);
    g_free(before_file);
    pcv_alert_engine_shutdown();
    pcv_config_shutdown();
    if (prior_path_copy) {
        g_setenv("PCV_CONFIG_PATH", prior_path_copy, TRUE);
    } else {
        g_unsetenv("PCV_CONFIG_PATH");
    }
    g_free(prior_path_copy);
    g_assert_cmpint(g_remove(path), ==, 0);
    g_free(path);
}

typedef struct {
    const gchar *path;
    PcvAlertConfigSetResult result;
    GError *error;
} DaemonConfigSetThreadContext;

static gpointer
daemon_config_set_thread(gpointer data)
{
    DaemonConfigSetThreadContext *context = data;
    context->result = pcv_daemon_config_set_value(
        context->path, "alert", "cpu_warn", "81", &context->error);
    return NULL;
}

static void
test_alert_daemon_set_validates_before_publish(void)
{
    gchar *path = write_alert_reload_fixture(
        "[alert]\n"
        "enabled=false\n"
        "cpu_warn=80\n");
    const gchar *prior_path = g_getenv("PCV_CONFIG_PATH");
    gchar *prior_path_copy = g_strdup(prior_path);
    g_setenv("PCV_CONFIG_PATH", path, TRUE);
    pcv_config_init();
    pcv_alert_engine_init();

    gchar *before_file = NULL;
    gsize before_file_len = 0;
    g_assert_true(g_file_get_contents(
        path, &before_file, &before_file_len, NULL));
    JsonObject *before = pcv_alert_engine_get_config();
    gint64 before_revision =
        json_object_get_int_member(before, "config_revision");
    json_object_unref(before);

    if (g_once_init_enter(&g_daemon_publish_hook_sync_initialized)) {
        g_mutex_init(&g_daemon_publish_hook_mu);
        g_cond_init(&g_daemon_publish_hook_cond);
        g_once_init_leave(&g_daemon_publish_hook_sync_initialized, 1);
    }
    g_mutex_lock(&g_daemon_publish_hook_mu);
    g_daemon_publish_hook_enabled = TRUE;
    g_daemon_publish_hook_entered = FALSE;
    g_daemon_publish_hook_release = FALSE;
    g_mutex_unlock(&g_daemon_publish_hook_mu);
    g_atomic_int_set(&g_daemon_config_publish_count, 0);

    DaemonConfigSetThreadContext context = {
        .path = path,
        .result = PCV_ALERT_CONFIG_SET_INVALID,
        .error = NULL,
    };
    GThread *writer = g_thread_new(
        "daemon-config-set", daemon_config_set_thread, &context);

    g_mutex_lock(&g_daemon_publish_hook_mu);
    gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (!g_daemon_publish_hook_entered) {
        if (!g_cond_wait_until(
                &g_daemon_publish_hook_cond,
                &g_daemon_publish_hook_mu,
                deadline)) {
            break;
        }
    }
    gboolean reached_publish_barrier =
        g_daemon_publish_hook_entered;
    g_mutex_unlock(&g_daemon_publish_hook_mu);

    gchar *barrier_file = NULL;
    gsize barrier_file_len = 0;
    gboolean barrier_file_read = g_file_get_contents(
        path, &barrier_file, &barrier_file_len, NULL);
    gint barrier_common_value =
        pcv_config_get_int("alert", "cpu_warn", -1);
    JsonObject *barrier = pcv_alert_engine_get_config();
    gint64 barrier_revision =
        json_object_get_int_member(barrier, "config_revision");
    gint barrier_runtime_value =
        json_object_get_int_member(barrier, "cpu_warn");
    gboolean barrier_source_valid =
        json_object_get_boolean_member(barrier, "daemon_config_valid");
    json_object_unref(barrier);

    g_mutex_lock(&g_daemon_publish_hook_mu);
    g_daemon_publish_hook_release = TRUE;
    g_cond_broadcast(&g_daemon_publish_hook_cond);
    g_mutex_unlock(&g_daemon_publish_hook_mu);
    g_thread_join(writer);
    g_mutex_lock(&g_daemon_publish_hook_mu);
    g_daemon_publish_hook_enabled = FALSE;
    g_mutex_unlock(&g_daemon_publish_hook_mu);

    g_assert_true(reached_publish_barrier);
    g_assert_true(barrier_file_read);
    g_assert_cmpuint(barrier_file_len, ==, before_file_len);
    g_assert_cmpmem(
        barrier_file, barrier_file_len, before_file, before_file_len);
    g_assert_cmpint(barrier_common_value, ==, 80);
    g_assert_cmpint(barrier_revision, ==, before_revision);
    g_assert_cmpint(barrier_runtime_value, ==, 80);
    g_assert_true(barrier_source_valid);
    g_assert_no_error(context.error);
    g_assert_cmpint(context.result, ==, PCV_ALERT_CONFIG_SET_OK);
    g_assert_cmpint(
        g_atomic_int_get(&g_daemon_config_publish_count), ==, 1);
    g_assert_cmpint(
        pcv_config_get_int("alert", "cpu_warn", -1), ==, 81);
    JsonObject *after = pcv_alert_engine_get_config();
    g_assert_cmpint(
        json_object_get_int_member(after, "config_revision"),
        ==, before_revision + 1);
    g_assert_cmpint(
        json_object_get_int_member(after, "cpu_warn"), ==, 81);
    json_object_unref(after);

    g_free(barrier_file);
    g_free(before_file);
    pcv_alert_engine_shutdown();
    pcv_config_shutdown();
    if (prior_path_copy) {
        g_setenv("PCV_CONFIG_PATH", prior_path_copy, TRUE);
    } else {
        g_unsetenv("PCV_CONFIG_PATH");
    }
    g_free(prior_path_copy);
    g_assert_cmpint(g_remove(path), ==, 0);
    g_free(path);
}

static void
test_non_alert_daemon_set_survives_invalid_alert_source(void)
{
    gchar *path = write_alert_reload_fixture(
        "[alert]\n"
        "enabled=true\n"
        "cpu_warn=invalid\n"
        "[storage]\n"
        "review_marker=before\n");
    const gchar *prior_path = g_getenv("PCV_CONFIG_PATH");
    gchar *prior_path_copy = g_strdup(prior_path);
    g_setenv("PCV_CONFIG_PATH", path, TRUE);
    pcv_config_init();
    pcv_alert_engine_init();

    JsonObject *actual = pcv_alert_engine_get_config();
    g_assert_false(json_object_get_boolean_member(actual,
                                                   "daemon_config_valid"));
    g_assert_false(json_object_get_boolean_member(actual, "enabled"));
    g_assert_cmpint(json_object_get_int_member(actual, "cpu_warn"), ==, 80);
    json_object_unref(actual);
    g_assert_cmpstr(pcv_config_get_string("storage", "review_marker", ""),
                    ==, "before");

    GError *error = NULL;
    g_assert_cmpint(
        pcv_daemon_config_set_value(
            path, "storage", "review_marker", "after", &error),
        ==, PCV_ALERT_CONFIG_SET_OK);
    g_assert_no_error(error);

      
                                                            
                                                              
                                   
       
    g_assert_false(pcv_daemon_config_set_alert_reload_is_fatal(
        "storage", PCV_ALERT_CONFIG_SET_INVALID));
    g_assert_false(pcv_daemon_config_set_alert_reload_is_fatal(
        "container", PCV_ALERT_CONFIG_SET_INVALID));
    g_assert_false(pcv_daemon_config_set_alert_reload_is_fatal(
        "backup", PCV_ALERT_CONFIG_SET_INVALID));
    g_assert_true(pcv_daemon_config_set_alert_reload_is_fatal(
        "alert", PCV_ALERT_CONFIG_SET_INVALID));
    g_assert_cmpstr(pcv_config_get_string("storage", "review_marker", ""),
                    ==, "after");

    actual = pcv_alert_engine_get_config();
    g_assert_false(json_object_get_boolean_member(actual,
                                                   "daemon_config_valid"));
    g_assert_cmpstr(json_object_get_string_member(actual,
                                                  "daemon_config_error"),
                    ==, "invalid_alert_config");
    g_assert_false(json_object_get_boolean_member(actual, "enabled"));
    g_assert_cmpint(json_object_get_int_member(actual, "cpu_warn"), ==, 80);
    json_object_unref(actual);

    pcv_alert_engine_shutdown();
    pcv_config_shutdown();
    if (prior_path_copy) {
        g_setenv("PCV_CONFIG_PATH", prior_path_copy, TRUE);
    } else {
        g_unsetenv("PCV_CONFIG_PATH");
    }
    g_free(prior_path_copy);
    g_assert_cmpint(g_remove(path), ==, 0);
    g_free(path);
}

static void
test_alert_config_secret_is_never_returned(void)
{
    engine_setup();
    JsonObject *patch = json_object_new();
    json_object_set_string_member(
        patch, "webhook_secret", "secret-response-sentinel");
    g_assert_cmpint(
        pcv_alert_engine_set_config(patch, config_revision()),
        ==, PCV_ALERT_CONFIG_SET_OK);
    json_object_unref(patch);

    JsonObject *actual = pcv_alert_engine_get_config();
    g_assert_false(json_object_has_member(actual, "webhook_secret"));
    g_assert_true(json_object_get_boolean_member(
        actual, "webhook_secret_configured"));
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(node, actual);
    gchar *serialized = json_to_string(node, FALSE);
    g_assert_null(strstr(serialized, "secret-response-sentinel"));
    g_free(serialized);
    json_node_free(node);
    json_object_unref(actual);

    gint64 revision = config_revision();
    JsonObject *read_only = json_object_new();
    json_object_set_boolean_member(
        read_only, "webhook_secret_configured", FALSE);
    g_assert_cmpint(
        pcv_alert_engine_set_config(read_only, revision),
        ==, PCV_ALERT_CONFIG_SET_INVALID);
    json_object_unref(read_only);

    actual = pcv_alert_engine_get_config();
    g_assert_cmpint(json_object_get_int_member(actual, "config_revision"),
                    ==, revision);
    g_assert_true(json_object_get_boolean_member(
        actual, "webhook_secret_configured"));
    json_object_unref(actual);
    engine_teardown();
}

static void
test_alert_config_snapshot_secret_is_wiped(void)
{
    engine_setup();
    gint before = g_atomic_int_get(&g_snapshot_wipe_count);
    JsonObject *actual = pcv_alert_engine_get_config();
    json_object_unref(actual);
    g_assert_cmpint(g_atomic_int_get(&g_snapshot_wipe_count), >, before);
    engine_teardown();
}

static void
test_alert_config_secret_tail_is_zero_after_shorter_updates(void)
{
    engine_setup();
    gchar long_secret[121];
    memset(long_secret, 'S', sizeof(long_secret) - 1);
    long_secret[sizeof(long_secret) - 1] = '\0';

    JsonObject *patch = json_object_new();
    json_object_set_string_member(patch, "webhook_secret", long_secret);
    g_assert_cmpint(pcv_alert_engine_set_config(patch, config_revision()),
                    ==, PCV_ALERT_CONFIG_SET_OK);
    json_object_unref(patch);

    patch = json_object_new();
    json_object_set_string_member(patch, "webhook_secret", "x");
    g_assert_cmpint(pcv_alert_engine_set_config(patch, config_revision()),
                    ==, PCV_ALERT_CONFIG_SET_OK);
    json_object_unref(patch);
    g_assert_true(pcv_alert_engine_test_secret_buffer_tail_zero());

    patch = json_object_new();
    json_object_set_string_member(patch, "webhook_secret", "");
    g_assert_cmpint(pcv_alert_engine_set_config(patch, config_revision()),
                    ==, PCV_ALERT_CONFIG_SET_OK);
    json_object_unref(patch);
    g_assert_true(pcv_alert_engine_test_secret_buffer_tail_zero());
    engine_teardown();
}

static void
test_alert_config_source_warning_lifecycle(void)
{
    pcv_alert_engine_init();
    JsonObject *invalid = complete_daemon_config(FALSE);
    json_object_set_int_member(invalid, "eval_period", 4);
    g_assert_cmpint(
        pcv_alert_engine_apply_daemon_config(
            invalid, PCV_ALERT_CONFIG_SOURCE_STARTUP),
        ==, PCV_ALERT_CONFIG_SET_INVALID);
    json_object_unref(invalid);

    JsonObject *patch = json_object_new();
    json_object_set_int_member(patch, "cpu_warn", 81);
    g_assert_cmpint(pcv_alert_engine_set_config(patch, 1),
                    ==, PCV_ALERT_CONFIG_SET_OK);
    json_object_unref(patch);
    JsonObject *actual = pcv_alert_engine_get_config();
    g_assert_false(json_object_get_boolean_member(actual,
                                                  "daemon_config_valid"));
    g_assert_cmpint(json_object_get_int_member(actual, "config_revision"), ==,
                    2);
    json_object_unref(actual);

    JsonObject *reload = complete_daemon_config(FALSE);
    g_assert_cmpint(
        pcv_alert_engine_apply_daemon_config(
            reload, PCV_ALERT_CONFIG_SOURCE_RELOAD),
        ==, PCV_ALERT_CONFIG_SET_OK);
    json_object_unref(reload);
    actual = pcv_alert_engine_get_config();
    g_assert_true(json_object_get_boolean_member(actual,
                                                 "daemon_config_valid"));
    g_assert_cmpstr(json_object_get_string_member(actual,
                                                  "daemon_config_error"),
                    ==, "");
    g_assert_cmpint(json_object_get_int_member(actual, "config_revision"), ==,
                    3);
    json_object_unref(actual);
    pcv_alert_engine_shutdown();
}

static void
test_alert_config_invalid_reload_preserves_runtime(void)
{
    engine_setup();
    gint64 revision = config_revision();
    JsonObject *invalid = complete_daemon_config(TRUE);
    json_object_set_int_member(invalid, "disk_warn", 90);
    json_object_set_int_member(invalid, "disk_crit", 90);
    json_object_set_string_member(invalid, "webhook_secret",
                                  "reload-secret-sentinel");
    g_assert_cmpint(
        pcv_alert_engine_apply_daemon_config(
            invalid, PCV_ALERT_CONFIG_SOURCE_RELOAD),
        ==, PCV_ALERT_CONFIG_SET_INVALID);
    json_object_unref(invalid);

    JsonObject *actual = pcv_alert_engine_get_config();
    g_assert_false(json_object_get_boolean_member(actual, "enabled"));
    g_assert_cmpint(json_object_get_int_member(actual, "disk_warn"), ==, 80);
    g_assert_cmpint(json_object_get_int_member(actual, "config_revision"), ==,
                    revision);
    g_assert_false(json_object_get_boolean_member(actual,
                                                  "daemon_config_valid"));
    const gchar *error =
        json_object_get_string_member(actual, "daemon_config_error");
    g_assert_cmpstr(error, ==, "invalid_alert_config");
    g_assert_null(strstr(error, "reload-secret-sentinel"));
    json_object_unref(actual);
    engine_teardown();
}

typedef struct {
    gint iterations;
} ConfigHammer;

static gpointer
config_writer_thread(gpointer data)
{
    ConfigHammer *hammer = data;
    for (gint i = 0; i < hammer->iterations; i++) {
        gboolean use_a = (i % 2) == 0;
        JsonObject *patch = json_object_new();
        json_object_set_int_member(patch, "cpu_warn", use_a ? 10 : 50);
        json_object_set_int_member(patch, "cpu_crit", use_a ? 20 : 60);
        json_object_set_int_member(patch, "mem_warn", use_a ? 30 : 70);
        json_object_set_int_member(patch, "mem_crit", use_a ? 40 : 80);
        json_object_set_string_member(
            patch, "webhook_url",
            use_a ? "https://a.example.com/hook" : "https://b.example.com/hook");
        json_object_set_string_member(patch, "webhook_format",
                                      use_a ? "slack" : "generic");
        PcvAlertConfigSetResult result =
            pcv_alert_engine_set_config(patch, config_revision());
        g_assert_cmpint(result, ==, PCV_ALERT_CONFIG_SET_OK);
        json_object_unref(patch);
    }
    return NULL;
}

static void
test_alert_config_concurrent_snapshot_consistent(void)
{
    engine_setup();
    JsonObject *initial = json_object_new();
    json_object_set_int_member(initial, "cpu_warn", 10);
    json_object_set_int_member(initial, "cpu_crit", 20);
    json_object_set_int_member(initial, "mem_warn", 30);
    json_object_set_int_member(initial, "mem_crit", 40);
    json_object_set_string_member(initial, "webhook_url",
                                  "https://a.example.com/hook");
    json_object_set_string_member(initial, "webhook_format", "slack");
    g_assert_cmpint(pcv_alert_engine_set_config(initial, config_revision()),
                    ==, PCV_ALERT_CONFIG_SET_OK);
    json_object_unref(initial);

    ConfigHammer hammer = {.iterations = 1000};
    GThread *writer = g_thread_new("alert-config-writer",
                                   config_writer_thread, &hammer);
    for (gint i = 0; i < hammer.iterations; i++) {
        JsonObject *snapshot = pcv_alert_engine_get_config();
        gint cpu_warn = (gint)json_object_get_int_member(snapshot, "cpu_warn");
        gint cpu_crit = (gint)json_object_get_int_member(snapshot, "cpu_crit");
        gint mem_warn = (gint)json_object_get_int_member(snapshot, "mem_warn");
        gint mem_crit = (gint)json_object_get_int_member(snapshot, "mem_crit");
        const gchar *url =
            json_object_get_string_member(snapshot, "webhook_url");
        const gchar *format =
            json_object_get_string_member(snapshot, "webhook_format");
        gboolean tuple_a = cpu_warn == 10 && cpu_crit == 20
            && mem_warn == 30 && mem_crit == 40
            && g_strcmp0(url, "https://a.example.com/hook") == 0
            && g_strcmp0(format, "slack") == 0;
        gboolean tuple_b = cpu_warn == 50 && cpu_crit == 60
            && mem_warn == 70 && mem_crit == 80
            && g_strcmp0(url, "https://b.example.com/hook") == 0
            && g_strcmp0(format, "generic") == 0;
        g_assert_true(tuple_a || tuple_b);
        json_object_unref(snapshot);
    }
    g_thread_join(writer);
    engine_teardown();
}

typedef struct {
    gint64 expected_revision;
    PcvAlertConfigSetResult result;
} AtomicCommitWriter;

typedef struct {
    GMutex mu;
    GCond cond;
    gboolean completed;
} AtomicCommitEvaluator;

static gpointer
atomic_commit_writer_thread(gpointer data)
{
    AtomicCommitWriter *writer = data;
    JsonObject *patch = json_object_new();
    json_object_set_int_member(patch, "cpu_warn", 70);
    writer->result = pcv_alert_engine_set_config(
        patch, writer->expected_revision);
    json_object_unref(patch);
    return NULL;
}

static gpointer
atomic_commit_evaluator_thread(gpointer data)
{
    AtomicCommitEvaluator *evaluator = data;
    pcv_alert_engine_test_eval_cpu_once(75.0);
    g_mutex_lock(&evaluator->mu);
    evaluator->completed = TRUE;
    g_cond_broadcast(&evaluator->cond);
    g_mutex_unlock(&evaluator->mu);
    return NULL;
}

static void
test_alert_config_commit_and_episode_reset_are_atomic(void)
{
    engine_setup();
    pcv_alert_engine_test_seed_cpu_warn_elapsed(10);
    gint history_before = alert_history_count();

    ensure_config_commit_hook_sync();
    g_config_commit_hook_enabled = TRUE;
    g_config_commit_hook_entered = FALSE;
    g_config_commit_hook_release = FALSE;

    AtomicCommitWriter writer = {
        .expected_revision = config_revision(),
        .result = PCV_ALERT_CONFIG_SET_INVALID,
    };
    GThread *writer_thread = g_thread_new(
        "alert-config-writer", atomic_commit_writer_thread, &writer);

    g_mutex_lock(&g_config_commit_hook_mu);
    while (!g_config_commit_hook_entered) {
        gint64 deadline = g_get_monotonic_time() + G_TIME_SPAN_SECOND;
        if (!g_cond_wait_until(&g_config_commit_hook_cond,
                               &g_config_commit_hook_mu, deadline)) {
            break;
        }
    }
    gboolean hook_entered = g_config_commit_hook_entered;
    g_mutex_unlock(&g_config_commit_hook_mu);

    AtomicCommitEvaluator evaluator = {0};
    g_mutex_init(&evaluator.mu);
    g_cond_init(&evaluator.cond);
    GThread *evaluator_thread = g_thread_new(
        "alert-config-evaluator", atomic_commit_evaluator_thread, &evaluator);

    g_mutex_lock(&evaluator.mu);
    if (!evaluator.completed) {
        gint64 deadline = g_get_monotonic_time() + 100 * G_TIME_SPAN_MILLISECOND;
        g_cond_wait_until(&evaluator.cond, &evaluator.mu, deadline);
    }
    gboolean completed_while_commit_paused = evaluator.completed;
    g_mutex_unlock(&evaluator.mu);

    g_mutex_lock(&g_config_commit_hook_mu);
    g_config_commit_hook_release = TRUE;
    g_cond_broadcast(&g_config_commit_hook_cond);
    g_mutex_unlock(&g_config_commit_hook_mu);

    g_thread_join(writer_thread);
    g_thread_join(evaluator_thread);
    g_config_commit_hook_enabled = FALSE;

    g_assert_true(hook_entered);
    g_assert_false(completed_while_commit_paused);
    g_assert_cmpint(writer.result, ==, PCV_ALERT_CONFIG_SET_OK);
    g_assert_cmpint(alert_history_count(), ==, history_before);

    g_cond_clear(&evaluator.cond);
    g_mutex_clear(&evaluator.mu);
    engine_teardown();
}

static gpointer
history_writer_thread(gpointer data)
{
    gint iterations = GPOINTER_TO_INT(data);
    for (gint i = 0; i < iterations; i++) {
        gchar *message = g_strdup_printf("event-%d", i);
        pcv_alert_fire_event("history-hammer", FALSE, i, message);
        g_free(message);
    }
    return NULL;
}

static void
test_alert_history_concurrent_snapshot(void)
{
    engine_setup();
    GThread *writer = g_thread_new("alert-history-writer",
                                   history_writer_thread,
                                   GINT_TO_POINTER(1000));
    for (gint i = 0; i < 1000; i++) {
        JsonArray *history = pcv_alert_engine_get_history();
        guint length = json_array_get_length(history);
        g_assert_cmpuint(length, <=, 1000);
        if (length > 0) {
            JsonObject *last =
                json_array_get_object_element(history, length - 1);
            gint64 alert_id =
                json_object_get_int_member(last, "alert_id");
            g_assert_cmpint(alert_id, >=, 1);
            pcv_alert_acknowledge(alert_id);
        }
        json_array_unref(history);
    }
    g_thread_join(writer);
    JsonArray *history = pcv_alert_engine_get_history();
    g_assert_cmpuint(json_array_get_length(history), ==, 1000);
    json_array_unref(history);
    engine_teardown();
}

static gpointer
vm_webhook_writer_thread(gpointer data)
{
    gint iterations = GPOINTER_TO_INT(data);
    for (gint i = 0; i < iterations; i++) {
        pcv_alert_set_vm_webhook("vm-race",
                                 (i % 2) ? "https://vm.example/a" : NULL);
    }
    return NULL;
}

static void
test_alert_webhook_concurrent_route_lookup(void)
{
    engine_setup();
    GThread *writer = g_thread_new("alert-vm-webhook-writer",
                                   vm_webhook_writer_thread,
                                   GINT_TO_POINTER(5000));
    for (gint i = 0; i < 5000; i++) {
        gchar *url = pcv_alert_engine_test_dup_vm_webhook("vm-race");
        g_assert_true(url == NULL
                      || g_strcmp0(url, "https://vm.example/a") == 0);
        g_free(url);
    }
    g_thread_join(writer);
    pcv_alert_set_vm_webhook("vm-race", NULL);
    engine_teardown();
}

static gint
alert_history_count(void)
{
    JsonArray *history = pcv_alert_engine_get_history();
    gint count = (gint)json_array_get_length(history);
    json_array_unref(history);
    return count;
}

static gboolean
wait_for_telemetry_calls(gint minimum, gint timeout_msec)
{
    gint waited = 0;
    while (waited < timeout_msec) {
        if (pcv_test_alert_telemetry_call_count() >= minimum) return TRUE;
        g_usleep(10 * 1000);
        waited += 10;
    }
    return pcv_test_alert_telemetry_call_count() >= minimum;
}

static JsonArray *
one_composite_rule(void)
{
    JsonArray *rules = json_array_new();
    JsonObject *rule = json_object_new();
    json_object_set_boolean_member(rule, "active", TRUE);
    json_object_set_string_member(rule, "metric_a", "CPU");
    json_object_set_double_member(rule, "thresh_a", 10.0);
    json_object_set_string_member(rule, "op", "AND");
    json_object_set_string_member(rule, "metric_b", "Memory");
    json_object_set_double_member(rule, "thresh_b", 10.0);
    json_object_set_string_member(rule, "level", "WARN");
    json_array_add_object_element(rules, rule);
    return rules;
}

static void
test_alert_config_disable_stops_evaluation(void)
{
    engine_setup();
    pcv_test_alert_telemetry_set(TRUE, 99.0, 99.0);

    JsonObject *enable = json_object_new();
    json_object_set_boolean_member(enable, "enabled", TRUE);
    json_object_set_int_member(enable, "cpu_warn", 10);
    json_object_set_int_member(enable, "cpu_crit", 20);
    json_object_set_int_member(enable, "mem_warn", 10);
    json_object_set_int_member(enable, "mem_crit", 20);
    json_object_set_int_member(enable, "eval_period", 5);
    json_object_set_int_member(enable, "dedup_window", 0);
    json_object_set_array_member(enable, "composite_rules",
                                 one_composite_rule());
    g_assert_cmpint(pcv_alert_engine_set_config(enable, config_revision()),
                    ==, PCV_ALERT_CONFIG_SET_OK);
    json_object_unref(enable);
    g_assert_true(wait_for_telemetry_calls(1, 2000));

    JsonObject *disable = json_object_new();
    json_object_set_boolean_member(disable, "enabled", FALSE);
    g_assert_cmpint(pcv_alert_engine_set_config(disable, config_revision()),
                    ==, PCV_ALERT_CONFIG_SET_OK);
    json_object_unref(disable);
    gint history_before = alert_history_count();

    pcv_test_alert_telemetry_set(TRUE, 99.0, 99.0);
    g_usleep(6 * G_USEC_PER_SEC);
    g_assert_cmpint(pcv_test_alert_telemetry_call_count(), ==, 0);
    g_assert_cmpint(alert_history_count(), ==, history_before);

    pcv_test_alert_telemetry_set(FALSE, 0.0, 0.0);
    engine_teardown();
}

static void
test_alert_config_disabled_keeps_external_events(void)
{
    engine_setup();
    gint before = alert_history_count();
    pcv_alert_record_security_event("security-disabled", "crit",
                                    "security still records");
    pcv_alert_fire_event("ops-disabled", FALSE, 1.0,
                         "operator event still records");
    JsonArray *history = pcv_alert_engine_get_history();
    g_assert_cmpuint(json_array_get_length(history), ==, before + 2);
    JsonObject *security =
        json_array_get_object_element(history, json_array_get_length(history) - 2);
    JsonObject *ops =
        json_array_get_object_element(history, json_array_get_length(history) - 1);
    g_assert_cmpstr(json_object_get_string_member(security, "metric"), ==,
                    "Security");
    g_assert_cmpstr(json_object_get_string_member(ops, "metric"), ==,
                    "ops-disabled");
    json_array_unref(history);
    engine_teardown();
}

static void
test_alert_config_worker_singleton_across_toggles(void)
{
    engine_setup();
    g_assert_cmpint(pcv_alert_engine_test_worker_start_count(), ==, 1);

    for (gint i = 0; i < 20; i++) {
        JsonObject *patch = json_object_new();
        json_object_set_boolean_member(patch, "enabled", (i % 2) == 0);
        g_assert_cmpint(
            pcv_alert_engine_set_config(patch, config_revision()),
            ==, PCV_ALERT_CONFIG_SET_OK);
        json_object_unref(patch);
    }

    g_assert_cmpint(pcv_alert_engine_test_worker_start_count(), ==, 1);
    engine_teardown();
}

                                                              
                                                           
                                                  
                                                   
                                                                        

                                                     
static gint64
last_alert_id(void)
{
    JsonArray *history = pcv_alert_engine_get_history();
    guint length = json_array_get_length(history);
    g_assert_cmpuint(length, >, 0);
    JsonObject *last = json_array_get_object_element(history, length - 1);
    gint64 alert_id = json_object_get_int_member(last, "alert_id");
    json_array_unref(history);
    return alert_id;
}

                                               
static gboolean
history_acknowledged(gint64 alert_id)
{
    JsonArray *history = pcv_alert_engine_get_history();
    gboolean acknowledged = FALSE;
    for (guint i = 0; i < json_array_get_length(history); i++) {
        JsonObject *rec = json_array_get_object_element(history, i);
        if (json_object_get_int_member(rec, "alert_id") == alert_id) {
            acknowledged = json_object_get_boolean_member(rec, "acknowledged");
            break;
        }
    }
    json_array_unref(history);
    return acknowledged;
}

                                                                 
static void
test_alert_ack_marks_history_record(void)
{
    engine_setup();
    pcv_alert_fire_event("ack-fixture", TRUE, 99.0, "ack me");
    gint64 alert_id = last_alert_id();

    g_assert_false(history_acknowledged(alert_id));
    g_assert_true(pcv_alert_acknowledge(alert_id));
    g_assert_true(history_acknowledged(alert_id));
    engine_teardown();
}

                                                            
static void
test_alert_ack_unknown_id_returns_false(void)
{
    engine_setup();
    pcv_alert_fire_event("ack-fixture", FALSE, 1.0, "present");
    g_assert_false(pcv_alert_acknowledge(G_GINT64_CONSTANT(999999999)));
    engine_teardown();
}

                                                 
static void
test_alert_ack_is_idempotent(void)
{
    engine_setup();
    pcv_alert_fire_event("ack-fixture", TRUE, 42.0, "twice");
    gint64 alert_id = last_alert_id();

    g_assert_true(pcv_alert_acknowledge(alert_id));
    g_assert_true(pcv_alert_acknowledge(alert_id));
    g_assert_true(history_acknowledged(alert_id));
    engine_teardown();
}

                                                             
                                                    
static void
test_alert_ack_requires_operator_role(void)
{
    pcv_dispatcher_init_policy_map();                                              
    g_assert_false(pcv_dispatcher_check_rbac("alert.ack", PCV_ROLE_VIEWER));
    g_assert_true(pcv_dispatcher_check_rbac("alert.ack", PCV_ROLE_OPERATOR));
    g_assert_true(pcv_dispatcher_check_rbac("alert.ack", PCV_ROLE_ADMIN));
}

void
test_alert_basic_register(void)
{
    g_test_add_func("/alert/config/valid", test_alert_config_valid);
    g_test_add_func("/alert/config/rejects_pair_without_partial_mutation",
                    test_alert_config_rejects_pair_without_partial_mutation);
    g_test_add_func("/alert/config/rejects_invalid_type",
                    test_alert_config_rejects_invalid_type);
    g_test_add_func("/alert/config/rejects_empty_patch",
                    test_alert_config_rejects_empty_patch);
    g_test_add_func("/alert/config/boundaries", test_alert_config_boundaries);
    g_test_add_func("/alert/config/rejects_equal_or_reversed_pairs",
                    test_alert_config_rejects_equal_or_reversed_pairs);
    g_test_add_func("/alert/config/rejects_format_url_and_overlong_strings",
                    test_alert_config_rejects_format_url_and_overlong_strings);
    g_test_add_func(
        "/alert/config/canonicalizes_case_insensitive_webhook_scheme",
        test_alert_config_canonicalizes_case_insensitive_webhook_scheme);
    g_test_add_func("/alert/config/rejects_unknown_and_read_only_keys",
                    test_alert_config_rejects_unknown_and_read_only_keys);
    g_test_add_func("/alert/config/valid_partial",
                    test_alert_config_valid_partial);
    g_test_add_func("/alert/config/disabled_loads_values",
                    test_alert_config_disabled_loads_values);
    g_test_add_func("/alert/config/invalid_source_uses_safe_defaults",
                    test_alert_config_invalid_source_uses_safe_defaults);
    g_test_add_func("/alert/config/invalid_numeric_source_uses_safe_defaults",
                    test_alert_config_invalid_numeric_source_uses_safe_defaults);
    g_test_add_func("/alert/config/empty_numeric_source_uses_safe_defaults",
                    test_alert_config_empty_numeric_source_uses_safe_defaults);
    g_test_add_func("/alert/config/empty_enabled_source_uses_safe_defaults",
                    test_alert_config_empty_enabled_source_uses_safe_defaults);
    g_test_add_func("/alert/config/empty_format_source_uses_safe_defaults",
                    test_alert_config_empty_format_source_uses_safe_defaults);
    g_test_add_func("/alert/config/whitespace_enabled_source_uses_safe_defaults",
                    test_alert_config_whitespace_enabled_source_uses_safe_defaults);
    g_test_add_func("/alert/config/whitespace_format_source_uses_safe_defaults",
                    test_alert_config_whitespace_format_source_uses_safe_defaults);
    g_test_add_func("/alert/config/disk_reload_updates_once",
                    test_alert_config_disk_reload_updates_once);
    g_test_add_func("/alert/config/disk_reload_failure_preserves_runtime",
                    test_alert_config_disk_reload_failure_preserves_runtime);
    g_test_add_func(
        "/alert/config/daemon_set_invalid_is_never_published",
        test_alert_daemon_set_invalid_value_is_never_published);
    g_test_add_func(
        "/alert/config/daemon_set_validates_before_publish",
        test_alert_daemon_set_validates_before_publish);
    g_test_add_func(
        "/alert/config/non_alert_set_survives_invalid_alert_source",
        test_non_alert_daemon_set_survives_invalid_alert_source);
    g_test_add_func("/alert/config/secret_is_never_returned",
                    test_alert_config_secret_is_never_returned);
    g_test_add_func("/alert/config/snapshot_secret_is_wiped",
                    test_alert_config_snapshot_secret_is_wiped);
    g_test_add_func("/alert/config/secret_tail_is_zero_after_shorter_updates",
                    test_alert_config_secret_tail_is_zero_after_shorter_updates);
    g_test_add_func("/alert/config/source_warning_lifecycle",
                    test_alert_config_source_warning_lifecycle);
    g_test_add_func("/alert/config/invalid_reload_preserves_runtime",
                    test_alert_config_invalid_reload_preserves_runtime);
    g_test_add_func("/alert/config/concurrent_snapshot_consistent",
                    test_alert_config_concurrent_snapshot_consistent);
    g_test_add_func("/alert/config/commit_and_episode_reset_are_atomic",
                    test_alert_config_commit_and_episode_reset_are_atomic);
    g_test_add_func("/alert/config/disable_stops_evaluation",
                    test_alert_config_disable_stops_evaluation);
    g_test_add_func("/alert/config/disabled_keeps_external_events",
                    test_alert_config_disabled_keeps_external_events);
    g_test_add_func("/alert/config/worker_singleton_across_toggles",
                    test_alert_config_worker_singleton_across_toggles);
    g_test_add_func("/alert/config/revision_conflict",
                    test_alert_config_revision_conflict);
    g_test_add_func("/alert/config/revision_increment_rules",
                    test_alert_config_revision_increment_rules);
    g_test_add_func("/alert/history/concurrent_snapshot",
                    test_alert_history_concurrent_snapshot);
    g_test_add_func("/alert/webhook/concurrent_route_lookup",
                    test_alert_webhook_concurrent_route_lookup);
    g_test_add_func("/alert/ack/marks_history_record",
                    test_alert_ack_marks_history_record);
    g_test_add_func("/alert/ack/unknown_id_returns_false",
                    test_alert_ack_unknown_id_returns_false);
    g_test_add_func("/alert/ack/idempotent", test_alert_ack_is_idempotent);
    g_test_add_func("/alert/ack/requires_operator_role",
                    test_alert_ack_requires_operator_role);
}
