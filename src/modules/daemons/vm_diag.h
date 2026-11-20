#ifndef PURECVISOR_VM_DIAG_H
#define PURECVISOR_VM_DIAG_H

   
                  
                                                               
                                                                         
  
                           
                                                         
                                                      
                                                                             
                                                                      
                                                                
                                        
  
                       
                                                             
                                                     
                                                           
                                                      
  
                                                                              
                
                                                                              
  
                                                                                          
                                                                                          
                                                                       
                                                                                                
  
                                                             
  
        
                                                      
                                                      
                                           
                                                          
                                            
                                                                
                                     
                                                
        
                                                          
                                                                     
        
                                               
                                                                     
   

#include <glib.h>
#include <libvirt/libvirt.h>                                     

#include "cgroup_psi.h"

G_BEGIN_DECLS

                                                             

                                                                 
#define PCV_VM_DIAG_MAX_VMS 256
                                                    
#define PCV_VM_DIAG_INTERVAL_MS 5000
                                                      
#define PCV_VM_DIAG_STALE_MS 15000
                                                    
#define PCV_VM_DIAG_MAX_GAP_MS 30000
                                                                     
#define PCV_VM_DIAG_GUEST_STALE_S 30
                                              
#define PCV_VM_DIAG_OWNER_REFRESH_CYCLES 12
                                       
#define PCV_VM_DIAG_MAX_HINTS 7
                                             
#define PCV_VM_DIAG_DISK_LAT_MIN_IOPS 5.0

                                                       
                                                                         
                      
_Static_assert(PCV_CGROUP_PSI_MAX_ENTRIES == PCV_VM_DIAG_MAX_VMS,
               "cgroup PSI scan cap must equal the VM diagnostics cap");

                                                                   

   
                        
                                                   
                                                    
                     
   
typedef enum {
    PCV_VM_DIAG_B_CPU        = 1u << 0,                           
    PCV_VM_DIAG_B_VCPU       = 1u << 1,                              
    PCV_VM_DIAG_B_VCPU_DELAY = 1u << 2,                              
    PCV_VM_DIAG_B_BALLOON    = 1u << 3,                                 
    PCV_VM_DIAG_B_GUEST      = 1u << 4,                                         
    PCV_VM_DIAG_B_RSS        = 1u << 5,                     
    PCV_VM_DIAG_B_DISK       = 1u << 6,                                              
    PCV_VM_DIAG_B_NET        = 1u << 7,                             
    PCV_VM_DIAG_B_CGROUP     = 1u << 8,                                        
} PcvVmDiagBundle;

   
                         
                                                                             
                                                    
   
typedef enum {
    PCV_VM_DIAG_M_CPU          = 1u << 0,
    PCV_VM_DIAG_M_VCPU_DELAY   = 1u << 1,
    PCV_VM_DIAG_M_GUEST_MEMORY = 1u << 2,
    PCV_VM_DIAG_M_RSS          = 1u << 3,
    PCV_VM_DIAG_M_DISK         = 1u << 4,
    PCV_VM_DIAG_M_NET          = 1u << 5,
    PCV_VM_DIAG_M_CGROUP       = 1u << 6,
} PcvVmDiagMissing;

                                                                    
typedef enum {
    PCV_VM_DIAG_Q_OK = 0,                           
    PCV_VM_DIAG_Q_WARMING_UP,                                                     
    PCV_VM_DIAG_Q_PARTIAL,                                        
    PCV_VM_DIAG_Q_BUSY,                                              
    PCV_VM_DIAG_Q_STOPPED,                           
} PcvVmDiagQuality;

                                      
typedef enum {
    PCV_VM_DIAG_R_NONE = 0,
    PCV_VM_DIAG_R_FIRST_SAMPLE,
    PCV_VM_DIAG_R_RESET,
    PCV_VM_DIAG_R_BUSY,
} PcvVmDiagReason;

                                         
typedef enum {
    PCV_VM_DIAG_H_CPU_WAIT = 0,
    PCV_VM_DIAG_H_CPU_THROTTLED,
    PCV_VM_DIAG_H_MEM_SWAP,
    PCV_VM_DIAG_H_MEM_PRESSURE,
    PCV_VM_DIAG_H_IO_PRESSURE,
    PCV_VM_DIAG_H_DISK_LATENCY,
    PCV_VM_DIAG_H_GUEST_STATS_STALE,
    PCV_VM_DIAG_H_COUNT
} PcvVmDiagHintCode;

   
                                                                                        
                                              
                               
   
const gchar *pcv_vm_diag_hint_code_name(PcvVmDiagHintCode code);

                    
typedef enum {
    PCV_VM_DIAG_L_NOTICE = 1,
    PCV_VM_DIAG_L_WARN = 2,
} PcvVmDiagHintLevel;

                                                                  
typedef enum {
    PCV_VM_DIAG_S_STARTING = 0,
    PCV_VM_DIAG_S_OK,
    PCV_VM_DIAG_S_ERROR,
} PcvVmDiagSamplerState;

                                               
typedef enum {
    PCV_VM_DIAG_E_NONE = 0,
    PCV_VM_DIAG_E_CONNECT_FAILED,
    PCV_VM_DIAG_E_RUNNING_STATS_FAILED,
    PCV_VM_DIAG_E_DEFINED_STATS_FAILED,
} PcvVmDiagError;

   
                                                         
                                                              
                                       
                                                         
   
const gchar *pcv_vm_diag_error_name(PcvVmDiagError e);

                                                                    

                                                                            
typedef struct {
    guint   idx;                                
    guint64 delay_ns;                        
} PcvVmDiagVcpuDelay;

   
                          
                                                  
  
                                                        
  
                                                        
                                                 
   
typedef struct {
    gchar   *name;                           
    gchar   *uuid;                                   
    gint     domain_id;                                   
    gint     state;                                                    
    gboolean active;                                                     
    guint    present;                                 
    gboolean busy;                                                   
    gint64   mono_us;                                     
    gint64   wall_ms;                                        

    guint64  cpu_time_ns;                        
    guint    vcpus;                                   
    guint    vcpus_max;                               
    GArray  *vcpu_delay;                                                              

    guint64  current_kb;                                    
    guint64  maximum_kb;                                    
    guint64  rss_kb;                                
    guint64  last_update_s;                                                     
    gboolean have_usable, have_unused, have_available;
    gboolean have_swap_in, have_swap_out, have_major_fault;
    guint64  usable_kb, unused_kb, available_kb;
    guint64  swap_in_kb, swap_out_kb, major_fault;

    guint64  rd_bytes, wr_bytes, rd_reqs, wr_reqs;                      
    guint64  rd_times_ns, wr_times_ns, fl_reqs, fl_times_ns;
    guint    disk_count;                       
    guint32  disk_set_hash;                          

    guint64  rx_bytes, tx_bytes, rx_pkts, tx_pkts;                      
    guint64  rx_drop, tx_drop, rx_errs, tx_errs;
    guint    net_count;                      
    guint32  net_set_hash;                           
} PcvVmDiagSample;

   
                                                     
                                            
   
void pcv_vm_diag_sample_clear(PcvVmDiagSample *s);

                                                                   
void pcv_vm_diag_sample_free(gpointer p);

   
                                                         
  
                                                         
                                     
  
                         
                                                                                 
                                                      
                                                                    
                                                                               
                                                     
                                                                                       
                                                                                     
  
                                                                 
                               
                                       
                                          
                                           
                                              
                                            
                                                     
                                                                 
   
gboolean pcv_vm_diag_parse(const virTypedParameter *params, gint nparams,
                           const gchar *name, const gchar *uuid, gint domain_id,
                           gboolean balloon_requested, gboolean block_requested,
                           PcvVmDiagSample *out);

                                                                    

   
                          
                                                  
                                                             
   
typedef struct {
    gchar   *name;                           
    gchar   *uuid;                          
    gchar   *owner;                                                      
    gint     domain_id;                           
    gint     state;                                    
    gboolean active;                                                
    gboolean have_vcpus;                           
    guint    vcpus;                             
    gboolean have_balloon;                            
    guint64  maximum_kb;                           
    guint64  current_kb;                           
} PcvVmDiagConfig;

                                                
void pcv_vm_diag_config_free(gpointer p);

   
                                          
                                                 
                                     
   
gboolean pcv_vm_diag_parse_config(const virTypedParameter *p, gint n, const gchar *name,
                                  const gchar *uuid, gint domain_id, PcvVmDiagConfig *out);

                                                                         

                                                                               
typedef enum {
    PCV_VM_DIAG_MEM_GUEST_USABLE = 0,                                                    
    PCV_VM_DIAG_MEM_GUEST_UNUSED,                                      
    PCV_VM_DIAG_MEM_RSS,                                            
    PCV_VM_DIAG_MEM_UNAVAILABLE,                                         
} PcvVmDiagMemSource;

   
                                                         
  
                                                                             
                                                         
  
                                                                                                                 
                                   
  
                                                 
   
PcvVmDiagMemSource pcv_vm_diag_mem_ladder(guint64 actual_kb, gboolean have_usable, guint64 usable_kb,
                                          gboolean have_unused, guint64 unused_kb, gboolean have_rss,
                                          guint64 rss_kb, guint64 *used_kb_out);

                                                                                                
const gchar *pcv_vm_diag_mem_source_name(PcvVmDiagMemSource s);

                                                                         

   
                              
                                                           
                                                      
   
typedef struct {
    guint   vms_running, vms_defined;
    guint   vcpu_running, vcpu_defined;
    guint64 mem_running_max_kb, mem_defined_max_kb, mem_running_alloc_kb;
} PcvVmDiagAllocation;

   
                                     
  
                                                                       
                                                                             
                                                                         
   
void pcv_vm_diag_allocation(const PcvVmDiagConfig *const *cfgs, guint n,
                            gboolean (*include)(const PcvVmDiagConfig *c, gpointer ud),
                            gpointer ud, PcvVmDiagAllocation *out);

                                                                       

                                                         
typedef struct {
    PcvVmDiagHintCode  code;
    PcvVmDiagHintLevel level;
    gdouble            value;                     
    gdouble            threshold;               
} PcvVmDiagHint;

   
                          
                                                             
                                                          
   
typedef struct {
    guint    present;                                                        
    guint64  cpu_time_ns, vcpu_delay_ns;
    guint64  disk_rd_bytes, disk_wr_bytes, disk_rd_reqs, disk_wr_reqs;
    guint64  disk_rd_times_ns, disk_wr_times_ns;
    guint64  net_rx_bytes, net_tx_bytes, net_rx_pkts, net_tx_pkts;
    gboolean have_throttle;                                  
    guint64  throttled_usec, nr_throttled, nr_periods;
} PcvVmDiagTotals;

   
                          
                                                                  
  
                                               
  
                                                                 
                                                      
                                                             
                    
   
typedef struct {
    gchar            *name;
    gchar            *uuid;
    gint              domain_id;
    gint              state;                                          
    PcvVmDiagQuality  quality;
    PcvVmDiagReason   reason;
    guint             missing;                                     
    gint64            observed_mono_us;                                   
    gint64            observed_wall_ms;
    gdouble           interval_ms;
    gint64            last_ok_at_ms;

    guint             vcpus;                          
    gdouble           mem_max_mb, mem_alloc_mb, mem_guest_used_mb;
    gdouble           mem_guest_available_mb, mem_rss_mb, guest_stats_age_s;
    gdouble           cpu_pct, cpu_host_pct;
    gdouble           disk_rd_bps, disk_wr_bps, disk_rd_iops, disk_wr_iops;
    gdouble           disk_rd_lat_ms, disk_wr_lat_ms, disk_fl_lat_ms;
    gdouble           net_rx_bps, net_tx_bps, net_rx_pps, net_tx_pps;
    gdouble           net_drop_ps, net_err_ps;
    gdouble           vcpu_delay_pct, vcpu_delay_max_pct;
    gdouble           throttled_pct, throttled_max_pct, throttled_periods_ratio;
    gboolean          have_psi;                                                      
    gdouble           psi[PCV_CGROUP_PSI_RES_COUNT][2];                                          
    gdouble           swap_in_kbps, swap_out_kbps, major_faults_ps;

    guint             n_hints;
    PcvVmDiagHint     hints[PCV_VM_DIAG_MAX_HINTS];

    PcvVmDiagTotals   tot;
} PcvVmDiagRecord;

                                                                     

   
                          
                                
                                                
   
typedef struct {
    gint64   observed_mono_us;                        
    gint64   observed_wall_ms;                             
    guint    logical_cpus;                                 
    guint    physical_cores;                                        
    guint64  mem_total_kb;                               
    gboolean have_psi[PCV_CGROUP_PSI_RES_COUNT][2];                             
    gdouble  psi[PCV_CGROUP_PSI_RES_COUNT][2];                                               
} PcvVmDiagHostIn;

   
                            
                                                  
  
                                                                       
                                              
   
typedef struct {
    PcvVmDiagSamplerState state;
    PcvVmDiagError        error;
    gint64                observed_mono_us;
    gint64                observed_wall_ms;
    guint                 tracked;                      
    guint                 truncated;                            
    PcvVmDiagHostIn       host;
    PcvVmDiagAllocation   alloc_all;                                          
    GPtrArray            *records;                                   
    GPtrArray            *configs;                                                       
} PcvVmDiagSnapshot;

                                                     
void pcv_vm_diag_snapshot_free(PcvVmDiagSnapshot *s);

   
                                     
                                                    
                                                                  
   
PcvVmDiagSnapshot *pcv_vm_diag_snapshot_dup(const PcvVmDiagSnapshot *s);

                                                                            

                                                       
typedef struct _PcvVmDiagTable PcvVmDiagTable;

                                                      
PcvVmDiagTable *pcv_vm_diag_table_new(void);

                            
void pcv_vm_diag_table_free(PcvVmDiagTable *t);

   
                                               
  
                                                    
                                      
  
                                                          
                                         
                                             
                                                        
                                                 
  
                            
                                                          
                                                                         
                                                                        
                                              
                                                                      
   
void pcv_vm_diag_table_step(PcvVmDiagTable *t, GPtrArray *running_samples, GPtrArray *configs,
                            GPtrArray *cgroup_stats, const PcvVmDiagHostIn *host,
                            PcvVmDiagSnapshot *out);

const PcvCgroupVmStat *pcv_vm_diag_cgroup_find(const GPtrArray *cgroup_stats, gint domain_id, const gchar *name);

G_END_DECLS

#endif                           
