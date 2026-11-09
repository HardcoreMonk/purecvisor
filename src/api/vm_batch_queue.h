   
                         
                                                    
  
                           
                                                          
                                                                                                    
                                                              
                                                                             
  
                                                              
                                                             
  
                                                             
                               
   
#ifndef PURECVISOR_VM_BATCH_QUEUE_H
#define PURECVISOR_VM_BATCH_QUEUE_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct PcvVmBatchQueue PcvVmBatchQueue;

                                                          
typedef struct PcvVmBatchSlots PcvVmBatchSlots;

                                                           
                                                        
typedef gboolean (*PcvVmBatchSubmitFn)(const gchar *vm, const gchar *actor, PcvVmBatchQueue *queue,
                                       gpointer user_data, gchar **reason);
                                                                            
typedef void (*PcvVmBatchRejectFn)(const gchar *vm, const gchar *actor, const gchar *reason, gpointer user_data);
                                             
typedef gboolean (*PcvVmBatchTerminatingFn)(void);

typedef struct {
    PcvVmBatchSubmitFn      submit;
    PcvVmBatchRejectFn      report;
    PcvVmBatchTerminatingFn terminating;
    gpointer                user_data;
    guint                   max_in_flight;                  
    PcvVmBatchSlots        *slots;                                       
    const gchar            *actor;                                                                                
} PcvVmBatchQueueOps;

   
                                                          
                                                             
   
void pcv_vm_batch_queue_run(const gchar *const *vms, guint n, const PcvVmBatchQueueOps *ops);

   
                                                                             
                          
                                                          
   
void pcv_vm_batch_queue_item_done(PcvVmBatchQueue *queue);

   
                                                                                                   
                                                    
   
PcvVmBatchSlots *pcv_vm_batch_slots_new(guint capacity);

   
                                                                                 
                                                    
   
void pcv_vm_batch_slots_free(PcvVmBatchSlots *slots);

   
                                          
                                 
   
guint pcv_vm_batch_slots_in_use(const PcvVmBatchSlots *slots);

   
                                                                                        
                                                                       
   
PcvVmBatchSlots *pcv_vm_batch_stop_slots(guint capacity);

G_END_DECLS

#endif                                  
