
#include "vm_template.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <errno.h>

#define TEMPLATE_DIR "/etc/purecvisor/templates"

static gchar *
_template_path(const gchar *name)
{
    return g_strdup_printf("%s/%s.json", TEMPLATE_DIR, name);
}

static JsonObject *
_template_to_json(const PcvVmTemplate *t)
{
    JsonObject *obj = json_object_new();

    json_object_set_string_member(obj, "name", t->name ? t->name : "");
    json_object_set_int_member(obj, "vcpu", t->vcpu);
    json_object_set_int_member(obj, "memory_mb", t->memory_mb);
    json_object_set_int_member(obj, "disk_gb", t->disk_gb);
    json_object_set_string_member(obj, "os_variant",
                                  t->os_variant ? t->os_variant : "");

    if (t->iso_path)
        json_object_set_string_member(obj, "iso_path", t->iso_path);
    if (t->network_bridge)
        json_object_set_string_member(obj, "network_bridge", t->network_bridge);
    if (t->cloud_init_user_data)
        json_object_set_string_member(obj, "cloud_init_user_data",
                                      t->cloud_init_user_data);
    if (t->description)
        json_object_set_string_member(obj, "description", t->description);

    return obj;
}

static PcvVmTemplate *
_parse_template_file(const gchar *path)
{
    JsonParser *parser = json_parser_new();
    GError *err = NULL;

    if (!json_parser_load_from_file(parser, path, &err)) {
        g_printerr("[template] Failed to parse %s: %s\n",
                    path, err->message);
        g_error_free(err);
        g_object_unref(parser);
        return NULL;
    }

    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_printerr("[template] Invalid JSON in %s\n", path);
        g_object_unref(parser);
        return NULL;
    }

    JsonObject *obj = json_node_get_object(root);

    PcvVmTemplate *t = g_new0(PcvVmTemplate, 1);

    if (json_object_has_member(obj, "name"))
        t->name = g_strdup(json_object_get_string_member(obj, "name"));
    if (json_object_has_member(obj, "vcpu"))
        t->vcpu = (gint)json_object_get_int_member(obj, "vcpu");
    if (json_object_has_member(obj, "memory_mb"))
        t->memory_mb = (gint)json_object_get_int_member(obj, "memory_mb");
    if (json_object_has_member(obj, "disk_gb"))
        t->disk_gb = (gint)json_object_get_int_member(obj, "disk_gb");
    if (json_object_has_member(obj, "os_variant"))
        t->os_variant = g_strdup(json_object_get_string_member(obj, "os_variant"));
    if (json_object_has_member(obj, "iso_path"))
        t->iso_path = g_strdup(json_object_get_string_member(obj, "iso_path"));
    if (json_object_has_member(obj, "network_bridge"))
        t->network_bridge = g_strdup(json_object_get_string_member(obj, "network_bridge"));
    if (json_object_has_member(obj, "cloud_init_user_data"))
        t->cloud_init_user_data = g_strdup(json_object_get_string_member(obj, "cloud_init_user_data"));
    if (json_object_has_member(obj, "description"))
        t->description = g_strdup(json_object_get_string_member(obj, "description"));

    g_object_unref(parser);
    return t;
}

static gchar *
_json_object_to_string(JsonObject *obj)
{
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(node, obj);

    JsonGenerator *gen = json_generator_new();
    json_generator_set_pretty(gen, TRUE);
    json_generator_set_indent(gen, 4);
    json_generator_set_root(gen, node);

    gchar *data = json_generator_to_data(gen, NULL);

    g_object_unref(gen);
    json_node_free(node);

    return data;
}

typedef struct {
    const gchar *name;
    gint         vcpu;
    gint         memory_mb;
    gint         disk_gb;
    const gchar *os_variant;
    const gchar *iso_path;
    const gchar *network_bridge;
    const gchar *description;
} PresetDef;

static const PresetDef builtin_presets[] = {
    {
        "ubuntu-small", 2, 2048, 20,
        "ubuntu24.04",
        "/pcvpool/iso/ubuntu-24.04-server-cloudimg-amd64.img",
        NULL,
        "Ubuntu 24.04 Small Instance (2 vCPU, 2 GB, 20 GB)"
    },
    {
        "ubuntu-medium", 4, 4096, 40,
        "ubuntu24.04",
        "/pcvpool/iso/ubuntu-24.04-server-cloudimg-amd64.img",
        NULL,
        "Ubuntu 24.04 Medium Instance (4 vCPU, 4 GB, 40 GB)"
    },
    {
        "ubuntu-large", 8, 8192, 80,
        "ubuntu24.04",
        "/pcvpool/iso/ubuntu-24.04-server-cloudimg-amd64.img",
        NULL,
        "Ubuntu 24.04 Large Instance (8 vCPU, 8 GB, 80 GB)"
    },
};

#define N_PRESETS (sizeof(builtin_presets) / sizeof(builtin_presets[0]))

void
pcv_vm_template_init(void)
{

    if (g_mkdir_with_parents(TEMPLATE_DIR, 0755) != 0) {
        g_printerr("[template] Failed to create %s: %s\n",
                    TEMPLATE_DIR, g_strerror(errno));
        return;
    }

    for (gsize i = 0; i < N_PRESETS; i++) {
        const PresetDef *p = &builtin_presets[i];
        gchar *path = _template_path(p->name);

        if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
            PcvVmTemplate tmpl = {
                .name            = (gchar *)p->name,
                .vcpu            = p->vcpu,
                .memory_mb       = p->memory_mb,
                .disk_gb         = p->disk_gb,
                .os_variant      = (gchar *)p->os_variant,
                .iso_path        = (gchar *)p->iso_path,
                .network_bridge  = (gchar *)p->network_bridge,
                .cloud_init_user_data = NULL,
                .description     = (gchar *)p->description,
            };
            GError *err = NULL;
            if (!pcv_vm_template_create(&tmpl, &err)) {
                g_printerr("[template] Preset '%s' creation failed: %s\n",
                            p->name, err ? err->message : "unknown");
                if (err) g_error_free(err);
            } else {
                g_print("[template] Created built-in preset: %s\n", p->name);
            }
        }
        g_free(path);
    }

    g_print("[template] Initialized (%s)\n", TEMPLATE_DIR);
}

void
pcv_vm_template_shutdown(void)
{

}

gboolean
pcv_vm_template_create(PcvVmTemplate *tmpl, GError **error)
{
    g_return_val_if_fail(tmpl != NULL, FALSE);
    g_return_val_if_fail(tmpl->name != NULL && tmpl->name[0] != '\0', FALSE);

    if (tmpl->iso_path && (strstr(tmpl->iso_path, "..") || tmpl->iso_path[0] != '/')) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                    "iso_path must be absolute and must not contain '..': %s",
                    tmpl->iso_path);
        return FALSE;
    }

    gchar *existing = _template_path(tmpl->name);
    if (g_file_test(existing, G_FILE_TEST_EXISTS)) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_EXIST,
                    "Template already exists: %s", tmpl->name);
        g_free(existing);
        return FALSE;
    }
    g_free(existing);

    if (g_mkdir_with_parents(TEMPLATE_DIR, 0755) != 0) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                    "Cannot create directory %s: %s",
                    TEMPLATE_DIR, g_strerror(errno));
        return FALSE;
    }

    JsonObject *obj = _template_to_json(tmpl);
    gchar *data = _json_object_to_string(obj);
    json_object_unref(obj);

    gchar *path = _template_path(tmpl->name);
    gboolean ok = g_file_set_contents(path, data, -1, error);

    if (ok)
        g_print("[template] Saved: %s\n", path);

    g_free(path);
    g_free(data);
    return ok;
}

gboolean
pcv_vm_template_delete(const gchar *name, GError **error)
{
    g_return_val_if_fail(name != NULL && name[0] != '\0', FALSE);

    gchar *path = _template_path(name);

    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
        g_free(path);
        return TRUE;
    }

    if (g_unlink(path) != 0) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                    "Failed to delete %s: %s", path, g_strerror(errno));
        g_free(path);
        return FALSE;
    }

    g_print("[template] Deleted: %s\n", path);
    g_free(path);
    return TRUE;
}

PcvVmTemplate *
pcv_vm_template_get(const gchar *name)
{
    g_return_val_if_fail(name != NULL, NULL);

    gchar *path = _template_path(name);
    PcvVmTemplate *t = NULL;

    if (g_file_test(path, G_FILE_TEST_EXISTS))
        t = _parse_template_file(path);

    g_free(path);
    return t;
}

GPtrArray *
pcv_vm_template_list(void)
{
    GPtrArray *arr = g_ptr_array_new_with_free_func(
                         (GDestroyNotify)pcv_vm_template_free);

    GDir *dir = g_dir_open(TEMPLATE_DIR, 0, NULL);
    if (!dir)
        return arr;

    const gchar *entry;
    while ((entry = g_dir_read_name(dir)) != NULL) {
        if (!g_str_has_suffix(entry, ".json"))
            continue;

        gchar *path = g_build_filename(TEMPLATE_DIR, entry, NULL);
        PcvVmTemplate *t = _parse_template_file(path);
        g_free(path);

        if (t)
            g_ptr_array_add(arr, t);
    }

    g_dir_close(dir);
    return arr;
}

void
pcv_vm_template_free(PcvVmTemplate *t)
{
    if (!t) return;
    g_free(t->name);
    g_free(t->os_variant);
    g_free(t->iso_path);
    g_free(t->network_bridge);
    g_free(t->cloud_init_user_data);
    g_free(t->description);
    g_free(t);
}
