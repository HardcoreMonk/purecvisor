








#ifndef PCV_OVA_IMPORT_XML_H
#define PCV_OVA_IMPORT_XML_H

#include <glib.h>

G_BEGIN_DECLS











gchar *pcv_ova_import_normalize_domain_xml(const gchar *domain_xml,
                                            const gchar *disk_path,
                                            gboolean block_device,
                                            const gchar *format,
                                            GError **error);

G_END_DECLS

#endif
