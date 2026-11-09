                                                                             
                               
                                                                                                   
  
                           
                                                                               
                                                                                   
                                                                               
                                              
                                                                        
                                                                       
  
                                                               
                                                                
                     
                                                                                
#ifndef PURECVISOR_LXC_USERNS_H
#define PURECVISOR_LXC_USERNS_H

#include <glib.h>

G_BEGIN_DECLS

#define PCV_LXC_SUBUID_PATH      "/etc/subuid"
#define PCV_LXC_SUBGID_PATH      "/etc/subgid"
                                                                            
#define PCV_LXC_ROOTLESS_DEFAULT "true"
                                                                      
#define PCV_LXC_USERNS_MIN_COUNT G_GINT64_CONSTANT(65536)
                                                
#define PCV_LXC_USERNS_ID_LIMIT  G_GINT64_CONSTANT(4294967295)
                                                       
#define PCV_LXC_USERNS_INCLUDE   "/usr/share/lxc/config/userns.conf"

   
                          
                                                       
                                                                   
   
typedef enum {
    PCV_LXC_ROOTLESS_OFF,                                                              
    PCV_LXC_ROOTLESS_ON,                                   
    PCV_LXC_ROOTLESS_REJECT,                                          
} PcvLxcRootlessDecision;

   
                    
                                                                                    
                                               
   
typedef enum {
    PCV_LXC_USERNS_READY = 0,
    PCV_LXC_USERNS_BAD_RANGE,                                                                   
    PCV_LXC_USERNS_OVERLAP,                                           
    PCV_LXC_USERNS_NO_ROOT_RANGE,                                                   
} PcvLxcUsernsHost;

   
                        
                                                                                 
                                                
   
typedef struct {
    gint64   start;                                                                  
    gint64   count;                       
    gboolean default_on;                                           
    gboolean default_unknown;                                        
} PcvLxcUsernsSettings;

   
                 
                                                                    
                                                     
   
typedef enum {
    PCV_LXC_ID_MAPPED,                                     
    PCV_LXC_ID_UNSHIFTED,                                
    PCV_LXC_ID_OUTSIDE,                             
} PcvLxcIdClass;

   
                           
                                                                           
                                                              
   
PcvLxcRootlessDecision pcv_lxc_rootless_decide(gint requested, gboolean config_default, gboolean host_ready);

   
                             
                                                                            
                                                                         
                                                 
                                                                                  
                                                                                      
                                                                                     
                                                               
   
PcvLxcUsernsHost pcv_lxc_userns_host_check(const gchar *subuid_path, const gchar *subgid_path,
                                           gint64 start, gint64 count);

   
                               
                                                                                                       
                                                 
                                                       
   
gchar *pcv_lxc_userns_host_message(PcvLxcUsernsHost cause, gint64 start, gint64 count);

   
                                
                                                                                              
                                                                                 
                                               
   
gboolean pcv_lxc_userns_parse_default(const gchar *raw, gboolean *unknown);

   
                                
                                                                                                   
                                                                             
                                
                                    
   
PcvLxcUsernsSettings pcv_lxc_userns_settings_load(void);

   
                                 
                                                                                         
                                                                                                                                      
                                                                     
                                                                                         
                                                            
   
gchar *pcv_lxc_userns_resource_lines(const gchar *bridge, guint memory_mb, guint vcpu_count);

   
                                      
                                                                                             
                                                                                         
                                                                                                                   
                                                                                      
                                     
                                                                    
   
gchar *pcv_lxc_userns_build_create_config(const gchar *default_text, gint64 start, gint64 count,
                                          const gchar *resource_text, gboolean *dropped_idmap);

   
                    
                                                                                           
                                                                                          
                                                                                         
                                                       
                                                     
   
typedef struct {
    const gchar *key;
    const gchar *value;
    gboolean     append;
} PcvLxcConfigEdit;

   
                                     
                                                                                     
                                                                                                    
                                                                          
                                                        
   
gchar *pcv_lxc_userns_config_apply_edits(const gchar *text, const PcvLxcConfigEdit *edits, gsize n_edits,
                                         GError **error);

   
                                 
                                                                                          
                                                 
   
gboolean pcv_lxc_userns_idmap_complete(const gchar *idmap_text);

   
                                     
                                                                                  
                                       
   
gboolean pcv_lxc_userns_config_has_include(const gchar *config_text);

   
                                                        
                                                                   
                                                  
   
PcvLxcIdClass pcv_lxc_userns_classify_id(gint64 id, gint64 start, gint64 count);
gint64        pcv_lxc_userns_shift_id(gint64 id, gint64 start, gint64 count);

   
                              
                                                                                     
                                                    
                                                                    
   
gchar *pcv_lxc_userns_getcap_text(const gchar *getcap_out, const gchar *path);

   
                                 
                                                                                          
                                                                                 
                                                                                      
                                                                                              
                                                                   
                                                             
   
gboolean pcv_lxc_mountinfo_is_readonly(const gchar *mountinfo_text, const gchar *mount_point, gboolean *found);

   
                             
                                                                                       
                                                                    
                                
   
GQuark pcv_lxc_mount_error_quark(void);
#define PCV_LXC_MOUNT_ERROR (pcv_lxc_mount_error_quark())

typedef enum {
    PCV_LXC_MOUNT_ERROR_BAD_PATH,                                                                        
    PCV_LXC_MOUNT_ERROR_NO_KERNEL_SUPPORT,                                    
} PcvLxcMountError;

                               
typedef enum {
    PCV_LXC_MOUNT_CLASS_INTERNAL,                         
    PCV_LXC_MOUNT_CLASS_BAD_REQUEST,                      
    PCV_LXC_MOUNT_CLASS_NO_KERNEL_SUPPORT,                
} PcvLxcMountErrorClass;

   
                                
                                                                                                  
                                                                                                    
                                                               
   
PcvLxcMountErrorClass pcv_lxc_mount_error_classify(const GError *error);

   
                           
                                                                                            
                                                                                        
                                                                                            
                                                                                                            
                                                                                                                                                       
                                                   
                                                                                         
   
int pcv_lxc_rootfs_walk_dir(int rootfs_fd, const gchar *container_path, gboolean create, GError **error);

   
                           
                                                                                                          
                                                                                                                  
                                                                                                           
                                                                                 
                                                                                          
                                                                                                                               
                                                                                                                               
                                                                                  
   
gboolean pcv_lxc_rootfs_mkdir_at(int dir_fd, const gchar *name, GError **error);

   
                            
                                                                                                 
                                                                                                   
                                                                                                      
                                                                          
                                                                                                       
                                                                                                          
                                                                        
                                               
                                                       
   
int pcv_lxc_rootfs_open_leaf(int rootfs_fd, const gchar *container_path, GError **error);

                                                                       
#define PCV_LXC_MOUNT_RO        (1u << 0)
                                                                         
#define PCV_LXC_MOUNT_RECURSIVE (1u << 1)

   
                              
                                                                                                            
                                                                                                            
                                                                                                                         
                                                                                                                                       
                                                                                                                 
                                                                                  
                                                                                                            
                                                                         
                                                                                                                  
                                                                                                         
                          
                                                                                                                 
                                                                                                                                  
                                                                 
                                                                        
                                                                            
                         
   
gboolean pcv_lxc_userns_mount_clone(int src_fd, int target_fd, guint flags, guint64 *mnt_id_out, GError **error);

   
                                     
                                                                                              
                                                                                              
                                                                                                               
                                                                                                    
                                                                                        
                          
                                                                                
                                                                    
   
gchar *pcv_lxc_config_remove_mount_entry(const gchar *text, const gchar *dest, guint *removed);

                                                                                         

   
                    
                                                                              
                                                    
   
typedef struct {
    gchar *path;
    guint  mode;
} PcvLxcUsernsFile;

   
                    
                                                                                 
                                                              
                                                                                                      
                                                                                          
                                                                                 
                                                                    
   
typedef struct {
    guint64     mapped, unshifted, outside, acl, unreadable;
    GHashTable *pairs;
    GPtrArray  *suid_files;
    GPtrArray  *cap_files;
    gchar      *first_outside;
} PcvLxcUsernsScan;

   
                                      
                                                                                                                
                                                                                 
                                                                                                            
                                                                
   
gchar *pcv_lxc_userns_write_create_config(const gchar *default_conf_path, gint64 start, gint64 count,
                                          const gchar *resource_text, GError **error);

   
                                       
                                                                             
                                    
   
void pcv_lxc_userns_remove_create_config(const gchar *dir);

   
                           
                                                                                                    
                                                                                       
                                                           
   
typedef gchar *(*PcvLxcConfigRewriteFunc)(const gchar *text, gconstpointer data, GError **error);

   
                        
                                                                                                  
                                                                                                                       
                                                                                                      
                                                                                                       
                                                           
   
GMutex *pcv_lxc_config_mutex(void);

   
                                 
                                                                                               
                                                                                        
                                                                                                      
                                                                     
   
gboolean pcv_lxc_userns_config_rewrite(const gchar *lxc_path, const gchar *name, PcvLxcConfigRewriteFunc rewrite,
                                       gconstpointer data, GError **error);

   
                              
                                                                                                   
                                                                                              
                                                                                      
                                                                                                  
                                                                               
                                                          
                                                                                                            
                                                                        
   
gboolean pcv_lxc_userns_config_edit(const gchar *lxc_path, const gchar *name, const PcvLxcConfigEdit *edits,
                                    gsize n_edits, GError **error);

   
                       
                                                                             
                                                                     
                                                                    
                                                                   
   
gboolean pcv_lxc_userns_scan(const gchar *rootfs, gint64 start, gint64 count, PcvLxcUsernsScan *out, GError **error);

   
                             
                                                                        
                        
   
void pcv_lxc_userns_scan_clear(PcvLxcUsernsScan *scan);

   
                               
                                                                                                  
                                                                                           
                                                                                                       
                                                                         
                                                                     
   
gboolean pcv_lxc_userns_shift_rootfs(const gchar *rootfs, const PcvLxcUsernsScan *scan, gint64 start, gint64 count,
                                     GError **error);

   
                                        
                                                                                     
                                                      
   
gchar *pcv_lxc_userns_unsearchable_ancestor(const gchar *dir);

   
                              
                                                                                    
                                                                                         
                                                                  
                                                                    
                                                                    
   
gboolean pcv_lxc_userns_settle_dirs(const gchar *lxc_path, const gchar *name, GError **error);

   
                            
                                                                                               
                                                                                  
                                                                                         
                                                                             
                                                                                             
                                                                                      
                                                             
   
gboolean pcv_lxc_userns_check_top(const gchar *lxc_path, const gchar *name, GError **error);

   
                                
                                                                                         
                                                                                
                                                                
   
gboolean pcv_lxc_userns_settle_rootfs(const gchar *lxc_path, const gchar *name, gint64 start, gint64 count,
                                      gboolean *fell_back, GError **error);

G_END_DECLS

#endif                              
