   
                     
                                                              
  
                           
                                                   
                                                    
                                        
  
              
                                                                                                          
                                                                                                                      
                                                                                        
                                                                             
                                           
                                                      
  
                   
                                                                         
                                                   
  
          
                                                       
                                                
                                                         
                                                            
                                                  
                                                   
                                                      
  
                                                          
                                                                   
                                                                      
                                            
                                                                 
                                                      
                                                  
                  
                                                    
                                                                   
                                                        
                                                                       
                                                                   
                                                                         
  
                          
                                                          
                                             
                                          
                                                                   
                                                            
  
                          
                                                             
                                                    
                                                                    
                                               
                                                          
                                       
                                                  
                               
                                                            
                                                         
                                             
                                          
  
              
                                                                  
  
                                
                                             
                                            
                                                       
  
             
                                                                    
                                                                
                                                            
                                                           
   
  
                                        
                                           
  
                
                                                                  
                                                      
                                            
                               
   

                                                                       
  
           
                                                                    
                                                                   
                                                                    
                                                          
                                                                      
                                                     
  
                  
                                                                 
                                                        
                                                                          
                                                                              
#include "vm_manager.h"
#include "virt_conn_pool.h"
#include "utils/pcv_config.h"
#if PCV_CLUSTER_ENABLED
#include "modules/cluster/cluster_manager.h"
#endif
#include "../../utils/pcv_spawn.h"                              

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <libvirt-gobject/libvirt-gobject.h>
#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include "modules/virt/vm_manager_internal.h"

                        
                                                                              
G_DEFINE_TYPE(PureCVisorVmManager, purecvisor_vm_manager, G_TYPE_OBJECT)

                                                                             
                        
                                      
                                          
                                                         
                                                                                
typedef enum {
    SIGNAL_VM_STARTED = 0,                          
    SIGNAL_VM_STOPPED,                              
    SIGNAL_VM_METRICS_UPDATED,                              
    N_SIGNALS
} PcvVmManagerSignalId;

static guint signals[N_SIGNALS] = { 0 };

                                                                             
                                     
                                                                                


                                                                             
                      
                                              
                                         
                              
                                                                                

   
                                  
                                                         
                                           
  
                               
  
                                                            
                                                         
   
static void purecvisor_vm_manager_finalize(GObject *object) {
    G_OBJECT_CLASS(purecvisor_vm_manager_parent_class)->finalize(object);
}

   
                                    
                                            
                                              
  
                                                       
                                                      
                  
  
                                                        
                                                                      
   
static void purecvisor_vm_manager_class_init(PureCVisorVmManagerClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = purecvisor_vm_manager_finalize;

                                                                         
                    
                                                                            

       
                                       
                                   
                                       
      
                                                              
                         
       
    signals[SIGNAL_VM_STARTED] =
        g_signal_new(PCV_VM_SIGNAL_STARTED,
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_LAST,
                     0,                                               
                     NULL, NULL,                                     
                     NULL,                                            
                     G_TYPE_NONE,                         
                     1,                                
                     G_TYPE_STRING);                         

       
                                       
                                   
                                       
      
                                                             
                         
       
    signals[SIGNAL_VM_STOPPED] =
        g_signal_new(PCV_VM_SIGNAL_STOPPED,
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_LAST,
                     0,
                     NULL, NULL,
                     NULL,
                     G_TYPE_NONE,
                     1,
                     G_TYPE_STRING);                         

       
                                               
                                 
                                                            
      
                                   
                                                    
                                            
                                               
       
    signals[SIGNAL_VM_METRICS_UPDATED] =
        g_signal_new(PCV_VM_SIGNAL_METRICS_UPDATED,
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_LAST,
                     0,
                     NULL, NULL,
                     NULL,
                     G_TYPE_NONE,
                     1,
                     G_TYPE_POINTER);                                 
}

                         
                                                            
static void purecvisor_vm_manager_init(PureCVisorVmManager *self) {
    (void)self;
}

   
                             
                        
  
                                                        
                                
  
                                                           
   
PureCVisorVmManager *purecvisor_vm_manager_new(void) {
    return g_object_new(PURECVISOR_TYPE_VM_MANAGER, NULL);
}

   
                                        
                                    
                                                                               
                             
  
                                                   
                                                       
   
gchar *
purecvisor_vm_resolve_network_bridge(const gchar *requested)
{
    if (!requested || !*requested)
        return g_strdup(pcv_config_get_string("network", "default_bridge", "pcvnat0"));
    if (g_strcmp0(requested, "none") == 0)
        return NULL;
    return g_strdup(requested);
}

   
                                         
                                   
                                          
  
                                                            
                                                        
  
                              
   
gboolean purecvisor_vm_manager_start_vm_finish(PureCVisorVmManager *manager,
                                               GAsyncResult *res,
                                               GError **error) {
    gboolean ok = g_task_propagate_boolean(G_TASK(res), error);
    if (ok) {
                                      
                                                       
                                                           
                                    
        LifecycleTaskData *data = g_task_get_task_data(G_TASK(res));
        g_signal_emit(manager, signals[SIGNAL_VM_STARTED], 0, data->name);
    }
    return ok;
}

   
                                        
                                   
  
                                                  
                                                        
  
                              
   
gboolean purecvisor_vm_manager_stop_vm_finish(PureCVisorVmManager *manager,
                                              GAsyncResult *res,
                                              GError **error) {
    gboolean ok = g_task_propagate_boolean(G_TASK(res), error);
    if (ok) {
                                        
        LifecycleTaskData *data = g_task_get_task_data(G_TASK(res));
        g_signal_emit(manager, signals[SIGNAL_VM_STOPPED], 0, data->name);
    }
    return ok;
}

                                                                             
                                                   
  
                                                   
                                                    
                                                                                
   
                                              
                                         
  
                                                    
                                                       
  
            
                                                     
                                   
                                                
  
                         
                            
                                                         
                                                
  
                                                            
   
void
purecvisor_vm_manager_emit_metrics_updated(PureCVisorVmManager *self,
                                           GHashTable          *cache)
{
    g_return_if_fail(PURECVISOR_IS_VM_MANAGER(self));
    g_signal_emit(self, signals[SIGNAL_VM_METRICS_UPDATED], 0, cache);
}
