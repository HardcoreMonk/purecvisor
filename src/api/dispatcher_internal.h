   
                              
                                                  
  
                           
                                                                         
                                                                       
                                                     
  
                                                               
   
#ifndef PCV_DISPATCHER_INTERNAL_H
#define PCV_DISPATCHER_INTERNAL_H

#include "api/dispatcher.h"
#include "modules/virt/vm_manager.h"

                                                                                
                                                                           
struct _PureCVisorDispatcher {
    GObject parent_instance;
    PureCVisorVmManager *vm_manager;                         
};

                                                                       
                                                             
extern PureCVisorVmManager *pcv_disp_vm_manager;

#endif                                
