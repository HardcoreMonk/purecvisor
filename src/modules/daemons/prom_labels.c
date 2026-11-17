   
                      
                                                                                            
  
                           
                                                                                                   
                                                        
                                                                                                     
                                                                                   
                                                                                                                    
                    
                                       
  
                                                                         
                                               
   
#include "modules/daemons/prometheus_exporter.h"

#include <stdarg.h>
#include <string.h>

                                                                
static gboolean
_label_putc(gchar *buf, gsize cap, gsize *len, gchar c)
{
    if (*len + 1 >= cap) return FALSE;
    buf[(*len)++] = c;
    return TRUE;
}

                                                             
                                                                            
gboolean
pcv_prom_labels_build(gchar *buf, gsize cap, ...)
{
    if (!buf || cap == 0) return FALSE;
    gsize len = 0;
    gboolean ok = TRUE;
    va_list ap;
    va_start(ap, cap);
    for (const gchar *key = va_arg(ap, const gchar *); key && ok; key = va_arg(ap, const gchar *)) {
        const gchar *val = va_arg(ap, const gchar *);
        if (len > 0) ok = _label_putc(buf, cap, &len, ',');
        for (const gchar *k = key; ok && *k; k++) ok = _label_putc(buf, cap, &len, *k);
        ok = ok && _label_putc(buf, cap, &len, '=') && _label_putc(buf, cap, &len, '"');
        for (const gchar *v = val ? val : ""; ok && *v; v++) {
            if (*v == '\\' || *v == '"' || *v == '\n')
                ok = _label_putc(buf, cap, &len, '\\') && _label_putc(buf, cap, &len, *v == '\n' ? 'n' : *v);
            else
                ok = _label_putc(buf, cap, &len, *v);
        }
        ok = ok && _label_putc(buf, cap, &len, '"');
    }
    va_end(ap);
    buf[ok ? len : 0] = '\0';
    return ok;
}

                                                     
static gboolean
_is_vm_key(const gchar *k, gsize n)
{
    return (n == 2 && !strncmp(k, "vm", 2)) || (n == 5 && !strncmp(k, "vm_id", 5)) || (n == 7 && !strncmp(k, "vm_name", 7));
}

                                                                                    
gint
pcv_prom_labels_vm_value(const gchar *line, gchar *out, gsize cap)
{
    if (!line || !out || cap == 0) return -1;
    out[0] = '\0';
    const gchar *sp = strchr(line, ' ');
    const gchar *p = strchr(line, '{');
    if (!p || (sp && sp < p)) return 0;                
    p++;
    gint found = 0;
    while (*p && *p != '}') {
        const gchar *k = p;
        while (g_ascii_isalnum(*p) || *p == '_') p++;
        const gsize klen = (gsize)(p - k);
        if (klen == 0 || *p != '=' || p[1] != '"') return -1;
        p += 2;
        const gboolean want = !found && _is_vm_key(k, klen);
        gsize len = 0;
        for (; *p && *p != '"'; p++) {
            gchar c = *p;
            if (c == '\\') {
                p++;
                if (*p == 'n') c = '\n';
                else if (*p == '\\' || *p == '"') c = *p;
                else return -1;
            }
            if (want) {
                if (len + 1 >= cap) return -1;
                out[len++] = c;
            }
        }
        if (*p != '"') return -1;                  
        p++;
        if (want) {
            out[len] = '\0';
            found = 1;
        }
        if (*p == ',') p++;
        else if (*p != '}') return -1;
    }
    if (*p != '}') return -1;
    if (!found) out[0] = '\0';
    return found;
}
