
















































#include "nfv_manager.h"
#include "ovn_manager.h"
#include "utils/pcv_spawn.h"
#include "utils/pcv_log.h"
#include <string.h>
#include <glib/gstdio.h>

#define NFV_LOG_DOM "nfv_manager"












static gboolean
_run(const gchar *cmd, gchar **out, GError **error)
{
    gchar **parsed = NULL;
    GError *pe = NULL;
    if (!g_shell_parse_argv(cmd, NULL, &parsed, &pe)) {
        if (pe) { if (error) g_propagate_error(error, pe); else g_error_free(pe); }
        return FALSE;
    }
    gchar *se = NULL;
    gboolean ok = pcv_spawn_sync((const gchar * const *)parsed, out, &se, error);
    if (!ok) PCV_LOG_WARN(NFV_LOG_DOM, "cmd failed: %s err=%s", cmd, se ? se : "");
    g_free(se);
    g_strfreev(parsed);
    return ok;
}
static gboolean
_run_shell(const gchar *cmd, gchar **out, GError **error)
{
    const gchar *argv[] = {"/bin/sh", "-c", cmd, NULL};
    gchar *se = NULL;
    gboolean ok = pcv_spawn_sync(argv, out, &se, error);
    if (!ok) PCV_LOG_WARN(NFV_LOG_DOM, "cmd failed: %s err=%s", cmd, se ? se : "");
    g_free(se);
    return ok;
}


void pcv_nfv_init(void) { PCV_LOG_INFO(NFV_LOG_DOM, "NFV manager initialized"); }


void pcv_nfv_shutdown(void) {}




















gboolean pcv_nfv_lb_create(const gchar *name, const gchar *vip, gint port,
                            const gchar *backends, GError **error)
{
    if (!name || !vip || !backends) {
        g_set_error(error, g_quark_from_static_string("nfv"), 1, "name, vip, backends required");
        return FALSE;
    }
    if (!pcv_ovn_is_available()) {
        g_set_error(error, g_quark_from_static_string("nfv"), 2, "OVN not available");
        return FALSE;
    }
    gchar *cmd = g_strdup_printf("ovn-nbctl lb-add %s %s:%d %s 2>&1", name, vip, port, backends);
    gboolean ok = _run(cmd, NULL, error);
    g_free(cmd);
    if (ok) PCV_LOG_INFO(NFV_LOG_DOM, "LB '%s' created (vip=%s:%d)", name, vip, port);
    return ok;
}










gboolean pcv_nfv_lb_delete(const gchar *name, GError **error)
{
    if (!name) { g_set_error(error, g_quark_from_static_string("nfv"), 1, "name required"); return FALSE; }

    gchar *cmd = g_strdup_printf("ovn-nbctl lb-del %s 2>/dev/null; true", name);

    gboolean ok = _run_shell(cmd, NULL, error);
    g_free(cmd);
    return ok;
}









JsonArray *pcv_nfv_lb_list(void)
{
    JsonArray *arr = json_array_new();
    if (!pcv_ovn_is_available()) return arr;
    gchar *out = NULL;
    if (_run_shell("ovn-nbctl lb-list 2>/dev/null", &out, NULL) && out) {
        gchar **lines = g_strsplit(out, "\n", -1);
        for (gint i = 0; lines[i] && lines[i][0]; i++) {
            JsonObject *lb = json_object_new();
            json_object_set_string_member(lb, "entry", lines[i]);
            json_array_add_object_element(arr, lb);
        }
        g_strfreev(lines);
    }
    g_free(out);
    return arr;
}



















gboolean pcv_nfv_fw_policy_create(const gchar *name, const gchar *sw, GError **error)
{
    if (!name || !sw) { g_set_error(error, g_quark_from_static_string("nfv"), 1, "name and switch required"); return FALSE; }

    gchar *path = g_strdup_printf("/var/run/purecvisor/nfv-policy-%s.json", name);
    gchar *content = g_strdup_printf("{\"name\":\"%s\",\"switch\":\"%s\",\"rules\":[]}", name, sw);
    gboolean ok = g_file_set_contents(path, content, -1, error);
    g_free(content); g_free(path);
    if (ok) PCV_LOG_INFO(NFV_LOG_DOM, "FW policy '%s' created for switch '%s'", name, sw);
    return ok;
}










gboolean pcv_nfv_fw_policy_delete(const gchar *name, GError **error)
{
    if (!name) { g_set_error(error, g_quark_from_static_string("nfv"), 1, "name required"); return FALSE; }
    gchar *path = g_strdup_printf("/var/run/purecvisor/nfv-policy-%s.json", name);
    g_unlink(path);
    g_free(path);
    return TRUE;
}










JsonArray *pcv_nfv_fw_policy_list(const gchar *sw __attribute__((unused)))
{
    JsonArray *arr = json_array_new();
    GDir *dir = g_dir_open("/var/run/purecvisor", 0, NULL);
    if (!dir) return arr;
    const gchar *name;
    while ((name = g_dir_read_name(dir)) != NULL) {
        if (g_str_has_prefix(name, "nfv-policy-") && g_str_has_suffix(name, ".json")) {
            gchar *path = g_build_filename("/var/run/purecvisor", name, NULL);
            gchar *content = NULL;
            if (g_file_get_contents(path, &content, NULL, NULL) && content) {
                JsonParser *p = json_parser_new();
                if (json_parser_load_from_data(p, content, -1, NULL))
                    json_array_add_element(arr, json_node_copy(json_parser_get_root(p)));
                g_object_unref(p);
            }
            g_free(content); g_free(path);
        }
    }
    g_dir_close(dir);
    return arr;
}




















gboolean pcv_nfv_chain_create(const gchar *name, const gchar *steps_json, GError **error)
{
    if (!name) { g_set_error(error, g_quark_from_static_string("nfv"), 1, "name required"); return FALSE; }
    gchar *path = g_strdup_printf("/var/run/purecvisor/nfv-chain-%s.json", name);
    gchar *content = g_strdup_printf("{\"name\":\"%s\",\"steps\":%s}", name, steps_json ? steps_json : "[]");
    gboolean ok = g_file_set_contents(path, content, -1, error);
    g_free(content); g_free(path);
    if (ok) PCV_LOG_INFO(NFV_LOG_DOM, "Service chain '%s' created", name);
    return ok;
}








gboolean pcv_nfv_chain_delete(const gchar *name, GError **error)
{
    if (!name) { g_set_error(error, g_quark_from_static_string("nfv"), 1, "name required"); return FALSE; }
    gchar *path = g_strdup_printf("/var/run/purecvisor/nfv-chain-%s.json", name);
    g_unlink(path);
    g_free(path);
    return TRUE;
}









JsonArray *pcv_nfv_chain_list(void)
{
    JsonArray *arr = json_array_new();
    GDir *dir = g_dir_open("/var/run/purecvisor", 0, NULL);
    if (!dir) return arr;
    const gchar *name;
    while ((name = g_dir_read_name(dir)) != NULL) {
        if (g_str_has_prefix(name, "nfv-chain-") && g_str_has_suffix(name, ".json")) {
            gchar *path = g_build_filename("/var/run/purecvisor", name, NULL);
            gchar *content = NULL;
            if (g_file_get_contents(path, &content, NULL, NULL) && content) {
                JsonParser *p = json_parser_new();
                if (json_parser_load_from_data(p, content, -1, NULL))
                    json_array_add_element(arr, json_node_copy(json_parser_get_root(p)));
                g_object_unref(p);
            }
            g_free(content); g_free(path);
        }
    }
    g_dir_close(dir);
    return arr;
}
