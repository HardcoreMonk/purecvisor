                                                                                      


                                                                 
                                                                     
                                                      
                   
  
                                                             
  
                 
                                                  
                                                  
                                   
                                       
  
                                 
  
                                            
   

#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "modules/network/ovs_overlay.h"
#if __has_include(<valgrind/valgrind.h>)
#include <valgrind/valgrind.h>
#else
#define RUNNING_ON_VALGRIND 0
#endif

                                
extern gboolean pcv_ovn_is_available(void);
extern JsonArray *pcv_ovn_switch_list(void);
extern JsonArray *pcv_ovn_router_list(void);
extern JsonArray *pcv_ovn_nat_list(const gchar *router);
extern JsonArray *pcv_ovn_nat_list_parse(const gchar *output);
extern JsonArray *pcv_ovn_dhcp_list(void);
extern JsonArray *pcv_ovn_acl_list(const gchar *sw);
extern JsonObject *pcv_ovn_status(void);
extern gboolean pcv_ovn_switch_delete(const gchar *name, GError **error);
extern gboolean pcv_ovn_router_delete(const gchar *name, GError **error);
                                                        
extern gboolean pcv_ovn_valid_id(const gchar *s);

                                                   

static void test_ovn_switch_list_empty(void) {
    JsonArray *arr = pcv_ovn_switch_list();
    g_assert_nonnull(arr);
    g_assert_cmpuint(json_array_get_length(arr), ==, 0);
    json_array_unref(arr);
}

static void test_ovn_router_list_empty(void) {
    JsonArray *arr = pcv_ovn_router_list();
    g_assert_nonnull(arr);
    g_assert_cmpuint(json_array_get_length(arr), ==, 0);
    json_array_unref(arr);
}

static void test_ovn_nat_list_empty(void) {
    JsonArray *arr = pcv_ovn_nat_list("nonexist");
    g_assert_nonnull(arr);
    g_assert_cmpuint(json_array_get_length(arr), ==, 0);
    json_array_unref(arr);
}

static void test_ovn_nat_list_parser_omits_cli_header(void) {
    const gchar *output =
        "TYPE             GATEWAY_PORT          MATCH                 EXTERNAL_IP        EXTERNAL_PORT    LOGICAL_IP          EXTERNAL_MAC         LOGICAL_PORT\n"
        "dnat_and_snat                                                192.0.2.11                          10.252.10.11\n"
        "snat                                                         192.0.2.10                          10.252.10.0/24\n";
    JsonArray *arr = pcv_ovn_nat_list_parse(output);
    g_assert_nonnull(arr);
    g_assert_cmpuint(json_array_get_length(arr), ==, 2);
    g_assert_true(g_str_has_prefix(json_array_get_string_element(arr, 0),
                                   "dnat_and_snat"));
    g_assert_true(g_str_has_prefix(json_array_get_string_element(arr, 1),
                                   "snat"));
    json_array_unref(arr);

    arr = pcv_ovn_nat_list_parse(
        "TYPE EXTERNAL_IP LOGICAL_IP\n");
    g_assert_cmpuint(json_array_get_length(arr), ==, 0);
    json_array_unref(arr);
}

static void test_ovn_dhcp_list_empty(void) {
    JsonArray *arr = pcv_ovn_dhcp_list();
    g_assert_nonnull(arr);
    g_assert_cmpuint(json_array_get_length(arr), ==, 0);
    json_array_unref(arr);
}

static void test_ovn_acl_list_empty(void) {
    JsonArray *arr = pcv_ovn_acl_list("nonexist");
    g_assert_nonnull(arr);
    g_assert_cmpuint(json_array_get_length(arr), ==, 0);
    json_array_unref(arr);
}

                                      

static void test_ovn_switch_delete_idempotent(void) {
    g_assert_true(pcv_ovn_switch_delete("nonexist-sw", NULL));
}

static void test_ovn_router_delete_idempotent(void) {
    g_assert_true(pcv_ovn_router_delete("nonexist-lr", NULL));
}

                                      
  
                                                           
                                                      
                                               
                                                
static void test_ovn_valid_id_rejects_injection(void) {
                    
    g_assert_true(pcv_ovn_valid_id("pcv-ls0"));
    g_assert_true(pcv_ovn_valid_id("tenant-alpha-ls"));
    g_assert_true(pcv_ovn_valid_id("10.0.0.1"));                      

                       
    g_assert_false(pcv_ovn_valid_id("ls add"));                             
    g_assert_false(pcv_ovn_valid_id("sw --may-exist"));                
    g_assert_false(pcv_ovn_valid_id("--priv"));                              
    g_assert_false(pcv_ovn_valid_id("--"));                           
    g_assert_false(pcv_ovn_valid_id("sw;ls-del x"));               
    g_assert_false(pcv_ovn_valid_id("sw\"quote"));                 
    g_assert_false(pcv_ovn_valid_id(""));                            
    g_assert_false(pcv_ovn_valid_id(NULL));                         
}

                           

static void test_ovn_status_structure(void) {
    JsonObject *obj = pcv_ovn_status();
    g_assert_nonnull(obj);
    const gchar *members[] = {
        "available", "installed", "northbound_connected", "southbound_connected",
        "northd_synced", "controller_configured", "chassis_registered", NULL
    };
    for (guint i = 0; members[i]; i++)
        g_assert_true(json_object_has_member(obj, members[i]));
    json_object_unref(obj);
}

#if !PCV_CLUSTER_ENABLED

typedef void (*OverlayFakeCommandHook)(const gchar *command, gboolean before,
                                       gpointer user_data);

typedef struct {
    GHashTable *bridges;
    GHashTable *kernel_links;
    GHashTable *bridge_uuids;
    GHashTable *bridge_owners;
    GHashTable *bridge_names;
    GHashTable *bridge_cidrs;
    GHashTable *bridge_datapaths;
    GHashTable *bridge_legacy_owners;
    GHashTable *port_bridges;


    GHashTable *port_row_owners;
    GHashTable *port_row_names;
    GHashTable *port_owners;
    GHashTable *port_names;
    GHashTable *port_types;
    GHashTable *port_keys;
    GHashTable *port_remote;
    GHashTable *port_local;
    GHashTable *port_extras;
    GHashTable *port_uuids;
    GHashTable *interface_uuids;
    GHashTable *port_interfaces;
    GHashTable *port_legacy_owners;
    guint next_uuid;
    GPtrArray *commands;
    gchar *fail_contains;
    guint fail_remaining;
    GIOErrorEnum fail_code;
    gchar *fail_after_contains;
    guint fail_after_remaining;
    gchar *race_bridge_name;
    gchar *mutate_contains;
    gchar *mutate_meta_path;



    gchar *extra_parent_bridge;
    gchar *extra_parent_port;
    gboolean saw_unbounded_ovs;
    GMutex state_mu;
    GMutex gate_mu;
    GCond gate_cond;
    gchar *block_contains;
    gboolean blocked;
    gboolean release;
    OverlayFakeCommandHook command_hook;
    gpointer command_hook_data;
} OverlayFake;

static OverlayFake O;
static gchar *O_meta_dir;

static gint
_fake_arg_index(const gchar * const *argv, const gchar *needle)
{
    for (gint i = 0; argv[i]; i++)
        if (g_strcmp0(argv[i], needle) == 0)
            return i;
    return -1;
}

static gchar *
_fake_join(const gchar * const *argv)
{
    GString *joined = g_string_new(NULL);
    for (gint i = 0; argv[i]; i++) {
        if (i)
            g_string_append_c(joined, ' ');
        g_string_append(joined, argv[i]);
    }
    return g_string_free(joined, FALSE);
}

static const gchar *
_fake_lookup(GHashTable *table, const gchar *key)
{
    const gchar *value = g_hash_table_lookup(table, key);
    return value ? value : "";
}

static void
_fake_set(GHashTable *table, const gchar *key, const gchar *value)
{
    g_hash_table_replace(table, g_strdup(key), g_strdup(value ? value : ""));
}

static void
_fake_assign_port_uuid(const gchar *port)
{
    if (g_hash_table_contains(O.port_uuids, port))
        return;
    gchar *uuid = g_strdup_printf("00000000-0000-0000-0000-%012u", ++O.next_uuid);
    _fake_set(O.port_uuids, port, uuid);
    g_free(uuid);
}

static void
_fake_assign_bridge_uuid(const gchar *bridge)
{
    if (g_hash_table_contains(O.bridge_uuids, bridge))
        return;
    gchar *uuid = g_strdup_printf("20000000-0000-0000-0000-%012u", ++O.next_uuid);
    _fake_set(O.bridge_uuids, bridge, uuid);
    g_free(uuid);
}

static void
_fake_assign_interface_uuid(const gchar *interface)
{
    if (g_hash_table_contains(O.interface_uuids, interface))
        return;
    gchar *uuid = g_strdup_printf("10000000-0000-0000-0000-%012u", ++O.next_uuid);
    _fake_set(O.interface_uuids, interface, uuid);
    _fake_set(O.port_interfaces, interface, uuid);
    g_free(uuid);
}

static gchar *
_fake_options_map(const gchar *port)
{
    const gchar *key = _fake_lookup(O.port_keys, port);
    const gchar *remote = _fake_lookup(O.port_remote, port);
    const gchar *local = _fake_lookup(O.port_local, port);
    const gchar *extras = _fake_lookup(O.port_extras, port);
    GString *map = g_string_new("{");
    if (*key) g_string_append_printf(map, "key=%s", key);
    if (*remote) g_string_append_printf(map, "%sremote_ip=%s",
                                        map->len > 1 ? "," : "", remote);
    if (*local) g_string_append_printf(map, "%slocal_ip=%s",
                                       map->len > 1 ? "," : "", local);
    if (*extras) g_string_append_printf(map, "%s%s",
                                        map->len > 1 ? "," : "", extras);
    g_string_append_c(map, '}');
    return g_string_free(map, FALSE);
}

static gchar *
_fake_options_json(const gchar *port)
{
    JsonArray *root = json_array_new();
    json_array_add_string_element(root, "map");
    JsonArray *pairs = json_array_new();
    const gchar *keys[] = {"key", "remote_ip", "local_ip"};
    GHashTable *tables[] = {O.port_keys, O.port_remote, O.port_local};
    for (guint i = 0; i < G_N_ELEMENTS(keys); i++) {
        const gchar *value = _fake_lookup(tables[i], port);
        if (!*value)
            continue;
        JsonArray *pair = json_array_new();
        json_array_add_string_element(pair, keys[i]);
        json_array_add_string_element(pair, value);
        json_array_add_array_element(pairs, pair);
    }
    const gchar *extras = _fake_lookup(O.port_extras, port);
    if (*extras) {
        gchar **entries = g_strsplit(extras, ",", -1);
        for (gint i = 0; entries[i]; i++) {
            gchar **kv = g_strsplit(entries[i], "=", 2);
            if (kv[0] && kv[1]) {
                JsonArray *pair = json_array_new();
                json_array_add_string_element(pair, kv[0]);
                json_array_add_string_element(pair, kv[1]);
                json_array_add_array_element(pairs, pair);
            }
            g_strfreev(kv);
        }
        g_strfreev(entries);
    }
    json_array_add_array_element(root, pairs);
    JsonNode *node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(node, root);
    gchar *json = json_to_string(node, FALSE);
    json_node_free(node);
    return json;
}

static void
_fake_replace_options(const gchar *port, const gchar *map)
{
    g_hash_table_remove(O.port_keys, port);
    g_hash_table_remove(O.port_remote, port);
    g_hash_table_remove(O.port_local, port);
    g_hash_table_remove(O.port_extras, port);
    gchar *copy = g_strdup(map ? map : "{}");
    gchar *body = g_strstrip(copy);
    if (*body == '{') body++;
    gchar *end = strrchr(body, '}');
    if (end) *end = '\0';
    GString *extras = g_string_new(NULL);
    gchar **entries = g_strsplit(body, ",", -1);
    for (gint i = 0; entries[i]; i++) {
        gchar **kv = g_strsplit(g_strstrip(entries[i]), "=", 2);
        if (kv[0] && kv[1]) {
            if (g_strcmp0(kv[0], "key") == 0) _fake_set(O.port_keys, port, kv[1]);
            else if (g_strcmp0(kv[0], "remote_ip") == 0) _fake_set(O.port_remote, port, kv[1]);
            else if (g_strcmp0(kv[0], "local_ip") == 0) _fake_set(O.port_local, port, kv[1]);
            else g_string_append_printf(extras, "%s%s=%s",
                                        extras->len ? "," : "", kv[0], kv[1]);
        }
        g_strfreev(kv);
    }
    if (extras->len)
        _fake_set(O.port_extras, port, extras->str);
    g_string_free(extras, TRUE);
    g_strfreev(entries);
    g_free(copy);
}

static void
_fake_remove_port_fields(const gchar *port)
{
    g_hash_table_remove(O.port_owners, port);
    g_hash_table_remove(O.port_names, port);
    g_hash_table_remove(O.port_types, port);
    g_hash_table_remove(O.port_keys, port);
    g_hash_table_remove(O.port_remote, port);
    g_hash_table_remove(O.port_local, port);
    g_hash_table_remove(O.port_extras, port);
    g_hash_table_remove(O.port_uuids, port);
    g_hash_table_remove(O.interface_uuids, port);
    g_hash_table_remove(O.port_interfaces, port);
    g_hash_table_remove(O.port_legacy_owners, port);
}

static void
_fake_remove_port(const gchar *port)
{
    _fake_remove_port_fields(port);
    g_hash_table_remove(O.port_bridges, port);
}

static void
_fake_remove_bridge(const gchar *bridge)
{
    GHashTableIter iter;
    gpointer key = NULL;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, O.port_bridges);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        if (g_strcmp0(value, bridge) == 0) {
            _fake_remove_port_fields(key);
            g_hash_table_iter_remove(&iter);
        }
    }
    g_hash_table_remove(O.bridges, bridge);
    g_hash_table_remove(O.bridge_uuids, bridge);
    g_hash_table_remove(O.bridge_owners, bridge);
    g_hash_table_remove(O.bridge_names, bridge);
    g_hash_table_remove(O.bridge_cidrs, bridge);
    g_hash_table_remove(O.bridge_datapaths, bridge);
    g_hash_table_remove(O.bridge_legacy_owners, bridge);
    g_hash_table_remove(O.port_uuids, bridge);
    g_hash_table_remove(O.interface_uuids, bridge);
    g_hash_table_remove(O.port_interfaces, bridge);
}

static const gchar *
_fake_field(const gchar *table, const gchar *row, const gchar *column)
{
    if (g_strcmp0(table, "Bridge") == 0) {
        if (g_strcmp0(column, "external_ids:pcv_overlay_owner") == 0)
            return _fake_lookup(O.bridge_owners, row);
        if (g_strcmp0(column, "external_ids:pcv_overlay_name") == 0)
            return _fake_lookup(O.bridge_names, row);
        if (g_strcmp0(column, "external_ids:purecvisor-owner") == 0)
            return _fake_lookup(O.bridge_legacy_owners, row);
        if (g_strcmp0(column, "datapath_type") == 0)
            return _fake_lookup(O.bridge_datapaths, row);
    } else if (g_strcmp0(table, "Port") == 0) {
        if (g_strcmp0(column, "_uuid") == 0)
            return _fake_lookup(O.port_uuids, row);
        if (g_strcmp0(column, "interfaces") == 0)
            return _fake_lookup(O.port_interfaces, row);
    } else if (g_strcmp0(table, "Interface") == 0) {
        if (g_strcmp0(column, "_uuid") == 0)
            return _fake_lookup(O.interface_uuids, row);
        if (g_strcmp0(column, "external_ids:pcv_overlay_owner") == 0)
            return _fake_lookup(O.port_owners, row);
        if (g_strcmp0(column, "external_ids:pcv_overlay_name") == 0)
            return _fake_lookup(O.port_names, row);
        if (g_strcmp0(column, "external_ids:purecvisor-owner") == 0)
            return _fake_lookup(O.port_legacy_owners, row);
        if (g_strcmp0(column, "type") == 0)
            return _fake_lookup(O.port_types, row);
        if (g_strcmp0(column, "options:key") == 0)
            return _fake_lookup(O.port_keys, row);
        if (g_strcmp0(column, "options:remote_ip") == 0)
            return _fake_lookup(O.port_remote, row);
        if (g_strcmp0(column, "options:local_ip") == 0)
            return _fake_lookup(O.port_local, row);
    }
    return "";
}

static gboolean
_fake_row_exists(const gchar *table, const gchar *row)
{
    if (g_strcmp0(table, "Bridge") == 0)
        return g_hash_table_contains(O.bridges, row);
    if (g_strcmp0(table, "Interface") == 0)
        return g_hash_table_contains(O.port_bridges, row);
    if (g_strcmp0(table, "Port") == 0)
        return g_hash_table_contains(O.port_uuids, row);
    return FALSE;
}

static gboolean
_fake_condition(const gchar *table, const gchar *row, const gchar *condition)
{
    if (!_fake_row_exists(table, row))
        return FALSE;
    const gchar *equal = strchr(condition, '=');
    if (!equal)
        return FALSE;
    gchar *column = g_strndup(condition, (gsize)(equal - condition));
    const gchar *expected = equal + 1;
    if (g_strcmp0(expected, "[]") == 0)
        expected = "";
    gboolean match = FALSE;
    if (g_strcmp0(table, "Bridge") == 0 &&
        g_strcmp0(column, "ports") == 0) {
        guint actual_count = 1;
        GHashTableIter iter;
        gpointer port = NULL;
        gpointer bridge = NULL;
        g_hash_table_iter_init(&iter, O.port_bridges);
        while (g_hash_table_iter_next(&iter, &port, &bridge))
            actual_count += g_strcmp0(bridge, row) == 0;
        if (*expected == '[') {
            gchar *copy = g_strdup(expected + 1);
            gchar *close = strrchr(copy, ']');
            if (close) *close = '\0';
            gchar **uuids = *copy ? g_strsplit(copy, ",", -1) : g_new0(gchar *, 1);
            guint expected_count = g_strv_length(uuids);
            match = expected_count == actual_count;
            const gchar *local_uuid = _fake_lookup(O.port_uuids, row);
            gboolean local_found = FALSE;
            for (guint i = 0; match && uuids[i]; i++) {
                gboolean found = g_strcmp0(uuids[i], local_uuid) == 0;
                local_found |= found;
                GHashTableIter uuid_iter;
                gpointer child = NULL;
                gpointer child_bridge = NULL;
                g_hash_table_iter_init(&uuid_iter, O.port_bridges);
                while (!found && g_hash_table_iter_next(&uuid_iter, &child,
                                                         &child_bridge))
                    found = g_strcmp0(child_bridge, row) == 0 &&
                            g_strcmp0(_fake_lookup(O.port_uuids, child), uuids[i]) == 0;
                match = found;
            }
            match = match && local_found;
            g_strfreev(uuids);
            g_free(copy);
        }
    } else if (g_strcmp0(table, "Interface") == 0 &&
               g_strcmp0(column, "options") == 0) {
        gchar *actual = _fake_options_map(row);
        match = g_strcmp0(actual, expected) == 0;
        g_free(actual);
    } else if (g_strcmp0(table, "Port") == 0 &&
               g_strcmp0(column, "interfaces") == 0) {
        gchar *copy = g_strdup(expected);
        gchar *value = copy;
        if (*value == '[')
            value++;
        gchar *close = strrchr(value, ']');
        if (close)
            *close = '\0';
        match = *value && strchr(value, ',') == NULL &&
                g_strcmp0(_fake_lookup(O.port_interfaces, row), value) == 0;
        g_free(copy);
    } else {
        match = g_strcmp0(_fake_field(table, row, column), expected) == 0;
    }
    g_free(column);
    return match;
}

static gint
_fake_segment_op(const gchar * const *argv, gint start, gint end,
                 gboolean *may_exist)
{
    gint op = start;
    *may_exist = FALSE;
    while (op < end && g_str_has_prefix(argv[op], "--")) {
        if (g_strcmp0(argv[op], "--may-exist") == 0)
            *may_exist = TRUE;
        op++;
    }
    return op;
}

static gboolean
_fake_transaction(const gchar * const *argv, GError **error)
{

    for (gint start = 0; argv[start]; ) {
        if (g_strcmp0(argv[start], "--") != 0) {
            start++;
            continue;
        }
        gint end = start + 1;
        while (argv[end] && g_strcmp0(argv[end], "--") != 0)
            end++;
        gboolean may_exist = FALSE;
        gint op = _fake_segment_op(argv, start + 1, end, &may_exist);
        if (op < end && g_strcmp0(argv[op], "wait-until") == 0) {
            const gchar *table = op + 1 < end ? argv[op + 1] : "";
            const gchar *row = op + 2 < end ? argv[op + 2] : "";
            for (gint i = op + 3; i < end; i++) {
                if (!_fake_condition(table, row, argv[i])) {
                    g_set_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                                "fake wait-until mismatch: %s %s %s",
                                table, row, argv[i]);
                    return FALSE;
                }
            }
        } else if (op < end && g_strcmp0(argv[op], "add-br") == 0 &&
                   op + 1 < end && !may_exist &&
                   g_hash_table_contains(O.bridges, argv[op + 1])) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                        "fake duplicate bridge: %s", argv[op + 1]);
            return FALSE;
        } else if (op < end && g_strcmp0(argv[op], "add-port") == 0 &&
                   op + 2 < end && !may_exist &&
                   g_hash_table_contains(O.port_bridges, argv[op + 2])) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                        "fake duplicate port: %s", argv[op + 2]);
            return FALSE;
        }
        start = end;
    }

    for (gint start = 0; argv[start]; ) {
        if (g_strcmp0(argv[start], "--") != 0) {
            start++;
            continue;
        }
        gint end = start + 1;
        while (argv[end] && g_strcmp0(argv[end], "--") != 0)
            end++;
        gboolean may_exist = FALSE;
        gint op = _fake_segment_op(argv, start + 1, end, &may_exist);
        if (op >= end || g_strcmp0(argv[op], "wait-until") == 0) {
            start = end;
            continue;
        }
        if (g_strcmp0(argv[op], "add-br") == 0 && op + 1 < end) {
            g_hash_table_add(O.bridges, g_strdup(argv[op + 1]));
            _fake_assign_bridge_uuid(argv[op + 1]);
            _fake_assign_port_uuid(argv[op + 1]);
            _fake_assign_interface_uuid(argv[op + 1]);
            _fake_set(O.port_types, argv[op + 1], "internal");
        } else if (g_strcmp0(argv[op], "del-br") == 0 && op + 1 < end) {
            _fake_remove_bridge(argv[op + 1]);
        } else if (g_strcmp0(argv[op], "add-port") == 0 && op + 2 < end) {
            _fake_set(O.port_bridges, argv[op + 2], argv[op + 1]);
            _fake_assign_port_uuid(argv[op + 2]);
            _fake_assign_interface_uuid(argv[op + 2]);
        } else if (g_strcmp0(argv[op], "del-port") == 0 && op + 1 < end) {
            const gchar *port = op + 2 < end ? argv[op + 2] : argv[op + 1];
            _fake_remove_port(port);
        } else if (g_strcmp0(argv[op], "set") == 0 && op + 3 < end) {
            const gchar *table = argv[op + 1];
            const gchar *row = argv[op + 2];
            for (gint i = op + 3; i < end; i++) {
                if (g_str_has_prefix(argv[i], "external_ids:pcv_overlay_owner=")) {
                    const gchar *value = strchr(argv[i], '=') + 1;
                    _fake_set(g_strcmp0(table, "Bridge") == 0
                                  ? O.bridge_owners : O.port_owners,
                              row, value);
                } else if (g_str_has_prefix(argv[i], "external_ids:pcv_overlay_name=")) {
                    const gchar *value = strchr(argv[i], '=') + 1;
                    _fake_set(g_strcmp0(table, "Bridge") == 0
                                  ? O.bridge_names : O.port_names,
                              row, value);
                } else if (g_str_has_prefix(argv[i], "type=")) {
                    _fake_set(O.port_types, row, strchr(argv[i], '=') + 1);
                } else if (g_str_has_prefix(argv[i], "options:key=")) {
                    _fake_set(O.port_keys, row, strrchr(argv[i], '=') + 1);
                } else if (g_str_has_prefix(argv[i], "options:remote_ip=")) {
                    _fake_set(O.port_remote, row, strrchr(argv[i], '=') + 1);
                } else if (g_str_has_prefix(argv[i], "options:local_ip=")) {
                    _fake_set(O.port_local, row, strrchr(argv[i], '=') + 1);
                } else if (g_str_has_prefix(argv[i], "options=")) {
                    _fake_replace_options(row, strchr(argv[i], '=') + 1);
                }
            }
        } else if (g_strcmp0(argv[op], "remove") == 0 && op + 4 < end &&
                   g_strcmp0(argv[op + 3], "external_ids") == 0) {
            GHashTable *table = NULL;
            if (g_strcmp0(argv[op + 1], "Bridge") == 0)
                table = g_strcmp0(argv[op + 4], "pcv_overlay_owner") == 0
                    ? O.bridge_owners : O.bridge_names;
            else
                table = g_strcmp0(argv[op + 4], "pcv_overlay_owner") == 0
                    ? O.port_owners : O.port_names;
            g_hash_table_remove(table, argv[op + 2]);
        }
        start = end;
    }
    return TRUE;
}

static gchar *
_fake_marker_snapshot_json(const gchar *table)
{
    JsonObject *root = json_object_new();
    JsonArray *headings = json_array_new();
    json_array_add_string_element(headings, "name");
    json_array_add_string_element(headings, "external_ids");
    json_object_set_array_member(root, "headings", headings);

    JsonArray *data = json_array_new();
    GHashTableIter iter;
    gpointer row_key = NULL;
    GHashTable *rows = g_strcmp0(table, "Bridge") == 0
        ? O.bridges : O.port_bridges;
    GHashTable *owners = g_strcmp0(table, "Bridge") == 0
        ? O.bridge_owners : O.port_owners;
    GHashTable *names = g_strcmp0(table, "Bridge") == 0
        ? O.bridge_names : O.port_names;
    g_hash_table_iter_init(&iter, rows);
    while (g_hash_table_iter_next(&iter, &row_key, NULL)) {
        const gchar *row_name = row_key;
        JsonArray *row = json_array_new();
        json_array_add_string_element(row, row_name);
        JsonArray *map = json_array_new();
        json_array_add_string_element(map, "map");
        JsonArray *pairs = json_array_new();
        const gchar *owner = _fake_lookup(owners, row_name);
        const gchar *name = _fake_lookup(names, row_name);
        if (*owner) {
            JsonArray *pair = json_array_new();
            json_array_add_string_element(pair, "pcv_overlay_owner");
            json_array_add_string_element(pair, owner);
            json_array_add_array_element(pairs, pair);
        }
        if (*name) {
            JsonArray *pair = json_array_new();
            json_array_add_string_element(pair, "pcv_overlay_name");
            json_array_add_string_element(pair, name);
            json_array_add_array_element(pairs, pair);
        }
        json_array_add_array_element(map, pairs);
        json_array_add_array_element(row, map);
        json_array_add_array_element(data, row);
    }
    json_object_set_array_member(root, "data", data);

    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(node, root);
    gchar *json = json_to_string(node, FALSE);
    json_node_free(node);
    json_object_unref(root);
    return json;
}

static JsonNode *
_fake_uuid_node(const gchar *uuid)
{
    JsonArray *tag = json_array_new();
    json_array_add_string_element(tag, "uuid");
    json_array_add_string_element(tag, uuid);
    JsonNode *node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(node, tag);
    return node;
}

static JsonNode *
_fake_uuid_set_node(GPtrArray *uuids)
{
    JsonArray *tag = json_array_new();
    json_array_add_string_element(tag, "set");
    JsonArray *members = json_array_new();
    for (guint i = 0; i < uuids->len; i++)
        json_array_add_element(members,
            _fake_uuid_node(g_ptr_array_index(uuids, i)));
    json_array_add_array_element(tag, members);
    JsonNode *node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(node, tag);
    return node;
}

static JsonNode *
_fake_string_map_node(GHashTable *values)
{
    JsonArray *tag = json_array_new();
    json_array_add_string_element(tag, "map");
    JsonArray *pairs = json_array_new();
    GHashTableIter iter;
    gpointer key = NULL;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, values);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        if (!value || !*(gchar *)value)
            continue;
        JsonArray *pair = json_array_new();
        json_array_add_string_element(pair, key);
        json_array_add_string_element(pair, value);
        json_array_add_array_element(pairs, pair);
    }
    json_array_add_array_element(tag, pairs);
    JsonNode *node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(node, tag);
    return node;
}

static GHashTable *_fake_string_map(void);

static JsonNode *
_fake_external_ids_node(const gchar *owner, const gchar *name)
{
    GHashTable *values = _fake_string_map();
    if (owner && *owner) _fake_set(values, "pcv_overlay_owner", owner);
    if (name && *name) _fake_set(values, "pcv_overlay_name", name);
    JsonNode *node = _fake_string_map_node(values);
    g_hash_table_destroy(values);
    return node;
}

static JsonNode *
_fake_interface_options_node(const gchar *name)
{
    GHashTable *values = _fake_string_map();
    const gchar *key = _fake_lookup(O.port_keys, name);
    const gchar *remote = _fake_lookup(O.port_remote, name);
    const gchar *local = _fake_lookup(O.port_local, name);
    if (*key) _fake_set(values, "key", key);
    if (*remote) _fake_set(values, "remote_ip", remote);
    if (*local) _fake_set(values, "local_ip", local);
    const gchar *extras = _fake_lookup(O.port_extras, name);
    if (*extras) {
        gchar **entries = g_strsplit(extras, ",", -1);
        for (gint i = 0; entries[i]; i++) {
            gchar **kv = g_strsplit(entries[i], "=", 2);
            if (kv[0] && kv[1]) _fake_set(values, kv[0], kv[1]);
            g_strfreev(kv);
        }
        g_strfreev(entries);
    }
    JsonNode *node = _fake_string_map_node(values);
    g_hash_table_destroy(values);
    return node;
}

static gchar *
_fake_bulk_snapshot_json(void)
{
    JsonArray *results = json_array_new();

    JsonObject *bridge_result = json_object_new();
    JsonArray *bridge_rows = json_array_new();
    GHashTableIter iter;
    gpointer key = NULL;
    g_hash_table_iter_init(&iter, O.bridges);
    while (g_hash_table_iter_next(&iter, &key, NULL)) {
        const gchar *name = key;
        JsonObject *row = json_object_new();
        json_object_set_member(row, "_uuid",
            _fake_uuid_node(_fake_lookup(O.bridge_uuids, name)));
        json_object_set_string_member(row, "name", name);
        GPtrArray *ports = g_ptr_array_new_with_free_func(g_free);
        g_ptr_array_add(ports, g_strdup(_fake_lookup(O.port_uuids, name)));
        GHashTableIter port_iter;
        gpointer port = NULL;
        gpointer bridge = NULL;
        g_hash_table_iter_init(&port_iter, O.port_bridges);
        while (g_hash_table_iter_next(&port_iter, &port, &bridge))
            if (g_strcmp0(bridge, name) == 0)
                g_ptr_array_add(ports,
                    g_strdup(_fake_lookup(O.port_uuids, port)));
        if (g_strcmp0(O.extra_parent_bridge, name) == 0 &&
            O.extra_parent_port && *O.extra_parent_port)
            g_ptr_array_add(ports, g_strdup(
                _fake_lookup(O.port_uuids, O.extra_parent_port)));
        json_object_set_member(row, "ports", _fake_uuid_set_node(ports));
        g_ptr_array_free(ports, TRUE);
        json_object_set_member(row, "external_ids", _fake_external_ids_node(
            _fake_lookup(O.bridge_owners, name),
            _fake_lookup(O.bridge_names, name)));
        json_object_set_string_member(row, "datapath_type",
                                      _fake_lookup(O.bridge_datapaths, name));
        json_array_add_object_element(bridge_rows, row);
    }
    json_object_set_array_member(bridge_result, "rows", bridge_rows);
    json_array_add_object_element(results, bridge_result);

    JsonObject *port_result = json_object_new();
    JsonArray *port_rows = json_array_new();
    g_hash_table_iter_init(&iter, O.port_uuids);
    while (g_hash_table_iter_next(&iter, &key, NULL)) {
        const gchar *name = key;
        JsonObject *row = json_object_new();
        json_object_set_member(row, "_uuid",
            _fake_uuid_node(_fake_lookup(O.port_uuids, name)));
        json_object_set_string_member(row, "name", name);
        GPtrArray *interfaces = g_ptr_array_new_with_free_func(g_free);
        const gchar *interface_uuid = _fake_lookup(O.port_interfaces, name);
        if (*interface_uuid)
            g_ptr_array_add(interfaces, g_strdup(interface_uuid));
        json_object_set_member(row, "interfaces",
                               _fake_uuid_set_node(interfaces));
        g_ptr_array_free(interfaces, TRUE);
        json_object_set_member(row, "external_ids",
                               _fake_external_ids_node(
                                   _fake_lookup(O.port_row_owners, name),
                                   _fake_lookup(O.port_row_names, name)));
        json_array_add_object_element(port_rows, row);
    }
    json_object_set_array_member(port_result, "rows", port_rows);
    json_array_add_object_element(results, port_result);

    JsonObject *interface_result = json_object_new();
    JsonArray *interface_rows = json_array_new();
    g_hash_table_iter_init(&iter, O.interface_uuids);
    while (g_hash_table_iter_next(&iter, &key, NULL)) {
        const gchar *name = key;
        JsonObject *row = json_object_new();
        json_object_set_member(row, "_uuid",
            _fake_uuid_node(_fake_lookup(O.interface_uuids, name)));
        json_object_set_string_member(row, "name", name);
        json_object_set_string_member(row, "type",
                                      _fake_lookup(O.port_types, name));
        json_object_set_member(row, "options",
                               _fake_interface_options_node(name));
        json_object_set_member(row, "external_ids", _fake_external_ids_node(
            _fake_lookup(O.port_owners, name),
            _fake_lookup(O.port_names, name)));
        json_array_add_object_element(interface_rows, row);
    }
    json_object_set_array_member(interface_result, "rows", interface_rows);
    json_array_add_object_element(results, interface_result);

    JsonNode *root = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(root, results);
    gchar *json = json_to_string(root, FALSE);
    json_node_free(root);
    return json;
}

static gchar *
_fake_ipv4_snapshot_json(void)
{
    JsonArray *links = json_array_new();
    GHashTableIter iter;
    gpointer key = NULL;
    g_hash_table_iter_init(&iter, O.bridges);
    while (g_hash_table_iter_next(&iter, &key, NULL)) {
        const gchar *name = key;
        JsonObject *link = json_object_new();
        json_object_set_string_member(link, "ifname", name);
        JsonArray *infos = json_array_new();
        const gchar *cidr = _fake_lookup(O.bridge_cidrs, name);
        if (*cidr) {
            gchar **parts = g_strsplit(cidr, "/", 2);
            JsonObject *info = json_object_new();
            json_object_set_string_member(info, "family", "inet");
            json_object_set_string_member(info, "local", parts[0]);
            json_object_set_int_member(info, "prefixlen",
                                       parts[1] ? g_ascii_strtoll(parts[1], NULL, 10) : 32);
            json_array_add_object_element(infos, info);
            g_strfreev(parts);
        }
        json_object_set_array_member(link, "addr_info", infos);
        json_array_add_object_element(links, link);
    }
    g_hash_table_iter_init(&iter, O.kernel_links);
    while (g_hash_table_iter_next(&iter, &key, NULL)) {
        const gchar *name = key;
        if (g_hash_table_contains(O.bridges, name))
            continue;
        JsonObject *link = json_object_new();
        json_object_set_string_member(link, "ifname", name);
        json_object_set_array_member(link, "addr_info", json_array_new());
        json_array_add_object_element(links, link);
    }
    JsonNode *root = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(root, links);
    gchar *json = json_to_string(root, FALSE);
    json_node_free(root);
    return json;
}

static gboolean
_overlay_fake_exec(const gchar * const *argv, gchar **stdout_out, GError **error)
{
    if (stdout_out)
        *stdout_out = NULL;
    gchar *command = _fake_join(argv);

    g_mutex_lock(&O.gate_mu);
    if (O.block_contains && strstr(command, O.block_contains)) {
        O.blocked = TRUE;
        g_cond_broadcast(&O.gate_cond);
        while (!O.release)
            g_cond_wait(&O.gate_cond, &O.gate_mu);
    }
    g_mutex_unlock(&O.gate_mu);

    g_mutex_lock(&O.state_mu);
    g_ptr_array_add(O.commands, g_strdup(command));
    if (g_strcmp0(argv[0], "ovs-vsctl") == 0 &&
        (!argv[1] || !g_str_has_prefix(argv[1], "--timeout=")))
        O.saw_unbounded_ovs = TRUE;
    if (g_strcmp0(argv[0], "ovsdb-client") == 0 &&
        (!argv[1] || !g_str_has_prefix(argv[1], "--timeout=")))
        O.saw_unbounded_ovs = TRUE;

    if (O.fail_remaining && O.fail_contains && strstr(command, O.fail_contains)) {
        O.fail_remaining--;
        g_set_error(error, G_IO_ERROR, O.fail_code,
                    "injected command failure: %s", command);
        g_mutex_unlock(&O.state_mu);
        g_free(command);
        return FALSE;
    }
    if (O.race_bridge_name) {
        gchar *needle = g_strdup_printf("add-br %s", O.race_bridge_name);
        if (strstr(command, needle)) {
            g_hash_table_add(O.bridges, g_strdup(O.race_bridge_name));
            _fake_assign_bridge_uuid(O.race_bridge_name);
            _fake_assign_port_uuid(O.race_bridge_name);
            _fake_assign_interface_uuid(O.race_bridge_name);
            _fake_set(O.bridge_owners, O.race_bridge_name, "foreign-race-token");
            _fake_set(O.bridge_names, O.race_bridge_name, "foreign-race");
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                        "injected absent-to-create race");
            g_free(needle);
            g_mutex_unlock(&O.state_mu);
            g_free(command);
            return FALSE;
        }
        g_free(needle);
    }
    if (O.mutate_contains && O.mutate_meta_path &&
        strstr(command, O.mutate_contains)) {
        (void)g_remove(O.mutate_meta_path);
        (void)g_mkdir(O.mutate_meta_path, 0700);
        g_clear_pointer(&O.mutate_contains, g_free);
    }
    if (O.command_hook)
        O.command_hook(command, TRUE, O.command_hook_data);

    if (g_strcmp0(argv[0], "ovsdb-client") == 0 &&
        _fake_arg_index(argv, "query") >= 0) {
        if (stdout_out)
            *stdout_out = _fake_bulk_snapshot_json();
        g_mutex_unlock(&O.state_mu);
        g_free(command);
        return TRUE;
    }

    if (g_strcmp0(argv[0], "ip") == 0) {
        if (_fake_arg_index(argv, "-j") >= 0) {
            if (stdout_out)
                *stdout_out = _fake_ipv4_snapshot_json();
            g_mutex_unlock(&O.state_mu);
            g_free(command);
            return TRUE;
        }
        gint show = _fake_arg_index(argv, "show");
        if (show >= 0 && argv[show + 2]) {
            const gchar *bridge = argv[show + 2];
            const gchar *cidr = _fake_lookup(O.bridge_cidrs, bridge);
            if (stdout_out)
                *stdout_out = *cidr
                    ? g_strdup_printf("1: %s inet %s scope global %s\n",
                                      bridge, cidr, bridge)
                    : g_strdup("");
        }
        gint replace = _fake_arg_index(argv, "replace");
        if (replace >= 0 && argv[replace + 3])
            _fake_set(O.bridge_cidrs, argv[replace + 3], argv[replace + 1]);
        g_mutex_unlock(&O.state_mu);
        g_free(command);
        return TRUE;
    }

    gint list = _fake_arg_index(argv, "list");
    if (_fake_arg_index(argv, "--format=json") >= 0 &&
        _fake_arg_index(argv, "--columns=options") >= 0 && list >= 0 &&
        g_strcmp0(argv[list + 1], "Interface") == 0 && argv[list + 2]) {
        if (stdout_out) {
            gchar *options = _fake_options_json(argv[list + 2]);
            *stdout_out = g_strdup_printf(
                "{\"data\":[[%s]],\"headings\":[\"options\"]}", options);
            g_free(options);
        }
        g_mutex_unlock(&O.state_mu);
        g_free(command);
        return TRUE;
    }
    if (_fake_arg_index(argv, "--format=json") >= 0 && list >= 0 &&
        argv[list + 1] &&
        (g_strcmp0(argv[list + 1], "Bridge") == 0 ||
         g_strcmp0(argv[list + 1], "Interface") == 0)) {
        if (stdout_out)
            *stdout_out = _fake_marker_snapshot_json(argv[list + 1]);
        g_mutex_unlock(&O.state_mu);
        g_free(command);
        return TRUE;
    }

    gint op = _fake_arg_index(argv, "find");
    if (op >= 0 && argv[op + 1] && argv[op + 2]) {
        const gchar *table = argv[op + 1];
        const gchar *predicate = argv[op + 2];
        const gchar *equal = strchr(predicate, '=');
        gchar *column = equal ? g_strndup(predicate, equal - predicate) : g_strdup("");
        const gchar *value = equal ? equal + 1 : "";
        gchar *match = NULL;
        if (g_strcmp0(column, "name") == 0) {
            if (_fake_row_exists(table, value))
                match = g_strdup(value);
        } else if (g_str_has_prefix(column, "external_ids:pcv_overlay_")) {
            GHashTable *fields = NULL;
            if (g_strcmp0(table, "Interface") == 0)
                fields = g_str_has_suffix(column, "owner")
                    ? O.port_owners : O.port_names;
            else if (g_strcmp0(table, "Bridge") == 0)
                fields = g_str_has_suffix(column, "owner")
                    ? O.bridge_owners : O.bridge_names;
            GHashTableIter iter;
            gpointer key = NULL;
            gpointer field = NULL;
            GString *matches = g_string_new(NULL);
            if (fields)
                g_hash_table_iter_init(&iter, fields);
            while (fields && g_hash_table_iter_next(&iter, &key, &field)) {
                if (g_strcmp0(field, value) == 0)
                    g_string_append_printf(matches, "%s\n", (gchar *)key);
            }
            match = g_string_free(matches, FALSE);
        }
        if (stdout_out)
            *stdout_out = match ? g_strdup(match) : g_strdup("");
        g_free(match); g_free(column);
        g_mutex_unlock(&O.state_mu);
        g_free(command);
        return TRUE;
    }

    op = _fake_arg_index(argv, "get");
    if (op >= 0 && argv[op + 3]) {
        if (g_strcmp0(argv[op + 1], "Interface") == 0 &&
            g_strcmp0(argv[op + 3], "options") == 0) {
            if (stdout_out)


                *stdout_out = _fake_options_map(argv[op + 2]);
            g_mutex_unlock(&O.state_mu);
            g_free(command);
            return TRUE;
        }
        const gchar *value = _fake_field(argv[op + 1], argv[op + 2], argv[op + 3]);
        if (stdout_out)
            *stdout_out = *value ? g_strdup_printf("%s\n", value)
                                 : g_strdup("[]\n");
        g_mutex_unlock(&O.state_mu);
        g_free(command);
        return TRUE;
    }

    op = _fake_arg_index(argv, "iface-to-br");
    if (op >= 0 && argv[op + 1]) {
        const gchar *bridge = g_hash_table_lookup(O.port_bridges, argv[op + 1]);
        if (!bridge) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                        "fake interface absent: %s", argv[op + 1]);
            g_mutex_unlock(&O.state_mu);
            g_free(command);
            return FALSE;
        }
        if (stdout_out)
            *stdout_out = g_strdup_printf("%s\n", bridge);
        g_mutex_unlock(&O.state_mu);
        g_free(command);
        return TRUE;
    }

    op = _fake_arg_index(argv, "list-ports");
    if (op >= 0 && argv[op + 1]) {
        GString *ports = g_string_new(NULL);
        GHashTableIter iter;
        gpointer key = NULL;
        gpointer bridge = NULL;
        g_hash_table_iter_init(&iter, O.port_bridges);
        while (g_hash_table_iter_next(&iter, &key, &bridge)) {
            if (g_strcmp0(bridge, argv[op + 1]) == 0)
                g_string_append_printf(ports, "%s\n", (gchar *)key);
        }
        if (stdout_out)
            *stdout_out = g_string_free(ports, FALSE);
        else
            g_string_free(ports, TRUE);
        g_mutex_unlock(&O.state_mu);
        g_free(command);
        return TRUE;
    }

    gboolean ok = _fake_transaction(argv, error);
    if (O.command_hook)
        O.command_hook(command, FALSE, O.command_hook_data);
    if (ok && O.fail_after_remaining && O.fail_after_contains &&
        strstr(command, O.fail_after_contains)) {
        O.fail_after_remaining--;
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                    "injected applied-then-timeout: %s", command);
        ok = FALSE;
    }
    g_mutex_unlock(&O.state_mu);
    g_free(command);
    return ok;
}

static void
_fake_seed_bridge(const gchar *name, const gchar *cidr,
                  const gchar *owner, const gchar *overlay_name)
{
    g_hash_table_add(O.bridges, g_strdup(name));
    _fake_assign_bridge_uuid(name);
    _fake_assign_port_uuid(name);
    _fake_assign_interface_uuid(name);
    _fake_set(O.port_types, name, "internal");
    _fake_set(O.bridge_cidrs, name, cidr);
    if (owner)
        _fake_set(O.bridge_owners, name, owner);
    if (overlay_name)
        _fake_set(O.bridge_names, name, overlay_name);
}

static void
_fake_seed_port(const gchar *bridge, const gchar *port, const gchar *owner,
                const gchar *overlay_name, const gchar *type, const gchar *key,
                const gchar *remote, const gchar *local)
{
    _fake_set(O.port_bridges, port, bridge);
    _fake_assign_port_uuid(port);
    _fake_assign_interface_uuid(port);
    if (owner) _fake_set(O.port_owners, port, owner);
    if (overlay_name) _fake_set(O.port_names, port, overlay_name);
    if (type) _fake_set(O.port_types, port, type);
    if (key) _fake_set(O.port_keys, port, key);
    if (remote) _fake_set(O.port_remote, port, remote);
    if (local) _fake_set(O.port_local, port, local);
}

static void
_overlay_fake_clear(void)
{
    if (!O.bridges)
        return;
    g_hash_table_destroy(O.bridges);
    g_hash_table_destroy(O.kernel_links);
    g_hash_table_destroy(O.bridge_uuids);
    g_hash_table_destroy(O.bridge_owners);
    g_hash_table_destroy(O.bridge_names);
    g_hash_table_destroy(O.bridge_cidrs);
    g_hash_table_destroy(O.bridge_datapaths);
    g_hash_table_destroy(O.bridge_legacy_owners);
    g_hash_table_destroy(O.port_bridges);
    g_hash_table_destroy(O.port_row_owners);
    g_hash_table_destroy(O.port_row_names);
    g_hash_table_destroy(O.port_owners);
    g_hash_table_destroy(O.port_names);
    g_hash_table_destroy(O.port_types);
    g_hash_table_destroy(O.port_keys);
    g_hash_table_destroy(O.port_remote);
    g_hash_table_destroy(O.port_local);
    g_hash_table_destroy(O.port_extras);
    g_hash_table_destroy(O.port_uuids);
    g_hash_table_destroy(O.interface_uuids);
    g_hash_table_destroy(O.port_interfaces);
    g_hash_table_destroy(O.port_legacy_owners);
    g_ptr_array_free(O.commands, TRUE);
    g_free(O.fail_contains);
    g_free(O.fail_after_contains);
    g_free(O.race_bridge_name);
    g_free(O.mutate_contains);
    g_free(O.mutate_meta_path);
    g_free(O.extra_parent_bridge);
    g_free(O.extra_parent_port);
    g_free(O.block_contains);
    g_mutex_clear(&O.state_mu);
    g_mutex_clear(&O.gate_mu);
    g_cond_clear(&O.gate_cond);
    memset(&O, 0, sizeof(O));
}

static GHashTable *
_fake_string_map(void)
{
    return g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
}

static void
_overlay_fake_reset(void)
{
    _overlay_fake_clear();
    g_mutex_init(&O.state_mu);
    g_mutex_init(&O.gate_mu);
    g_cond_init(&O.gate_cond);
    O.bridges = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    O.kernel_links = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    O.bridge_uuids = _fake_string_map();
    O.bridge_owners = _fake_string_map();
    O.bridge_names = _fake_string_map();
    O.bridge_cidrs = _fake_string_map();
    O.bridge_datapaths = _fake_string_map();
    O.bridge_legacy_owners = _fake_string_map();
    O.port_bridges = _fake_string_map();
    O.port_row_owners = _fake_string_map();
    O.port_row_names = _fake_string_map();
    O.port_owners = _fake_string_map();
    O.port_names = _fake_string_map();
    O.port_types = _fake_string_map();
    O.port_keys = _fake_string_map();
    O.port_remote = _fake_string_map();
    O.port_local = _fake_string_map();
    O.port_extras = _fake_string_map();
    O.port_uuids = _fake_string_map();
    O.interface_uuids = _fake_string_map();
    O.port_interfaces = _fake_string_map();
    O.port_legacy_owners = _fake_string_map();
    O.commands = g_ptr_array_new_with_free_func(g_free);
}

static void
_fake_fail_once_with_code(const gchar *contains, GIOErrorEnum code)
{
    g_free(O.fail_contains);
    O.fail_contains = g_strdup(contains);
    O.fail_remaining = 1;
    O.fail_code = code;
}

static void
_fake_fail_once(const gchar *contains)
{
    _fake_fail_once_with_code(contains, G_IO_ERROR_FAILED);
}

static void
_fake_apply_then_timeout_once(const gchar *contains)
{
    g_free(O.fail_after_contains);
    O.fail_after_contains = g_strdup(contains);
    O.fail_after_remaining = 1;
}

static guint
_fake_command_count(const gchar *contains)
{
    guint count = 0;
    g_mutex_lock(&O.state_mu);
    for (guint i = 0; i < O.commands->len; i++)
        if (strstr(g_ptr_array_index(O.commands, i), contains))
            count++;
    g_mutex_unlock(&O.state_mu);
    return count;
}

static void
_fake_block(const gchar *contains)
{
    g_mutex_lock(&O.gate_mu);
    g_free(O.block_contains);
    O.block_contains = g_strdup(contains);
    O.blocked = FALSE;
    O.release = FALSE;
    g_mutex_unlock(&O.gate_mu);
}

static void
_fake_wait_blocked(void)
{
    g_mutex_lock(&O.gate_mu);
    while (!O.blocked)
        g_cond_wait(&O.gate_cond, &O.gate_mu);
    g_mutex_unlock(&O.gate_mu);
}

static void
_fake_release(void)
{
    g_mutex_lock(&O.gate_mu);
    O.release = TRUE;
    g_cond_broadcast(&O.gate_cond);
    g_mutex_unlock(&O.gate_mu);
}

static void
_overlay_test_remove_tree(const gchar *path)
{
    if (!path)
        return;
    if (!g_file_test(path, G_FILE_TEST_IS_DIR)) {
        (void)g_remove(path);
        return;
    }
    GDir *dir = g_dir_open(path, 0, NULL);
    if (dir) {
        const gchar *name = NULL;
        while ((name = g_dir_read_name(dir)) != NULL) {
            gchar *child = g_build_filename(path, name, NULL);
            _overlay_test_remove_tree(child);
            g_free(child);
        }
        g_dir_close(dir);
    }
    (void)g_rmdir(path);
}

static void
_overlay_effect_setup(void)
{
    GError *error = NULL;
    _overlay_fake_reset();
    O_meta_dir = g_dir_make_tmp("pcv-overlay-effect-XXXXXX", &error);
    g_assert_no_error(error);
    pcv_overlay_set_restore_snapshot_test_hook(NULL, NULL);
    pcv_overlay_set_lifecycle_test_hook(NULL, NULL);
    pcv_overlay_set_metadata_test_hook(NULL, NULL);
    pcv_overlay_set_dir_sync_test_hook(NULL, NULL);
    pcv_overlay_set_metadata_stat_test_hook(NULL, NULL);
    pcv_overlay_set_metadata_limit_for_test(0);
    pcv_overlay_set_restore_deadline_for_test(0);
    pcv_overlay_set_test_context(_overlay_fake_exec, O_meta_dir);
    pcv_overlay_init("192.0.2.10");
}

static void
_overlay_effect_teardown(void)
{
    pcv_overlay_shutdown();
    pcv_overlay_set_restore_snapshot_test_hook(NULL, NULL);
    pcv_overlay_set_lifecycle_test_hook(NULL, NULL);
    pcv_overlay_set_metadata_test_hook(NULL, NULL);
    pcv_overlay_set_dir_sync_test_hook(NULL, NULL);
    pcv_overlay_set_metadata_stat_test_hook(NULL, NULL);
    pcv_overlay_set_metadata_limit_for_test(0);
    pcv_overlay_set_restore_deadline_for_test(0);
    pcv_overlay_set_test_context(NULL, NULL);
    _overlay_test_remove_tree(O_meta_dir);
    g_clear_pointer(&O_meta_dir, g_free);
    g_assert_false(O.saw_unbounded_ovs);
    _overlay_fake_clear();
}

static gchar *
_overlay_meta_path(const gchar *name)
{
    gchar *filename = g_strdup_printf("overlay-%s.meta", name);
    gchar *path = g_build_filename(O_meta_dir, filename, NULL);
    g_free(filename);
    return path;
}

static gboolean
_test_write_metadata(const gchar *path, const gchar *data, gssize length,
                     GError **error)
{
    if (!g_file_set_contents(path, data, length, error))
        return FALSE;
    if (g_chmod(path, 0600) == 0)
        return TRUE;
    gint saved_errno = errno;
    g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                "Cannot chmod test metadata '%s': %s", path,
                g_strerror(saved_errno));
    return FALSE;
}

static guint
_overlay_peer_count(const gchar *name)
{
    GError *error = NULL;
    JsonObject *info = pcv_overlay_info(name, &error);
    g_assert_no_error(error);
    g_assert_nonnull(info);
    guint count = json_array_get_length(json_object_get_array_member(info, "peers"));
    json_object_unref(info);
    return count;
}

static gchar *
_meta_owner(const gchar *path, gint64 *schema, gint64 *generation)
{
    JsonParser *parser = json_parser_new();
    GError *error = NULL;
    g_assert_true(json_parser_load_from_file(parser, path, &error));
    g_assert_no_error(error);
    JsonObject *object = json_node_get_object(json_parser_get_root(parser));
    *schema = json_object_get_int_member(object, "schema_version");
    *generation = json_object_get_int_member(object, "generation");
    gchar *owner = g_strdup(json_object_get_string_member(object, "owner_token"));
    g_object_unref(parser);
    return owner;
}

typedef struct {
    const gchar *contains;
    const gchar *path;
    const gchar *contents;
    gboolean before;
    gboolean fired;
} FakeFileRace;

static void
_fake_file_race_hook(const gchar *command, gboolean before, gpointer user_data)
{
    FakeFileRace *race = user_data;
    if (race->fired || before != race->before ||
        !strstr(command, race->contains))
        return;
    GError *error = NULL;
    g_assert_true(g_file_set_contents(race->path, race->contents, -1, &error));
    g_assert_no_error(error);
    race->fired = TRUE;
}

typedef struct {
    const gchar *contains;
    const gchar *bridge;
    const gchar *port;
    const gchar *owner;
    const gchar *overlay_name;
    gboolean after;
    gboolean fired;
} FakePortRace;

static void
_fake_port_race_hook(const gchar *command, gboolean before, gpointer user_data)
{
    FakePortRace *race = user_data;
    if (race->fired || before == race->after ||
        !strstr(command, race->contains))
        return;
    _fake_seed_port(race->bridge, race->port, race->owner,
                    race->overlay_name, "vxlan", "777",
                    "198.51.100.77", "192.0.2.10");
    race->fired = TRUE;
}

typedef struct {
    const gchar *contains;
    const gchar *port;
    const gchar *new_bridge;
    gboolean fired;
} FakeMoveRace;

static void
_fake_move_race_hook(const gchar *command, gboolean before, gpointer user_data)
{
    FakeMoveRace *race = user_data;
    if (race->fired || !before || !strstr(command, race->contains))
        return;
    _fake_set(O.port_bridges, race->port, race->new_bridge);
    race->fired = TRUE;
}

typedef struct {
    const gchar *contains;
    const gchar *failure;
    gboolean fired;
} FakeArmFailure;

static void
_fake_arm_failure_hook(const gchar *command, gboolean before, gpointer user_data)
{
    FakeArmFailure *arm = user_data;
    if (arm->fired || !before || !strstr(command, arm->contains))
        return;
    g_free(O.fail_contains);
    O.fail_contains = g_strdup(arm->failure);
    O.fail_remaining = 1;
    O.fail_code = G_IO_ERROR_FAILED;
    arm->fired = TRUE;
}

typedef struct {
    const gchar *contains;
    const gchar *rollback_failure;
    const gchar *canonical_port;
    const gchar *sidecar_path;
    gboolean inject_postcheck_drift;
    gboolean fired;
} LegacyRollbackFault;

typedef struct {
    const gchar *migration_fragment;
    const gchar *rollback_fragment;
    const gchar *canonical_port;
    gboolean fired;
} LegacyAppliedTimeoutFault;

static void
_legacy_rollback_fault_hook(const gchar *command, gboolean before,
                            gpointer user_data)
{
    LegacyRollbackFault *fault = user_data;
    if (fault->fired || before || !strstr(command, fault->contains))
        return;
    fault->fired = TRUE;
    if (fault->inject_postcheck_drift)
        _fake_set(O.port_extras, fault->canonical_port, "dst_port=9999");
    if (fault->sidecar_path) {
        GError *error = NULL;
        g_assert_true(_test_write_metadata(fault->sidecar_path,
                                           "external-claim", -1, &error));
        g_assert_no_error(error);
    }
    g_free(O.fail_contains);
    O.fail_contains = g_strdup(fault->rollback_failure);
    O.fail_remaining = 1;
    O.fail_code = G_IO_ERROR_FAILED;
}

static void
_legacy_rollback_applied_timeout_hook(const gchar *command, gboolean before,
                                      gpointer user_data)
{
    LegacyAppliedTimeoutFault *fault = user_data;
    if (fault->fired || before ||
        !strstr(command, fault->migration_fragment))
        return;
    fault->fired = TRUE;


    _fake_set(O.port_extras, fault->canonical_port, "dst_port=9999");
    g_free(O.fail_after_contains);
    O.fail_after_contains = g_strdup(fault->rollback_fragment);
    O.fail_after_remaining = 1;
}

static void
_metadata_residue_hook(const gchar *overlay_name, const gchar *canonical,
                       const gchar *claim, gpointer user_data)
{
    gboolean *fired = user_data;
    if (*fired)
        return;
    *fired = TRUE;
    g_assert_cmpint(g_remove(claim), ==, 0);
    g_assert_cmpint(g_mkdir(claim, 0700), ==, 0);
    gchar *child = g_build_filename(claim, "external", NULL);
    GError *error = NULL;
    g_assert_true(g_file_set_contents(child, "do-not-delete", -1, &error));
    g_assert_no_error(error);
    g_free(child);
    (void)overlay_name;
    (void)canonical;
}

typedef struct {
    guint calls;
    guint fail_at;
} DirSyncFault;

static gboolean
_dir_sync_fault_hook(GError **error, gpointer user_data)
{
    DirSyncFault *fault = user_data;
    fault->calls++;
    if (fault->calls != fault->fail_at)
        return TRUE;
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "injected metadata directory sync failure");
    return FALSE;
}

typedef struct {
    guint calls;
    guint fail_at;
} MetadataStatFault;

static gboolean
_metadata_stat_fault_hook(const gchar *path, GError **error,
                          gpointer user_data)
{
    MetadataStatFault *fault = user_data;
    fault->calls++;
    if (fault->calls != fault->fail_at)
        return TRUE;
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "injected post-O_EXCL metadata fstat failure for %s", path);
    return FALSE;
}

static void test_overlay_list_empty_single(void)
{
    JsonArray *array = pcv_overlay_list(NULL);
    g_assert_nonnull(array);
    g_assert_cmpuint(json_array_get_length(array), ==, 0);
    json_array_unref(array);
}

static void test_overlay_disabled_and_validation(void)
{
    GError *error = NULL;
    JsonObject *info = pcv_overlay_info("pcvoverlay0", NULL);
    g_assert_cmpstr(json_object_get_string_member(info, "error"), ==,
                    "overlay disabled");
    json_object_unref(info);
    g_assert_false(pcv_overlay_delete("pcvoverlay0", &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_clear_error(&error);
    g_assert_false(pcv_overlay_remove_peer("pcvoverlay0", "192.0.2.20", &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_clear_error(&error);

    g_assert_false(pcv_overlay_create("1234567890abcdef", 100, NULL, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);
    g_assert_false(pcv_overlay_create("pcvovl0", 0, NULL, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);
    g_assert_false(pcv_overlay_create("pcvovl0", 16777216, NULL, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);
    g_assert_false(pcv_overlay_add_peer("pcvovl0", "2001:db8::1", &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);
    g_assert_false(pcv_overlay_create("pcvovl0", 100,
                                      "2001:db8::1/64", &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);
}

static void test_overlay_disabled_reads_expose_metadata_residue(void)
{
    GError *error = NULL;
    _overlay_fake_reset();
    O_meta_dir = g_dir_make_tmp("pcv-overlay-disabled-XXXXXX", &error);
    g_assert_no_error(error);
    pcv_overlay_set_test_context(_overlay_fake_exec, O_meta_dir);

    JsonArray *clean = pcv_overlay_list(&error);
    g_assert_no_error(error);
    g_assert_nonnull(clean);
    g_assert_cmpuint(json_array_get_length(clean), ==, 0);
    json_array_unref(clean);

    const gchar *entries[] = {
        "overlay-disabled.meta",
        "overlay-disabled.meta.updating",
        "overlay-disabled.meta.deleting",
    };
    for (guint i = 0; i < G_N_ELEMENTS(entries); i++) {
        gchar *path = g_build_filename(O_meta_dir, entries[i], NULL);
        g_assert_true(g_file_set_contents(path, "residue", -1, &error));
        g_assert_no_error(error);
        JsonArray *list = pcv_overlay_list(&error);
        g_assert_null(list);
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
        g_clear_error(&error);
        JsonObject *info = pcv_overlay_info("disabled", &error);
        g_assert_null(info);
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
        g_clear_error(&error);
        g_assert_cmpint(g_remove(path), ==, 0);
        g_free(path);
    }

    pcv_overlay_set_test_context(NULL, NULL);
    _overlay_test_remove_tree(O_meta_dir);
    g_clear_pointer(&O_meta_dir, g_free);
    _overlay_fake_clear();
}

static void test_overlay_ipv4_cidr_only_is_fail_closed(void)
{
    _overlay_effect_setup();
    GError *error = NULL;
    guint create_before = _fake_command_count("add-br v6create");
    g_assert_false(pcv_overlay_create("v6create", 207,
                                      "2001:db8::1/64", &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);
    g_assert_cmpuint(_fake_command_count("add-br v6create"), ==,
                     create_before);
    gchar *meta = _overlay_meta_path("v6create");
    g_assert_false(g_file_test(meta, G_FILE_TEST_EXISTS));
    g_free(meta);

    meta = _overlay_meta_path("v6restore");
    const gchar *json =
        "{\"schema_version\":2,\"name\":\"v6restore\",\"vni\":208,"
        "\"cidr\":\"2001:db8::1/64\",\"owner_token\":"
        "\"c0000000-0000-4000-8000-000000000002\",\"generation\":1,"
        "\"peers\":[]}";
    g_assert_true(_test_write_metadata(meta, json, -1, &error));
    g_assert_no_error(error);
    pcv_overlay_restore();
    JsonArray *list = pcv_overlay_list(&error);
    g_assert_null(list);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    g_assert_cmpuint(_fake_command_count("add-br v6restore"), ==, 0);
    g_assert_false(g_hash_table_contains(O.bridges, "v6restore"));
    g_free(meta);
    _overlay_effect_teardown();
}

static void test_overlay_peer_port_name_is_injective(void)
{
    gchar *a = pcv_overlay_peer_port_name(100, "192.168.0.104");
    gchar *b = pcv_overlay_peer_port_name(1, "10.1.0.104");
    gchar *c = pcv_overlay_peer_port_name(101, "192.168.0.104");
    gchar *maximum = pcv_overlay_peer_port_name(16777215, "255.255.255.255");
    g_assert_cmpstr(a, ==, "v000064c0a80068");
    g_assert_cmpstr(b, ==, "v0000010a010068");
    g_assert_cmpstr(maximum, ==, "vffffffffffffff");
    g_assert_cmpstr(a, !=, b);
    g_assert_cmpstr(a, !=, c);
    g_assert_cmpuint(strlen(a), ==, 15);
    g_assert_null(pcv_overlay_peer_port_name(0, "192.168.0.104"));
    g_assert_null(pcv_overlay_peer_port_name(100, "not-an-ip"));
    g_free(a); g_free(b); g_free(c); g_free(maximum);
}

static void test_overlay_create_ownership_and_exact_contract(void)
{
    _overlay_effect_setup();
    GError *error = NULL;
    _fake_seed_bridge("foreign0", "", "foreign-token", "foreign0");
    g_assert_false(pcv_overlay_create("foreign0", 200, NULL, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS);
    g_clear_error(&error);
    g_assert_false(pcv_overlay_delete("foreign0", &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_assert_true(g_hash_table_contains(O.bridges, "foreign0"));

    O.race_bridge_name = g_strdup("race0");
    g_assert_false(pcv_overlay_create("race0", 201, NULL, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS);
    g_clear_error(&error);
    g_assert_cmpstr(_fake_lookup(O.bridge_owners, "race0"), ==,
                    "foreign-race-token");
    g_clear_pointer(&O.race_bridge_name, g_free);

    g_assert_true(pcv_overlay_create("ovl100", 100, "10.100.0.1/24", &error));
    g_assert_no_error(error);
    gchar *owner = g_strdup(_fake_lookup(O.bridge_owners, "ovl100"));
    g_assert_true(g_uuid_string_is_valid(owner));
    g_assert_cmpstr(_fake_lookup(O.bridge_names, "ovl100"), ==, "ovl100");
    g_assert_true(pcv_overlay_create("ovl100", 100, "10.100.0.1/24", &error));
    g_assert_no_error(error);
    g_assert_false(pcv_overlay_create("ovl100", 101, "10.100.0.1/24", &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS);
    g_clear_error(&error);
    g_assert_false(pcv_overlay_create("other100", 100, NULL, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS);
    g_clear_error(&error);

    gchar *meta = _overlay_meta_path("ovl100");
    gint64 schema = 0;
    gint64 generation = 0;
    gchar *meta_token = _meta_owner(meta, &schema, &generation);
    g_assert_cmpint(schema, ==, 2);
    g_assert_cmpint(generation, ==, 1);
    g_assert_cmpstr(meta_token, ==, owner);
    gchar *canonical_meta = NULL;
    gsize canonical_meta_len = 0;
    g_assert_true(g_file_get_contents(meta, &canonical_meta,
                                     &canonical_meta_len, &error));
    g_assert_no_error(error);
    const gchar *stale_meta =
        "{\"schema_version\":2,\"name\":\"ovl100\",\"vni\":100,"
        "\"cidr\":\"10.100.0.1/24\",\"owner_token\":"
        "\"33333333-3333-4333-8333-333333333333\","
        "\"generation\":2,\"peers\":[]}";
    g_assert_true(_test_write_metadata(meta, stale_meta, -1, &error));
    g_assert_no_error(error);
    guint actual_before = _fake_command_count("set Bridge ovl100");
    g_assert_false(pcv_overlay_create("ovl100", 100, "10.100.0.1/24", &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    g_assert_cmpuint(_fake_command_count("set Bridge ovl100"), ==,
                     actual_before);
    g_assert_cmpstr(_fake_lookup(O.bridge_owners, "ovl100"), ==, owner);
    g_assert_true(_test_write_metadata(meta, canonical_meta,
                                      (gssize)canonical_meta_len, &error));
    g_assert_no_error(error);
    g_free(canonical_meta);
    g_free(meta_token); g_free(meta);

    JsonArray *list = pcv_overlay_list(&error);
    g_assert_no_error(error);
    g_assert_cmpuint(json_array_get_length(list), ==, 1);
    JsonObject *row = json_array_get_object_element(list, 0);
    g_assert_cmpstr(json_object_get_string_member(row, "name"), ==, "ovl100");
    g_assert_true(json_object_get_boolean_member(row, "active"));
    json_array_unref(list);

    _fake_remove_bridge("ovl100");
    list = pcv_overlay_list(&error);
    g_assert_no_error(error);
    row = json_array_get_object_element(list, 0);
    g_assert_false(json_object_get_boolean_member(row, "active"));
    json_array_unref(list);
    g_assert_true(pcv_overlay_create("ovl100", 100, "10.100.0.1/24", &error));
    g_assert_no_error(error);



    g_assert_true(g_hash_table_contains(O.bridges, "ovl100"));
    g_assert_cmpstr(_fake_lookup(O.bridge_owners, "ovl100"), ==, owner);
    _fake_set(O.bridge_owners, "ovl100", "foreign-read-token");
    list = pcv_overlay_list(&error);
    g_assert_null(list);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS);
    g_clear_error(&error);
    _fake_set(O.bridge_owners, "ovl100", owner);



    gchar *local_uuid = g_strdup(_fake_lookup(O.port_uuids, "ovl100"));
    gchar *local_fence = g_strdup_printf("ports=[%s]", local_uuid);
    g_assert_true(pcv_overlay_delete("ovl100", &error));
    g_assert_no_error(error);
    g_assert_cmpuint(_fake_command_count("ports=[]"), ==, 0);
    g_assert_cmpuint(_fake_command_count(local_fence), >, 0);
    g_assert_false(g_hash_table_contains(O.bridges, "ovl100"));
    g_assert_true(pcv_overlay_delete("ovl100", &error));
    g_assert_no_error(error);
    g_free(local_fence); g_free(local_uuid); g_free(owner);
    _overlay_effect_teardown();
}

static void test_overlay_kernel_link_name_collision_is_preserved(void)
{
    _overlay_effect_setup();
    GError *error = NULL;
    g_hash_table_add(O.kernel_links, g_strdup("kerncreate"));
    g_assert_false(pcv_overlay_create("kerncreate", 205,
                                      "10.205.0.1/24", &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS);
    g_clear_error(&error);
    g_assert_cmpuint(_fake_command_count("add-br kerncreate"), ==, 0);
    g_assert_cmpuint(_fake_command_count("link set kerncreate"), ==, 0);
    g_assert_cmpuint(_fake_command_count("addr replace 10.205.0.1/24"), ==, 0);
    g_assert_true(g_hash_table_contains(O.kernel_links, "kerncreate"));

    gchar *meta = _overlay_meta_path("kernrestore");
    const gchar *json =
        "{\"schema_version\":2,\"name\":\"kernrestore\",\"vni\":206,"
        "\"cidr\":\"10.206.0.1/24\",\"owner_token\":"
        "\"c0000000-0000-4000-8000-000000000001\",\"generation\":1,"
        "\"peers\":[]}";
    g_assert_true(_test_write_metadata(meta, json, -1, &error));
    g_assert_no_error(error);
    g_hash_table_add(O.kernel_links, g_strdup("kernrestore"));
    pcv_overlay_restore();
    JsonArray *list = pcv_overlay_list(&error);
    g_assert_null(list);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    g_assert_cmpuint(_fake_command_count("add-br kernrestore"), ==, 0);
    g_assert_cmpuint(_fake_command_count("link set kernrestore"), ==, 0);
    g_assert_cmpuint(_fake_command_count("addr replace 10.206.0.1/24"), ==, 0);
    g_assert_true(g_hash_table_contains(O.kernel_links, "kernrestore"));
    g_free(meta);
    _overlay_effect_teardown();
}

static void test_overlay_peer_scope_options_and_remove(void)
{
    _overlay_effect_setup();
    GError *error = NULL;
    g_assert_true(pcv_overlay_create("ovla", 310, "10.31.0.1/24", &error));
    g_assert_true(pcv_overlay_create("ovlb", 311, "10.32.0.1/24", &error));
    g_assert_no_error(error);
    g_assert_false(pcv_overlay_add_peer("ovla", "192.0.2.10", &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);

    g_assert_true(pcv_overlay_add_peer("ovla", "192.168.0.105", &error));
    g_assert_true(pcv_overlay_add_peer("ovlb", "192.168.0.105", &error));
    g_assert_no_error(error);
    gchar *porta = pcv_overlay_peer_port_name(310, "192.168.0.105");
    gchar *portb = pcv_overlay_peer_port_name(311, "192.168.0.105");
    g_assert_cmpstr(porta, !=, portb);
    g_assert_cmpstr(_fake_lookup(O.port_bridges, porta), ==, "ovla");
    g_assert_cmpstr(_fake_lookup(O.port_bridges, portb), ==, "ovlb");
    g_assert_cmpstr(_fake_lookup(O.port_keys, porta), ==, "310");
    g_assert_cmpstr(_fake_lookup(O.port_remote, porta), ==, "192.168.0.105");
    g_assert_cmpstr(_fake_lookup(O.port_local, porta), ==, "192.0.2.10");
    g_assert_cmpstr(_fake_lookup(O.port_owners, porta), ==,
                    _fake_lookup(O.bridge_owners, "ovla"));

    _fake_set(O.port_local, porta, "198.51.100.9");
    g_assert_false(pcv_overlay_add_peer("ovla", "192.168.0.105", &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS);
    g_clear_error(&error);
    g_assert_cmpstr(_fake_lookup(O.port_local, porta), ==, "198.51.100.9");
    _fake_set(O.port_local, porta, "192.0.2.10");
    g_assert_true(pcv_overlay_add_peer("ovla", "192.168.0.105", &error));
    g_assert_no_error(error);

    gchar *foreign = pcv_overlay_peer_port_name(310, "192.168.0.104");
    _fake_seed_port("foreign0", foreign, "foreign-token", "foreign0",
                    "vxlan", "310", "192.168.0.104", "192.0.2.10");
    g_assert_false(pcv_overlay_add_peer("ovla", "192.168.0.104", &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS);
    g_clear_error(&error);
    g_assert_cmpstr(_fake_lookup(O.port_owners, foreign), ==, "foreign-token");

    g_assert_true(pcv_overlay_remove_peer("ovla", "192.168.0.105", &error));
    g_assert_no_error(error);
    g_assert_false(g_hash_table_contains(O.port_bridges, porta));
    g_assert_true(g_hash_table_contains(O.port_bridges, portb));
    g_assert_cmpuint(_overlay_peer_count("ovla"), ==, 0);



    const gchar *owner = _fake_lookup(O.bridge_owners, "ovla");
    _fake_seed_port("ovla", porta, owner, "ovla", "vxlan", "310",
                    "192.168.0.105", "192.0.2.10");
    g_assert_false(pcv_overlay_remove_peer("ovla", "192.168.0.105", &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS);
    g_clear_error(&error);
    _fake_remove_port(porta);
    _fake_seed_port("ovla", "vxlan-0-105", NULL, NULL, "vxlan", "310",
                    "192.168.0.105", "192.0.2.10");
    g_assert_false(pcv_overlay_remove_peer("ovla", "192.168.0.105", &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS);
    g_clear_error(&error);
    _fake_remove_port("vxlan-0-105");
    g_assert_true(pcv_overlay_remove_peer("ovla", "192.168.0.105", &error));
    g_assert_no_error(error);
    g_free(foreign); g_free(porta); g_free(portb);
    _overlay_effect_teardown();
}

static void test_overlay_peer_drift_and_port_identity_fail_closed(void)
{
    _overlay_effect_setup();
    GError *error = NULL;
    const gchar *peer = "192.168.0.109";
    g_assert_true(pcv_overlay_create("drift0", 312, NULL, &error));
    g_assert_true(pcv_overlay_add_peer("drift0", peer, &error));
    g_assert_no_error(error);
    gchar *port = pcv_overlay_peer_port_name(312, peer);
    gchar *meta = _overlay_meta_path("drift0");
    gint64 schema = 0;
    gint64 generation = 0;
    gchar *owner = _meta_owner(meta, &schema, &generation);
    g_assert_cmpint(generation, ==, 2);

    struct {
        GHashTable *table;
        const gchar *bad;
        const gchar *good;
    } drift[] = {
        {O.port_types, "internal", "vxlan"},
        {O.port_keys, "999", "312"},
        {O.port_remote, "198.51.100.9", peer},
        {O.port_local, "198.51.100.10", "192.0.2.10"},
    };
    for (guint i = 0; i < G_N_ELEMENTS(drift); i++) {
        _fake_set(drift[i].table, port, drift[i].bad);
        guint del_before = _fake_command_count("del-port drift0");
        guint set_before = _fake_command_count("set Interface");
        g_assert_false(pcv_overlay_add_peer("drift0", peer, &error));
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS);
        g_clear_error(&error);
        g_assert_false(pcv_overlay_remove_peer("drift0", peer, &error));
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS);
        g_clear_error(&error);
        g_assert_cmpuint(_fake_command_count("del-port drift0"), ==,
                         del_before);
        g_assert_cmpuint(_fake_command_count("set Interface"), ==,
                         set_before);
        g_assert_cmpstr(_fake_lookup(drift[i].table, port), ==, drift[i].bad);
        gchar *observed = _meta_owner(meta, &schema, &generation);
        g_assert_cmpint(generation, ==, 2);
        g_assert_cmpstr(observed, ==, owner);
        g_free(observed);
        _fake_set(drift[i].table, port, drift[i].good);
    }

    const gchar *interface_uuid = _fake_lookup(O.interface_uuids, port);
    gchar *expected_uuid = g_strdup(interface_uuid);



    _fake_set(O.port_interfaces, port,
              _fake_lookup(O.interface_uuids, "drift0"));
    guint del_before = _fake_command_count("del-port drift0");
    g_assert_false(pcv_overlay_add_peer("drift0", peer, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS);
    g_clear_error(&error);
    g_assert_false(pcv_overlay_remove_peer("drift0", peer, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS);
    g_clear_error(&error);
    g_assert_cmpuint(_fake_command_count("del-port drift0"), ==, del_before);
    g_assert_true(g_hash_table_contains(O.port_bridges, port));
    gchar *observed = _meta_owner(meta, &schema, &generation);
    g_assert_cmpint(generation, ==, 2);
    g_free(observed);
    _fake_set(O.port_interfaces, port, expected_uuid);

    g_assert_true(pcv_overlay_remove_peer("drift0", peer, &error));
    g_assert_no_error(error);
    gchar *orphan = pcv_overlay_peer_port_name(312, "192.168.0.110");
    _fake_seed_port("drift0", orphan, owner, "drift0", "vxlan", "312",
                    "192.168.0.110", "192.0.2.10");
    g_assert_false(pcv_overlay_add_peer("drift0", "192.168.0.110", &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS);
    g_clear_error(&error);
    g_assert_true(g_hash_table_contains(O.port_bridges, orphan));
    observed = _meta_owner(meta, &schema, &generation);
    g_assert_cmpint(generation, ==, 3);
    g_free(observed);

    g_free(orphan);
    g_free(expected_uuid);
    g_free(owner);
    g_free(meta);
    g_free(port);
    _overlay_effect_teardown();
}

static void test_overlay_generation_and_owner_timeline(void)
{
    _overlay_effect_setup();
    GError *error = NULL;
    gchar *meta = _overlay_meta_path("ovlgen");
    gint64 schema = 0;
    gint64 generation = 0;

    g_assert_true(pcv_overlay_create("ovlgen", 320, NULL, &error));
    g_assert_no_error(error);
    gchar *owner = _meta_owner(meta, &schema, &generation);
    g_assert_cmpint(schema, ==, 2);
    g_assert_cmpint(generation, ==, 1);



    g_assert_true(pcv_overlay_create("ovlgen", 320, NULL, &error));
    g_assert_no_error(error);
    gchar *observed = _meta_owner(meta, &schema, &generation);
    g_assert_cmpint(generation, ==, 1);
    g_assert_cmpstr(observed, ==, owner);
    g_free(observed);

    pcv_overlay_shutdown();
    pcv_overlay_init("192.0.2.10");
    pcv_overlay_restore();
    observed = _meta_owner(meta, &schema, &generation);
    g_assert_cmpint(generation, ==, 1);
    g_assert_cmpstr(observed, ==, owner);
    g_free(observed);

    g_assert_true(pcv_overlay_add_peer("ovlgen", "192.168.0.108", &error));
    g_assert_no_error(error);
    observed = _meta_owner(meta, &schema, &generation);
    g_assert_cmpint(generation, ==, 2);
    g_assert_cmpstr(observed, ==, owner);
    g_free(observed);



    g_assert_true(pcv_overlay_add_peer("ovlgen", "192.168.0.108", &error));
    g_assert_no_error(error);
    observed = _meta_owner(meta, &schema, &generation);
    g_assert_cmpint(generation, ==, 2);
    g_assert_cmpstr(observed, ==, owner);
    g_free(observed);

    g_assert_true(pcv_overlay_remove_peer("ovlgen", "192.168.0.108", &error));
    g_assert_no_error(error);
    observed = _meta_owner(meta, &schema, &generation);
    g_assert_cmpint(generation, ==, 3);
    g_assert_cmpstr(observed, ==, owner);
    g_free(observed);

    g_assert_true(pcv_overlay_remove_peer("ovlgen", "192.168.0.108", &error));
    g_assert_no_error(error);
    observed = _meta_owner(meta, &schema, &generation);
    g_assert_cmpint(generation, ==, 3);
    g_assert_cmpstr(observed, ==, owner);
    g_free(observed);

    g_assert_true(pcv_overlay_add_peer("ovlgen", "192.168.0.108", &error));
    g_assert_no_error(error);
    observed = _meta_owner(meta, &schema, &generation);
    g_assert_cmpint(generation, ==, 4);
    g_assert_cmpstr(observed, ==, owner);
    g_free(observed);

    g_free(owner);
    g_free(meta);
    _overlay_effect_teardown();
}

static void test_overlay_generation_overflow_is_fail_closed(void)
{
    _overlay_effect_setup();
    GError *error = NULL;
    const gchar *add_owner = "11111111-1111-4111-8111-111111111111";
    const gchar *remove_owner = "22222222-2222-4222-8222-222222222222";
    gchar *add_meta = _overlay_meta_path("genaddmax");
    gchar *remove_meta = _overlay_meta_path("genremmax");
    gchar *remove_port =
        pcv_overlay_peer_port_name(322, "192.168.0.109");

    _fake_seed_bridge("genaddmax", "", add_owner, "genaddmax");
    _fake_seed_bridge("genremmax", "", remove_owner, "genremmax");
    _fake_seed_port("genremmax", remove_port, remove_owner, "genremmax",
                    "vxlan", "322", "192.168.0.109", "192.0.2.10");
    g_assert_true(_test_write_metadata(
        add_meta,
        "{\"schema_version\":2,\"name\":\"genaddmax\",\"vni\":321,"
        "\"cidr\":\"\",\"owner_token\":"
        "\"11111111-1111-4111-8111-111111111111\","
        "\"generation\":9223372036854775807,\"peers\":[]}", -1, &error));
    g_assert_no_error(error);
    g_assert_true(_test_write_metadata(
        remove_meta,
        "{\"schema_version\":2,\"name\":\"genremmax\",\"vni\":322,"
        "\"cidr\":\"\",\"owner_token\":"
        "\"22222222-2222-4222-8222-222222222222\","
        "\"generation\":9223372036854775807,"
        "\"peers\":[\"192.168.0.109\"]}", -1, &error));
    g_assert_no_error(error);
    pcv_overlay_restore();

    gchar *add_before = NULL;
    gchar *remove_before = NULL;
    gsize add_before_len = 0;
    gsize remove_before_len = 0;
    g_assert_true(g_file_get_contents(add_meta, &add_before, &add_before_len,
                                     &error));
    g_assert_no_error(error);
    g_assert_true(g_file_get_contents(remove_meta, &remove_before,
                                     &remove_before_len, &error));
    g_assert_no_error(error);

    gchar *add_port = pcv_overlay_peer_port_name(321, "192.168.0.110");
    g_assert_false(pcv_overlay_add_peer("genaddmax", "192.168.0.110", &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE);
    g_clear_error(&error);
    g_assert_false(g_hash_table_contains(O.port_bridges, add_port));
    g_assert_false(pcv_overlay_remove_peer("genremmax", "192.168.0.109",
                                           &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE);
    g_clear_error(&error);
    g_assert_true(g_hash_table_contains(O.port_bridges, remove_port));

    gchar *add_after = NULL;
    gchar *remove_after = NULL;
    gsize add_after_len = 0;
    gsize remove_after_len = 0;
    g_assert_true(g_file_get_contents(add_meta, &add_after, &add_after_len,
                                     &error));
    g_assert_no_error(error);
    g_assert_true(g_file_get_contents(remove_meta, &remove_after,
                                     &remove_after_len, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(add_after_len, ==, add_before_len);
    g_assert_cmpmem(add_after, add_after_len, add_before, add_before_len);
    g_assert_cmpuint(remove_after_len, ==, remove_before_len);
    g_assert_cmpmem(remove_after, remove_after_len,
                    remove_before, remove_before_len);

    g_free(add_before); g_free(add_after); g_free(remove_before);
    g_free(remove_after); g_free(add_port); g_free(remove_port);
    g_free(add_meta); g_free(remove_meta);
    _overlay_effect_teardown();
}

static void test_overlay_failure_transactions_and_foreign_child(void)
{
    _overlay_effect_setup();
    GError *error = NULL;
    _fake_fail_once("ip link set ovlfail up");
    g_assert_false(pcv_overlay_create("ovlfail", 401, NULL, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_clear_error(&error);
    g_assert_false(g_hash_table_contains(O.bridges, "ovlfail"));

    g_assert_true(pcv_overlay_create("ovlops", 402, NULL, &error));
    g_assert_true(pcv_overlay_add_peer("ovlops", "192.168.0.106", &error));
    g_assert_no_error(error);
    gchar *port = pcv_overlay_peer_port_name(402, "192.168.0.106");
    _fake_fail_once("del-port ovlops");
    g_assert_false(pcv_overlay_remove_peer("ovlops", "192.168.0.106", &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_clear_error(&error);
    g_assert_true(g_hash_table_contains(O.port_bridges, port));
    g_assert_cmpuint(_overlay_peer_count("ovlops"), ==, 1);

    gchar *ops_meta = _overlay_meta_path("ovlops");
    gchar *tomb = g_strdup_printf("%s.deleting", ops_meta);
    g_free(ops_meta);
    g_assert_true(g_file_set_contents(tomb, "busy", -1, &error));
    g_assert_no_error(error);
    guint del_before = _fake_command_count("del-br ovlops");
    g_assert_false(pcv_overlay_delete("ovlops", &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_assert_cmpuint(_fake_command_count("del-br ovlops"), ==, del_before);
    g_assert_cmpint(g_remove(tomb), ==, 0);

    const gchar *owner = _fake_lookup(O.bridge_owners, "ovlops");
    _fake_seed_port("ovlops", "tap-foreign", "another-token", "vm",
                    "", "", "", "");
    g_assert_false(pcv_overlay_delete("ovlops", &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_assert_true(g_hash_table_contains(O.bridges, "ovlops"));
    g_assert_true(g_hash_table_contains(O.port_bridges, "tap-foreign"));
    g_assert_true(*owner != '\0');

    g_free(tomb); g_free(port);
    _overlay_effect_teardown();
}

static void
test_overlay_applied_then_timeout_is_reconciled(void)
{
    _overlay_effect_setup();
    GError *error = NULL;

    _fake_apply_then_timeout_once("add-br applycreate");
    g_assert_false(pcv_overlay_create("applycreate", 403, NULL, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
    g_clear_error(&error);
    g_assert_false(g_hash_table_contains(O.bridges, "applycreate"));
    JsonArray *list = pcv_overlay_list(&error);
    g_assert_no_error(error);
    g_assert_cmpuint(json_array_get_length(list), ==, 0);
    json_array_unref(list);

    g_assert_true(pcv_overlay_create("applyops", 404, NULL, &error));
    g_assert_no_error(error);
    gchar *port = pcv_overlay_peer_port_name(404, "192.168.0.104");
    gchar *add = g_strdup_printf("add-port applyops %s", port);
    _fake_apply_then_timeout_once(add);
    g_assert_false(pcv_overlay_add_peer("applyops", "192.168.0.104",
                                        &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
    g_clear_error(&error);
    g_assert_false(g_hash_table_contains(O.port_bridges, port));
    g_assert_cmpuint(_overlay_peer_count("applyops"), ==, 0);

    g_assert_true(pcv_overlay_add_peer("applyops", "192.168.0.104",
                                       &error));
    g_assert_no_error(error);
    gchar *del = g_strdup_printf("del-port applyops %s", port);
    _fake_apply_then_timeout_once(del);
    g_assert_true(pcv_overlay_remove_peer("applyops", "192.168.0.104",
                                          &error));
    g_assert_no_error(error);
    g_assert_false(g_hash_table_contains(O.port_bridges, port));
    g_assert_cmpuint(_overlay_peer_count("applyops"), ==, 0);

    _fake_apply_then_timeout_once("del-br applyops");
    g_assert_true(pcv_overlay_delete("applyops", &error));
    g_assert_no_error(error);
    g_assert_false(g_hash_table_contains(O.bridges, "applyops"));

    g_free(add);
    g_free(del);
    g_free(port);
    _overlay_effect_teardown();
}

static void test_overlay_metadata_add_failure_rolls_back_actual(void)
{
    _overlay_effect_setup();
    GError *error = NULL;
    g_assert_true(pcv_overlay_create("ovlmeta", 501, NULL, &error));
    gchar *meta = _overlay_meta_path("ovlmeta");
    gchar *port = pcv_overlay_peer_port_name(501, "192.168.0.107");
    O.mutate_contains = g_strdup_printf("add-port ovlmeta %s", port);
    O.mutate_meta_path = g_strdup(meta);
    g_assert_false(pcv_overlay_add_peer("ovlmeta", "192.168.0.107", &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_assert_false(g_hash_table_contains(O.port_bridges, port));
    JsonArray *list = pcv_overlay_list(&error);
    g_assert_null(list);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    pcv_overlay_restore();
    list = pcv_overlay_list(&error);
    g_assert_null(list);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    pcv_overlay_shutdown();
    pcv_overlay_init("192.0.2.10");
    pcv_overlay_restore();
    list = pcv_overlay_list(&error);
    g_assert_null(list);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    _overlay_test_remove_tree(meta);
    g_free(meta); g_free(port);
    _overlay_effect_teardown();
}

static void test_overlay_rollback_failure_is_persistently_degraded(void)
{


    _overlay_effect_setup();
    GError *error = NULL;
    gchar *meta = _overlay_meta_path("rbbridge");
    O.mutate_contains = g_strdup("ip link set rbbridge up");
    O.mutate_meta_path = g_strdup(meta);
    FakeArmFailure bridge_arm = {
        .contains = "ip link set rbbridge up",
        .failure = "del-br rbbridge",
    };
    O.command_hook = _fake_arm_failure_hook;
    O.command_hook_data = &bridge_arm;
    g_assert_false(pcv_overlay_create("rbbridge", 505, NULL, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_assert_true(bridge_arm.fired);
    g_assert_true(g_hash_table_contains(O.bridges, "rbbridge"));
    JsonArray *list = pcv_overlay_list(&error);
    g_assert_null(list);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    pcv_overlay_restore();
    list = pcv_overlay_list(&error);
    g_assert_null(list);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    pcv_overlay_shutdown();
    pcv_overlay_init("192.0.2.10");
    pcv_overlay_restore();
    list = pcv_overlay_list(&error);
    g_assert_null(list);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    g_free(meta);
    _overlay_effect_teardown();



    _overlay_effect_setup();
    g_assert_true(pcv_overlay_create("rbpeer", 506, NULL, &error));
    g_assert_no_error(error);
    meta = _overlay_meta_path("rbpeer");
    gchar *port = pcv_overlay_peer_port_name(506, "192.168.0.107");
    gchar *add_needle = g_strdup_printf("add-port rbpeer %s", port);
    gchar *del_needle = g_strdup_printf("del-port rbpeer %s", port);
    O.mutate_contains = g_strdup(add_needle);
    O.mutate_meta_path = g_strdup(meta);
    FakeArmFailure peer_arm = {
        .contains = add_needle,
        .failure = del_needle,
    };
    O.command_hook = _fake_arm_failure_hook;
    O.command_hook_data = &peer_arm;
    g_assert_false(pcv_overlay_add_peer("rbpeer", "192.168.0.107", &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_assert_true(peer_arm.fired);
    g_assert_true(g_hash_table_contains(O.port_bridges, port));
    list = pcv_overlay_list(&error);
    g_assert_null(list);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    pcv_overlay_restore();
    list = pcv_overlay_list(&error);
    g_assert_null(list);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    pcv_overlay_shutdown();
    pcv_overlay_init("192.0.2.10");
    pcv_overlay_restore();
    list = pcv_overlay_list(&error);
    g_assert_null(list);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    g_free(del_needle); g_free(add_needle); g_free(port); g_free(meta);
    _overlay_effect_teardown();
}

static void test_overlay_marker_orphan_survives_periodic_and_restart_audit(void)
{
    _overlay_effect_setup();
    GError *error = NULL;




    _fake_seed_bridge("orphan0", "", "88888888-8888-4888-8888-888888888888",
                      "orphan0");
    pcv_overlay_restore();
    JsonArray *list = pcv_overlay_list(&error);
    g_assert_null(list);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    g_assert_false(pcv_overlay_create("blocked-orphan", 507, NULL, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    g_assert_cmpuint(_fake_command_count("add-br blocked-orphan"), ==, 0);

    pcv_overlay_shutdown();
    pcv_overlay_init("192.0.2.10");
    pcv_overlay_restore();
    list = pcv_overlay_list(&error);
    g_assert_null(list);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    _overlay_effect_teardown();
}

static void test_overlay_registry_requires_canonical_metadata_bijection(void)
{
    _overlay_effect_setup();
    GError *error = NULL;
    g_assert_true(pcv_overlay_create("missingmeta", 508, NULL, &error));
    g_assert_no_error(error);
    gchar *meta = _overlay_meta_path("missingmeta");
    g_assert_cmpint(g_remove(meta), ==, 0);



    pcv_overlay_restore();
    JsonArray *list = pcv_overlay_list(&error);
    g_assert_null(list);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    g_assert_false(pcv_overlay_create("blocked-missing", 509, NULL, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    g_assert_cmpuint(_fake_command_count("add-br blocked-missing"), ==, 0);

    pcv_overlay_shutdown();
    pcv_overlay_init("192.0.2.10");
    pcv_overlay_restore();
    list = pcv_overlay_list(&error);
    g_assert_null(list);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    g_free(meta);
    _overlay_effect_teardown();
}

static void test_overlay_legacy_exact_migration(void)
{
    _overlay_effect_setup();
    GError *error = NULL;
    _fake_seed_bridge("legacy0", "10.61.0.1/24", NULL, NULL);
    _fake_seed_port("legacy0", "vxlan-0-104", NULL, NULL, "vxlan", "610",
                    "192.168.0.104", "192.0.2.10");
    gchar *meta = _overlay_meta_path("legacy0");
    g_assert_true(_test_write_metadata(
        meta,
        "{\"name\":\"legacy0\",\"vni\":610,\"cidr\":\"10.61.0.1/24\","
        "\"peers\":[\"192.168.0.104\"]}", -1, &error));
    g_assert_no_error(error);
    pcv_overlay_restore();

    JsonArray *list = pcv_overlay_list(&error);
    g_assert_no_error(error);
    g_assert_cmpuint(json_array_get_length(list), ==, 1);
    json_array_unref(list);
    gint64 schema = 0;
    gint64 generation = 0;
    gchar *owner = _meta_owner(meta, &schema, &generation);
    g_assert_cmpint(schema, ==, 2);
    g_assert_cmpint(generation, ==, 1);
    g_assert_true(g_uuid_string_is_valid(owner));
    g_assert_cmpstr(_fake_lookup(O.bridge_owners, "legacy0"), ==, owner);
    gchar *canonical = pcv_overlay_peer_port_name(610, "192.168.0.104");
    g_assert_true(g_hash_table_contains(O.port_bridges, canonical));
    g_assert_cmpstr(_fake_lookup(O.port_owners, canonical), ==, owner);
    g_assert_false(g_hash_table_contains(O.port_bridges, "vxlan-0-104"));
    g_free(canonical); g_free(owner); g_free(meta);
    _overlay_effect_teardown();
}

static void test_overlay_legacy_applied_timeout_outcomes(void)
{
    GError *error = NULL;



    _overlay_effect_setup();
    _fake_seed_bridge("legapply", "", NULL, NULL);
    _fake_seed_port("legapply", "vxlan-0-104", NULL, NULL, "vxlan", "611",
                    "192.168.0.104", "192.0.2.10");
    gchar *meta = _overlay_meta_path("legapply");
    const gchar *legacy_json =
        "{\"name\":\"legapply\",\"vni\":611,\"cidr\":\"\","
        "\"peers\":[\"192.168.0.104\"]}";
    g_assert_true(_test_write_metadata(meta, legacy_json, -1, &error));
    g_assert_no_error(error);
    _fake_apply_then_timeout_once("set Bridge legapply");
    pcv_overlay_restore();
    JsonArray *list = pcv_overlay_list(&error);
    g_assert_no_error(error);
    g_assert_nonnull(list);
    g_assert_cmpuint(json_array_get_length(list), ==, 1);
    json_array_unref(list);
    gchar *canonical = pcv_overlay_peer_port_name(611, "192.168.0.104");
    g_assert_true(g_hash_table_contains(O.port_bridges, canonical));
    g_assert_false(g_hash_table_contains(O.port_bridges, "vxlan-0-104"));
    gint64 schema = 0;
    gint64 generation = 0;
    gchar *owner = _meta_owner(meta, &schema, &generation);
    g_assert_cmpint(schema, ==, 2);
    g_assert_cmpint(generation, ==, 1);
    g_assert_true(g_uuid_string_is_valid(owner));
    g_free(owner); g_free(canonical); g_free(meta);
    _overlay_effect_teardown();




    _overlay_effect_setup();
    _fake_seed_bridge("legrollto", "", NULL, NULL);
    _fake_seed_port("legrollto", "vxlan-0-104", NULL, NULL, "vxlan", "612",
                    "192.168.0.104", "192.0.2.10");
    meta = _overlay_meta_path("legrollto");
    legacy_json =
        "{\"name\":\"legrollto\",\"vni\":612,\"cidr\":\"\","
        "\"peers\":[\"192.168.0.104\"]}";
    g_assert_true(_test_write_metadata(meta, legacy_json, -1, &error));
    g_assert_no_error(error);
    canonical = pcv_overlay_peer_port_name(612, "192.168.0.104");
    LegacyAppliedTimeoutFault fault = {
        .migration_fragment = "set Bridge legrollto",
        .rollback_fragment =
            "remove Bridge legrollto external_ids pcv_overlay_owner",
        .canonical_port = canonical,
    };
    O.command_hook = _legacy_rollback_applied_timeout_hook;
    O.command_hook_data = &fault;
    pcv_overlay_restore();
    g_assert_true(fault.fired);
    list = pcv_overlay_list(&error);
    g_assert_null(list);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    g_assert_true(g_hash_table_contains(O.port_bridges, "vxlan-0-104"));
    g_assert_false(g_hash_table_contains(O.port_bridges, canonical));
    g_assert_cmpstr(_fake_lookup(O.bridge_owners, "legrollto"), ==, "");
    gchar *contents = NULL;
    gsize length = 0;
    g_assert_true(g_file_get_contents(meta, &contents, &length, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(length, ==, strlen(legacy_json));
    g_assert_cmpmem(contents, length, legacy_json, strlen(legacy_json));
    g_free(contents); g_free(canonical); g_free(meta);
    _overlay_effect_teardown();
}

typedef enum {
    LEGACY_RACE_DATAPATH,
    LEGACY_RACE_BRIDGE_OWNER,
    LEGACY_RACE_INTERFACE_OWNER,
} LegacyNamespaceRaceKind;

typedef struct {
    const gchar *name;
    LegacyNamespaceRaceKind kind;
    gboolean fired;
} LegacyNamespaceRace;

static void
_legacy_namespace_race_hook(const gchar *command, gboolean before,
                            gpointer user_data)
{
    LegacyNamespaceRace *race = user_data;
    if (!before || race->fired)
        return;
    gchar *needle = g_strdup_printf("set Bridge %s", race->name);
    gboolean matches = strstr(command, needle) != NULL;
    g_free(needle);
    if (!matches)
        return;
    race->fired = TRUE;
    if (race->kind == LEGACY_RACE_DATAPATH)
        _fake_set(O.bridge_datapaths, race->name, "netdev");
    else if (race->kind == LEGACY_RACE_BRIDGE_OWNER)
        _fake_set(O.bridge_legacy_owners, race->name, "dpdk-bridge");
    else
        _fake_set(O.port_legacy_owners, "vxlan-0-104", "dpdk-port");
}

static void test_overlay_legacy_migration_namespace_races_fail_closed(void)
{
    for (guint i = 0; i < 3; i++) {
        _overlay_effect_setup();
        GError *error = NULL;
        gchar *name = g_strdup_printf("legacyrace%u", i);
        gint vni = 614 + (gint)i;
        gchar *key = g_strdup_printf("%d", vni);
        _fake_seed_bridge(name, "", NULL, NULL);
        _fake_seed_port(name, "vxlan-0-104", NULL, NULL, "vxlan", key,
                        "192.168.0.104", "192.0.2.10");
        gchar *meta = _overlay_meta_path(name);
        gchar *json = g_strdup_printf(
            "{\"name\":\"%s\",\"vni\":%d,\"cidr\":\"\","
            "\"peers\":[\"192.168.0.104\"]}", name, vni);
        g_assert_true(_test_write_metadata(meta, json, -1, &error));
        g_assert_no_error(error);
        LegacyNamespaceRace race = {
            .name = name,
            .kind = (LegacyNamespaceRaceKind)i,
        };
        O.command_hook = _legacy_namespace_race_hook;
        O.command_hook_data = &race;
        pcv_overlay_restore();
        g_assert_true(race.fired);
        JsonArray *list = pcv_overlay_list(&error);
        g_assert_null(list);
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
        g_clear_error(&error);
        gchar *canonical =
            pcv_overlay_peer_port_name(vni, "192.168.0.104");
        g_assert_true(g_hash_table_contains(O.port_bridges, "vxlan-0-104"));
        g_assert_false(g_hash_table_contains(O.port_bridges, canonical));
        g_assert_cmpstr(_fake_lookup(O.bridge_owners, name), ==, "");
        gchar *contents = NULL;
        g_assert_true(g_file_get_contents(meta, &contents, NULL, &error));
        g_assert_no_error(error);
        g_assert_null(strstr(contents, "schema_version"));
        g_free(contents); g_free(canonical); g_free(json);
        g_free(meta); g_free(key); g_free(name);
        _overlay_effect_teardown();
    }
}

static void test_overlay_legacy_mismatch_and_residue_fail_closed(void)
{
    _overlay_effect_setup();
    GError *error = NULL;
    _fake_seed_bridge("legacybad", "10.62.0.1/24", NULL, NULL);
    _fake_seed_port("legacybad", "vxlan-0-104", NULL, NULL, "vxlan", "620",
                    "192.168.0.104", "198.51.100.1");
    gchar *meta = _overlay_meta_path("legacybad");
    g_assert_true(_test_write_metadata(
        meta,
        "{\"name\":\"legacybad\",\"vni\":620,\"cidr\":\"10.62.0.1/24\","
        "\"peers\":[\"192.168.0.104\"]}", -1, &error));
    g_assert_no_error(error);
    pcv_overlay_restore();
    JsonArray *list = pcv_overlay_list(&error);
    g_assert_null(list);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    JsonObject *info = pcv_overlay_info("partschema", &error);
    g_assert_null(info);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);


    _fake_set(O.port_local, "vxlan-0-104", "192.0.2.10");
    _fake_seed_port("legacybad", "tap-extra", NULL, NULL, "", "", "", "");
    pcv_overlay_restore();
    list = pcv_overlay_list(&error);
    g_assert_null(list);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    g_assert_false(pcv_overlay_delete("legacybad", &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_assert_false(pcv_overlay_remove_peer("legacybad", "192.168.0.104", &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_assert_true(g_hash_table_contains(O.bridges, "legacybad"));
    g_assert_true(g_hash_table_contains(O.port_bridges, "vxlan-0-104"));
    g_assert_cmpstr(_fake_lookup(O.bridge_owners, "legacybad"), ==, "");



    _fake_remove_port("tap-extra");
    pcv_overlay_restore();
    list = pcv_overlay_list(&error);
    g_assert_no_error(error);
    g_assert_nonnull(list);
    g_assert_cmpuint(json_array_get_length(list), ==, 1);
    json_array_unref(list);
    g_free(meta);
    _overlay_effect_teardown();
}

static void test_overlay_legacy_foreign_markers_are_never_adopted(void)
{
    const struct {
        const gchar *name;
        const gchar *bridge_owner;
        const gchar *bridge_name;
        const gchar *port_owner;
        const gchar *port_name;
    } cases[] = {
        {"markbown", "foreign-owner", NULL, NULL, NULL},
        {"markbname", NULL, "foreign-name", NULL, NULL},
        {"markpown", NULL, NULL, "foreign-owner", NULL},
        {"markpname", NULL, NULL, NULL, "foreign-name"},
    };

    for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
        _overlay_effect_setup();
        GError *error = NULL;
        gint vni = 640 + (gint)i;
        _fake_seed_bridge(cases[i].name, "", cases[i].bridge_owner,
                          cases[i].bridge_name);
        gchar *vni_key = g_strdup_printf("%d", vni);
        _fake_seed_port(cases[i].name, "vxlan-0-104", cases[i].port_owner,
                        cases[i].port_name, "vxlan", vni_key,
                        "192.168.0.104", "192.0.2.10");
        g_free(vni_key);
        gchar *meta = _overlay_meta_path(cases[i].name);
        gchar *json = g_strdup_printf(
            "{\"name\":\"%s\",\"vni\":%d,\"cidr\":\"\","
            "\"peers\":[\"192.168.0.104\"]}", cases[i].name, vni);
        g_assert_true(_test_write_metadata(meta, json, -1, &error));
        g_assert_no_error(error);
        gchar *before = NULL;
        gsize before_len = 0;
        g_assert_true(g_file_get_contents(meta, &before, &before_len, &error));
        g_assert_no_error(error);

        pcv_overlay_restore();
        JsonArray *list = pcv_overlay_list(&error);
        g_assert_null(list);
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
        g_clear_error(&error);
        gchar *canonical = pcv_overlay_peer_port_name(vni, "192.168.0.104");
        g_assert_false(g_hash_table_contains(O.port_bridges, canonical));
        g_assert_true(g_hash_table_contains(O.port_bridges, "vxlan-0-104"));
        g_assert_cmpstr(_fake_lookup(O.bridge_owners, cases[i].name), ==,
                        cases[i].bridge_owner ? cases[i].bridge_owner : "");
        g_assert_cmpstr(_fake_lookup(O.bridge_names, cases[i].name), ==,
                        cases[i].bridge_name ? cases[i].bridge_name : "");
        g_assert_cmpstr(_fake_lookup(O.port_owners, "vxlan-0-104"), ==,
                        cases[i].port_owner ? cases[i].port_owner : "");
        g_assert_cmpstr(_fake_lookup(O.port_names, "vxlan-0-104"), ==,
                        cases[i].port_name ? cases[i].port_name : "");
        gchar *after = NULL;
        gsize after_len = 0;
        g_assert_true(g_file_get_contents(meta, &after, &after_len, &error));
        g_assert_no_error(error);
        g_assert_cmpuint(after_len, ==, before_len);
        g_assert_cmpmem(after, after_len, before, before_len);

        g_free(after); g_free(before); g_free(canonical);
        g_free(json); g_free(meta);
        _overlay_effect_teardown();
    }
}

static void test_overlay_restore_rejects_partial_and_unknown_schema(void)
{
    _overlay_effect_setup();
    GError *error = NULL;
    const struct {
        const gchar *name;
        const gchar *json;
    } cases[] = {
        {"partschema",
         "{\"schema_version\":2,\"name\":\"partschema\",\"vni\":630,"
         "\"cidr\":\"\",\"peers\":[]}"},
        {"partowner",
         "{\"name\":\"partowner\",\"vni\":631,\"cidr\":\"\","
         "\"owner_token\":\"11111111-1111-4111-8111-111111111111\","
         "\"peers\":[]}"},
        {"partgen",
         "{\"name\":\"partgen\",\"vni\":632,\"cidr\":\"\","
         "\"generation\":1,\"peers\":[]}"},
        {"schema3",
         "{\"schema_version\":3,\"name\":\"schema3\",\"vni\":633,"
         "\"cidr\":\"\",\"owner_token\":"
         "\"22222222-2222-4222-8222-222222222222\","
         "\"generation\":1,\"peers\":[]}"},
        {"extrakey",
         "{\"name\":\"extrakey\",\"vni\":634,\"cidr\":\"\","
         "\"peers\":[],\"unexpected\":true}"},
    };
    gchar *paths[G_N_ELEMENTS(cases)] = {0};
    for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
        paths[i] = _overlay_meta_path(cases[i].name);
        g_assert_true(_test_write_metadata(paths[i], cases[i].json, -1, &error));
        g_assert_no_error(error);
    }
    pcv_overlay_restore();
    JsonArray *list = pcv_overlay_list(&error);
    g_assert_null(list);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
        gchar *needle = g_strdup_printf("add-br %s", cases[i].name);
        g_assert_cmpuint(_fake_command_count(needle), ==, 0);
        g_free(needle);
        g_assert_cmpint(g_remove(paths[i]), ==, 0);
        g_free(paths[i]);
    }
    pcv_overlay_restore();
    list = pcv_overlay_list(&error);
    g_assert_no_error(error);
    g_assert_nonnull(list);
    g_assert_cmpuint(json_array_get_length(list), ==, 0);
    json_array_unref(list);
    _overlay_effect_teardown();
}

static void test_overlay_restore_exposes_delete_tombstone(void)
{
    _overlay_effect_setup();
    GError *error = NULL;
    gchar *meta = _overlay_meta_path("tomb0");
    gchar *tombstone = g_strdup_printf("%s.deleting", meta);
    const gchar *snapshot =
        "{\"schema_version\":2,\"name\":\"tomb0\",\"vni\":650,"
        "\"cidr\":\"\",\"owner_token\":"
        "\"55555555-5555-4555-8555-555555555555\","
        "\"generation\":1,\"peers\":[]}";
    g_assert_true(_test_write_metadata(tombstone, snapshot, -1, &error));
    g_assert_no_error(error);

    pcv_overlay_restore();
    JsonArray *list = pcv_overlay_list(&error);
    g_assert_null(list);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    JsonObject *info = pcv_overlay_info("tomb0", &error);
    g_assert_null(info);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    g_assert_cmpuint(_fake_command_count("add-br tomb0"), ==, 0);

    g_assert_cmpint(g_remove(tombstone), ==, 0);
    pcv_overlay_restore();
    list = pcv_overlay_list(&error);
    g_assert_no_error(error);
    g_assert_nonnull(list);
    g_assert_cmpuint(json_array_get_length(list), ==, 0);
    json_array_unref(list);

    g_free(tombstone);
    g_free(meta);
    _overlay_effect_teardown();
}

static void
_stale_snapshot_hook(const gchar *path, gpointer user_data)
{
    const gchar *replacement = user_data;
    GError *error = NULL;
    g_assert_true(_test_write_metadata(path, replacement, -1, &error));
    g_assert_no_error(error);
}

static void
_restore_deadline_sleep_hook(const gchar *path, gpointer user_data)
{
    (void)path;
    g_usleep(GPOINTER_TO_UINT(user_data));
}

typedef struct {
    const gchar *command_fragment;
    guint sleep_usec;
    gboolean fired;
} RestoreCommandSleep;

static void
_restore_command_sleep_hook(const gchar *command, gboolean before,
                            gpointer user_data)
{
    RestoreCommandSleep *sleep = user_data;
    if (!before && !sleep->fired &&
        strstr(command, sleep->command_fragment)) {
        sleep->fired = TRUE;
        g_usleep(sleep->sleep_usec);
    }
}

static void
test_overlay_restore_total_deadline_is_bounded(void)
{
    _overlay_effect_setup();
    GError *error = NULL;
    g_assert_true(pcv_overlay_create("deadline0", 773, NULL, &error));
    g_assert_no_error(error);
    pcv_overlay_shutdown();

    pcv_overlay_set_restore_deadline_for_test(1);
    pcv_overlay_set_restore_snapshot_test_hook(
        _restore_deadline_sleep_hook, GUINT_TO_POINTER(10000));
    pcv_overlay_init("192.0.2.10");
    guint commands_before = O.commands->len;
    gint64 started = g_get_monotonic_time();
    pcv_overlay_restore();
    gint64 elapsed = g_get_monotonic_time() - started;
    g_assert_cmpint(elapsed, <, 500 * 1000);
    g_assert_cmpuint(O.commands->len, ==, commands_before);

    JsonArray *list = pcv_overlay_list(&error);
    g_assert_null(list);
    g_assert_nonnull(error);
    g_assert_true(strstr(error->message, "deadline") != NULL ||
                  strstr(error->message, "rejected") != NULL);
    g_clear_error(&error);
    _overlay_effect_teardown();
}

static void
test_overlay_restore_deadline_keeps_cleanup_budget(void)
{
    _overlay_effect_setup();
    GError *error = NULL;
    const gchar *snapshot =
        "{\"schema_version\":2,\"name\":\"deadline-clean\",\"vni\":774,"
        "\"cidr\":\"\",\"owner_token\":"
        "\"88888888-8888-4888-8888-888888888888\","
        "\"generation\":1,\"peers\":[\"192.168.0.104\",\"192.168.0.108\"]}";
    gchar *meta = _overlay_meta_path("deadline-clean");
    g_assert_true(_test_write_metadata(meta, snapshot, -1, &error));
    g_assert_no_error(error);

    gchar *first_port = pcv_overlay_peer_port_name(774, "192.168.0.104");
    gchar *fragment = g_strdup_printf("add-port deadline-clean %s", first_port);
    RestoreCommandSleep sleep = {
        .command_fragment = fragment,


        .sleep_usec = 1550 * 1000,
    };
    O.command_hook = _restore_command_sleep_hook;
    O.command_hook_data = &sleep;
    pcv_overlay_shutdown();
    pcv_overlay_set_restore_deadline_for_test(3000);
    pcv_overlay_init("192.0.2.10");

    gint64 started = g_get_monotonic_time();
    pcv_overlay_restore();
    g_assert_cmpint(g_get_monotonic_time() - started, <, 3000 * 1000);
    g_assert_true(sleep.fired);
    g_assert_false(g_hash_table_contains(O.bridges, "deadline-clean"));
    g_assert_false(g_hash_table_contains(O.port_bridges, first_port));
    JsonArray *list = pcv_overlay_list(&error);
    g_assert_null(list);
    g_assert_nonnull(error);
    g_assert_true(strstr(error->message, "deadline") != NULL ||
                  strstr(error->message, "rejected") != NULL);
    g_clear_error(&error);

    O.command_hook = NULL;
    O.command_hook_data = NULL;
    g_free(fragment);
    g_free(first_port);
    g_free(meta);
    _overlay_effect_teardown();
}

static void test_overlay_restore_rejects_stale_snapshot(void)
{
    GError *error = NULL;
    _overlay_fake_reset();
    O_meta_dir = g_dir_make_tmp("pcv-overlay-stale-XXXXXX", &error);
    g_assert_no_error(error);
    gchar *meta = _overlay_meta_path("stale0");
    const gchar *initial =
        "{\"schema_version\":2,\"name\":\"stale0\",\"vni\":701,\"cidr\":\"\","
        "\"owner_token\":\"11111111-1111-4111-8111-111111111111\","
        "\"generation\":1,\"peers\":[]}";
    const gchar *changed =
        "{\"schema_version\":2,\"name\":\"stale0\",\"vni\":701,\"cidr\":\"\","
        "\"owner_token\":\"11111111-1111-4111-8111-111111111111\","
        "\"generation\":2,\"peers\":[]}";
    g_assert_true(_test_write_metadata(meta, initial, -1, &error));
    g_assert_no_error(error);
    pcv_overlay_set_test_context(_overlay_fake_exec, O_meta_dir);
    pcv_overlay_set_restore_snapshot_test_hook(_stale_snapshot_hook,
                                               (gpointer)changed);
    pcv_overlay_init("192.0.2.10");
    pcv_overlay_restore();
    JsonArray *list = pcv_overlay_list(&error);
    g_assert_null(list);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    g_assert_cmpuint(_fake_command_count("add-br stale0"), ==, 0);
    pcv_overlay_restore();
    list = pcv_overlay_list(&error);
    g_assert_no_error(error);
    g_assert_nonnull(list);
    g_assert_cmpuint(json_array_get_length(list), ==, 1);
    json_array_unref(list);
    g_free(meta);
    _overlay_effect_teardown();
}

static void test_overlay_legacy_rejects_extra_ip_and_option(void)
{
    const struct {
        const gchar *name;
        gboolean extra_ip;
        gboolean extra_option;
        gboolean swapped_interface;
    } cases[] = {
        {"legacyip", TRUE, FALSE, FALSE},
        {"legacyopt", FALSE, TRUE, FALSE},
        {"legacyport", FALSE, FALSE, TRUE},
    };
    for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
        _overlay_effect_setup();
        GError *error = NULL;
        gint vni = 720 + (gint)i;
        const gchar *cidr = cases[i].extra_ip
            ? "10.72.0.1/24 scope global legacyip\n2: legacyip inet 10.72.0.2/24"
            : "10.72.0.1/24";
        _fake_seed_bridge(cases[i].name, cidr, NULL, NULL);
        gchar *key = g_strdup_printf("%d", vni);
        _fake_seed_port(cases[i].name, "vxlan-0-104", NULL, NULL, "vxlan",
                        key, "192.168.0.104", "192.0.2.10");
        if (cases[i].extra_option)
            _fake_set(O.port_extras, "vxlan-0-104", "dst_port=4789");
        if (cases[i].swapped_interface)
            _fake_set(O.port_interfaces, "vxlan-0-104",
                      "20000000-0000-4000-8000-000000000002");
        gchar *meta = _overlay_meta_path(cases[i].name);
        gchar *json = g_strdup_printf(
            "{\"name\":\"%s\",\"vni\":%d,\"cidr\":\"10.72.0.1/24\","
            "\"peers\":[\"192.168.0.104\"]}", cases[i].name, vni);
        g_assert_true(_test_write_metadata(meta, json, -1, &error));
        g_assert_no_error(error);

        pcv_overlay_restore();
        JsonArray *list = pcv_overlay_list(&error);
        g_assert_null(list);
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
        g_clear_error(&error);
        g_assert_true(g_hash_table_contains(O.port_bridges, "vxlan-0-104"));
        gchar *canonical = pcv_overlay_peer_port_name(vni, "192.168.0.104");
        g_assert_false(g_hash_table_contains(O.port_bridges, canonical));
        g_assert_cmpstr(_fake_lookup(O.bridge_owners, cases[i].name), ==, "");
        g_free(canonical); g_free(json); g_free(meta); g_free(key);
        _overlay_effect_teardown();
    }
}

static void test_overlay_legacy_rejects_dpdk_and_other_owner_markers(void)
{
    for (guint i = 0; i < 2; i++) {
        _overlay_effect_setup();
        GError *error = NULL;
        const gchar *name = i == 0 ? "legacydpdk" : "legacyown";
        _fake_seed_bridge(name, "", NULL, NULL);
        _fake_seed_port(name, "vxlan-0-104", NULL, NULL, "vxlan", "725",
                        "192.168.0.104", "192.0.2.10");
        if (i == 0) {
            _fake_set(O.bridge_datapaths, name, "netdev");
            _fake_set(O.bridge_legacy_owners, name, "bridge");
        } else {
            _fake_set(O.port_legacy_owners, "vxlan-0-104", "dpdk-port");
        }
        gchar *meta = _overlay_meta_path(name);
        gchar *json = g_strdup_printf(
            "{\"name\":\"%s\",\"vni\":725,\"cidr\":\"\","
            "\"peers\":[\"192.168.0.104\"]}", name);
        g_assert_true(_test_write_metadata(meta, json, -1, &error));
        g_assert_no_error(error);
        pcv_overlay_restore();
        JsonArray *list = pcv_overlay_list(&error);
        g_assert_null(list);
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
        g_clear_error(&error);
        g_assert_cmpstr(_fake_lookup(O.bridge_owners, name), ==, "");
        g_assert_true(g_hash_table_contains(O.port_bridges, "vxlan-0-104"));
        g_free(json); g_free(meta);
        _overlay_effect_teardown();
    }
}

static void test_overlay_legacy_rollback_failure_is_degraded(void)
{
    for (guint i = 0; i < 2; i++) {
        _overlay_effect_setup();
        GError *error = NULL;
        const gchar *name = i == 0 ? "legacyrbpost" : "legacyrbmeta";
        gint vni = 726 + (gint)i;
        _fake_seed_bridge(name, "", NULL, NULL);
        gchar *key = g_strdup_printf("%d", vni);
        _fake_seed_port(name, "vxlan-0-104", NULL, NULL, "vxlan", key,
                        "192.168.0.104", "192.0.2.10");
        gchar *meta = _overlay_meta_path(name);
        gchar *json = g_strdup_printf(
            "{\"name\":\"%s\",\"vni\":%d,\"cidr\":\"\","
            "\"peers\":[\"192.168.0.104\"]}", name, vni);
        g_assert_true(_test_write_metadata(meta, json, -1, &error));
        g_assert_no_error(error);
        gchar *canonical =
            pcv_overlay_peer_port_name(vni, "192.168.0.104");
        gchar *migration = g_strdup_printf("add-port %s %s", name, canonical);
        gchar *rollback = g_strdup_printf(
            "remove Bridge %s external_ids pcv_overlay_owner", name);
        gchar *sidecar = i == 1 ? g_strdup_printf("%s.updating", meta) : NULL;
        LegacyRollbackFault fault = {
            .contains = migration,
            .rollback_failure = rollback,
            .canonical_port = canonical,
            .sidecar_path = sidecar,
            .inject_postcheck_drift = i == 0,
        };
        O.command_hook = _legacy_rollback_fault_hook;
        O.command_hook_data = &fault;

        pcv_overlay_restore();
        g_assert_true(fault.fired);
        JsonArray *list = pcv_overlay_list(&error);
        g_assert_null(list);
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
        g_clear_error(&error);
        g_assert_true(g_hash_table_contains(O.port_bridges, canonical));
        g_assert_true(*_fake_lookup(O.bridge_owners, name) != '\0');
        g_assert_false(pcv_overlay_create("blockedrb", 729 + (gint)i,
                                          NULL, &error));
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
        g_clear_error(&error);
        g_assert_cmpuint(_fake_command_count("add-br blockedrb"), ==, 0);

        g_free(sidecar); g_free(rollback); g_free(migration);
        g_free(canonical); g_free(json); g_free(meta); g_free(key);
        _overlay_effect_teardown();
    }
}

static void test_overlay_v2_closed_owner_inventory(void)
{
    _overlay_effect_setup();
    GError *error = NULL;
    g_assert_true(pcv_overlay_create("v2closed", 730, NULL, &error));
    g_assert_true(pcv_overlay_add_peer("v2closed", "192.168.0.104", &error));
    g_assert_no_error(error);
    gchar *owner = g_strdup(_fake_lookup(O.bridge_owners, "v2closed"));
    pcv_overlay_shutdown();

    _fake_seed_bridge("foreignv2", "", NULL, NULL);
    _fake_seed_port("foreignv2", "owner-orphan", owner, NULL, "vxlan", "730",
                    "198.51.100.9", "192.0.2.10");
    pcv_overlay_init("192.0.2.10");
    pcv_overlay_restore();
    JsonArray *list = pcv_overlay_list(&error);
    g_assert_null(list);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    g_assert_true(g_hash_table_contains(O.port_bridges, "owner-orphan"));

    _fake_remove_port("owner-orphan");
    pcv_overlay_restore();
    list = pcv_overlay_list(&error);
    g_assert_no_error(error);
    g_assert_nonnull(list);
    json_array_unref(list);
    pcv_overlay_shutdown();

    _fake_seed_bridge("shadowv2", "", owner, NULL);
    pcv_overlay_init("192.0.2.10");
    pcv_overlay_restore();
    list = pcv_overlay_list(&error);
    g_assert_null(list);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    g_assert_true(g_hash_table_contains(O.bridges, "shadowv2"));
    _fake_remove_bridge("shadowv2");
    pcv_overlay_restore();
    list = pcv_overlay_list(&error);
    g_assert_no_error(error);
    g_assert_nonnull(list);
    json_array_unref(list);
    g_free(owner);
    _overlay_effect_teardown();
}

static void test_overlay_v2_restore_allows_unmarked_workload_child(void)
{
    _overlay_effect_setup();
    GError *error = NULL;
    g_assert_true(pcv_overlay_create("v2tap", 731, NULL, &error));
    g_assert_true(pcv_overlay_add_peer("v2tap", "192.168.0.104", &error));
    g_assert_no_error(error);
    _fake_seed_port("v2tap", "vnet42", NULL, NULL, "", NULL,
                    NULL, NULL);

    pcv_overlay_shutdown();
    pcv_overlay_init("192.0.2.10");
    pcv_overlay_restore();
    JsonArray *list = pcv_overlay_list(&error);
    g_assert_no_error(error);
    g_assert_cmpuint(json_array_get_length(list), ==, 1);
    JsonObject *row = json_array_get_object_element(list, 0);
    g_assert_cmpint(json_object_get_int_member(row, "peer_count"), ==, 1);
    json_array_unref(list);
    JsonObject *info = pcv_overlay_info("v2tap", &error);
    g_assert_nonnull(info);
    g_assert_no_error(error);
    g_assert_true(json_object_get_boolean_member(info, "active"));
    json_object_unref(info);

    g_assert_true(pcv_overlay_remove_peer("v2tap", "192.168.0.104", &error));
    g_assert_no_error(error);
    g_assert_false(pcv_overlay_delete("v2tap", &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_BUSY);
    g_clear_error(&error);
    g_assert_true(g_hash_table_contains(O.bridges, "v2tap"));
    g_assert_true(g_hash_table_contains(O.port_bridges, "vnet42"));
    _fake_remove_port("vnet42");
    g_assert_true(pcv_overlay_delete("v2tap", &error));
    g_assert_no_error(error);
    _overlay_effect_teardown();
}

static void test_overlay_metadata_abnormal_dentries_are_bounded(void)
{
    _overlay_effect_setup();
    GError *error = NULL;
    gchar *target = g_build_filename(O_meta_dir, "external.json", NULL);
    gchar *symlink_meta = _overlay_meta_path("sym0");
    gchar *fifo_meta = _overlay_meta_path("fifo0");
    gchar *group_meta = _overlay_meta_path("group0");
    gchar *quarantine = g_build_filename(
        O_meta_dir, "overlay-q0.meta.updating.quarantine.deadbeef", NULL);
    g_assert_true(g_file_set_contents(target, "{}", -1, &error));
    g_assert_no_error(error);
    g_assert_cmpint(symlink(target, symlink_meta), ==, 0);
    g_assert_cmpint(mkfifo(fifo_meta, 0600), ==, 0);
    g_assert_true(g_file_set_contents(
        group_meta,
        "{\"schema_version\":2,\"name\":\"group0\",\"vni\":734,"
        "\"cidr\":\"\",\"owner_token\":"
        "\"88888888-8888-4888-8888-888888888888\","
        "\"generation\":1,\"peers\":[]}", -1, &error));
    g_assert_no_error(error);
    g_assert_cmpint(g_chmod(group_meta, 0620), ==, 0);
    g_assert_true(g_file_set_contents(quarantine, "residue", -1, &error));
    g_assert_no_error(error);

    gint64 started = g_get_monotonic_time();
    pcv_overlay_restore();
    g_assert_cmpint(g_get_monotonic_time() - started, <, G_TIME_SPAN_SECOND);
    JsonArray *list = pcv_overlay_list(&error);
    g_assert_null(list);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    g_assert_cmpint(g_remove(symlink_meta), ==, 0);
    g_assert_cmpint(g_remove(fifo_meta), ==, 0);
    g_assert_cmpint(g_remove(group_meta), ==, 0);
    g_assert_cmpint(g_remove(quarantine), ==, 0);
    g_assert_cmpint(g_remove(target), ==, 0);
    pcv_overlay_restore();
    list = pcv_overlay_list(&error);
    g_assert_no_error(error);
    g_assert_nonnull(list);
    g_assert_cmpuint(json_array_get_length(list), ==, 0);
    json_array_unref(list);
    g_free(quarantine); g_free(group_meta); g_free(fifo_meta);
    g_free(symlink_meta); g_free(target);
    _overlay_effect_teardown();
}

static void test_overlay_metadata_directory_cardinality_is_bounded(void)
{
    _overlay_effect_setup();
    GError *error = NULL;
    for (guint i = 0; i <= 64; i++) {
        gchar *filename = g_strdup_printf("foreign-%03u", i);
        gchar *path = g_build_filename(O_meta_dir, filename, NULL);
        g_assert_true(_test_write_metadata(path, "x", 1, &error));
        g_assert_no_error(error);
        g_free(path);
        g_free(filename);
    }
    guint query_before = _fake_command_count("ovsdb-client");
    gint64 started = g_get_monotonic_time();
    pcv_overlay_restore();
    g_assert_cmpint(g_get_monotonic_time() - started, <, G_USEC_PER_SEC);
    g_assert_cmpuint(_fake_command_count("ovsdb-client"), ==, query_before);
    JsonArray *list = pcv_overlay_list(&error);
    g_assert_null(list);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    _overlay_effect_teardown();

    _overlay_effect_setup();
    for (guint i = 0; i <= 16; i++) {
        gchar *filename = g_strdup_printf("overlay-cap%02u.meta", i);
        gchar *path = g_build_filename(O_meta_dir, filename, NULL);
        g_assert_true(_test_write_metadata(path, "{}", 2, &error));
        g_assert_no_error(error);
        g_free(path);
        g_free(filename);
    }
    query_before = _fake_command_count("ovsdb-client");
    pcv_overlay_restore();
    g_assert_cmpuint(_fake_command_count("ovsdb-client"), ==, query_before);
    list = pcv_overlay_list(&error);
    g_assert_null(list);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    _overlay_effect_teardown();
}

static void test_overlay_canonical_sidecar_blocks_restore_mutation(void)
{
    _overlay_effect_setup();
    GError *error = NULL;
    gchar *v2_meta = _overlay_meta_path("sidev2");
    gchar *v2_sidecar = g_strdup_printf("%s.updating", v2_meta);
    g_assert_true(_test_write_metadata(
        v2_meta,
        "{\"schema_version\":2,\"name\":\"sidev2\",\"vni\":735,"
        "\"cidr\":\"\",\"owner_token\":"
        "\"99999999-9999-4999-8999-999999999999\","
        "\"generation\":1,\"peers\":[]}", -1, &error));
    g_assert_no_error(error);
    g_assert_true(_test_write_metadata(v2_sidecar, "old", -1, &error));
    g_assert_no_error(error);

    _fake_seed_bridge("sidelegacy", "", NULL, NULL);
    _fake_seed_port("sidelegacy", "vxlan-0-104", NULL, NULL, "vxlan",
                    "736", "192.168.0.104", "192.0.2.10");
    gchar *legacy_meta = _overlay_meta_path("sidelegacy");
    gchar *legacy_sidecar = g_strdup_printf("%s.deleting", legacy_meta);
    g_assert_true(_test_write_metadata(
        legacy_meta,
        "{\"name\":\"sidelegacy\",\"vni\":736,\"cidr\":\"\","
        "\"peers\":[\"192.168.0.104\"]}", -1, &error));
    g_assert_no_error(error);
    g_assert_true(_test_write_metadata(legacy_sidecar, "old", -1, &error));
    g_assert_no_error(error);



    pcv_overlay_restore();
    JsonArray *list = pcv_overlay_list(&error);
    g_assert_null(list);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    g_assert_cmpuint(_fake_command_count("add-br sidev2"), ==, 0);
    g_assert_cmpuint(_fake_command_count("set Bridge sidelegacy"), ==, 0);
    g_assert_cmpstr(_fake_lookup(O.bridge_owners, "sidelegacy"), ==, "");
    g_assert_true(g_hash_table_contains(O.port_bridges, "vxlan-0-104"));
    g_free(legacy_sidecar); g_free(legacy_meta);
    g_free(v2_sidecar); g_free(v2_meta);
    _overlay_effect_teardown();
}

static void test_overlay_global_preflight_precedes_all_actual_mutation(void)
{
    _overlay_effect_setup();
    GError *error = NULL;
    gchar *clean = _overlay_meta_path("aaa-clean");
    gchar *bad = _overlay_meta_path("zzz-bad");
    g_assert_true(_test_write_metadata(
        clean,
        "{\"schema_version\":2,\"name\":\"aaa-clean\",\"vni\":739,"
        "\"cidr\":\"\",\"owner_token\":"
        "\"91919191-9191-4191-8191-919191919191\","
        "\"generation\":1,\"peers\":[]}", -1, &error));
    g_assert_no_error(error);
    g_assert_true(_test_write_metadata(bad, "{corrupt", -1, &error));
    g_assert_no_error(error);

    pcv_overlay_restore();
    JsonArray *list = pcv_overlay_list(&error);
    g_assert_null(list);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    g_assert_cmpuint(_fake_command_count("add-br aaa-clean"), ==, 0);
    g_assert_cmpuint(_fake_command_count("ip link set aaa-clean"), ==, 0);
    g_assert_false(g_hash_table_contains(O.bridges, "aaa-clean"));
    g_free(bad);
    g_free(clean);
    _overlay_effect_teardown();
}

static void test_overlay_restore_error_blocks_all_mutations(void)
{
    _overlay_effect_setup();
    GError *error = NULL;
    g_assert_true(pcv_overlay_create("guardb", 737, NULL, &error));
    g_assert_no_error(error);
    gchar *residue = g_build_filename(
        O_meta_dir, "overlay-bad.meta.quarantine.external", NULL);
    g_assert_true(_test_write_metadata(residue, "residue", -1, &error));
    g_assert_no_error(error);
    pcv_overlay_restore();

    g_assert_false(pcv_overlay_create("guardc", 738, NULL, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    g_assert_false(pcv_overlay_add_peer("guardb", "192.168.0.104", &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    g_assert_false(pcv_overlay_remove_peer("guardb", "192.168.0.104", &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    g_assert_false(pcv_overlay_delete("guardb", &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    g_assert_cmpuint(_fake_command_count("add-br guardc"), ==, 0);
    g_assert_cmpuint(_fake_command_count("add-port guardb"), ==, 0);
    g_assert_cmpuint(_fake_command_count("del-port guardb"), ==, 0);
    g_assert_cmpuint(_fake_command_count("del-br guardb"), ==, 0);

    g_assert_cmpint(g_remove(residue), ==, 0);
    pcv_overlay_restore();
    g_assert_true(pcv_overlay_delete("guardb", &error));
    g_assert_no_error(error);
    g_free(residue);
    _overlay_effect_teardown();
}

static void test_overlay_transaction_races_preserve_foreign_state(void)
{
    _overlay_effect_setup();
    GError *error = NULL;
    g_assert_true(pcv_overlay_create("racechild", 740, NULL, &error));
    g_assert_no_error(error);
    gchar *meta = _overlay_meta_path("racechild");
    FakePortRace child_race = {
        .contains = "del-br racechild", .bridge = "racechild",
        .port = "late-child", .after = FALSE,
    };
    O.command_hook = _fake_port_race_hook;
    O.command_hook_data = &child_race;
    g_assert_false(pcv_overlay_delete("racechild", &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
    g_clear_error(&error);
    g_assert_true(g_file_test(meta, G_FILE_TEST_EXISTS));
    g_assert_true(g_hash_table_contains(O.bridges, "racechild"));
    g_assert_true(g_hash_table_contains(O.port_bridges, "late-child"));
    _fake_remove_port("late-child");
    O.command_hook = NULL;
    g_assert_true(pcv_overlay_delete("racechild", &error));
    g_assert_no_error(error);
    g_free(meta);

    g_assert_true(pcv_overlay_create("claim0", 741, NULL, &error));
    gchar *claim_meta = _overlay_meta_path("claim0");
    gchar *update = g_strdup_printf("%s.updating", claim_meta);
    FakeFileRace claim_race = {
        .contains = "add-port claim0", .path = update,
        .contents = "external-claim", .before = FALSE,
    };
    O.command_hook = _fake_file_race_hook;
    O.command_hook_data = &claim_race;
    gchar *claim_port = pcv_overlay_peer_port_name(741, "192.168.0.105");
    g_assert_false(pcv_overlay_add_peer("claim0", "192.168.0.105", &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    gchar *external = NULL;
    g_assert_true(g_file_get_contents(update, &external, NULL, &error));
    g_assert_no_error(error);
    g_assert_cmpstr(external, ==, "external-claim");
    g_assert_false(g_hash_table_contains(O.port_bridges, claim_port));
    JsonArray *list = pcv_overlay_list(&error);
    g_assert_null(list);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    g_assert_cmpint(g_remove(update), ==, 0);
    O.command_hook = NULL;
    pcv_overlay_restore();

    g_assert_true(pcv_overlay_add_peer("claim0", "192.168.0.105", &error));
    g_assert_no_error(error);
    _fake_seed_bridge("foreignmove", "", NULL, NULL);
    gchar *set_needle = g_strdup_printf("wait-until Interface %s", claim_port);
    FakeMoveRace move_race = {
        .contains = set_needle, .port = claim_port,
        .new_bridge = "foreignmove",
    };
    O.command_hook = _fake_move_race_hook;
    O.command_hook_data = &move_race;
    g_assert_false(pcv_overlay_add_peer("claim0", "192.168.0.105", &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
    g_clear_error(&error);
    g_assert_cmpstr(_fake_lookup(O.port_bridges, claim_port), ==, "foreignmove");
    g_assert_cmpstr(_fake_lookup(O.port_local, claim_port), ==, "192.0.2.10");
    O.command_hook = NULL;
    g_free(set_needle); g_free(external); g_free(claim_port);
    g_free(update); g_free(claim_meta);
    _overlay_effect_teardown();
}

static void test_overlay_committed_metadata_residue_is_fail_closed(void)
{
    GError *error = NULL;
    _overlay_fake_reset();
    O_meta_dir = g_dir_make_tmp("pcv-overlay-residue-XXXXXX", &error);
    g_assert_no_error(error);
    gboolean hook_fired = FALSE;
    pcv_overlay_set_test_context(_overlay_fake_exec, O_meta_dir);
    pcv_overlay_set_metadata_test_hook(_metadata_residue_hook, &hook_fired);
    pcv_overlay_init("192.0.2.10");
    g_assert_true(pcv_overlay_create("degraded", 750, NULL, &error));
    g_assert_no_error(error);
    gchar *port = pcv_overlay_peer_port_name(750, "192.168.0.106");
    gchar *meta = _overlay_meta_path("degraded");
    gchar *update = g_strdup_printf("%s.updating", meta);
    g_assert_false(pcv_overlay_add_peer("degraded", "192.168.0.106", &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_assert_true(hook_fired);
    g_assert_true(g_hash_table_contains(O.port_bridges, port));
    gint64 schema = 0;
    gint64 generation = 0;
    gchar *owner = _meta_owner(meta, &schema, &generation);
    g_assert_cmpint(generation, ==, 2);
    g_assert_true(g_file_test(update, G_FILE_TEST_IS_DIR));
    JsonArray *list = pcv_overlay_list(&error);
    g_assert_null(list);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    g_free(owner); g_free(update); g_free(meta); g_free(port);
    _overlay_effect_teardown();
}

static void test_overlay_delete_cleanup_residue_is_fail_closed(void)
{
    GError *error = NULL;
    _overlay_fake_reset();
    O_meta_dir = g_dir_make_tmp("pcv-overlay-delete-residue-XXXXXX", &error);
    g_assert_no_error(error);
    gboolean hook_fired = FALSE;
    pcv_overlay_set_test_context(_overlay_fake_exec, O_meta_dir);
    pcv_overlay_set_metadata_test_hook(_metadata_residue_hook, &hook_fired);
    pcv_overlay_init("192.0.2.10");
    g_assert_true(pcv_overlay_create("deldirty", 751, NULL, &error));
    g_assert_no_error(error);
    gchar *meta = _overlay_meta_path("deldirty");
    gchar *tomb = g_strdup_printf("%s.deleting", meta);
    g_assert_false(pcv_overlay_delete("deldirty", &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_assert_true(hook_fired);
    g_assert_false(g_hash_table_contains(O.bridges, "deldirty"));
    g_assert_true(g_file_test(tomb, G_FILE_TEST_IS_DIR));
    JsonArray *list = pcv_overlay_list(&error);
    g_assert_null(list);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    g_free(tomb); g_free(meta);
    _overlay_effect_teardown();
}

static void test_overlay_directory_sync_commit_boundaries(void)
{
    GError *error = NULL;




    _overlay_fake_reset();
    O_meta_dir = g_dir_make_tmp("pcv-overlay-sync-create-XXXXXX", &error);
    g_assert_no_error(error);
    DirSyncFault create_fault = {.fail_at = 1};
    pcv_overlay_set_test_context(_overlay_fake_exec, O_meta_dir);
    pcv_overlay_set_dir_sync_test_hook(_dir_sync_fault_hook, &create_fault);
    pcv_overlay_init("192.0.2.10");
    g_assert_false(pcv_overlay_create("syncnew", 752, NULL, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    gchar *meta = _overlay_meta_path("syncnew");
    g_assert_true(g_hash_table_contains(O.bridges, "syncnew"));
    g_assert_true(g_file_test(meta, G_FILE_TEST_IS_REGULAR));
    JsonArray *list = pcv_overlay_list(&error);
    g_assert_null(list);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    pcv_overlay_restore();
    list = pcv_overlay_list(&error);
    g_assert_no_error(error);
    g_assert_cmpuint(json_array_get_length(list), ==, 1);
    json_array_unref(list);
    pcv_overlay_shutdown();
    pcv_overlay_init("192.0.2.10");
    pcv_overlay_restore();
    list = pcv_overlay_list(&error);
    g_assert_no_error(error);
    g_assert_cmpuint(json_array_get_length(list), ==, 1);
    json_array_unref(list);
    g_free(meta);
    _overlay_effect_teardown();



    _overlay_fake_reset();
    O_meta_dir = g_dir_make_tmp("pcv-overlay-sync-update-XXXXXX", &error);
    g_assert_no_error(error);
    DirSyncFault update_fault = {.fail_at = 3};
    pcv_overlay_set_test_context(_overlay_fake_exec, O_meta_dir);
    pcv_overlay_set_dir_sync_test_hook(_dir_sync_fault_hook, &update_fault);
    pcv_overlay_init("192.0.2.10");
    g_assert_true(pcv_overlay_create("syncupd", 753, NULL, &error));
    g_assert_no_error(error);
    gchar *port = pcv_overlay_peer_port_name(753, "192.168.0.104");
    meta = _overlay_meta_path("syncupd");
    gchar *update = g_strdup_printf("%s.updating", meta);
    g_assert_false(pcv_overlay_add_peer("syncupd", "192.168.0.104", &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_assert_true(g_hash_table_contains(O.port_bridges, port));
    g_assert_true(g_file_test(update, G_FILE_TEST_IS_REGULAR));
    gint64 schema = 0;
    gint64 generation = 0;
    gchar *owner = _meta_owner(meta, &schema, &generation);
    g_assert_cmpint(generation, ==, 2);
    list = pcv_overlay_list(&error);
    g_assert_null(list);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    g_assert_cmpint(g_remove(update), ==, 0);
    pcv_overlay_restore();
    list = pcv_overlay_list(&error);
    g_assert_no_error(error);
    g_assert_cmpuint(json_array_get_length(list), ==, 1);
    json_array_unref(list);
    g_free(owner); g_free(update); g_free(meta); g_free(port);
    _overlay_effect_teardown();



    _overlay_fake_reset();
    O_meta_dir = g_dir_make_tmp("pcv-overlay-sync-delete-XXXXXX", &error);
    g_assert_no_error(error);
    DirSyncFault delete_fault = {.fail_at = 3};
    pcv_overlay_set_test_context(_overlay_fake_exec, O_meta_dir);
    pcv_overlay_set_dir_sync_test_hook(_dir_sync_fault_hook, &delete_fault);
    pcv_overlay_init("192.0.2.10");
    g_assert_true(pcv_overlay_create("syncdel", 754, NULL, &error));
    g_assert_no_error(error);
    meta = _overlay_meta_path("syncdel");
    gchar *tomb = g_strdup_printf("%s.deleting", meta);
    g_assert_false(pcv_overlay_delete("syncdel", &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_assert_false(g_hash_table_contains(O.bridges, "syncdel"));
    g_assert_false(g_file_test(meta, G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(tomb, G_FILE_TEST_EXISTS));
    list = pcv_overlay_list(&error);
    g_assert_null(list);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    pcv_overlay_restore();
    list = pcv_overlay_list(&error);
    g_assert_no_error(error);
    g_assert_cmpuint(json_array_get_length(list), ==, 0);
    json_array_unref(list);
    g_free(tomb); g_free(meta);
    _overlay_effect_teardown();
}

static void test_overlay_metadata_directory_ownership_and_creation_sync(void)
{
    GError *error = NULL;




    _overlay_fake_reset();
    O_meta_dir = g_dir_make_tmp("pcv-overlay-unsafe-dir-XXXXXX", &error);
    g_assert_no_error(error);
    g_assert_cmpint(g_chmod(O_meta_dir, 0770), ==, 0);
    pcv_overlay_set_test_context(_overlay_fake_exec, O_meta_dir);
    pcv_overlay_init("192.0.2.10");
    pcv_overlay_restore();
    JsonArray *list = pcv_overlay_list(&error);
    g_assert_null(list);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    g_assert_false(pcv_overlay_create("unsafedir", 757, NULL, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    g_assert_cmpuint(_fake_command_count("add-br unsafedir"), ==, 0);
    g_assert_cmpint(g_chmod(O_meta_dir, 0700), ==, 0);
    pcv_overlay_restore();
    list = pcv_overlay_list(&error);
    g_assert_no_error(error);
    g_assert_cmpuint(json_array_get_length(list), ==, 0);
    json_array_unref(list);
    _overlay_effect_teardown();




    _overlay_fake_reset();
    gchar *parent = g_dir_make_tmp("pcv-overlay-new-dir-XXXXXX", &error);
    g_assert_no_error(error);
    O_meta_dir = g_build_filename(parent, "overlay", NULL);
    DirSyncFault create_dir_fault = {.fail_at = 1};
    pcv_overlay_set_test_context(_overlay_fake_exec, O_meta_dir);
    pcv_overlay_set_dir_sync_test_hook(_dir_sync_fault_hook,
                                       &create_dir_fault);
    pcv_overlay_init("192.0.2.10");
    pcv_overlay_restore();
    g_assert_cmpuint(create_dir_fault.calls, ==, 1);
    list = pcv_overlay_list(&error);
    g_assert_null(list);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    pcv_overlay_restore();
    list = pcv_overlay_list(&error);
    g_assert_no_error(error);
    g_assert_cmpuint(json_array_get_length(list), ==, 0);
    json_array_unref(list);
    g_assert_true(pcv_overlay_create("newdir", 758, NULL, &error));
    g_assert_no_error(error);
    _overlay_effect_teardown();
    g_assert_cmpint(g_rmdir(parent), ==, 0);
    g_free(parent);
}

static void test_overlay_post_exclusive_stat_failure_is_degraded(void)
{
    GError *error = NULL;
    _overlay_fake_reset();
    O_meta_dir = g_dir_make_tmp("pcv-overlay-stat-fault-XXXXXX", &error);
    g_assert_no_error(error);
    MetadataStatFault fault = {.fail_at = 1};
    pcv_overlay_set_test_context(_overlay_fake_exec, O_meta_dir);
    pcv_overlay_set_metadata_stat_test_hook(
        _metadata_stat_fault_hook, &fault);
    pcv_overlay_init("192.0.2.10");

    gchar *meta = _overlay_meta_path("statfail");
    g_assert_false(pcv_overlay_create("statfail", 755, NULL, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_assert_cmpuint(fault.calls, ==, 1);
    g_assert_false(g_hash_table_contains(O.bridges, "statfail"));
    g_assert_true(g_file_test(meta, G_FILE_TEST_IS_REGULAR));
    JsonArray *list = pcv_overlay_list(&error);
    g_assert_null(list);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    g_assert_false(pcv_overlay_create("blocked", 756, NULL, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    g_assert_cmpuint(_fake_command_count("add-br blocked"), ==, 0);

    g_assert_cmpint(g_remove(meta), ==, 0);
    pcv_overlay_restore();
    list = pcv_overlay_list(&error);
    g_assert_no_error(error);
    g_assert_cmpuint(json_array_get_length(list), ==, 0);
    json_array_unref(list);
    g_free(meta);
    _overlay_effect_teardown();
}

static void test_overlay_metadata_exact_size_boundary_precedes_actual(void)
{
    _overlay_effect_setup();
    GError *error = NULL;
    g_assert_true(pcv_overlay_create("metacap", 759, NULL, &error));
    g_assert_no_error(error);
    gchar *meta = _overlay_meta_path("metacap");
    gchar *before = NULL;
    gsize before_len = 0;
    g_assert_true(g_file_get_contents(meta, &before, &before_len, &error));
    g_assert_no_error(error);

    pcv_overlay_shutdown();
    pcv_overlay_set_metadata_limit_for_test(before_len);
    pcv_overlay_init("192.0.2.10");
    pcv_overlay_restore();
    JsonArray *list = pcv_overlay_list(&error);
    g_assert_no_error(error);
    g_assert_cmpuint(json_array_get_length(list), ==, 1);
    json_array_unref(list);

    gchar *port = pcv_overlay_peer_port_name(759, "198.51.100.20");
    gchar *needle = g_strdup_printf("add-port metacap %s", port);
    guint commands_before = _fake_command_count(needle);
    g_assert_false(pcv_overlay_add_peer("metacap", "198.51.100.20", &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE);
    g_clear_error(&error);
    g_assert_cmpuint(_fake_command_count(needle), ==, commands_before);
    g_assert_false(g_hash_table_contains(O.port_bridges, port));
    gchar *after = NULL;
    gsize after_len = 0;
    g_assert_true(g_file_get_contents(meta, &after, &after_len, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(after_len, ==, before_len);
    g_assert_cmpmem(after, after_len, before, before_len);

    g_free(after); g_free(needle); g_free(port); g_free(before); g_free(meta);
    _overlay_effect_teardown();
}

static void test_overlay_peer_limit_rejects_before_actual_and_restore(void)
{
    _overlay_effect_setup();
    GError *error = NULL;
    g_assert_true(pcv_overlay_create("peerlimit", 761, NULL, &error));
    g_assert_no_error(error);
    for (guint i = 1; i <= 64; i++) {
        gchar *peer = g_strdup_printf("198.51.100.%u", i);
        g_assert_true(pcv_overlay_add_peer("peerlimit", peer, &error));
        g_assert_no_error(error);
        g_free(peer);
    }
    gchar *overflow_port = pcv_overlay_peer_port_name(761, "198.51.100.65");
    gchar *overflow_needle =
        g_strdup_printf("add-port peerlimit %s", overflow_port);
    guint commands_before = _fake_command_count(overflow_needle);
    g_assert_false(pcv_overlay_add_peer("peerlimit", "198.51.100.65", &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE);
    g_clear_error(&error);
    g_assert_cmpuint(_fake_command_count(overflow_needle), ==, commands_before);
    g_assert_false(g_hash_table_contains(O.port_bridges, overflow_port));
    g_free(overflow_needle); g_free(overflow_port);
    _overlay_effect_teardown();

    _overlay_effect_setup();
    GString *json = g_string_new(
        "{\"name\":\"peerparse\",\"vni\":762,\"cidr\":\"\",\"peers\":[");
    for (guint i = 1; i <= 65; i++)
        g_string_append_printf(json, "%s\"203.0.113.%u\"",
                               i == 1 ? "" : ",", i);
    g_string_append(json, "]}");
    gchar *meta = _overlay_meta_path("peerparse");
    g_assert_true(_test_write_metadata(meta, json->str, (gssize)json->len,
                                       &error));
    g_assert_no_error(error);
    pcv_overlay_restore();
    JsonArray *list = pcv_overlay_list(&error);
    g_assert_null(list);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    g_assert_false(g_hash_table_contains(O.bridges, "peerparse"));
    g_free(meta); g_string_free(json, TRUE);
    _overlay_effect_teardown();
}

static void test_overlay_bulk_restore_max_cardinality_is_constant(void)
{
    _overlay_effect_setup();






    const gboolean instrumented = RUNNING_ON_VALGRIND != 0;
    const gint64 elapsed_limit = (instrumented ? 300 : 30) * G_USEC_PER_SEC;
    if (instrumented) {
        pcv_overlay_shutdown();
        pcv_overlay_set_restore_deadline_for_test(300000);
        pcv_overlay_init("192.0.2.10");
    }
    GError *error = NULL;
    for (guint overlay_index = 0; overlay_index < 16; overlay_index++) {
        gchar *name = g_strdup_printf("bulk%02u", overlay_index);
        gchar *owner = g_strdup_printf(
            "a0000000-0000-4000-8000-%012u", overlay_index + 1);
        gint vni = 1000 + (gint)overlay_index;
        _fake_seed_bridge(name, "", owner, name);
        GString *json = g_string_new(NULL);
        g_string_append_printf(
            json,
            "{\"schema_version\":2,\"name\":\"%s\",\"vni\":%d,"
            "\"cidr\":\"\",\"owner_token\":\"%s\","
            "\"generation\":1,\"peers\":[",
            name, vni, owner);
        for (guint peer_index = 1; peer_index <= 64; peer_index++) {
            gchar *peer = g_strdup_printf("198.51.%u.%u", overlay_index,
                                           peer_index);
            gchar *port = pcv_overlay_peer_port_name(vni, peer);
            gchar *key = g_strdup_printf("%d", vni);
            _fake_seed_port(name, port, owner, name, "vxlan", key, peer,
                            "192.0.2.10");
            g_string_append_printf(json, "%s\"%s\"",
                                   peer_index == 1 ? "" : ",", peer);
            g_free(key);
            g_free(port);
            g_free(peer);
        }
        g_string_append(json, "]}");
        gchar *meta = _overlay_meta_path(name);
        g_assert_true(_test_write_metadata(meta, json->str,
                                           (gssize)json->len, &error));
        g_assert_no_error(error);
        g_free(meta);
        g_string_free(json, TRUE);
        g_free(owner);
        g_free(name);
    }

    guint command_start = O.commands->len;
    gint64 started = g_get_monotonic_time();
    pcv_overlay_restore();
    gint64 elapsed = g_get_monotonic_time() - started;


    guint command_end = O.commands->len;
    g_test_message("bulk restore elapsed=%" G_GINT64_FORMAT
                   " us, commands=%u, instrumented=%d, limit=%" G_GINT64_FORMAT,
                   elapsed, command_end - command_start, instrumented,
                   elapsed_limit);
    JsonArray *list = pcv_overlay_list(&error);
    g_assert_no_error(error);
    g_assert_nonnull(list);
    g_assert_cmpint(elapsed, <, elapsed_limit);
    g_assert_cmpuint(command_end - command_start, ==, 4);
    g_assert_cmpuint(_fake_command_count("ovsdb-client --timeout=5 query"), ==,
                     2);
    g_assert_cmpuint(_fake_command_count("ip -j -4 addr show"), ==, 2);
    for (guint i = command_start; i < command_end; i++)
        g_assert_false(g_str_has_prefix(g_ptr_array_index(O.commands, i),
                                        "ovs-vsctl"));

    g_assert_cmpuint(json_array_get_length(list), ==, 16);
    for (guint i = 0; i < json_array_get_length(list); i++)
        g_assert_cmpint(json_object_get_int_member(
                            json_array_get_object_element(list, i),
                            "peer_count"), ==, 64);
    json_array_unref(list);
    _overlay_effect_teardown();
}

static void test_overlay_bulk_inverse_relationships_fail_closed(void)
{
    GError *error = NULL;




    _overlay_effect_setup();
    const gchar *first_owner = "b0000000-0000-4000-8000-000000000010";
    const gchar *second_owner = "b0000000-0000-4000-8000-000000000011";
    gchar *meta = _overlay_meta_path("aamiss");
    const gchar *first_json =
        "{\"schema_version\":2,\"name\":\"aamiss\",\"vni\":1098,"
        "\"cidr\":\"\",\"owner_token\":"
        "\"b0000000-0000-4000-8000-000000000010\",\"generation\":1,"
        "\"peers\":[\"198.51.100.10\"]}";
    g_assert_true(_test_write_metadata(meta, first_json, -1, &error));
    g_assert_no_error(error);
    g_free(meta);
    _fake_seed_bridge("aamiss", "", first_owner, "aamiss");

    meta = _overlay_meta_path("zzmix");
    const gchar *second_json =
        "{\"schema_version\":2,\"name\":\"zzmix\",\"vni\":1099,"
        "\"cidr\":\"\",\"owner_token\":"
        "\"b0000000-0000-4000-8000-000000000011\",\"generation\":1,"
        "\"peers\":[\"198.51.100.11\",\"198.51.100.12\"]}";
    g_assert_true(_test_write_metadata(meta, second_json, -1, &error));
    g_assert_no_error(error);
    g_free(meta);
    _fake_seed_bridge("zzmix", "", second_owner, "zzmix");
    gchar *present = pcv_overlay_peer_port_name(1099, "198.51.100.11");
    gchar *missing = pcv_overlay_peer_port_name(1099, "198.51.100.12");
    _fake_seed_port("zzmix", present, second_owner, "zzmix", "vxlan",
                    "1099", "198.51.100.11", "192.0.2.10");
    _fake_seed_bridge("zzshare", "", NULL, NULL);
    _fake_seed_port("zzshare", "tapzzshare", NULL, NULL, "", NULL, NULL,
                    NULL);
    _fake_set(O.port_interfaces, "tapzzshare",
              _fake_lookup(O.interface_uuids, present));
    gchar *missing_add = g_strdup_printf("add-port zzmix %s", missing);
    pcv_overlay_restore();
    JsonArray *list = pcv_overlay_list(&error);
    g_assert_null(list);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    g_assert_cmpuint(_fake_command_count(missing_add), ==, 0);
    g_assert_false(g_hash_table_contains(O.port_bridges, missing));
    g_assert_true(g_hash_table_contains(O.port_bridges, present));
    g_free(missing_add); g_free(missing); g_free(present);
    _overlay_effect_teardown();



    _overlay_effect_setup();
    const gchar *owner = "b0000000-0000-4000-8000-000000000001";
    const gchar *mixed_json =
        "{\"schema_version\":2,\"name\":\"bulkmix\",\"vni\":1100,"
        "\"cidr\":\"\",\"owner_token\":"
        "\"b0000000-0000-4000-8000-000000000001\",\"generation\":1,"
        "\"peers\":[\"198.51.100.1\",\"198.51.100.2\"]}";
    meta = _overlay_meta_path("bulkmix");
    g_assert_true(_test_write_metadata(meta, mixed_json, -1, &error));
    g_assert_no_error(error);
    _fake_seed_bridge("bulkmix", "", owner, "bulkmix");
    gchar *p1 = pcv_overlay_peer_port_name(1100, "198.51.100.1");
    gchar *p2 = pcv_overlay_peer_port_name(1100, "198.51.100.2");
    _fake_seed_port("bulkmix", p1, owner, "bulkmix", "vxlan", "1100",
                    "198.51.100.1", "192.0.2.10");
    _fake_seed_bridge("sharebr", "", NULL, NULL);
    _fake_seed_port("sharebr", "tapshare", NULL, NULL, "", NULL, NULL,
                    NULL);
    _fake_set(O.port_interfaces, "tapshare",
              _fake_lookup(O.interface_uuids, p1));
    missing_add = g_strdup_printf("add-port bulkmix %s", p2);
    pcv_overlay_restore();
    list = pcv_overlay_list(&error);
    g_assert_null(list);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    g_assert_cmpuint(_fake_command_count(missing_add), ==, 0);
    g_assert_false(g_hash_table_contains(O.port_bridges, p2));
    g_assert_true(g_hash_table_contains(O.port_bridges, p1));
    g_free(missing_add); g_free(p2); g_free(p1); g_free(meta);
    _overlay_effect_teardown();



    _overlay_effect_setup();
    g_assert_true(pcv_overlay_create("bulkdel", 1101, NULL, &error));
    g_assert_no_error(error);
    meta = _overlay_meta_path("bulkdel");
    _fake_seed_bridge("otherbr", "", NULL, NULL);
    O.extra_parent_bridge = g_strdup("otherbr");
    O.extra_parent_port = g_strdup("bulkdel");
    guint delete_before = _fake_command_count("del-br bulkdel");
    g_assert_false(pcv_overlay_delete("bulkdel", &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_assert_cmpuint(_fake_command_count("del-br bulkdel"), ==, delete_before);
    g_assert_true(g_hash_table_contains(O.bridges, "bulkdel"));
    g_assert_true(g_file_test(meta, G_FILE_TEST_EXISTS));
    g_free(meta);
    _overlay_effect_teardown();



    _overlay_effect_setup();
    const gchar *legacy_json =
        "{\"name\":\"bulkleg\",\"vni\":1102,\"cidr\":\"\","
        "\"peers\":[\"192.168.0.104\"]}";
    meta = _overlay_meta_path("bulkleg");
    g_assert_true(_test_write_metadata(meta, legacy_json, -1, &error));
    g_assert_no_error(error);
    _fake_seed_bridge("bulkleg", "", NULL, NULL);
    _fake_seed_port("bulkleg", "vxlan-0-104", NULL, NULL, "vxlan",
                    "1102", "192.168.0.104", "192.0.2.10");
    _fake_seed_bridge("legacyshare", "", NULL, NULL);
    _fake_seed_port("legacyshare", "taplegacy", NULL, NULL, "", NULL,
                    NULL, NULL);
    _fake_set(O.port_interfaces, "taplegacy",
              _fake_lookup(O.interface_uuids, "vxlan-0-104"));
    gchar *canonical = pcv_overlay_peer_port_name(1102, "192.168.0.104");
    pcv_overlay_restore();
    list = pcv_overlay_list(&error);
    g_assert_null(list);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    g_assert_true(g_hash_table_contains(O.port_bridges, "vxlan-0-104"));
    g_assert_false(g_hash_table_contains(O.port_bridges, canonical));
    g_assert_cmpuint(_fake_command_count("set Bridge bulkleg"), ==, 0);
    g_free(canonical); g_free(meta);
    _overlay_effect_teardown();



    _overlay_effect_setup();
    const gchar *absent_json =
        "{\"schema_version\":2,\"name\":\"bulkabs\",\"vni\":1103,"
        "\"cidr\":\"\",\"owner_token\":"
        "\"b0000000-0000-4000-8000-000000000003\",\"generation\":1,"
        "\"peers\":[\"198.51.100.3\"]}";
    meta = _overlay_meta_path("bulkabs");
    g_assert_true(_test_write_metadata(meta, absent_json, -1, &error));
    g_assert_no_error(error);
    _fake_seed_bridge("absforeign", "", NULL, NULL);
    canonical = pcv_overlay_peer_port_name(1103, "198.51.100.3");
    _fake_seed_port("absforeign", canonical, NULL, NULL, "vxlan", "1103",
                    "198.51.100.3", "192.0.2.10");
    pcv_overlay_restore();
    list = pcv_overlay_list(&error);
    g_assert_null(list);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    g_assert_cmpuint(_fake_command_count("add-br bulkabs"), ==, 0);
    g_assert_true(g_hash_table_contains(O.port_bridges, canonical));
    g_free(canonical); g_free(meta);
    _overlay_effect_teardown();




    _overlay_effect_setup();
    const gchar *portmark_owner =
        "b0000000-0000-4000-8000-000000000004";
    const gchar *portmark_json =
        "{\"schema_version\":2,\"name\":\"portmark\",\"vni\":1104,"
        "\"cidr\":\"\",\"owner_token\":"
        "\"b0000000-0000-4000-8000-000000000004\",\"generation\":1,"
        "\"peers\":[\"198.51.100.4\"]}";
    meta = _overlay_meta_path("portmark");
    g_assert_true(_test_write_metadata(meta, portmark_json, -1, &error));
    g_assert_no_error(error);
    _fake_seed_bridge("portmark", "", portmark_owner, "portmark");
    canonical = pcv_overlay_peer_port_name(1104, "198.51.100.4");
    _fake_seed_port("portmark", canonical, portmark_owner, "portmark",
                    "vxlan", "1104", "198.51.100.4", "192.0.2.10");
    _fake_set(O.port_row_owners, canonical, portmark_owner);
    guint portmark_mutations = O.commands->len;
    pcv_overlay_restore();
    list = pcv_overlay_list(&error);
    g_assert_null(list);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    g_assert_cmpuint(O.commands->len, ==, portmark_mutations + 2);
    g_assert_cmpstr(_fake_lookup(O.port_row_owners, canonical), ==,
                    portmark_owner);
    g_free(canonical); g_free(meta);
    _overlay_effect_teardown();

    _overlay_effect_setup();
    _fake_seed_bridge("portforeign", "", NULL, NULL);
    _fake_seed_port("portforeign", "tapportforeign", NULL, NULL, "",
                    NULL, NULL, NULL);
    _fake_set(O.port_row_names, "tapportforeign", "orphan-overlay");
    pcv_overlay_restore();
    list = pcv_overlay_list(&error);
    g_assert_null(list);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    g_assert_cmpstr(_fake_lookup(O.port_row_names, "tapportforeign"), ==,
                    "orphan-overlay");
    _overlay_effect_teardown();
}

static void test_overlay_restore_rolls_back_multi_peer_actual(void)
{
    _overlay_effect_setup();
    GError *error = NULL;
    gchar *meta = _overlay_meta_path("multi0");
    gchar *p1 = pcv_overlay_peer_port_name(760, "192.168.0.107");
    gchar *p2 = pcv_overlay_peer_port_name(760, "192.168.0.108");
    const gchar *json =
        "{\"schema_version\":2,\"name\":\"multi0\",\"vni\":760,"
        "\"cidr\":\"\",\"owner_token\":"
        "\"77777777-7777-4777-8777-777777777777\",\"generation\":1,"
        "\"peers\":[\"192.168.0.107\",\"192.168.0.108\"]}";
    g_assert_true(_test_write_metadata(meta, json, -1, &error));
    g_assert_no_error(error);
    gchar *failure = g_strdup_printf("add-port multi0 %s", p2);
    _fake_fail_once(failure);
    pcv_overlay_restore();
    JsonArray *list = pcv_overlay_list(&error);
    g_assert_null(list);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    g_assert_false(g_hash_table_contains(O.bridges, "multi0"));
    g_assert_false(g_hash_table_contains(O.port_bridges, p1));
    g_assert_false(g_hash_table_contains(O.port_bridges, p2));
    g_assert_true(g_file_test(meta, G_FILE_TEST_EXISTS));
    g_free(failure); g_free(p2); g_free(p1); g_free(meta);
    _overlay_effect_teardown();
}

typedef struct {
    JsonArray *array;
    GError *error;
    gint done;
} ListThread;

static gpointer
_list_thread(gpointer data)
{
    ListThread *state = data;
    state->array = pcv_overlay_list(&state->error);
    g_atomic_int_set(&state->done, 1);
    return NULL;
}

typedef struct {
    const gchar *name;
    gint vni;
    gboolean result;
    GError *error;
    gint done;
} CreateThread;

typedef struct {
    gint done;
} ShutdownThread;
static gpointer _shutdown_thread(gpointer data);

typedef struct {
    GMutex mu;
    GCond cond;
    gboolean entered;
    gboolean release;
} LifecycleGate;

static void
_lifecycle_gate_hook(const gchar *operation, gpointer user_data)
{
    LifecycleGate *gate = user_data;
    if (g_strcmp0(operation, "create") != 0)
        return;
    g_mutex_lock(&gate->mu);
    gate->entered = TRUE;
    g_cond_broadcast(&gate->cond);
    while (!gate->release)
        g_cond_wait(&gate->cond, &gate->mu);
    g_mutex_unlock(&gate->mu);
}

static gpointer
_create_thread(gpointer data)
{
    CreateThread *state = data;
    state->result = pcv_overlay_create(state->name, state->vni, NULL,
                                       &state->error);
    g_atomic_int_set(&state->done, 1);
    return NULL;
}

typedef struct {
    const gchar *target;
    gboolean armed;
    gboolean fired;
    gboolean deleted;
    GError *error;
} RestoreDeleteRace;

typedef struct {
    GMutex mu;
    GCond cond;
    gboolean armed;
    gboolean entered;
    gboolean release;
    gboolean fired;
} RestoreSnapshotGate;

typedef struct {
    gint done;
} RestoreThread;

static void
_restore_snapshot_gate_hook(const gchar *path, gpointer user_data)
{
    RestoreSnapshotGate *gate = user_data;
    g_mutex_lock(&gate->mu);
    if (!gate->armed || gate->fired) {
        g_mutex_unlock(&gate->mu);
        return;
    }
    gate->fired = TRUE;
    gate->entered = TRUE;
    g_cond_broadcast(&gate->cond);
    while (!gate->release)
        g_cond_wait(&gate->cond, &gate->mu);
    g_mutex_unlock(&gate->mu);
    (void)path;
}

static gpointer
_snapshot_restore_thread(gpointer data)
{
    RestoreThread *state = data;
    pcv_overlay_restore();
    g_atomic_int_set(&state->done, 1);
    return NULL;
}

static void
_restore_delete_hook(const gchar *path, gpointer user_data)
{
    RestoreDeleteRace *race = user_data;
    if (!race->armed || race->fired || !g_str_has_suffix(path, race->target))
        return;
    race->fired = TRUE;
    race->deleted = pcv_overlay_delete("scan-delete", &race->error);
}

static void test_overlay_restore_skips_completed_concurrent_delete(void)
{
    _overlay_effect_setup();
    GError *error = NULL;
    g_assert_true(pcv_overlay_create("scan-delete", 770, NULL, &error));
    g_assert_no_error(error);
    pcv_overlay_shutdown();
    RestoreDeleteRace race = {.target = "overlay-scan-delete.meta"};
    pcv_overlay_set_restore_snapshot_test_hook(_restore_delete_hook, &race);
    pcv_overlay_init("192.0.2.10");
    pcv_overlay_restore();
    race.armed = TRUE;
    pcv_overlay_restore();
    g_assert_true(race.fired);
    g_assert_true(race.deleted);
    g_assert_no_error(race.error);
    JsonArray *list = pcv_overlay_list(&error);
    g_assert_no_error(error);
    g_assert_nonnull(list);
    g_assert_cmpuint(json_array_get_length(list), ==, 0);
    json_array_unref(list);
    _overlay_effect_teardown();
}

static void test_overlay_restore_discards_snapshot_stale_after_add(void)
{
    _overlay_effect_setup();
    GError *error = NULL;
    g_assert_true(pcv_overlay_create("scanadd", 772, NULL, &error));
    g_assert_no_error(error);
    pcv_overlay_shutdown();

    RestoreSnapshotGate gate = {0};
    g_mutex_init(&gate.mu);
    g_cond_init(&gate.cond);
    pcv_overlay_set_restore_snapshot_test_hook(
        _restore_snapshot_gate_hook, &gate);
    pcv_overlay_init("192.0.2.10");
    pcv_overlay_restore();

    g_mutex_lock(&gate.mu);
    gate.armed = TRUE;
    g_mutex_unlock(&gate.mu);
    RestoreThread state = {0};
    GThread *thread = g_thread_new("overlay-stale-scan",
                                   _snapshot_restore_thread,
                                   &state);
    g_mutex_lock(&gate.mu);
    while (!gate.entered)
        g_cond_wait(&gate.cond, &gate.mu);
    g_mutex_unlock(&gate.mu);

    g_assert_true(pcv_overlay_add_peer("scanadd", "192.168.0.104", &error));
    g_assert_no_error(error);
    g_mutex_lock(&gate.mu);
    gate.release = TRUE;
    g_cond_broadcast(&gate.cond);
    g_mutex_unlock(&gate.mu);
    g_thread_join(thread);
    g_assert_cmpint(g_atomic_int_get(&state.done), ==, 1);

    JsonArray *list = pcv_overlay_list(&error);
    g_assert_no_error(error);
    g_assert_nonnull(list);
    g_assert_cmpuint(json_array_get_length(list), ==, 1);
    JsonObject *row = json_array_get_object_element(list, 0);
    g_assert_cmpint(json_object_get_int_member(row, "peer_count"), ==, 1);
    json_array_unref(list);
    g_mutex_clear(&gate.mu);
    g_cond_clear(&gate.cond);
    _overlay_effect_teardown();
}




static void test_overlay_restore_preflight_blocks_mutation(void)
{
    _overlay_effect_setup();
    GError *error = NULL;
    g_assert_true(pcv_overlay_create("scanbusy", 775, NULL, &error));
    g_assert_no_error(error);
    _fake_block("ovsdb-client --timeout=5 query");
    RestoreThread state = {0};
    GThread *thread = g_thread_new("overlay-preflight", _snapshot_restore_thread,
                                   &state);
    _fake_wait_blocked();

    gboolean created = pcv_overlay_create("during775", 776, NULL, &error);
    gboolean bridge_absent = !g_hash_table_contains(O.bridges, "during775");
    gchar *path = _overlay_meta_path("during775");
    gboolean meta_absent = !g_file_test(path, G_FILE_TEST_EXISTS);
    _fake_release();
    g_thread_join(thread);
    g_assert_false(created);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_BUSY);
    g_clear_error(&error);
    g_assert_true(bridge_absent);
    g_assert_true(meta_absent);
    g_assert_cmpint(g_atomic_int_get(&state.done), ==, 1);

    g_assert_true(pcv_overlay_create("during775", 776, NULL, &error));
    g_assert_no_error(error);
    g_assert_true(g_hash_table_contains(O.bridges, "during775"));
    g_assert_true(g_file_test(path, G_FILE_TEST_IS_REGULAR));
    g_free(path);
    _overlay_effect_teardown();
}

static void test_overlay_shutdown_drains_admitted_public_operation(void)
{
    GError *error = NULL;
    _overlay_fake_reset();
    O_meta_dir = g_dir_make_tmp("pcv-overlay-public-drain-XXXXXX", &error);
    g_assert_no_error(error);
    LifecycleGate gate = {0};
    g_mutex_init(&gate.mu);
    g_cond_init(&gate.cond);
    pcv_overlay_set_test_context(_overlay_fake_exec, O_meta_dir);
    pcv_overlay_set_lifecycle_test_hook(_lifecycle_gate_hook, &gate);
    pcv_overlay_init("192.0.2.10");

    CreateThread create_state = {.name = "publicdrain", .vni = 771};
    GThread *create = g_thread_new("overlay-public-create", _create_thread,
                                   &create_state);
    g_mutex_lock(&gate.mu);
    while (!gate.entered)
        g_cond_wait(&gate.cond, &gate.mu);
    g_mutex_unlock(&gate.mu);

    ShutdownThread shutdown_state = {0};
    GThread *shutdown = g_thread_new("overlay-public-shutdown", _shutdown_thread,
                                     &shutdown_state);
    g_usleep(50000);
    g_assert_cmpint(g_atomic_int_get(&shutdown_state.done), ==, 0);
    g_mutex_lock(&gate.mu);
    gate.release = TRUE;
    g_cond_broadcast(&gate.cond);
    g_mutex_unlock(&gate.mu);
    g_thread_join(create);
    g_thread_join(shutdown);
    g_assert_true(create_state.result);
    g_assert_no_error(create_state.error);
    g_assert_cmpint(g_atomic_int_get(&shutdown_state.done), ==, 1);
    g_cond_clear(&gate.cond);
    g_mutex_clear(&gate.mu);
    _overlay_effect_teardown();
}

static void test_overlay_probe_timeout_and_lock_free_snapshot(void)
{
    _overlay_effect_setup();
    GError *error = NULL;
    g_assert_true(pcv_overlay_create("ovlview", 801, NULL, &error));
    g_assert_no_error(error);

    _fake_block("--columns=name,external_ids list Bridge");
    ListThread list_state = {0};
    GThread *list_thread = g_thread_new("overlay-list", _list_thread, &list_state);
    _fake_wait_blocked();

    CreateThread create_state = {.name = "other801", .vni = 802};
    GThread *create_thread = g_thread_new("overlay-create", _create_thread,
                                          &create_state);
    for (guint i = 0; i < 100 && !g_atomic_int_get(&create_state.done); i++)
        g_usleep(1000);
    gboolean progressed_without_list_probe =
        g_atomic_int_get(&create_state.done) != 0;
    _fake_release();
    g_thread_join(create_thread);
    g_thread_join(list_thread);
    g_assert_true(progressed_without_list_probe);
    g_assert_true(create_state.result);
    g_assert_no_error(create_state.error);
    g_assert_nonnull(list_state.array);
    g_assert_no_error(list_state.error);
    json_array_unref(list_state.array);

    _fake_fail_once_with_code("--columns=name,external_ids list Bridge",
                              G_IO_ERROR_TIMED_OUT);
    JsonArray *failed = pcv_overlay_list(&error);
    g_assert_null(failed);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
    g_clear_error(&error);
    g_assert_false(O.saw_unbounded_ovs);
    _overlay_effect_teardown();
}

static gpointer
_restore_thread(gpointer data)
{
    (void)data;
    pcv_overlay_restore();
    return NULL;
}

static gpointer
_shutdown_thread(gpointer data)
{
    ShutdownThread *state = data;
    pcv_overlay_shutdown();
    g_atomic_int_set(&state->done, 1);
    return NULL;
}

static void test_overlay_shutdown_drains_restore_worker(void)
{
    _overlay_effect_setup();
    GError *error = NULL;
    g_assert_true(pcv_overlay_create("drain0", 901, NULL, &error));
    g_assert_no_error(error);
    pcv_overlay_shutdown();
    pcv_overlay_init("192.0.2.10");




    _fake_block("ovsdb-client --timeout=5 query");
    GThread *restore = g_thread_new("overlay-restore", _restore_thread, NULL);
    _fake_wait_blocked();
    ShutdownThread shutdown_state = {0};
    GThread *shutdown = g_thread_new("overlay-shutdown", _shutdown_thread,
                                     &shutdown_state);
    g_usleep(50000);
    g_assert_cmpint(g_atomic_int_get(&shutdown_state.done), ==, 0);
    _fake_release();
    g_thread_join(restore);
    g_thread_join(shutdown);
    g_assert_cmpint(g_atomic_int_get(&shutdown_state.done), ==, 1);
    _overlay_effect_teardown();
}
#endif

              

void test_ovn_register(void) {
    g_test_add_func("/ovn/switch_list/empty",      test_ovn_switch_list_empty);
    g_test_add_func("/ovn/router_list/empty",      test_ovn_router_list_empty);
    g_test_add_func("/ovn/nat_list/empty",         test_ovn_nat_list_empty);
    g_test_add_func("/ovn/nat_list/header_omitted",
                    test_ovn_nat_list_parser_omits_cli_header);
    g_test_add_func("/ovn/dhcp_list/empty",        test_ovn_dhcp_list_empty);
    g_test_add_func("/ovn/acl_list/empty",         test_ovn_acl_list_empty);
    g_test_add_func("/ovn/switch_delete/idempotent", test_ovn_switch_delete_idempotent);
    g_test_add_func("/ovn/router_delete/idempotent", test_ovn_router_delete_idempotent);
    g_test_add_func("/ovn/valid_id/rejects_injection", test_ovn_valid_id_rejects_injection);
    g_test_add_func("/ovn/status/structure",       test_ovn_status_structure);
#if !PCV_CLUSTER_ENABLED
    g_test_add_func("/overlay/list/empty_single",  test_overlay_list_empty_single);
    g_test_add_func("/overlay/validation/disabled_and_inputs",
                    test_overlay_disabled_and_validation);
    g_test_add_func("/overlay/validation/disabled_residue",
                    test_overlay_disabled_reads_expose_metadata_residue);
    g_test_add_func("/overlay/validation/ipv4_cidr_only",
                    test_overlay_ipv4_cidr_only_is_fail_closed);
    g_test_add_func("/overlay/peer/injective_port_name",
                    test_overlay_peer_port_name_is_injective);
    g_test_add_func("/overlay/effects/create_ownership",
                    test_overlay_create_ownership_and_exact_contract);
    g_test_add_func("/overlay/effects/kernel_link_collision",
                    test_overlay_kernel_link_name_collision_is_preserved);
    g_test_add_func("/overlay/effects/peer_scope",
                    test_overlay_peer_scope_options_and_remove);
    g_test_add_func("/overlay/effects/peer_drift_port_identity",
                    test_overlay_peer_drift_and_port_identity_fail_closed);
    g_test_add_func("/overlay/effects/generation_owner_timeline",
                    test_overlay_generation_and_owner_timeline);
    g_test_add_func("/overlay/effects/generation_overflow",
                    test_overlay_generation_overflow_is_fail_closed);
    g_test_add_func("/overlay/effects/failure_transactions",
                    test_overlay_failure_transactions_and_foreign_child);
    g_test_add_func("/overlay/effects/applied_then_timeout",
                    test_overlay_applied_then_timeout_is_reconciled);
    g_test_add_func("/overlay/effects/metadata_add_rollback",
                    test_overlay_metadata_add_failure_rolls_back_actual);
    g_test_add_func("/overlay/effects/rollback_failure_degraded",
                    test_overlay_rollback_failure_is_persistently_degraded);
    g_test_add_func("/overlay/effects/marker_orphan_persistent_audit",
                    test_overlay_marker_orphan_survives_periodic_and_restart_audit);
    g_test_add_func("/overlay/effects/registry_metadata_bijection",
                    test_overlay_registry_requires_canonical_metadata_bijection);
    g_test_add_func("/overlay/restore/legacy_exact_migration",
                    test_overlay_legacy_exact_migration);
    g_test_add_func("/overlay/restore/legacy_applied_timeout",
                    test_overlay_legacy_applied_timeout_outcomes);
    g_test_add_func("/overlay/restore/legacy_namespace_races",
                    test_overlay_legacy_migration_namespace_races_fail_closed);
    g_test_add_func("/overlay/restore/legacy_mismatch",
                    test_overlay_legacy_mismatch_and_residue_fail_closed);
    g_test_add_func("/overlay/restore/legacy_foreign_markers",
                    test_overlay_legacy_foreign_markers_are_never_adopted);
    g_test_add_func("/overlay/restore/schema_fail_closed",
                    test_overlay_restore_rejects_partial_and_unknown_schema);
    g_test_add_func("/overlay/restore/tombstone_residue",
                    test_overlay_restore_exposes_delete_tombstone);
    g_test_add_func("/overlay/restore/stale_snapshot",
                    test_overlay_restore_rejects_stale_snapshot);
    g_test_add_func("/overlay/concurrency/restore_total_deadline",
                    test_overlay_restore_total_deadline_is_bounded);
    g_test_add_func("/overlay/concurrency/restore_cleanup_deadline",
                    test_overlay_restore_deadline_keeps_cleanup_budget);
    g_test_add_func("/overlay/restore/legacy_extra_state",
                    test_overlay_legacy_rejects_extra_ip_and_option);
    g_test_add_func("/overlay/restore/legacy_other_owner",
                    test_overlay_legacy_rejects_dpdk_and_other_owner_markers);
    g_test_add_func("/overlay/restore/legacy_rollback_failure",
                    test_overlay_legacy_rollback_failure_is_degraded);
    g_test_add_func("/overlay/restore/v2_closed_inventory",
                    test_overlay_v2_closed_owner_inventory);
    g_test_add_func("/overlay/restore/v2_unmarked_workload_child",
                    test_overlay_v2_restore_allows_unmarked_workload_child);
    g_test_add_func("/overlay/restore/abnormal_dentries",
                    test_overlay_metadata_abnormal_dentries_are_bounded);
    g_test_add_func("/overlay/restore/metadata_cardinality",
                    test_overlay_metadata_directory_cardinality_is_bounded);
    g_test_add_func("/overlay/restore/canonical_sidecar_no_mutation",
                    test_overlay_canonical_sidecar_blocks_restore_mutation);
    g_test_add_func("/overlay/restore/global_preflight_no_mutation",
                    test_overlay_global_preflight_precedes_all_actual_mutation);
    g_test_add_func("/overlay/effects/restore_error_global_gate",
                    test_overlay_restore_error_blocks_all_mutations);
    g_test_add_func("/overlay/effects/transaction_races",
                    test_overlay_transaction_races_preserve_foreign_state);
    g_test_add_func("/overlay/effects/committed_residue",
                    test_overlay_committed_metadata_residue_is_fail_closed);
    g_test_add_func("/overlay/effects/delete_cleanup_residue",
                    test_overlay_delete_cleanup_residue_is_fail_closed);
    g_test_add_func("/overlay/effects/directory_sync_boundaries",
                    test_overlay_directory_sync_commit_boundaries);
    g_test_add_func("/overlay/effects/metadata_directory_security",
                    test_overlay_metadata_directory_ownership_and_creation_sync);
    g_test_add_func("/overlay/effects/post_exclusive_stat_failure",
                    test_overlay_post_exclusive_stat_failure_is_degraded);
    g_test_add_func("/overlay/effects/metadata_size_boundary",
                    test_overlay_metadata_exact_size_boundary_precedes_actual);
    g_test_add_func("/overlay/effects/peer_limit",
                    test_overlay_peer_limit_rejects_before_actual_and_restore);
    g_test_add_func("/overlay/restore/bulk_max_cardinality",
                    test_overlay_bulk_restore_max_cardinality_is_constant);
    g_test_add_func("/overlay/restore/bulk_inverse_relationships",
                    test_overlay_bulk_inverse_relationships_fail_closed);
    g_test_add_func("/overlay/restore/multi_peer_rollback",
                    test_overlay_restore_rolls_back_multi_peer_actual);
    g_test_add_func("/overlay/restore/completed_delete_race",
                    test_overlay_restore_skips_completed_concurrent_delete);
    g_test_add_func("/overlay/concurrency/restore_add_epoch",
                    test_overlay_restore_discards_snapshot_stale_after_add);
    g_test_add_func("/overlay/concurrency/restore_preflight_admission",
                    test_overlay_restore_preflight_blocks_mutation);
    g_test_add_func("/overlay/concurrency/probe_lock_free_timeout",
                    test_overlay_probe_timeout_and_lock_free_snapshot);
    g_test_add_func("/overlay/concurrency/shutdown_drain",
                    test_overlay_shutdown_drains_restore_worker);
    g_test_add_func("/overlay/concurrency/public_shutdown_drain",
                    test_overlay_shutdown_drains_admitted_public_operation);
#endif
}
