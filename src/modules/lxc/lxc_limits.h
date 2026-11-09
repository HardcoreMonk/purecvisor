                                                                             
                               
                                                                                 
                                                           
  
                           
                                                                                            
                                                                                     
                                                                                    
                                                                         
                                                                                    
                                                           
                                                                                 
                                                                                                        
                                                                                               
  
                                                                         
                                              
                                                                                
#ifndef PURECVISOR_LXC_LIMITS_H
#define PURECVISOR_LXC_LIMITS_H

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

                                                       
#define PCV_LXC_CGROUP_ROOT   "/sys/fs/cgroup"
                                          
#define PCV_LXC_CPU_PERIOD_US 100000

                                                                                                              
gboolean pcv_lxc_cgroup_write(const gchar *path, const gchar *value, GError **error);

                                                                                    
gchar *pcv_lxc_limits_cpu_max(const gchar *cfg_cpu_max, const gchar *cfg_shares);

                                                                          
gboolean pcv_lxc_limits_memory_bytes(const gchar *cfg_value, guint64 *bytes_out);

                                                                                                
                                                                                                   
guint64 pcv_lxc_cgroup_read_u64(const gchar *cgroup_root, const gchar *name, const gchar *filename);

                                                                                         
guint64 pcv_lxc_cgroup_cpu_usage_ns(const gchar *cgroup_root, const gchar *name);

                                                                                                              
                                                 
guint64 pcv_lxc_cgroup_cpu_time_ns(const gchar *cgroup_root, const gchar *name);

                                                                                          
                                      
typedef struct {
    guint64 cpu_time_ns;                               
    gint64  mono_ns;                              
    gint    init_pid;                                                                                   
} PcvLxcCpuSample;

                                                                                                           
                                                                                                        
                                                                                                        
gdouble pcv_lxc_cpu_percent_between(const PcvLxcCpuSample *prev, const PcvLxcCpuSample *cur);

                                                                                         
                                                                                                  
                                                                               
gdouble pcv_lxc_cpu_sample_record(const gchar *name, gint init_pid, guint64 cpu_time_ns, gint64 mono_ns);

                                                                                    
void pcv_lxc_cpu_sample_forget(const gchar *name);

                                                                                       
const gchar *pcv_lxc_metrics_cgroup_root(void);

                                                                                      
void pcv_lxc_metrics_set_cgroup_root_for_test(const gchar *root);

                                                                                                             
                                                                   
gboolean pcv_lxc_limits_reapply(const gchar *lxc_path, const gchar *name, const gchar *cgroup_root, GError **error);

G_END_DECLS

#endif                              
