




















#include "storage_tier.h"
#include "utils/pcv_spawn.h"
#include "utils/pcv_log.h"
#include "utils/pcv_config.h"
#include <string.h>
#include <stdio.h>


#define TIER_LOG_DOM  "storage_tier"


#define MAX_TIERS     8









typedef struct {
    gchar          name[32];
    gchar          pool[64];
    PcvStorageTier type;
    gboolean       active;
} TierDef;


static struct {
    TierDef   tiers[MAX_TIERS];
    gint      count;
    GMutex    mu;
    gboolean  initialized;
} G = {0};











static gboolean
_run_cmd(const gchar *cmd, gchar **out, GError **error)
{
    gchar **parsed = NULL;
    GError *pe = NULL;
    if (!g_shell_parse_argv(cmd, NULL, &parsed, &pe)) {
        if (pe) { if (error) g_propagate_error(error, pe); else g_error_free(pe); }
        return FALSE;
    }
    gchar *std_err = NULL;
    gboolean ok = pcv_spawn_sync((const gchar * const *)parsed, out, &std_err, error);
    g_free(std_err);
    g_strfreev(parsed);
    return ok;
}
static gboolean
_run_shell(const gchar *cmd, gchar **out, GError **error)
{
    const gchar *argv[] = {"/bin/sh", "-c", cmd, NULL};
    gchar *std_err = NULL;
    gboolean ok = pcv_spawn_sync(argv, out, &std_err, error);
    g_free(std_err);
    return ok;
}










void
pcv_storage_tier_init(void)
{
    g_mutex_init(&G.mu);
    G.count = 0;


    g_strlcpy(G.tiers[0].name, "ssd", sizeof(G.tiers[0].name));
    g_strlcpy(G.tiers[0].pool, pcv_config_get_zvol_pool(), sizeof(G.tiers[0].pool));
    G.tiers[0].type = PCV_TIER_SSD;
    G.tiers[0].active = TRUE;
    G.count = 1;

    G.initialized = TRUE;
    PCV_LOG_INFO(TIER_LOG_DOM, "Storage tier initialized (default: ssd → %s)",
                 G.tiers[0].pool);
}


void
pcv_storage_tier_shutdown(void)
{
    g_mutex_clear(&G.mu);
    G.initialized = FALSE;
}












JsonArray *
pcv_storage_tier_list(void)
{
    JsonArray *arr = json_array_new();
    g_mutex_lock(&G.mu);
    for (gint i = 0; i < G.count; i++) {
        TierDef *t = &G.tiers[i];
        if (!t->active) continue;
        JsonObject *obj = json_object_new();
        json_object_set_string_member(obj, "name", t->name);
        json_object_set_string_member(obj, "pool", t->pool);
        json_object_set_string_member(obj, "type",
            t->type == PCV_TIER_NVME ? "nvme" :
            t->type == PCV_TIER_SSD  ? "ssd"  : "hdd");



        gchar *cmd = g_strdup_printf("zfs list -Hp -o used,avail %s 2>/dev/null", t->pool);
        gchar *out = NULL;
        if (_run_shell(cmd, &out, NULL) && out) {
            gint64 used = 0, avail = 0;
            sscanf(out, "%ld\t%ld", &used, &avail);
            json_object_set_int_member(obj, "used_bytes", used);
            json_object_set_int_member(obj, "avail_bytes", avail);
        }
        g_free(out);
        g_free(cmd);

        json_array_add_object_element(arr, obj);
    }
    g_mutex_unlock(&G.mu);
    return arr;
}








JsonObject *
pcv_storage_tier_info(const gchar *tier_name)
{
    if (!tier_name) return NULL;
    g_mutex_lock(&G.mu);
    for (gint i = 0; i < G.count; i++) {
        if (g_strcmp0(G.tiers[i].name, tier_name) == 0 && G.tiers[i].active) {
            TierDef *t = &G.tiers[i];
            JsonObject *obj = json_object_new();
            json_object_set_string_member(obj, "name", t->name);
            json_object_set_string_member(obj, "pool", t->pool);
            json_object_set_string_member(obj, "type",
                t->type == PCV_TIER_NVME ? "nvme" :
                t->type == PCV_TIER_SSD  ? "ssd"  : "hdd");
            g_mutex_unlock(&G.mu);
            return obj;
        }
    }
    g_mutex_unlock(&G.mu);
    return NULL;
}











gboolean
pcv_storage_tier_create(const gchar *name, const gchar *pool,
                         PcvStorageTier type, GError **error)
{
    if (!name || !pool) {
        g_set_error(error, g_quark_from_static_string("tier"), 1, "name and pool required");
        return FALSE;
    }
    g_mutex_lock(&G.mu);
    if (G.count >= MAX_TIERS) {
        g_mutex_unlock(&G.mu);
        g_set_error(error, g_quark_from_static_string("tier"), 2, "max tiers reached");
        return FALSE;
    }
    gint idx = G.count++;
    g_strlcpy(G.tiers[idx].name, name, sizeof(G.tiers[idx].name));
    g_strlcpy(G.tiers[idx].pool, pool, sizeof(G.tiers[idx].pool));
    G.tiers[idx].type = type;
    G.tiers[idx].active = TRUE;
    g_mutex_unlock(&G.mu);

    PCV_LOG_INFO(TIER_LOG_DOM, "Tier '%s' created (pool=%s, type=%d)", name, pool, type);
    return TRUE;
}

















const gchar *
pcv_storage_tier_auto_select(gint vcpu, gint ram_mb)
{

    if (vcpu >= 4 && ram_mb >= 8192) {
        g_mutex_lock(&G.mu);
        for (gint i = 0; i < G.count; i++) {
            if (G.tiers[i].type == PCV_TIER_NVME && G.tiers[i].active) {
                g_mutex_unlock(&G.mu);
                return G.tiers[i].pool;
            }
        }
        g_mutex_unlock(&G.mu);
    }

    return pcv_config_get_zvol_pool();
}




















gboolean
pcv_storage_tier_migrate(const gchar *vm_name, const gchar *target_tier,
                          GError **error)
{
    if (!vm_name || !target_tier) {
        g_set_error(error, g_quark_from_static_string("tier"), 1, "vm_name and target_tier required");
        return FALSE;
    }


    const gchar *target_pool = NULL;
    g_mutex_lock(&G.mu);
    for (gint i = 0; i < G.count; i++) {
        if (g_strcmp0(G.tiers[i].name, target_tier) == 0 && G.tiers[i].active) {
            target_pool = G.tiers[i].pool;
            break;
        }
    }
    g_mutex_unlock(&G.mu);

    if (!target_pool) {
        g_set_error(error, g_quark_from_static_string("tier"), 3, "tier '%s' not found", target_tier);
        return FALSE;
    }


    gchar *src = g_strdup_printf("%s/%s", pcv_config_get_zvol_pool(), vm_name);
    gchar *dst = g_strdup_printf("%s/%s", target_pool, vm_name);
    gchar *cmd = g_strdup_printf(
        "zfs snapshot %s@tier-migrate && "
        "zfs send %s@tier-migrate | zfs recv %s && "
        "zfs destroy %s@tier-migrate",
        src, src, dst, src);

    gboolean ok = _run_cmd(cmd, NULL, error);
    g_free(cmd);
    g_free(src);
    g_free(dst);

    if (ok)
        PCV_LOG_INFO(TIER_LOG_DOM, "VM '%s' migrated to tier '%s'", vm_name, target_tier);
    return ok;
}


















gboolean
pcv_storage_qos_set(const gchar *vm_name, gint64 read_bps, gint64 write_bps,
                     gint64 read_iops, gint64 write_iops, GError **error)
{
    if (!vm_name) {
        g_set_error(error, g_quark_from_static_string("qos"), 1, "vm_name required");
        return FALSE;
    }


    GString *cmd = g_string_new("");
    g_string_printf(cmd, "virsh blkiotune %s", vm_name);
    if (read_bps > 0)
        g_string_append_printf(cmd, " --device-read-bytes-sec %ld", (long)read_bps);
    if (write_bps > 0)
        g_string_append_printf(cmd, " --device-write-bytes-sec %ld", (long)write_bps);
    if (read_iops > 0)
        g_string_append_printf(cmd, " --device-read-iops-sec %ld", (long)read_iops);
    if (write_iops > 0)
        g_string_append_printf(cmd, " --device-write-iops-sec %ld", (long)write_iops);
    g_string_append(cmd, " --live 2>&1");

    gboolean ok = _run_cmd(cmd->str, NULL, error);
    g_string_free(cmd, TRUE);

    if (ok)
        PCV_LOG_INFO(TIER_LOG_DOM, "QoS set for VM '%s': rbps=%ld wbps=%ld riops=%ld wiops=%ld",
                     vm_name, (long)read_bps, (long)write_bps, (long)read_iops, (long)write_iops);
    return ok;
}









JsonObject *
pcv_storage_qos_get(const gchar *vm_name)
{
    if (!vm_name) return NULL;

    gchar *cmd = g_strdup_printf("virsh blkiotune %s 2>/dev/null", vm_name);
    gchar *out = NULL;
    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "vm_name", vm_name);

    if (_run_shell(cmd, &out, NULL) && out) {
        json_object_set_string_member(obj, "raw", out);
    }
    g_free(out);
    g_free(cmd);
    return obj;
}










gboolean
pcv_storage_qos_delete(const gchar *vm_name, GError **error)
{

    return pcv_storage_qos_set(vm_name, 0, 0, 0, 0, error);
}
