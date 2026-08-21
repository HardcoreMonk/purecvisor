   
                            
                                                        
  
                           
                                                                          
                                                                    
                                                                       
                                                                
  
                       
                                                  
                                                         
   
#ifndef PURECVISOR_PCV_SHARED_BRIDGE_H
#define PURECVISOR_PCV_SHARED_BRIDGE_H

#include <glib.h>

G_BEGIN_DECLS

gboolean pcv_shared_bridge_bpf_prepare(const gchar *store_dir, GError **error);
gboolean pcv_shared_bridge_bpf_is_prepared(void);
const gchar *pcv_shared_bridge_bpf_sha256(void);

gboolean pcv_shared_bridge_attach(const gchar *physical_if,
                                  const gchar *portal_if,
                                  const guint8 host_mac[6],
                                  guint32 mtu,
                                  guint32 generation,
                                  GError **error);

gboolean pcv_shared_bridge_detach(const gchar *physical_if,
                                  const gchar *portal_if,
                                  GError **error);

void pcv_shared_bridge_portal_names(const gchar *bridge_name,
                                    gchar bridge_end[16],
                                    gchar portal_end[16]);

G_END_DECLS
#endif
