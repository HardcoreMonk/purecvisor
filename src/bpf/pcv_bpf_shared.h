   
                                                     
  
                           
                                                   
                                                    
                                        
  
                                                       
                                                    
                                               
  
                                                            
                                                           
                                                                   
                                                
                                                                
                               
                                                    
                                              
                                                             
   
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
};

#endif                       
