
#include "vm_iface.h"
#include <string.h>
#include "../../utils/pcv_spawn.h"

#define VM_IFACE_SPAWN_TIMEOUT_SEC 30

GPtrArray *
pcv_vm_iface_parse_domiflist(const gchar *out)
{
    GPtrArray *arr = g_ptr_array_new_with_free_func(g_free);
    if (!out) return arr;
    gchar **lines = g_strsplit(out, "\n", -1);
    for (gchar **l = lines; *l; l++) {
        gchar *trimmed = g_strstrip(*l);
        if (g_str_has_prefix(trimmed, "vnet") || g_str_has_prefix(trimmed, "tap")) {
            gchar *space = strchr(trimmed, ' ');
            g_ptr_array_add(arr, space ? g_strndup(trimmed, (gsize)(space - trimmed))
                                       : g_strdup(trimmed));
        }
    }
    g_strfreev(lines);
    return arr;
}

GPtrArray *
pcv_vm_iface_list(const gchar *vm_name)
{
    const gchar *argv[] = {"virsh", "domiflist", vm_name, NULL};
    gchar *out = NULL;
    if (!pcv_spawn_sync_timeout(argv, &out, NULL, VM_IFACE_SPAWN_TIMEOUT_SEC, NULL)) {
        g_free(out);
        return g_ptr_array_new_with_free_func(g_free);
    }
    GPtrArray *arr = pcv_vm_iface_parse_domiflist(out);
    g_free(out);
    return arr;
}
