   
                       
                                                             
  
                           
                                                   
                                                    
                                        
  
                                                              
                                                        
                                                            
                                                      
                                                          
                                                      
  
                                                             
                                                                     
                                                            
                                                   
                                                             
                                    
  
            
                                                                         
                                                                
                                                                           
  
                     
                                                                  
                                                           
                                                 
                                                      
                                                                
                                                                    
                                                   
  
                   
                                        
                                                                       
                                                                     
                                                             
                                                      
                                                
                                     
  
                
                                                                
                                                        
                                                          
                            
   
#ifndef PURECVISOR_PCV_SURICATA_H
#define PURECVISOR_PCV_SURICATA_H

#include <glib.h>
#include <json-glib/json-glib.h>
#include "modules/security/security_event.h"

G_BEGIN_DECLS

typedef enum {
    PCV_SURICATA_ACTIVE,
    PCV_SURICATA_INACTIVE,
    PCV_SURICATA_FAILED,
    PCV_SURICATA_ABSENT
} PcvSuricataState;

   
                                  
                                                                      
                                              
  
                                                                     
  
                                                           
                                                          
                                                             
                                                      
                      
  
                                             
   
PcvSuricataState pcv_suricata_state_from_output(const char *out);

   
                      
  
                                                      
  
                                                       
                                                            
                                               
                                          
  
                                                        
                                                         
  
                                 
   
PcvSuricataState pcv_suricata_probe(void);

   
                       
                            
  
                                                          
  
                                                               
                                                             
                                                             
                         
  
                                                    
  
                                                      
   
gboolean pcv_suricata_reload(GError **error);

   
                          
            
  
                                                              
                                                      
   
const char *pcv_suricata_state_str(PcvSuricataState s);

   
                             
  
                                                       
  
                                                    
                                                                      
                                            
                                                 
                                                 
   
void pcv_suricata_health_start(void);

   
                            
  
                                                       
  
                                                     
                                                   
                              
   
void pcv_suricata_health_stop(void);

  
                                                                          
  
                                            
                                                                   
                                                            
                                                                  
                                                            
                                       
  
                                  
                                                         
                                                                     
                                                       
                                       
                                                           
                                                                             
                                                                       
                                
   

                                                                             
#define PCV_SURICATA_EVE_PATH "/var/log/suricata/eve.json"

                                                         
                                               
#define PCV_SURICATA_RATE_PER_SEC 50
#define PCV_SURICATA_RATE_BURST   100

   
                                                         
  
                                                             
                                                      
                                               
                                           
                                                  
                                          
                                            
   
typedef struct {
    gdouble  tokens;
    gint64   last_update_usec;
    gboolean seeded;
} PcvSuricataRateState;

   
                           
                      
                                                          
                                         
  
                                                                
  
                                                                  
                                               
  
                                                  
                                                       
   
gboolean pcv_suricata_rate_allow(PcvSuricataRateState *st, gint64 now_usec);

                                                          
                                              
                          
#define PCV_SURICATA_EVE_MAX_LINE (64 * 1024)

   
                                                                         
   
typedef enum {
    PCV_SURICATA_LINE_FEED_PENDING,                                
    PCV_SURICATA_LINE_FEED_READY,                                             
    PCV_SURICATA_LINE_FEED_DISCARDED,                                         
    PCV_SURICATA_LINE_FEED_CAP_EXCEEDED                                       
} PcvSuricataLineFeedResult;

   
                                                
                                                              
                                                          
                  
                                                           
                                                  
                                                     
                               
                                                                     
                                                             
                                                
                                           
  
                                                             
  
                                                          
                                              
                                            
                                                    
                                           
             
  
                                                     
                                                     
                                         
          
  
                                                              
   
PcvSuricataLineFeedResult pcv_suricata_eve_line_feed(GString *linebuf,
                                                      gboolean *discarding,
                                                      guint64 *oversized_count,
                                                      gchar c);

   
                             
                                                          
                                                              
                                      
                                                               
                                                   
  
                                                               
  
                                         
                                                              
                                                                      
                                                                   
                                                                            
                                                                  
                                                                          
                                                           
                                  
                                                                        
                                                     
                                                             
  
                                                                    
   
gboolean pcv_suricata_eve_to_event(JsonObject *eve, PcvSecurityEvent *out, GError **error);

   
                          
                                                                      
                           
                                                              
  
                                                        
  
                                                                
                                   
   
void pcv_suricata_get_stats(guint64 *alerts_crit, guint64 *alerts_warn,
                             guint64 *alerts_info, guint64 *eve_dropped);

   
                               
  
                                                          
  
                                                               
                                                               
                                           
                                                         
                                                   
                                        
                                                                  
                                                         
                               
   
void pcv_suricata_eve_tail_start(void);

   
                              
  
                                                  
  
                                                          
                                                    
                              
   
void pcv_suricata_eve_tail_stop(void);

   
                                 
  
                                                           
                                                            
                                         
  
                                      
   
gboolean pcv_suricata_eve_tail_running(void);

  
                                                                     
                                                    
                                                                     
   

   
                                     
                                     
                                                        
  
                                                           
  
                                                   
                                                         
                                                            
                                          
                                                                  
                            
  
                                                                
              
   
void pcv_suricata_engine_status_cached(PcvSuricataState *state,
                                        gboolean *binary_present);

   
                             
  
                                                               
  
                                                 
                                                       
                                                                
                                                                            
                                                               
                                                                   
  
                                                                 
                                                                 
                                            
                
   
void pcv_suricata_metrics_tick(void);

   
                                                                          
                                                         
          
   
typedef struct {
    gint64   mtime;
    gint64   size;
    guint64  count;
    gboolean valid;
} PcvSuricataRulesCountCache;

   
                                                      
                                                                  
                                                                
                                   
                                                           
                                                      
  
                                                         
                                                   
                                                     
                                                   
  
                   
   
guint64 pcv_suricata_rules_count_cached(const gchar *path,
                                         PcvSuricataRulesCountCache *cache,
                                         gboolean *recomputed);

   
                            
  
                                                         
  
                                                   
                                                                
                                                
  
                                          
   
guint64 pcv_suricata_rules_count(void);

  
                                                                     
                                      
  
                      
                                                                     
                                                       
                                                     
                                                  
  
                       
                                                               
                                                             
                                                       
                              
  
                                  
                                                                    
                                               
                                                   
                                                               
                                                                     
   

#define PCV_SURICATA_POLICY_DIR  "/var/lib/purecvisor"
#define PCV_SURICATA_POLICY_FILE "suricata_policy.json"
#define PCV_SURICATA_POLICY_PATH PCV_SURICATA_POLICY_DIR "/" PCV_SURICATA_POLICY_FILE

                                                  
typedef struct {
    gboolean  inspect;
    gchar    *profile;
} PcvSuricataTenantPolicy;

                                                                   
  
                                                                    
                                                                 
                                                         
                                            
                                                  
typedef struct {
    gboolean    enforce;                                                      
    GHashTable *tenants;
    GHashTable *drop_sids;                                           
} PcvSuricataPolicy;

                                                       
                                          
PcvSuricataPolicy *pcv_suricata_policy_new(void);
                                                                 
                                                          
void pcv_suricata_policy_free(PcvSuricataPolicy *p);

   
                                 
                     
  
                                                              
  
                                             
                                                                
   
PcvSuricataPolicy *pcv_suricata_policy_load_file(const gchar *path);

   
                                 
              
                                                       
                            
  
                                                                 
  
                                                  
   
gboolean pcv_suricata_policy_save_file(const PcvSuricataPolicy *p,
                                        const gchar *path, GError **error);

   
                                                         
                                                            
                        
                                                      
                                                                          
                                                                            
                                                                   
                               
  
                                                                
                                                                
                                                               
  
                       
   
gboolean pcv_suricata_policy_apply(PcvSuricataPolicy *p, const gchar *tenant,
                                    const gboolean *inspect, const gchar *profile,
                                    const gchar *auto_isolate, GError **error);

   
                               
                                                                  
               
                                                                         
                                                         
                                                 
  
                                                 
   
JsonObject *pcv_suricata_policy_to_json(const PcvSuricataPolicy *p,
                                         const gchar *tenant);

  
                                                                  
  
                                                       
                                                  
                                
   

   
                                                
                                                       
                                                       
                        
                                                                
                                                           
                               
  
                                                 
                                                
                                               
                                                    
                                                
  
                                                             
  
                                                 
   
gboolean pcv_suricata_policy_set_drop_sids(PcvSuricataPolicy *p, JsonArray *sids,
                                            gboolean add, GError **error);

   
                                 
                                                       
                           
  
                                                      
                                                  
                                                      
  
                                    
   
GHashTable *pcv_suricata_policy_drop_sids(const PcvSuricataPolicy *p);

                                       

   
                           
                                                       
                                                            
                                                             
                                                   
   
gboolean pcv_suricata_policy_set(const gchar *tenant, const gboolean *inspect,
                                  const gchar *profile, const gchar *auto_isolate,
                                  GError **error);

   
                                
                                               
                                                          
                              
   
JsonObject *pcv_suricata_policy_get_json(const gchar *tenant);

   
                               
                                                        
                                                      
                                            
                                                
   
void pcv_suricata_policy_summary(gboolean *enforce, guint *tenant_count);

  
                                                               
  
                           
                                                
                                                         
                                     
                                                        
                                                  
                            
                                                                  
                                               
                                            
  
                           
                                                                
                                                          
                                                         
                                                     
                                                 
                                                  
                                                    
   

   
                                            
                                                      
                                                                     
                                               
                          
   
gboolean pcv_suricata_policy_set_drop_sids_global(JsonArray *sids, gboolean add,
                                                   GError **error);

   
                                          
                                                        
                                                             
                           
                                                                   
   
GHashTable *pcv_suricata_policy_drop_sids_snapshot(void);

   
                                        
                                                   
                                                      
                       
   
gboolean pcv_suricata_policy_drop_sids_commit(GError **error);

   
                                          
                                                
                                                            
                                                      
   
void pcv_suricata_policy_drop_sids_rollback(GHashTable *prev);

  
                                                                     
                                       
  
                           
                                                       
                                                                  
                                                            
                                                        
                                                                         
                                                                    
                                             
                                                     
  
               
                                                         
                                         
                                                                     
   

                                                    
                                                  
                           
#define PCV_SURICATA_ISOLATE_CAP 4096

                            
typedef enum {
    PCV_SURICATA_ISOLATE_DRY_RUN,                                              
    PCV_SURICATA_ISOLATE_ENFORCE,                                        
    PCV_SURICATA_ISOLATE_SKIP_INVALID,                                         
    PCV_SURICATA_ISOLATE_SKIP_ACTIONED,                      
    PCV_SURICATA_ISOLATE_SKIP_CAP                                   
} PcvSuricataIsolateDecision;

   
                                                 
                                                              
                                                                       
                                              
                                                             
  
                                                                
                                                                  
                                                                      
                                          
  
                                       
   
PcvSuricataIsolateDecision
pcv_suricata_isolate_decide(gboolean enforce, const gchar *src_ip,
                             GHashTable *actioned);

   
                                                
                                                  
                                                   
                            
                   
  
                                                            
                                                      
                                               
  
                                 
   
gboolean pcv_suricata_target_src_ip(const gchar *target, gchar *out, gsize out_sz);

G_END_DECLS

#endif                                
