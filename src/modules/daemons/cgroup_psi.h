#ifndef PURECVISOR_CGROUP_PSI_H
#define PURECVISOR_CGROUP_PSI_H

   
                     
                                                                               
  
                           
                                                   
                                                    
                                        
  
          
                                                       
                                                                        
                                                   
                                                      
                                                 
                                                            
                               
  
                                                                              
                   
                                                                              
  
                                
                                                                                        
                                                                                          
                                                                           
  
                                                      
                                 
  
                                                                              
                             
                                                                              
  
                                                          
  
                                                            
                                                                       
                                                                                      
  
                                    
                                                         
                                                                        
  
                                                     
                                                                          
                                                                              
  
                                                               
                                                             
                                                       
                                                                     
                                                      
  
                             
                                                              
                                                                  
                                                      
                                                
                                                      
  
                                                                              
                                                                
                                                                              
  
                                                                                 
                                                                         
                                 
                                                                    
                                                                       
                                                                
  
                                                                  
                                                                             
                                                      
                                       
                                                           
  
        
                                                                 
                                                                  
                                                             
                                      
         
                                                              
        
                                                     
   

#include <glib.h>

G_BEGIN_DECLS

                                                                 
                                          
#define PCV_CGROUP_PSI_ROOT_DEFAULT "/sys/fs/cgroup"

   
                                  
  
                                                                 
                                                 
  
                                                                           
                                                                           
                                                               
                                               
   
#define PCV_CGROUP_PSI_MAX_ENTRIES 64

                                                           
#define PCV_CGROUP_PSI_NAME_MAX 128

                                        
#define PCV_CGROUP_PSI_RELPATH_MAX 320

   
                         
                                                              
                                              
   
typedef enum {
    PCV_CGROUP_PSI_KIND_VM = 0,                                    
    PCV_CGROUP_PSI_KIND_CONTAINER = 1                                   
} PcvCgroupPsiKind;

   
                          
                                                     
  
                                         
   
typedef struct {
    gchar            name[PCV_CGROUP_PSI_NAME_MAX];                                     
    gchar            rel_path[PCV_CGROUP_PSI_RELPATH_MAX];                                 
    PcvCgroupPsiKind kind;                                                   
} PcvCgroupPsiRef;

   
                             
                                               
  
                                                     
                                            
  
                                              
                                                              
                                                  
   
typedef struct {
    gboolean have_some;                                 
    gboolean have_full;                                 
    gdouble  some_avg10;                                
    gdouble  some_avg60;                                
    gdouble  some_avg300;                                
    gdouble  some_total_sec;                         
    gdouble  full_avg10;                                
    gdouble  full_avg60;                                
    gdouble  full_avg300;                                
    gdouble  full_total_sec;                         
} PcvCgroupPsiSample;

   
                                                    
  
                                                        
                                          
  
                                                        
                                                             
                                                        
                                                      
                             
  
                                               
                                        
                                   
                                                              
   
gboolean pcv_cgroup_psi_unescape(const gchar *raw, gchar *out, gsize out_sz);

   
                                                             
  
                                                                    
                                
  
                                                     
                                                        
                                                 
                                                                
                  
                                                        
                                                            
                                    
  
                                                       
                                                         
                                                                             
   
gboolean pcv_cgroup_psi_ref_from_machine_dir(const gchar *dirname, PcvCgroupPsiRef *out);

   
                                                   
  
                                                       
  
                                                      
                                                                               
                                                       
                                                         
                                                     
  
                                            
                                                                                
                                                            
                                    
                                  
                                                             
   
gboolean pcv_cgroup_psi_ref_from_lxc_dir(const gchar *parent_rel, const gchar *dirname,
                                         PcvCgroupPsiRef *out);

   
                                             
  
                                           
  
                                                   
                                                          
                                                   
  
                                                      
                                                     
                                         
  
                                
                                
                                                            
   
gboolean pcv_cgroup_psi_parse(const gchar *text, PcvCgroupPsiSample *out);

   
                                                     
  
                                                     
                                                     
  
                                                                     
                          
                                                    
                                                      
                                                            
  
                                                       
                                        
                                
                                                        
                                    
   
gint pcv_cgroup_psi_scan(const gchar *cgroup_root, PcvCgroupPsiRef *out,
                         gint max_out, gboolean *truncated);

   
                                            
  
                                                            
  
                                                        
                                                         
              
  
                                   
                                                    
                                              
                            
                                                     
   
gboolean pcv_cgroup_psi_read(const gchar *cgroup_root, const PcvCgroupPsiRef *ref,
                             const gchar *resource, PcvCgroupPsiSample *out);

   
                                                    
  
                                         
  
                                        
  
                                                                    
                                                        
                                                                   
                    
  
                                          
                               
                                                   
   
void pcv_cgroup_psi_format_labels(const PcvCgroupPsiRef *ref, gchar *out, gsize out_sz);

   
                                          
  
                                                      
  
                                                                 
  
                                           
                                   
                                             
                      
                                 
   
void pcv_cgroup_psi_format_metric(const gchar *resource, const gchar *type,
                                  const gchar *suffix, gchar *out, gsize out_sz);

   
                                                           
  
                                                
                              
  
                                                                   
                                                   
                                                
                    
  
                                                          
                                                                   
                                                                   
                                                       
          
  
                                                                            
                                                             
                         
   
gint pcv_cgroup_psi_collect(const gchar *cgroup_root);

G_END_DECLS

#endif                              
