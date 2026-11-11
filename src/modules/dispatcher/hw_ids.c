   
                 
                                                                    
  
                           
                                                                                                  
                                                                                                              
                                                                                                
                                                          
                                                                                          
                                                                                         
                                                                                
                                                                         
  
                                                                         
                                             
   
#include "hw_ids.h"

#include <string.h>

#define PCV_HW_IDS_MAX_NAME_BYTES 256U                                                     

struct PcvHwIds {
    GHashTable *vendors;                    
    GHashTable *devices;                         
    gboolean    loaded;                                                    
};

                                                                                  
static gchar *ids_root_path(const gchar *root, const gchar *relative)
{
    if (!root || root[0] == '\0' || g_strcmp0(root, "/") == 0)
        return g_strdup_printf("/%s", relative);
    return g_build_filename(root, relative, NULL);
}

                                                                                 
                                                                         
static gchar *ids_clean_name(const gchar *raw)
{
    gchar *valid = g_utf8_make_valid(raw, -1);
    g_strstrip(valid);
    for (gchar *p = valid; *p; p++) {
        if ((guchar)*p < 0x20)
            *p = ' ';
    }
    g_strstrip(valid);
    if (valid[0] == '\0') {
        g_free(valid);
        return NULL;
    }
    if (strlen(valid) > PCV_HW_IDS_MAX_NAME_BYTES) {
        gchar *cut = g_strndup(valid, PCV_HW_IDS_MAX_NAME_BYTES);
        while (!g_utf8_validate(cut, -1, NULL) && cut[0] != '\0')
            cut[strlen(cut) - 1] = '\0';
        g_free(valid);
        valid = cut;
    }
    return valid;
}

                                                         
static gboolean ids_is_hex4_prefix(const gchar *text)
{
    for (guint i = 0; i < 4; i++) {
        if (!g_ascii_isxdigit(text[i]))
            return FALSE;
    }
    return g_ascii_isspace(text[4]);
}

                                                   
static gboolean ids_key4(const gchar *s)
{
    return s && strlen(s) == 4;
}

                                                      
                                                                       
PcvHwIds *pcv_hw_ids_load(const gchar *root, PcvHwIdsKind kind)
{
    PcvHwIds *ids = g_new0(PcvHwIds, 1);
    ids->vendors = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    ids->devices = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

    const gchar *const pci[] = { "usr/share/misc/pci.ids", "usr/share/hwdata/pci.ids" };
    const gchar *const usb[] = { "usr/share/misc/usb.ids", "usr/share/hwdata/usb.ids" };
    const gchar *const *candidates = kind == PCV_HW_IDS_USB ? usb : pci;
    gchar *contents = NULL;
    gsize length = 0;
    for (guint i = 0; i < 2; i++) {
        gchar *path = ids_root_path(root, candidates[i]);
        gboolean ok = g_file_get_contents(path, &contents, &length, NULL);
        g_free(path);
        if (ok && length <= PCV_HW_IDS_MAX_BYTES)
            break;
        g_clear_pointer(&contents, g_free);
    }
    if (!contents)
        return ids;
    ids->loaded = TRUE;

    gchar **lines = g_strsplit(contents, "\n", -1);
    gchar *current_vendor = NULL;
    for (guint i = 0; lines[i] != NULL; i++) {
        const gchar *line = lines[i];
        if (kind == PCV_HW_IDS_USB && g_str_has_prefix(line, "C "))
            break;                                                                         
        if (line[0] != '\t' && ids_is_hex4_prefix(line)) {
            g_free(current_vendor);
            current_vendor = g_ascii_strdown(line, 4);
            gchar *name = ids_clean_name(line + 5);
            if (name)
                g_hash_table_replace(ids->vendors, g_strdup(current_vendor), name);
        } else if (line[0] == '\t' && line[1] != '\t'
                   && current_vendor && ids_is_hex4_prefix(line + 1)) {
            gchar *device_id = g_ascii_strdown(line + 1, 4);
            gchar *key = g_strdup_printf("%s:%s", current_vendor, device_id);
            gchar *name = ids_clean_name(line + 6);
            if (name)
                g_hash_table_replace(ids->devices, key, name);
            else
                g_free(key);
            g_free(device_id);
        }
    }
    g_free(current_vendor);
    g_strfreev(lines);
    g_free(contents);
    return ids;
}

                                      
const gchar *pcv_hw_ids_vendor(const PcvHwIds *ids, const gchar *vendor_hex4)
{
    if (!ids || !ids_key4(vendor_hex4))
        return NULL;
    return g_hash_table_lookup(ids->vendors, vendor_hex4);
}

                                            
const gchar *pcv_hw_ids_device(const PcvHwIds *ids, const gchar *vendor_hex4, const gchar *device_hex4)
{
    if (!ids || !ids_key4(vendor_hex4) || !ids_key4(device_hex4))
        return NULL;
    gchar key[10];
    g_snprintf(key, sizeof key, "%s:%s", vendor_hex4, device_hex4);
    return g_hash_table_lookup(ids->devices, key);
}

                                      
gboolean pcv_hw_ids_available(const PcvHwIds *ids)
{
    return ids != NULL && ids->loaded;
}

                                  
void pcv_hw_ids_free(PcvHwIds *ids)
{
    if (!ids)
        return;
    g_hash_table_unref(ids->vendors);
    g_hash_table_unref(ids->devices);
    g_free(ids);
}
