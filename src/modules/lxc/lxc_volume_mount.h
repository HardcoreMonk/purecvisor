                                                                             
                                     
                                                                                                        
                                                                     
  
                           
                                                                                                                  
                             
                                                                                             
                                                                                                
                                                                                                            
                                                                                                             
                                                                                                           
                                                                            
                                                                                 
                                                               
  
                                                                          
                                                           
                                                                                
#ifndef PURECVISOR_LXC_VOLUME_MOUNT_H
#define PURECVISOR_LXC_VOLUME_MOUNT_H

#include <glib.h>
#include "lxc_volume_guard.h"                                   
#include "lxc_mountinfo.h"                                    

G_BEGIN_DECLS

                                                                                                                      
                                                                                                    
                                                                                                          
                                                           
                                                 

                                                                                         
                                                    
#define PCV_LXC_VOLUME_TYPE_MISMATCH_MSG "container_path must be the same type as host_path (both directories or both regular files)"

                                                                                                          
                                                                                                
#define PCV_LXC_VOLUME_ROOT_TARGET_MSG "container_path must name an entry below the root"

                                                                                                                    
                                                                                                     
                                                                                         
#define PCV_LXC_VOLUME_OCCUPIED_FMT \
    "container_path already has a volume attached while the container is running: %s; detach it first, or attach while the container is stopped"

                                                                                                                             
                                                                                                 
                                                                                               
                                                                                        
#define PCV_LXC_VOLUME_INSIDE_FMT \
    "container_path is inside another attached volume while the container is running: %s (volume at %s); attach while the container is stopped " \
    "(the path must already exist inside that volume's source)"

                                                                                                                     
                                                                                                            
                                                                                                            
#define PCV_LXC_VOLUME_COVER_FMT "container_path would cover another attached volume: %s; detach it first"

                                                                                                
                                                                                                          
                                                                                           
                                                            
#define PCV_LXC_VOLUME_NEST_MISSING_FMT \
    "container_path is inside the attached volume at %s, but %s does not exist in that volume's source; create it there first"

                                                                                                                     
                                                                                                                             
                                                                  
#define PCV_LXC_VOLUME_NEST_CTX_FMT "container_path is inside the attached volume at %s (%s in that volume's source): %s"

                                                                                            
                                                                                                        
                                                    
                                                                                                                      
                                                                                              
                                                                                                           
                                                                                                                                  
                                                                                                                                           
                                                                                                    
                                                                                           
                                                                                                         
                                                                                               
                                                                                                       
                                                                                              
                                                                                    
                                                                                                                     
                                                                                                                             
                                                
                                                                               
                                    
gboolean        pcv_lxc_volume_mount_running(const gchar *lxc_path, const gchar *name, int src_fd, const gchar *container_path,
                                             gboolean readonly, guint64 *mnt_id_out, GError **error);

   
                                
                                                                                                                      
                                                                                                      
                                                                                                            
                                                                                                       
                                                                                                              
                                                                                           
                                                                                                
                                                         
                                                                                                               
                                                                                                                                      
                                                                                                          
                                                                             
   
gboolean pcv_lxc_volume_target_ensure(const gchar *lxc_path, const gchar *name, int *rootfs_fd, const gchar *container_path,
                                      gboolean src_is_dir, GError **error);

   
                         
                                                                                                            
                                                                                                                    
                                                                                                                                  
                                                                                                     
                                                                                               
                                                                                                         
                                                                                                            
                                                                                                                  
                                                                                                     
                                                                                                                          
                                                                                                                  
                                                                                                                   
                                                                                                  
                                                                                        
                                                                                                               
                                                                                      
                                                                                                                    
                                        
                                                                             
                                                                                                                                 
                              
                                                                                           
                             
                                                                                                                                             
                                               
                                                                                                                               
                                                                                              
                                                                                                                             
                                                                                   
                                                   
   
gboolean pcv_lxc_volume_attach(const gchar *lxc_path, const gchar *name, int src_fd, const gchar *canon,
                               const gchar *container_path, gboolean readonly, gboolean running, GError **error);

   
                             
                                                                                                            
                                            
                                                                                                    
                                                                                               
                                                                            
                                                                                                               
                                                                                                          
                                                                                
                                                                                                        
                                                                                                                                 
                                                                                                            
                                                                                            
                                                                            
   
gboolean pcv_lxc_volume_nest_check(const GPtrArray *lines, guint i, gboolean src_is_dir, gboolean *nested, GError **error);

   
                              
                                                                                                          
                                                                                                    
                                                                                             
                                                                                        
                                                                            
                                                                
   
gboolean pcv_lxc_volume_config_save(const gchar *lxc_path, const gchar *name, const gchar *mount_value, GError **error);

   
                              
                                                                                                
                                                                                     
                        
                                            
                           
                                                           
   
void pcv_lxc_volume_hook_ensure(const gchar *lxc_path, const gchar *name);

                                                                                                         
                                                                                         
                                                                                            
                                                                                                             
                                                                             

                                                                  
                                   
GQuark pcv_lxc_volume_error_quark(void);
#define PCV_LXC_VOLUME_ERROR (pcv_lxc_volume_error_quark())
typedef enum { PCV_LXC_VOLUME_ERROR_CONFLICT,                    
               PCV_LXC_VOLUME_ERROR_INTERNAL,               
               PCV_LXC_VOLUME_ERROR_STAGING_MISMATCH                                                                            
             } PcvLxcVolumeError;

                                                                                 
                                                        
typedef struct {
    gchar   *(*read_mountinfo)(GError **error);                                                                   
    int      (*open_dir)(const gchar *path, GError **error);                                                                                                      
    gchar   *(*fd_path)(int fd);                                                                                             
    int      (*walk)(int root_fd, const gchar *rel, gboolean create, GError **error);                              
    int      (*open_leaf)(int root_fd, const gchar *rel, GError **error);                                                                                
    gboolean (*mnt_id)(int fd, guint64 *id_out);                                                               
    gboolean (*make_private)(int fd, GError **error);                                                                                                              
    gboolean (*umount_fd)(int fd, GError **error);                                                                                
    gboolean (*remove_at)(int dir_fd, const gchar *name, GError **error);                                                            
    GPtrArray *(*list_at)(int dir_fd, GError **error);                                                                                        
                                       
    int      (*source_open)(const gchar *host_path, gchar **canon_out, GError **error);                                              
    int      (*prepare_dir)(int parent_fd, const gchar *name, guint mode, gint64 gid, GError **error);                                          
                                                                                                                                                             
                                                                                                                                                                    
    int      (*prepare_slot)(int dir_fd, const gchar *name, gboolean is_dir, GError **error);                                                                  
    gboolean (*mount_clone)(int src_fd, int target_fd, guint flags, guint64 *mnt_id_out, GError **error);                                                            
                                                                                                                                               
} PcvLxcMountOps;

   
                                   
                                                         
                                          
   
void pcv_lxc_volume_set_ops_for_test(const PcvLxcMountOps *ops);                                        

   
                                     
                                                                                                              
                                                                              
                                           
   
void pcv_lxc_volume_set_roots_for_test(const gchar *volstage_root, const gchar *runtime_root);                    

                                                                                                                    
                                                      

                                                                                            
                                                                                        
                                                           
#define PCV_LXC_RESIDUE_MAX_ATTEMPTS 64

                                                                                                        
                                                                                        
                                                                                                        
                                                                           
                                                                                   
#define PCV_LXC_VOLUME_STACK_SHARED_FMT \
    "stacked volume mounts at %s include a shared mount from before the upgrade and were left in place; unmounting them directly can detach host mounts — follow the GUIDE procedure \"pre-upgrade stacked volume mounts\""

   
                                
                                                                                               
                                                                     
                                               
  
                                                        
                                                                                                        
                                                                                             
                                                                                                    
                                                                                           
                                                                                     
                                                                                               
                                                         
                                                                                                       
                                                                                                          
                                                                                                 
                                                                             
                                                                     
                                                                                                                                            
                                                                                      
                                                                                                                                    
                                                                               
                                                                   
   
gboolean pcv_lxc_rootfs_residue_clear(const gchar *lxc_path, const gchar *name, guint *cleared, GError **error);

                                                                                                        
                                                                                                     
                                                                                                  
                                                                                                               
                                                                                       
                                                                           
                                                                                 

   
                                
                                                  
                               
  
                                                                             
                                                                                                  
                                                                                                                                
                                                
                                                                                                  
                                                                                      
                                                                                                                     
                                                                                                      
                                                                                                           
                                                                                                                               
                                                                                           
                                                                          
                                                                                                                 
                                                                                                                       
                                                                                                                                 
                                                                                          
                                           
                                                                                
                                    
   
gboolean pcv_lxc_volume_start_prepare(const gchar *lxc_path, const gchar *name, GError **error);

   
                              
                                                                                                        
                                                                                                       
                                              
   
void pcv_lxc_volume_start_abort(const gchar *lxc_path, const gchar *name);

   
                                 
                                                                                                      
                                                                                              
                                                                                             
                                            
   
void pcv_lxc_runtime_config_remove(const gchar *name);

                                                                                                           
                                                  
typedef gboolean (*PcvLxcStorageChildFn)(const gchar * const *argv, gchar **stderr_out, GError **error);

   
                                             
                                                                                           
                                     
   
void pcv_lxc_volume_set_storage_child_for_test(PcvLxcStorageChildFn fn);

   
                                    
                                                   
                                             
   
const PcvLxcMountOps *pcv_lxc_volume_real_ops_for_test(void);

   
                            
                                                                                           
                                                                          
                                                     
   
const PcvLxcMountOps *pcv_lxc_volume_mount_ops(void);

                                                                                                       
                                                                                                       
                                                                                            
                                                                                             
                                                                                     
                                                                  

                                                                                              
                                                                                                   
                                                                                             
                                                                            
typedef struct {
    const PcvLxcMountEntry *entry;                                               
    gchar      *dest;                                              
    guint       ordinal;
    gchar      *slot;                                               
    gboolean    ident_ok;
    PcvVolIdent ident;                                                               
} PcvLxcVolumeLine;

                                                                                                          
                                                                                
typedef struct { GPtrArray *entries; GPtrArray *lines; } PcvLxcVolumeSet;

   
                           
                                                                                                                       
                                                        
                                                                                      
                                                                                                                
               
                                                                                                       
                                                                                                   
                        
                                                                
                                                                          
   
PcvLxcVolumeSet *pcv_lxc_volume_set_load(const gchar *config_text, const gchar *name, const gchar *host_mountinfo);

   
                           
                                                           
                               
   
void             pcv_lxc_volume_set_free(PcvLxcVolumeSet *set);

   
                                
                                                                               
                                                                                       
                                                                                                          
                                                                                                           
                                                                    
   
gboolean pcv_lxc_volume_ident_from_fd(int fd, const gchar *canon, const gchar *host_mountinfo, PcvVolIdent *out);

                                                                                                           
                                                                                                              
                                                                                                
                                                                                          
                                                                                             
                                                                                          
                            
                                              
                                                                                
                         

   
                                   
                                                                                                                   
                                                                                                                
                                                                                                          
                                                                                                                             
                                                                                     
                                   
                                                                                                                              
                                               
                                                                              
   
PcvLxcVolumeSet *pcv_lxc_volume_set_load_running(const gchar *lxc_path, const gchar *name, const gchar *config_text,
                                                 const gchar *target_dest, GError **error);

   
                                     
                                                                                     
                                                                                                             
                                                                                    
                                                                                                      
                                                                                                                                                                                          
                                                                             
                                                                                                                       
                                                                                                                             
                                                                                                         
                                                                
                                                                                     
                                        
   
gboolean pcv_lxc_volume_unmount_attributed(const gchar *lxc_path, const gchar *name, const PcvLxcVolumeSet *set,
                                           const gchar *target_dest, GError **error);

   
                             
                                                                                                  
                                                                                                            
                                                                                                          
                                                                             
                                                                                                                      
                                 
                                                   
   
gboolean pcv_lxc_volume_unmount_id(const gchar *lxc_path, const gchar *name, guint64 mnt_id, GError **error);

   
                                  
                                                                                                       
                                                                                                     
                                                                                                  
                                        
                                                                                               
                     
                                                                                
                                                                                                      
                                               
   
gboolean pcv_lxc_volume_staging_unmount(const gchar *name, const gchar *target_dest, GError **error);

G_END_DECLS

#endif                                    
