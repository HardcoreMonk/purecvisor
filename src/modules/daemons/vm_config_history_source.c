#include "modules/daemons/vm_config_history.h"

#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>
#include <string.h>

#include "api/dispatcher_policy.h"
#include "modules/audit/pcv_audit.h"
#include "modules/daemons/vm_config_history_nudge.h"
#include "modules/resource/resource_gate.h"
#include "modules/virt/virt_conn_pool.h"

static GPtrArray *
_lv_list(gpointer data, GError **err)
{
    (void)data;
    virConnectPtr c = virt_conn_pool_acquire();
    if (!c) {
        g_set_error(err, G_IO_ERROR, G_IO_ERROR_FAILED, "libvirt connection unavailable");
        return NULL;
    }
    virDomainPtr *doms = NULL;
    int n = virConnectListAllDomains(c, &doms, 0);
    GPtrArray *out = n >= 0 ? g_ptr_array_new_with_free_func(g_free) : NULL;
    for (int i = 0; i < n; i++) {
        char uuid[VIR_UUID_STRING_BUFLEN];
        if (virDomainGetUUIDString(doms[i], uuid) == 0)
            g_ptr_array_add(out, g_ascii_strdown(uuid, -1));
        virDomainFree(doms[i]);
    }
    free(doms);
    if (!out)
        g_set_error(err, G_IO_ERROR, G_IO_ERROR_FAILED, "domain list failed");
    virt_conn_pool_release(c);
    return out;
}

static gboolean
_lv_read(gpointer data, const gchar *uuid, PcvVmCfgRaw *out, gboolean *exists, GError **err)
{
    (void)data;
    virConnectPtr c = virt_conn_pool_acquire();
    if (!c) {
        g_set_error(err, G_IO_ERROR, G_IO_ERROR_FAILED, "libvirt connection unavailable");
        return FALSE;
    }
    virDomainPtr dom = virDomainLookupByUUIDString(c, uuid);
    gboolean ok = TRUE;
    if (!dom) {
        virErrorPtr ve = virGetLastError();
        *exists = FALSE;
        ok = ve && ve->code == VIR_ERR_NO_DOMAIN;
        virResetLastError();
    } else {
        *exists = TRUE;
        out->persistent = virDomainIsPersistent(dom) == 1;
        gboolean active = virDomainIsActive(dom) == 1;
        out->domain_id = active ? (gint64)virDomainGetID(dom) : -1;
        char *x = virDomainGetXMLDesc(dom, VIR_DOMAIN_XML_INACTIVE);
        out->inactive_xml = x ? g_strdup(x) : NULL;
        free(x);
        if (active) {
            x = virDomainGetXMLDesc(dom, 0);
            out->live_xml = x ? g_strdup(x) : NULL;
            free(x);
        }
        int autostart = 0;
        out->autostart = virDomainGetAutostart(dom, &autostart) == 0 && autostart;
        const char *nm = virDomainGetName(dom);
        out->name = g_strdup(nm ? nm : "");
        ok = out->inactive_xml != NULL;
        virDomainFree(dom);
    }
    if (!ok)
        g_set_error(err, G_IO_ERROR, G_IO_ERROR_FAILED, "domain read failed");
    virt_conn_pool_release(c);
    return ok;
}

static gchar *
_lv_resolve(gpointer data, const gchar *name_or_uuid)
{
    (void)data;
    if (!name_or_uuid || !*name_or_uuid)
        return NULL;
    if (strlen(name_or_uuid) == 36 && name_or_uuid[8] == '-' && name_or_uuid[13] == '-')
        return g_ascii_strdown(name_or_uuid, -1);
    virConnectPtr c = virt_conn_pool_acquire();
    if (!c)
        return NULL;
    gchar *out = NULL;
    virDomainPtr dom = virDomainLookupByName(c, name_or_uuid);
    if (dom) {
        char uuid[VIR_UUID_STRING_BUFLEN];
        if (virDomainGetUUIDString(dom, uuid) == 0)
            out = g_ascii_strdown(uuid, -1);
        virDomainFree(dom);
    } else {
        virResetLastError();
    }
    virt_conn_pool_release(c);
    return out;
}

static gchar *
_lv_owner_tenant(gpointer data, const gchar *owner)
{
    (void)data;
    const gchar *t = pcv_res_owner_tenant(owner);
    return (t && *t) ? g_strdup(t) : NULL;
}

static const PcvVmCfgHistSource k_libvirt_source = {
    .list_domains = _lv_list, .read = _lv_read, .resolve = _lv_resolve, .owner_tenant = _lv_owner_tenant};

void
pcv_vm_cfg_hist_start_default(void)
{
    PcvVmCfgHistConfig c;
    pcv_vm_cfg_hist_config_load(&c);

    pcv_dispatcher_register_authorized_hook(pcv_vm_cfg_nudge_authorized_hook, NULL);
    pcv_audit_set_completion_tap(pcv_vm_cfg_nudge_audit_tap);
    pcv_vm_cfg_hist_start_with(&c, &k_libvirt_source, NULL);
    pcv_vm_cfg_hist_config_clear(&c);
}
