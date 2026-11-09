   
                   
                                                      
  
                           
                                                   
                                                    
                                        
  
                                                    
                                                   
                                                
                                                     
                                                      
  
                                                                       
         
                                                                       
                                                  
                                                           
                                           
  
                                                                       
           
                                                                       
                                                             
                                                         
                                                      
  
           
                                               
                                                               
                                                 
                                       
  
                                                                       
         
                                                                       
                       
                                                                          
                 
                                        
                                                                           
                                                  
                     
                
      
                                 
                                                             
  
                                                                       
                             
                                                                       
                                                  
                                                     
                                   
                                         
                                               
                                                               
  
                                                                       
        
                                                                       
                                                        
                                                          
                                      
                                           
  
                                                                       
         
                                                                       
                                                                             
                                                       
                                                                                  
                                               
   
                              
  
                            
   

#ifndef PURECVISOR_VM_STATE_H
#define PURECVISOR_VM_STATE_H

#include <glib.h>

G_BEGIN_DECLS

                                                          

   
                                           
  
                                               
                                               
                                       
  
                    
                                            
                                                                        
                                                                           
                                                                          
                                                                       
                                                                           
                                                                     
                                                                              
  
                      
                                                     
                                       
  
                      
                                                    
                                   
                           
                               
   
typedef enum {
    VM_OP_NONE      = 0,                              
    VM_OP_STARTING  = 1,                            
    VM_OP_STOPPING  = 2,                                     
    VM_OP_DELETING  = 3,                                       
    VM_OP_CREATING  = 4,                                         
    VM_OP_TUNING    = 5,                                        
    VM_OP_SNAPSHOT  = 6,                           
    VM_OP_MIGRATING = 7,                                   
} VmPendingOp;

                                                             

   
                                                           
                                              
  
                                                    
                                         
  
          
                  
                                         
                                               
                                                   
                                         
                                     
  
                                                                      
   
void init_pending_state_machine(void);

   
                                             
  
                                                                 
                                                             
                         
  
                                                                                 
                                          
                                                    
                                                                  
                                                          
   
typedef struct _PcvVmLock PcvVmLock;

   
                                                        
                                                       
                                                               
                                                           
                                                                                         
   
[[nodiscard]] PcvVmLock *pcv_vm_lock_acquire(const gchar *vm_id, gint op_type, gchar **err_msg);

   
                                                                           
                                                                
   
void pcv_vm_lock_release(PcvVmLock *lock);

   
                                                                            
                                                              
   
gboolean pcv_vm_lock_renew(const PcvVmLock *lock);

   
                                       
                                        
   
const gchar *pcv_vm_lock_key(const PcvVmLock *lock);

                                                                        
typedef enum { PCV_VM_KEY_RESOLVED, PCV_VM_KEY_NOT_FOUND, PCV_VM_KEY_UNAVAILABLE } PcvVmKeyResolve;

                                                                             
typedef PcvVmKeyResolve (*PcvVmLockKeyResolver)(const gchar *vm_id, gchar **name_out);

   
                                                                  
                                                    
   
void pcv_vm_state_set_key_resolver(PcvVmLockKeyResolver resolver);

   
                                                                         
                                                                        
                                        
   
PcvVmKeyResolve pcv_vm_lock_key_resolve_libvirt(const gchar *uuid, gchar **name_out);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(PcvVmLock, pcv_vm_lock_release)

   
                                       
  
                                                   
  
          
                                                   
                                                                   
  
                                                         
   
void shutdown_pending_state_machine(void);

   
                                                  
  
                                                 
                                     
  
                                            
                     
  
                    
   
gint pcv_vm_state_get_lock_count(void);

   
                        
  
                                               
                                         
  
                                                            
                                                    
  
                   
   
gint pcv_vm_state_cleanup_expired(void);

   
                                                                       
  
                                                     
                                                     
                                            
  
                                       
                                                          
                                                                                 
                                                              
                
  
                                                                              
                                                             
  
                                              
                                                                     
   
gboolean pcv_vm_state_start_in_progress(const gchar *vm_id);

                                                     

   
                                                             
  
                                                  
                                                
  
                                              
                                               
   
typedef enum {
    PCV_VM_UNKNOWN   = 0,                              
    PCV_VM_RUNNING   = 1,                                  
    PCV_VM_STOPPED   = 2,                                 
    PCV_VM_PAUSED    = 3,                                               
    PCV_VM_MIGRATING = 4,                    
    PCV_VM_ERROR     = 5,              
} PcvVmRuntimeState;

   
                                         
  
                                                      
  
                       
                            
   
PcvVmRuntimeState pcv_vm_state_get_runtime(const gchar *vm_name);

   
                                                          
  
                                                         
                                                                
  
                                                                    
                                                                             
                                                   
                                                                                  
                                               
  
                       
                            
   
PcvVmRuntimeState pcv_vm_state_get_runtime_nowait(const gchar *vm_name);

   
                                       
  
                                                           
  
                         
                                   
   
const gchar *pcv_vm_state_runtime_str(PcvVmRuntimeState state);

   
                                                                            
  
                                                            
                                                        
                                                                      
  
                                                                
                                                                       
                                                                   
  
                                                                                         
                                                             
                                                                          
                                                                 
   
gboolean pcv_vm_death_should_release_cores(PcvVmRuntimeState now, gboolean start_in_progress);

G_END_DECLS

#endif                            
