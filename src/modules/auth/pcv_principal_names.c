   
                              
                                                                                     
  
                           
                                                                                                                    
                                                                                   
                                                                                 
                                                                        
  
                                                                           
                              
   
#include "pcv_principal_names.h"

#include <string.h>

#include "api/grpc_server.h"                                       
#include "modules/audit/pcv_audit.h"                                                    

G_DEFINE_QUARK(pcv-principal-error-quark, pcv_principal_error)

                                                                          
static const gchar *const k_reserved[] = {
    PCV_GRPC_CALLER_SUBJECT,
    PCV_AUDIT_ACTOR_LOCAL,
    PCV_AUDIT_ACTOR_SYSTEM,
    PCV_PRINCIPAL_AI_AGENT,
    PCV_PRINCIPAL_AI_OPS,
    PCV_PRINCIPAL_UNKNOWN_ACTOR,
};

                                             
gboolean
pcv_principal_name_is_reserved(const gchar *name)
{
    if (!name)
        return FALSE;
    for (gsize i = 0; i < G_N_ELEMENTS(k_reserved); i++)
        if (strcmp(name, k_reserved[i]) == 0)
            return TRUE;
    return FALSE;
}

                                                                                                     
                           
static gboolean
_is_trim_char(gchar c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

                                                         
gchar *
pcv_principal_name_normalize(const gchar *name)
{
    if (!name)
        return NULL;
    const gchar *s = name;
    while (*s && _is_trim_char(*s))
        s++;
    const gchar *e = s + strlen(s);
    while (e > s && _is_trim_char(e[-1]))
        e--;
    return g_strndup(s, (gsize)(e - s));
}

                                                   
gboolean
pcv_principal_signup_name_conflict(const GError *err)
{
    if (!err)
        return FALSE;
    return err->domain == PCV_PRINCIPAL_ERROR || g_error_matches(err, G_IO_ERROR, G_IO_ERROR_EXISTS);
}

                                                 
gboolean
pcv_principal_tenant_lookup_applies(gint key_role)
{
    return key_role < 0;
}

                                                                                               
gboolean
pcv_rpc_method_name_is_valid(const gchar *name)
{
    if (!name || !g_ascii_isalnum(name[0]))
        return FALSE;
    gsize n = 1;
    for (const gchar *p = name + 1; *p; p++, n++) {
        if (n >= 127)
            return FALSE;
        if (!g_ascii_isalnum(*p) && *p != '.' && *p != '_' && *p != '-')
            return FALSE;
    }
    return TRUE;
}
