
#include "handler_storage.h"
#include "rpc_utils.h"
#include "../../utils/pcv_spawn.h"
#include "../../utils/pcv_config.h"
#include "../../utils/pcv_log.h"
#include "modules/storage/zfs_driver.h"
#include <glib.h>
#include <string.h>

static JsonArray* execute_zfs_command_to_json(const gchar *command, const gchar **keys, int num_keys) {
    gchar *std_out = NULL;
    gchar *std_err = NULL;
    gint exit_status = 0;
    GError *error = NULL;

    JsonArray *json_arr = json_array_new();

    gchar **parsed_argv = NULL;
    if (!g_shell_parse_argv(command, NULL, &parsed_argv, &error)) {
        g_printerr("[ZFS] Failed to parse command: %s\n", error->message);
        g_error_free(error);
        return json_arr;
    }
    if (!pcv_spawn_sync((const gchar * const *)parsed_argv, &std_out, &std_err, &error)) {
        g_printerr("[ZFS] Command failed: %s\n", error ? error->message : "unknown");
        if (error) g_error_free(error);
        g_strfreev(parsed_argv);
        return json_arr;
    }
    g_strfreev(parsed_argv);
    exit_status = 0;

    if (exit_status == 0 && std_out != NULL) {
        gchar **lines = g_strsplit(std_out, "\n", -1);
        for (int i = 0; lines[i] != NULL; i++) {
            if (strlen(lines[i]) == 0) continue;

            gchar **tokens = g_strsplit(lines[i], "\t", -1);
            JsonObject *row_obj = json_object_new();

            for (int j = 0; j < num_keys && tokens[j] != NULL; j++) {
                gchar *clean_val = g_strstrip(g_strdup(tokens[j]));
                json_object_set_string_member(row_obj, keys[j], clean_val);
                g_free(clean_val);
            }
            json_array_add_object_element(json_arr, row_obj);
            g_strfreev(tokens);
        }
        g_strfreev(lines);
    }

    g_free(std_out);
    g_free(std_err);
    return json_arr;
}

void handle_storage_pool_list_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {

    const gchar *cmd = "zpool list -H -o name,size,alloc,free,health";
    const gchar *keys[] = {"name", "size", "alloc", "free", "health"};

    JsonArray *result_arr = execute_zfs_command_to_json(cmd, keys, 5);

    JsonNode *result_node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(result_node, result_arr);

    gchar *resp = pure_rpc_build_success_response(rpc_id, result_node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

void handle_storage_zvol_list_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {

    const gchar *cmd = "zfs list -t volume -H -o name,volsize,used,refer,compressratio,dedup,written";
    const gchar *keys[] = {"name", "volsize", "used", "refer", "compression_ratio", "dedup", "written"};

    JsonArray *result_arr = execute_zfs_command_to_json(cmd, keys, 7);

    JsonNode *result_node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(result_node, result_arr);

    gchar *resp = pure_rpc_build_success_response(rpc_id, result_node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

void handle_storage_zvol_create_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {

    const gchar *zvol_path = json_object_get_string_member(params, "zvol_path");
    if (!zvol_path)
        zvol_path = json_object_get_string_member(params, "name");

    const gchar *size = json_object_get_string_member(params, "size");
    gchar *size_buf = NULL;
    if (!size && json_object_has_member(params, "size_gb")) {
        gint64 gb = json_object_get_int_member(params, "size_gb");
        size_buf = g_strdup_printf("%ldG", (long)gb);
        size = size_buf;
    }

    if (!zvol_path || !zvol_path[0] || !size) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing 'zvol_path'/'name' or 'size'/'size_gb'");
        pure_uds_server_send_response(server, connection, resp); g_free(resp); g_free(size_buf); return;
    }

    for (const gchar *p = zvol_path; *p; p++) {
        if (!g_ascii_isalnum(*p) && *p != '_' && *p != '-' && *p != '.' && *p != '/') {
            gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                "Invalid zvol name — allowed: [a-zA-Z0-9_.-/]");
            pure_uds_server_send_response(server, connection, resp);
            g_free(resp); g_free(size_buf); return;
        }
    }

    gchar *full_path = NULL;
    if (!strchr(zvol_path, '/')) {
        full_path = g_strdup_printf("%s/%s", pcv_config_get_zvol_pool(), zvol_path);
        zvol_path = full_path;
    }

    const gchar *zfs_argv[] = {"zfs", "create", "-V", size, zvol_path, NULL};
    gchar *std_err = NULL;
    GError *error = NULL;

    if (!pcv_spawn_sync(zfs_argv, NULL, &std_err, &error)) {
        gchar *err_msg = error ? error->message
                       : (std_err ? g_strstrip(std_err) : "Unknown ZFS execution error");
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, err_msg);
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
    } else {
        PCV_LOG_INFO("storage", "zvol created: %s (%s)", zvol_path, size);
        JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(res_node, json_object_new());
        gchar *resp = pure_rpc_build_success_response(rpc_id, res_node);
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
    }

    g_free(full_path);
    g_free(size_buf);
    if (std_err) g_free(std_err);
    if (error) g_error_free(error);
}

void handle_storage_zvol_delete_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {

    const gchar *zvol_path = json_object_get_string_member(params, "zvol_path");
    if (!zvol_path)
        zvol_path = json_object_get_string_member(params, "name");

    if (!zvol_path || !zvol_path[0]) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing 'zvol_path' or 'name'");
        pure_uds_server_send_response(server, connection, resp); g_free(resp); return;
    }

    gchar *full_path = NULL;
    if (!strchr(zvol_path, '/')) {
        full_path = g_strdup_printf("%s/%s", pcv_config_get_zvol_pool(), zvol_path);
        zvol_path = full_path;
    }

    {
        gboolean bad = (zvol_path[0] == '/') || strstr(zvol_path, "..") || strstr(zvol_path, "//");
        for (const gchar *c = zvol_path; !bad && *c; c++) {
            if (!g_ascii_isalnum(*c) && *c != '_' && *c != '-' && *c != '.' && *c != '/')
                bad = TRUE;
        }
        if (bad) {
            gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid zvol_path");
            pure_uds_server_send_response(server, connection, resp); g_free(resp);
            g_free(full_path);
            return;
        }
        const gchar *type_argv[] = {"zfs", "list", "-H", "-o", "type", zvol_path, NULL};
        gchar *type_out = NULL;
        gboolean ok = pcv_spawn_sync(type_argv, &type_out, NULL, NULL);
        gboolean is_vol = ok && type_out && g_strcmp0(g_strstrip(type_out), "volume") == 0;
        g_free(type_out);
        if (!is_vol) {
            gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
                "Refused: target is not a zvol (volume) — parent datasets cannot be recursively destroyed");
            pure_uds_server_send_response(server, connection, resp); g_free(resp);
            g_free(full_path);
            return;
        }
    }

    const gchar *zfs_argv[] = {"zfs", "destroy", "-r", zvol_path, NULL};
    gchar *std_err = NULL;
    GError *error = NULL;

    if (!pcv_spawn_sync(zfs_argv, NULL, &std_err, &error)) {
        gchar *err_msg = error ? error->message
                       : (std_err ? g_strstrip(std_err) : "Unknown ZFS execution error");
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, err_msg);
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
    } else {
        PCV_LOG_INFO("storage", "zvol destroyed: %s", zvol_path);
        JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(res_node, json_object_new());
        gchar *resp = pure_rpc_build_success_response(rpc_id, res_node);
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
    }

    g_free(full_path);
    if (std_err) g_free(std_err);
    if (error) g_error_free(error);
}

void handle_storage_pool_create_request(JsonObject *params, const gchar *rpc_id,
                                         UdsServer *server, GSocketConnection *connection)
{
    const gchar *name = json_object_get_string_member(params, "name");
    const gchar *vdev_type = json_object_has_member(params, "vdev_type")
        ? json_object_get_string_member(params, "vdev_type") : NULL;

    if (!name || !json_object_has_member(params, "disks")) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing 'name' or 'disks'");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

    JsonArray *disks_arr = json_object_get_array_member(params, "disks");
    guint n_disks = json_array_get_length(disks_arr);
    if (n_disks == 0) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "'disks' array is empty");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

    const gchar **disk_list = g_new0(const gchar *, n_disks);
    for (guint i = 0; i < n_disks; i++) {
        disk_list[i] = json_array_get_string_element(disks_arr, i);
    }

    GError *error = NULL;

    const gchar *compression = NULL;
    if (json_object_has_member(params, "compression"))
        compression = json_object_get_string_member(params, "compression");
    gboolean ok = purecvisor_zfs_create_pool(name, vdev_type, disk_list, (gint)n_disks, compression, &error);
    g_free(disk_list);

    if (!ok) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            error ? error->message : "zpool create failed");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        if (error) g_error_free(error);
        return;
    }

    JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
    JsonObject *res_obj = json_object_new();
    json_object_set_string_member(res_obj, "pool", name);
    json_object_set_boolean_member(res_obj, "created", TRUE);
    json_node_take_object(res_node, res_obj);

    gchar *resp = pure_rpc_build_success_response(rpc_id, res_node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

void handle_storage_pool_destroy_request(JsonObject *params, const gchar *rpc_id,
                                          UdsServer *server, GSocketConnection *connection)
{
    const gchar *name = json_object_get_string_member(params, "name");
    if (!name) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing 'name'");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

    GError *error = NULL;
    gboolean ok = purecvisor_zfs_destroy_pool(name, &error);

    if (!ok) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            error ? error->message : "zpool destroy failed");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        if (error) g_error_free(error);
        return;
    }

    JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
    JsonObject *res_obj = json_object_new();
    json_object_set_string_member(res_obj, "pool", name);
    json_object_set_boolean_member(res_obj, "destroyed", TRUE);
    json_node_take_object(res_node, res_obj);

    gchar *resp = pure_rpc_build_success_response(rpc_id, res_node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

void handle_storage_pool_scrub_request(JsonObject *params, const gchar *rpc_id,
                                        UdsServer *server, GSocketConnection *connection)
{
    const gchar *name = json_object_get_string_member(params, "name");
    if (!name) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing 'name'");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

    GError *error = NULL;
    gboolean ok = purecvisor_zfs_scrub_pool(name, &error);

    if (!ok) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            error ? error->message : "zpool scrub failed");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        if (error) g_error_free(error);
        return;
    }

    JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
    JsonObject *res_obj = json_object_new();
    json_object_set_string_member(res_obj, "pool", name);
    json_object_set_boolean_member(res_obj, "scrub_started", TRUE);
    json_node_take_object(res_node, res_obj);

    gchar *resp = pure_rpc_build_success_response(rpc_id, res_node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

#include "../../utils/pcv_config.h"
#include <sys/stat.h>

void handle_iso_list(JsonObject *params, const gchar *rpc_id,
                      UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    const gchar *iso_dirs_str = pcv_config_get_iso_dirs();
    gchar **dirs = g_strsplit(iso_dirs_str, ",", -1);
    JsonArray *arr = json_array_new();

    for (gint i = 0; dirs[i] != NULL; i++) {
        gchar *iso_dir = g_strstrip(dirs[i]);
        if (iso_dir[0] == '\0') continue;
        if (strstr(iso_dir, "..")) continue;

        GDir *dir = g_dir_open(iso_dir, 0, NULL);
        if (!dir) continue;

        const gchar *name;
        while ((name = g_dir_read_name(dir))) {
            if (g_str_has_suffix(name, ".iso") || g_str_has_suffix(name, ".ISO") ||
                g_str_has_suffix(name, ".img") || g_str_has_suffix(name, ".IMG")) {
                gchar *full = g_strdup_printf("%s/%s", iso_dir, name);
                struct stat st;
                JsonObject *obj = json_object_new();
                json_object_set_string_member(obj, "name", name);
                json_object_set_string_member(obj, "path", full);
                json_object_set_string_member(obj, "dir", iso_dir);
                if (stat(full, &st) == 0)
                    json_object_set_int_member(obj, "size_mb", (gint64)(st.st_size / (1024*1024)));
                json_array_add_object_element(arr, obj);
                g_free(full);
            }
        }
        g_dir_close(dir);
    }
    g_strfreev(dirs);

    JsonNode *iso_node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(iso_node, arr);
    gchar *iso_resp = pure_rpc_build_success_response(rpc_id, iso_node);
    pure_uds_server_send_response(server, connection, iso_resp);
    g_free(iso_resp);
}
