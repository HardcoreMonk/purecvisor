                                                                             
                                   
                                                                                                              
  
                           
                                                                                                
                                                                                                   
                                                   
  
                       
                                                                   
                                                                                
#include "modules/virt/vm_owner_meta.h"

#include <gio/gio.h>
#include <libvirt/virterror.h>
#include <stdlib.h>

#include "modules/virt/vm_owner.h"

                                                                         
static gchar *(*g_read_fn)(virDomainPtr dom, gboolean live);
static gint g_restores;

                                                      
gchar *
pcv_vm_owner_metadata_fragment(const gchar *owner)
{
    if (!owner || !*owner)
        return NULL;
    gchar *escaped = g_markup_escape_text(owner, -1);
    gchar *fragment = g_strdup_printf("<owner>%s</owner>", escaped);
    g_free(escaped);
    return fragment;
}

                                                                                                       
                                                                               
                                                                         
gchar *
pcv_vm_owner_metadata_read(virDomainPtr dom, gboolean live)
{
    if (!dom)
        return NULL;
    char *xml = virDomainGetXMLDesc(dom, live ? 0 : VIR_DOMAIN_XML_INACTIVE);
    if (!xml) {
        virResetLastError();
        return NULL;
    }
    gchar *owner = pcv_vm_owner_from_domain_xml(xml);
    free(xml);
    return owner;
}

                                                                
static gboolean
_write(virDomainPtr dom, const gchar *owner, unsigned int flags)
{
    gchar *fragment = pcv_vm_owner_metadata_fragment(owner);
    gboolean ok = virDomainSetMetadata(dom, VIR_DOMAIN_METADATA_ELEMENT, fragment, fragment ? "pcv" : NULL,
                                       PCV_VM_OWNER_NS_URI, flags) == 0;
    if (!ok)
        virResetLastError();
    g_free(fragment);
    return ok;
}

                                                             
static void
_restore(virDomainPtr dom, const gchar *previous, gboolean live)
{
    g_atomic_int_inc(&g_restores);
    (void)_write(dom, previous, VIR_DOMAIN_AFFECT_CONFIG);
    if (live)
        (void)_write(dom, previous, VIR_DOMAIN_AFFECT_LIVE);
}

                                                              
gboolean
pcv_vm_owner_metadata_set(virDomainPtr dom, const gchar *owner, const gchar *previous, gboolean *live_updated,
                          GError **error)
{
    if (live_updated)
        *live_updated = FALSE;
    if (!dom || !owner || !*owner) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "config");
        return FALSE;
    }
    const gboolean active = virDomainIsActive(dom) == 1;
    if (!_write(dom, owner, VIR_DOMAIN_AFFECT_CONFIG)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "config");
        return FALSE;
    }
    if (active && !_write(dom, owner, VIR_DOMAIN_AFFECT_LIVE)) {
        _restore(dom, previous, FALSE);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "live");
        return FALSE;
    }
    gchar *(*read_fn)(virDomainPtr, gboolean) = g_read_fn ? g_read_fn : pcv_vm_owner_metadata_read;
    gchar *config = read_fn(dom, FALSE);
    gchar *live = active ? read_fn(dom, TRUE) : NULL;
    const gboolean ok = g_strcmp0(config, owner) == 0 && (!active || g_strcmp0(live, owner) == 0);
    g_free(config);
    g_free(live);
    if (!ok) {
        _restore(dom, previous, active);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "verify");
        return FALSE;
    }
    if (live_updated)
        *live_updated = active;
    return TRUE;
}

                                                                                            
void pcv_vm_owner_meta_set_read_fn_for_test(gchar *(*fn)(virDomainPtr dom, gboolean live));
void
pcv_vm_owner_meta_set_read_fn_for_test(gchar *(*fn)(virDomainPtr dom, gboolean live))
{
    g_read_fn = fn;
}

                                                                             
guint pcv_vm_owner_meta_restores_for_test(void);
guint
pcv_vm_owner_meta_restores_for_test(void)
{
    return (guint)g_atomic_int_get(&g_restores);
}
