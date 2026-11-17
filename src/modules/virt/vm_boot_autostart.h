   
                            
                                                                        
  
                           
                                                                        
                                                                          
                                                                                                  
                                                                                                      
                                                                          
                                                                           
                                                                             
                                                                           
                                                                         
  
                                                               
                                                                       
  
            
                                                                                   
                                                                                                                  
                                                                                                              
  
                                                            
   
#ifndef PURECVISOR_VIRT_VM_BOOT_AUTOSTART_H
#define PURECVISOR_VIRT_VM_BOOT_AUTOSTART_H

#include <glib.h>
#include <libvirt/libvirt.h>                                       

G_BEGIN_DECLS

                                                                               
#define PCV_BOOT_AUTOSTART_MARKER_PATH  "/var/lib/purecvisor/vm_boot_autostart.json"
#define PCV_BOOT_AUTOSTART_BOOT_ID_PATH "/proc/sys/kernel/random/boot_id"
#define PCV_BOOT_AUTOSTART_ORIGIN       "boot-autostart"

                                                       
typedef enum {
    PCV_BOOT_MARKER_ABSENT,
    PCV_BOOT_MARKER_OK,
    PCV_BOOT_MARKER_CORRUPT
} PcvBootMarkerState;

   
                                                                                
                                                                                       
                                                    
   
typedef struct {
    PcvBootMarkerState state;
    gchar             *boot_id;
    gchar             *decision;
    gchar            **candidates;
} PcvBootMarker;

                                                                                   
typedef enum {
    PCV_BOOT_DECIDE_RECORD_ONLY,                                   
    PCV_BOOT_DECIDE_SAME_BOOT,                                    
    PCV_BOOT_DECIDE_ACT,                                          
    PCV_BOOT_DECIDE_DISABLED,                               
    PCV_BOOT_DECIDE_ERROR                                        
} PcvBootDecision;

   
                                                                                                  
                                                                              
                                                                                        
                                                         
   
gboolean pcv_boot_autostart_read_boot_id_at(const gchar *path, gchar **boot_id_out, GError **error);

   
                                                                       
                                                                         
                                                                               
                                                      
                                          
   
void pcv_boot_autostart_marker_read_at(const gchar *path, PcvBootMarker *out);

   
                                                                                                                   
                                                                                              
                                                                                               
                                                                    
   
gboolean pcv_boot_autostart_marker_write_at(const gchar *path, const gchar *boot_id, PcvBootDecision decision,
                                            const gchar *const *candidates, GError **error);

   
                                                                         
                           
   
void pcv_boot_autostart_marker_clear(PcvBootMarker *m);

   
                                                           
                                           
                                                                               
                                           
                                           
                                                 
                                                    
                                                           
   
PcvBootDecision pcv_boot_autostart_decide(gboolean enabled, const gchar *current_boot_id,
                                          const PcvBootMarker *marker, gboolean *write_now_out);

   
                                                                                              
                                
   
const gchar *pcv_boot_autostart_decision_name(PcvBootDecision d);

                                                                

   
                                                                               
                                                                                                                
                                                                                             
                                                 
   
typedef struct {
    const gchar *default_bridge;
    gboolean   (*physical_mode)(const gchar *bridge, gchar **mode_out, GError **error);
    gboolean   (*vpc_managed)(const gchar *bridge);
    gboolean   (*iface_exists)(const gchar *bridge);
} PcvBootBridgeOracle;

                                                                    
typedef enum {
    PCV_BOOT_CLASS_CANDIDATE,                   
    PCV_BOOT_CLASS_NOT_PCV,                                  
    PCV_BOOT_CLASS_BRIDGE_MISSING,                      
    PCV_BOOT_CLASS_STATE_REASON,                                     
    PCV_BOOT_CLASS_ALREADY_ACTIVE,                
    PCV_BOOT_CLASS_XML_ERROR                           
} PcvBootClass;

                                                   
typedef struct {
    guint not_pcv;
    guint bridge_missing;
    guint state_reason;
    guint query_error;
} PcvBootScanCounts;

   
                                                                                
                                                 
                                                                                             
                         
                                 
                                                                            
   
gboolean pcv_boot_autostart_bridge_in_set(const PcvBootBridgeOracle *o, const gchar *bridge);

   
                                                                                                      
                                                                                
                                                                                  
                                                             
                                                           
   
gboolean pcv_boot_autostart_iface_exists_at(const gchar *sysfs_net_root, const gchar *name);

   
                                                                                                               
         
                                                                                                      
                                                                                             
                                                                                        
                                                                                          
                                                                                    
                                                                                            
                                                                                      
                                                                                 
                                                                             
   
gboolean pcv_boot_autostart_reason_allowed(int shutoff_reason);

   
                                                        
                                                                                                        
                                                                                                  
                                                                             
                                             
   
PcvBootClass pcv_boot_autostart_classify(int state, int reason, const gchar *inactive_xml,
                                         const PcvBootBridgeOracle *o, gchar **detail_out);

   
                                                                                                       
                                     
                     
   
const gchar *pcv_boot_autostart_class_name(PcvBootClass c);

   
                                                                         
                                                                                                       
                                                                                 
                                                                              
                                                     
                                                            
   
GPtrArray *pcv_boot_autostart_scan(virConnectPtr conn, const PcvBootBridgeOracle *o, PcvBootScanCounts *counts,
                                   GError **error);

                                                                

   
                                                                        
                                                                                                                       
                                                   
   
PcvBootClass pcv_boot_autostart_recheck(int state, int reason);

                                                                        
typedef enum {
    PCV_BOOT_SUBMIT_ASYNC,                                                              
    PCV_BOOT_SUBMIT_ALREADY_ACTIVE,                                       
    PCV_BOOT_SUBMIT_LOCK_BUSY,                                                    
    PCV_BOOT_SUBMIT_REJECTED                                                   
} PcvBootSubmit;

                                                     
typedef struct {
    guint started;
    guint failed;
    guint rejected;
    guint already_active;
    guint lock_busy;
    guint skipped_state_reason;
} PcvBootRunTally;

                                                               
typedef struct PcvBootRun PcvBootRun;

   
                                                                        
                                                                           
                                                                                                
                                                            
                                                                                                        
                                                                                     
                                                                                                                
                                                                        
                                                        
   
typedef struct {
    gboolean      (*recheck)(const gchar *vm, int *state_out, int *reason_out, gpointer user_data);
    PcvBootSubmit (*submit)(const gchar *vm, PcvBootRun *run, gchar **reason_out, gpointer user_data);
    void          (*report)(const gchar *vm, const gchar *reason, gpointer user_data);
    gboolean      (*terminating)(void);
    void          (*count)(const gchar *result, gpointer user_data);
    void          (*finished)(const PcvBootRunTally *tally, gpointer user_data);
    gpointer        user_data;
} PcvBootRunOps;

   
                                                                                                 
                                                                 
                                                      
   
void pcv_boot_autostart_run_queue(const gchar *const *vms, guint n, const PcvBootRunOps *ops);

   
                                                                                          
                                                        
                                                         
   
void pcv_boot_autostart_item_done(PcvBootRun *run, gboolean ok);

G_END_DECLS

#endif                                          
