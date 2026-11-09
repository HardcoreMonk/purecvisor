   
                                                     
  
                           
                                                   
                                                    
                                        
  
                                                       
                                                    
                                               
  
                                                            
                                                           
                                                                   
                                                
                                                                
                               
                                                    
                                              
                                                             
   
#ifndef PCV_BPF_SHARED_H                                  
#define PCV_BPF_SHARED_H

                                                     
#define PCV_LSM_PATH_MAX   256
                                                                    
#define PCV_LSM_COMM_MAX    16

                                                               
                                                               
                                                            
                           
                                                    
                                                    

                                                           
                                                         
                                                        
enum pcv_lsm_hook {
    PCV_LSM_HOOK_BPRM = 1,                                         
    PCV_LSM_HOOK_FILE_OPEN = 2,                           
};

                                
                                                       
                                                          
                                                                 
                                                    
                                                                         
                                                                   
                                                            
                                                           
                                                             
                                               
struct pcv_lsm_event {
    unsigned long long ktime_ns;                                                     
    unsigned int  pid;                                                         
    unsigned long long cgroup_id;                                               
    unsigned int  hook;                                        
    char comm[PCV_LSM_COMM_MAX];                                   
    char path[PCV_LSM_PATH_MAX];                   
    unsigned int  s_dev;                                                                    
                                                                                    
};

                                                                  
                                                            
_Static_assert(sizeof(struct pcv_lsm_event) == 304,
               "pcv_lsm_event size must stay 304 (s_dev lives in tail padding)");
_Static_assert(__builtin_offsetof(struct pcv_lsm_event, path) == 44,
               "pcv_lsm_event.path offset must stay 44 (old/new records share 300 bytes)");
_Static_assert(__builtin_offsetof(struct pcv_lsm_event, s_dev) == 300,
               "pcv_lsm_event.s_dev must sit after path at offset 300 (tail padding)");

#endif                       
