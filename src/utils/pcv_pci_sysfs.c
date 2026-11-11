                                                                             
                            
                                                                                  
  
                           
                                                                             
                                                                                                  
                                                                       
                                                                              
  
                                                                                      
                                                            
                                                                                
#include "pcv_pci_sysfs.h"

#include <string.h>

#include "utils/pcv_validate.h"

#define PCI_GROUP_ID_MAX_DIGITS 10U                                 

                                                                   
static gboolean pci_group_id_valid(const gchar *name)
{
    gsize len = strlen(name);
    if (len == 0 || len > PCI_GROUP_ID_MAX_DIGITS)
        return FALSE;
    for (gsize i = 0; i < len; i++) {
        if (!g_ascii_isdigit(name[i]))
            return FALSE;
    }
    return TRUE;
}

                                                                                                
static gchar *pci_sys_path(const gchar *root, const gchar *relative)
{
    if (!root || root[0] == '\0' || g_strcmp0(root, "/") == 0)
        return g_strdup_printf("/%s", relative);
    return g_build_filename(root, relative, NULL);
}

                                                                                 
gboolean pcv_pci_iommu_group_dir(const gchar *root, const gchar *bdf, gchar **group_dir_out, gchar **group_id_out,
                                 GError **error)
{
    if (group_dir_out)
        *group_dir_out = NULL;
    if (group_id_out)
        *group_id_out = NULL;
    if (!bdf || !pcv_validate_pci_addr(bdf)) {
        g_set_error_literal(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "invalid PCI address");
        return FALSE;
    }

    g_autofree gchar *rel = g_strdup_printf("sys/bus/pci/devices/%s/iommu_group", bdf);
    g_autofree gchar *link = pci_sys_path(root, rel);
    g_autofree gchar *target = g_file_read_link(link, NULL);
    if (!target)
        return TRUE;                                                              

    g_autofree gchar *id = g_path_get_basename(target);
    if (!pci_group_id_valid(id)) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "IOMMU group link of %s does not name a group number", bdf);
        return FALSE;
    }
    if (group_dir_out) {
        g_autofree gchar *group_rel = g_strdup_printf("sys/kernel/iommu_groups/%s", id);
        *group_dir_out = pci_sys_path(root, group_rel);
    }
    if (group_id_out)
        *group_id_out = g_steal_pointer(&id);
    return TRUE;
}
