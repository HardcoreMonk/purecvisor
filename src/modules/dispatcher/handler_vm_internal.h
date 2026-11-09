   
                              
                                                                                                  
  
                           
                                                                                 
                                                              
                                                                               
                                                                                    
  
                                                             
   
#ifndef PCV_HANDLER_VM_INTERNAL_H
#define PCV_HANDLER_VM_INTERNAL_H

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <libvirt/libvirt.h>
#include <libxml/tree.h>
#include "api/uds_server.h"
#include "modules/core/vm_state.h"

                                                          
virDomainPtr pure_virt_get_domain(virConnectPtr conn, const gchar *identifier);

                                                                    
              
  
                                             
                                                  
  
                                                          
                                                       
  
               
                                                                  
                                                                         
                                                            
                                                                       
typedef struct {
    gchar *vm_id;                                                           
    gchar *action;                                                             
    gint cpu_quota;                                                                    
    gint mem_quota_mb;                                            
    gchar *rpc_id;                                         
    UdsServer *server;                                       
    GSocketConnection *connection;                                  
                                                  
    gint out_cpu_pct;                                   
    gint out_mem_pct;                                   
    gint out_vcpu;                        
    gint64 out_memory_mb;                     
    gint64 out_disk_rd;                       
    gint64 out_disk_wr;                       
    gint64 out_net_rx;                         
    gint64 out_net_tx;                         
    gint64 out_disk_rd_req;                       
    gint64 out_disk_wr_req;                        
    gint64 out_net_rx_pkts;                           
    gint64 out_net_tx_pkts;                           
                                
    gint page_offset;                                      
    gint page_limit;                                       
                                                      
                                                         
    PcvVmLock *lock;
} VmLifecycleCtx;

                                                               
void pcv_disp_free_lifecycle_ctx(gpointer data);
const gchar *
pcv_disp_guest_get_vm_name(JsonObject *params);
void
pcv_disp_send_rpc_error(UdsServer *server, GSocketConnection *connection,
                const gchar *rpc_id, gint code, const gchar *message);
xmlNodePtr
pcv_disp_xml_direct_child(xmlNodePtr parent, const gchar *name);

#endif                                
