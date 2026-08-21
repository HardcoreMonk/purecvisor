   
                  
                                        
  
                           
                                                   
                                                    
                                        
  
                                                     
                                                    
                                                 
  
                                                            
                                                                
                                                      
                                                                 
  
                                                  
                                                         
                                          
                                                      
         
  
          
                       
                        
                                                                                  
  
                          
                                       
                                                                
   

#ifndef PURECVISOR_PCV_BPF_H
#define PURECVISOR_PCV_BPF_H
#include <glib.h>
G_BEGIN_DECLS

   
                               
  
                                                    
                                                                         
                                                                   
   
typedef enum {
    PCV_BPF_OK,
    PCV_BPF_DEGRADED_NO_BTF,                           
    PCV_BPF_DEGRADED_NO_LSM,                             
} PcvBpfHealth;

                                       
                                                           
                                                           
gboolean pcv_bpf_caps_btf_path(const char *btf_path);
                                                       
                                                  
                                                                  
                                            
gboolean pcv_bpf_caps_lsm_path(const char *lsm_path);
gboolean pcv_bpf_caps_btf(void);                                           
gboolean pcv_bpf_caps_lsm(void);                                            

                                                     
                                                      
#define PCV_BPF_ERROR (pcv_bpf_error_quark())
                                                        
                                                                     
GQuark pcv_bpf_error_quark(void);
typedef enum {
    PCV_BPF_ERROR_MANIFEST,                                         
    PCV_BPF_ERROR_SHA_MISMATCH,                                      
    PCV_BPF_ERROR_LOAD,                                        
    PCV_BPF_ERROR_PIN,                                       
    PCV_BPF_ERROR_NO_LIBBPF,                                 
} PcvBpfError;

                                                        
                                                   
                                                            
typedef struct {
    gchar name[64];                                
    gchar file[128];                                   
    gchar sha256[65];                                  
    gchar min_ver[16];                           
    gchar loader[16];                                          
    gboolean req_btf;                             
    gboolean req_lsm;                                 
} PcvBpfManifestEntry;

   
                                                                   
                                                      
                                                                   
                                
   
GPtrArray *pcv_bpf_manifest_load(const char *store_dir, GError **error);

   
                                                                              
                                                       
                                              
   
gboolean   pcv_bpf_verify_sha(const char *store_dir,
                              const PcvBpfManifestEntry *e, GError **error);

                                                 
                                                         
typedef enum {
    PCV_BPF_REHYDRATE_FRESH,                  
    PCV_BPF_REHYDRATE_REATTACH,                                  
    PCV_BPF_REHYDRATE_UPGRADE,                                 
    PCV_BPF_REHYDRATE_ORPHAN,                            
} PcvBpfRehydrateAction;

   
                                                        
                                                   
                                                
                                                                 
                                     
                                                
                                    
   
PcvBpfRehydrateAction pcv_bpf_rehydrate_decide(
    const char *state_sha, gboolean pin_exists,
    gboolean state_exists, const char *manifest_sha);

   
                                                                    
                                                     
                                                                   
                                                       
                                                           
                       
   
guint pcv_bpf_count_pinned_links_path(const char *pindir);

                                                             
                                                 
                                                           

   
                                                               
                                                                 
                                                                    
                                                          
                                              
   
gboolean pcv_bpf_load_and_pin(const char *store_dir,
                              const PcvBpfManifestEntry *entry, GError **error);

   
                                                                    
                                                         
                                                        
                                           
   
gboolean pcv_bpf_rehydrate(const char *store_dir, GError **error);

                                                     
                                                             
                                                                   
gboolean       pcv_bpf_init(const char *store_dir, GError **error);
                                                                      
                                                               
                                                    
                                          
void           pcv_bpf_seal(void);
                                                          
gboolean       pcv_bpf_is_sealed(void);
                                                                   
                                                             
PcvBpfHealth   pcv_bpf_health(void);
                                                     
                                                  
                                                       
                                                  
                                                   
const char    *pcv_bpf_health_reason(void);                   
void           pcv_bpf_shutdown(void);

                                                       
                                                                   
                                                                     
                                                                 
  
                                                              
                                            
  
                                                              
                                                         
                                               
                                                   
                                                    
                       
                                               
                                           
void pcv_bpf_consumer_start(void);
                                                              
                                                               
                                                            
void pcv_bpf_consumer_stop(void);

                                                       
                                              
                                                                      
                                                          
                                                   
  
          
                                                                                 
                                                                               
                                                                               
                                                                        
                                                                        
  
                                                    
                                                         
                                                   
                                                          
                                                           
   
void pcv_bpf_metrics_tick(void);

G_END_DECLS
#endif
