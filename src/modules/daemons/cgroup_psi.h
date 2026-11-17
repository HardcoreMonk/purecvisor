#ifndef PURECVISOR_CGROUP_PSI_H
#define PURECVISOR_CGROUP_PSI_H

   
                     
                                                                               
  
                           
                                                   
                                                    
                                        
  
          
                                                       
                                                                        
                                                   
                                                      
                                                 
                                                            
                               
  
                                                                              
                   
                                                                              
  
                                
                                                                                        
                                                                                          
                                                                           
  
                                                      
                                 
  
                                                                              
                             
                                                                              
  
                                                          
  
                                                            
                                                                       
                                                                                      
  
                                    
                                                         
                                                                        
  
                                                     
                                                                          
                                                                              
  
                                                               
                                                             
                                                       
                                                                     
                                                      
  
                             
                                                              
                                                                  
                                                      
                                                
                                                      
  
                                                                              
                                                                                                       
                                                                              
  
                                                                                 
                                                                         
                                 
                                                                    
                                                                       
                                                                
  
                                                                  
                                                                                          
                                                                              
                        
                                       
                                                           
  
                                                      
                                                 
                                                                             
                                                                        
                
  
                                                                              
                                                  
                                                                              
  
                                                                                     
                                                                     
                                                                              
                                                                    
                                         
  
        
                                                                 
                                                                  
                                                             
                                      
         
                                                              
        
                                             
                                                                                 
                                                                               
                
   

#include <glib.h>

G_BEGIN_DECLS

                                                                 
                                          
#define PCV_CGROUP_PSI_ROOT_DEFAULT "/sys/fs/cgroup"

   
                                  
  
                                                                     
                                                      
                                                                         
                                                
  
                                                               
                                                                 
                                                                                    
                                                                         
  
                                                                         
   
#define PCV_CGROUP_PSI_MAX_ENTRIES 256

                                                                   
                                                               
                                              
#define PCV_CGROUP_PSI_PROM_CONTAINER_MAX 64

                                                              
                                                    
#define PCV_CGROUP_CPU_CHILD_NAME_MAX 32

                                                           
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
    gint             domain_id;                                                           
    gchar            norm_name[PCV_CGROUP_PSI_NAME_MAX];                                     
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

   
                        
                                                                       
                                       
   
typedef enum {
    PCV_CGROUP_PSI_RES_CPU = 0,                         
    PCV_CGROUP_PSI_RES_MEMORY = 1,                         
    PCV_CGROUP_PSI_RES_IO = 2,                         
    PCV_CGROUP_PSI_RES_COUNT = 3
} PcvCgroupPsiRes;

   
                           
                                              
  
                                                  
                                                        
  
                                                                     
                                                               
   
typedef struct {
    gboolean have;                              
    guint64  nr_periods;                            
    guint64  nr_throttled;                              
    guint64  throttled_usec;                              
} PcvCgroupCpuStat;

   
                            
                                                                       
                                                      
   
typedef struct {
    gchar   name[PCV_CGROUP_CPU_CHILD_NAME_MAX];                                         
    guint64 throttled_usec;                                                           
} PcvCgroupCpuChild;

   
                          
                                                                    
  
                                                               
  
                                                                              
                                                                             
                                                                                
                                                              
                                                                               
                   
                                                      
   
typedef struct {
    PcvCgroupPsiRef    ref;                                                             
    gboolean           have_psi[PCV_CGROUP_PSI_RES_COUNT];                      
    PcvCgroupPsiSample psi[PCV_CGROUP_PSI_RES_COUNT];                             
    PcvCgroupCpuStat   sum;                                                              
    GArray            *children;                                                           
} PcvCgroupVmStat;

   
                                                    
  
                                                        
                                          
  
                                                        
                                                             
                                                        
                                                      
                             
  
                                               
                                        
                                   
                                                              
   
gboolean pcv_cgroup_psi_unescape(const gchar *raw, gchar *out, gsize out_sz);

   
                                                             
  
                                                                    
                                
  
                                                     
                                                        
                                                 
                                                                
                  
                                                        
                                                            
                                    
  
                                                       
                                                         
                                                                             
   
gboolean pcv_cgroup_psi_ref_from_machine_dir(const gchar *dirname, PcvCgroupPsiRef *out);

   
                                                         
  
                                                                              
                                                         
                                         
  
                                                                        
                                                       
  
                                              
                             
   
gint pcv_cgroup_psi_domain_id_from_scope(const gchar *dirname);

   
                                                        
  
                                                         
                                                                      
                    
  
                           
                                       
                                                                              
                                                                    
                                                                  
                                                
                                                  
  
                                       
                                
                                                    
   
void pcv_cgroup_psi_normalize_vm_name(const gchar *vm_name, gchar *out, gsize out_sz);

   
                                                               
  
                                               
  
                                                                          
                                                          
  
                                         
                          
                                            
   
gboolean pcv_cgroup_cpu_stat_parse(const gchar *text, PcvCgroupCpuStat *out);

   
                                                                   
                                        
   
void pcv_cgroup_vm_stat_free(gpointer p);

   
                                               
  
                                                    
                                                    
                                                  
  
                                                                        
                                                   
  
                             
                               
                                                       
                                      
                                            
                                             
   
gboolean pcv_cgroup_psi_prom_selected(const PcvCgroupPsiRef *refs, gint n, gint vm_cap,
                                      gint ctr_cap, gboolean *selected_out);

   
                                                   
  
                                                       
  
                                                      
                                                                               
                                                       
                                                         
                                                     
  
                                            
                                                                                
                                                            
                                    
                                  
                                                             
   
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

   
                                                           
  
                                                
                              
  
                                                                   
                                                   
                                                
                    
  
                                                          
                                                                   
                                                                                
                                                       
          
  
                                                               
                                                                               
                                                  
  
                                                                             
                                                                             
                                                                             
                                                             
                         
   
gint pcv_cgroup_psi_collect(const gchar *cgroup_root, GPtrArray *vm_stats_out);

G_END_DECLS

#endif                              
