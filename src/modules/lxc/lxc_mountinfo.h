                                                                             
                                  
                                                                                             
  
                           
                                                                               
                                                                                          
                                                                                   
                                                                                   
                                                                                          
                                
                                                           
  
                                                                               
                                                                                 
                                                                                
#ifndef PURECVISOR_LXC_MOUNTINFO_H
#define PURECVISOR_LXC_MOUNTINFO_H

#include <glib.h>

G_BEGIN_DECLS

#define PCV_MOUNTINFO_PATH_MAX 4096

                                                                                  
typedef struct {
    guint64     mnt_id;                            
    guint64     parent_id;                                                                                       
                                                                                    
    guint       dev_major, dev_minor;              
    const char *root;                                                                         
    const char *mount_point;                             
    const char *options;                           
    const char *optional;                                                               
} PcvMountinfoLine;

                                                                                          
                                                                                          
typedef struct { guint major, minor; const char *root; } PcvVolIdent;

#define PCV_VOL_ATTR_NONE      (-1)                      
#define PCV_VOL_ATTR_AMBIGUOUS (-2)                                              

   
                       
                                                                              
                                                                                           
                                                               
   
gsize    pcv_mountinfo_parse(char *text, PcvMountinfoLine *out, gsize cap, gboolean *truncated);

   
                            
                                                                             
                                 
                                                                                                 
                                                                             
   
gssize   pcv_mountinfo_last_under(const PcvMountinfoLine *lines, gsize n, const char *prefix,
                                  const guint64 *skip_ids, gsize n_skip);

   
                       
                                                                                                      
                                                                                                
                                                                                                   
                     
                                                                        
   
void     pcv_mountinfo_chain(const PcvMountinfoLine *lines, const gsize *members, gsize m, guint *depth_out, guint64 *top_out);

   
                               
                                                                                                 
                                                                                                     
                                                                                                       
                                                                                
   
gsize    pcv_mountinfo_stack_visible(const PcvMountinfoLine *lines, const gsize *members, gsize m, gsize k, gboolean *covered_shared);

   
                           
                                                                                             
                                                
   
gboolean pcv_mountinfo_chain_has(const PcvMountinfoLine *lines, const gsize *members, gsize m, gsize k, const guint64 *ids, gsize n_ids);

   
                        
                                                                                    
                                                                                       
                                                      
   
gssize   pcv_mountinfo_top_at(const PcvMountinfoLine *lines, gsize n, const char *path);

                                                                                                
                                                             
gboolean pcv_mountinfo_path_normalize(const char *in, char *out, gsize cap);
                                                                                                     
gboolean pcv_mountinfo_path_equal(const char *a, const char *b);
                                                                    
gboolean pcv_mountinfo_parse_dev(const char *s, guint *major, guint *minor);
                                                                           
gsize    pcv_mountinfo_fmt_u64(guint64 v, char *buf, gsize cap);

                                                                                                                        
                                          
gsize    pcv_vol_ident_root_cap(const char *fs_root, const char *canon_source);

   
                            
                                                                                              
                                                                                                         
                                                                                 
                                                                 
   
gboolean pcv_vol_ident_from_mount(guint major, guint minor, const char *fs_root, const char *mount_point,
                                  const char *canon_source, char *root_buf, gsize root_cap, PcvVolIdent *out);

                                                                                                      
                                                                                             
gboolean pcv_vol_ident_covers(const PcvVolIdent *id, guint major, guint minor, const char *root);

                                                                                                          
                                                                                                                           
                                                                                     
gboolean pcv_vol_dest_equal(const char *a, const char *b);

   
                     
                                                                                                          
                                                   
                                                                                                
                                                                                                       
                                                                                                         
                                                          
                                                                        
   
gint     pcv_vol_attribute(const PcvVolIdent *const *idents, const char *const *dests, gsize n,
                           guint major, guint minor, const char *root, const char *mount_dest);

   
                            
                                                                                                              
                                                                                                      
                                                                                                                      
                                                                    
                                                                      
   
gboolean pcv_proc_cgroup_v2_under(const char *text, const char *cgroup);

G_END_DECLS

#endif                                 
