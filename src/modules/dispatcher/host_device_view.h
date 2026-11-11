   
                           
                                                                                              
  
                           
                                                                                                       
                                                                                     
                                                                                             
           
                                                                                                   
                                                                                       
                                                                                              
                                             
                                                                                   
                                                                 
                                                                                
                                                                                          
                                                                              
  
                                                                                 
                                    
  
            
                                                                                                                                  
   
#ifndef PURECVISOR_DISPATCHER_HOST_DEVICE_VIEW_H
#define PURECVISOR_DISPATCHER_HOST_DEVICE_VIEW_H

#include <glib.h>
#include <json-glib/json-glib.h>

#include "modules/dispatcher/host_device_scan.h"
#include "modules/resource/resource_domain_refs.h"
#include "modules/resource/resource_registry.h"

G_BEGIN_DECLS

                                                                                     
#define PCV_HOST_DEVICE_MAX 512

                                                                                                            
                                                                                               
typedef struct {
    gchar   *bdf;
    gboolean live;
    gboolean persistent;
    gchar   *managed;
} PcvPciHostdevEntry;

   
                                                                                                    
                                                                                                        
                                                                                       
                                                                                 
                                       
                                                                       
   
gboolean pcv_pci_hostdev_list(const gchar *live_xml, const gchar *config_xml, GPtrArray **out, GError **error);

                                                                                       
               
typedef enum {
    PCV_HOST_READ_USE,
    PCV_HOST_READ_SKIP,
    PCV_HOST_READ_INACTIVE_ONLY,
    PCV_HOST_READ_FAIL,
} PcvHostReadOutcome;

   
                                                                                                    
                                                                                                            
                                                                                                    
                                                                                                                  
                                                
                                                                   
   
PcvHostReadOutcome pcv_host_device_read_outcome(gboolean xml_ok, gboolean err_is_no_domain, PcvResDomState active_state,
                                                gboolean is_live_read);

   
                                                                                                 
                                                                              
                                     
   
typedef struct {
    gchar   *name;
    gchar   *owner;
    gboolean active;
    gboolean persistent;
    gchar   *live_xml;
    gchar   *config_xml;
} PcvHostVmDef;

                                                                                               
void pcv_host_vm_def_free(gpointer def);

                                                                                                          
                                              
typedef gboolean (*PcvHostVisibleFn)(gpointer ctx, PcvResKind kind, const gchar *id);

                                                                                                          
                                                                    
typedef struct {
    gint             role;
    const gchar     *subject;
    PcvHostVisibleFn visible;
    gpointer         ctx;
} PcvHostViewCaller;

   
                                                                                                          
                                                                                                  
                                           
   
gchar *pcv_host_device_read_error_message(const PcvHostViewCaller *caller, const gchar *vm_name);

   
                                                                                                              
                                                                                     
                                                                                             
                                                                                                    
                                                             
   
JsonObject *pcv_host_device_view_build(const PcvHostScan *scan, const GPtrArray *vms, const PcvHostViewCaller *caller,
                                       guint max_devices, GError **error);

G_END_DECLS

#endif                                               
