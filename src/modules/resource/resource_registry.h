                                                                             
                                           
                                                                                     
  
                           
                                                                                                      
                                                                                 
                                                                        
                                                                                                          
                                                                       
                                                                                          
                                                           
                                                                                                            
                                                                                    
                                                                          
                                   
  
                                                                            
                                                                         
                                                                           
                                                                                
#ifndef PURECVISOR_RESOURCE_REGISTRY_H
#define PURECVISOR_RESOURCE_REGISTRY_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

                                                                                
typedef enum {
    PCV_RES_IMAGE,
    PCV_RES_ISO,
    PCV_RES_PCI,
    PCV_RES_USB,
    PCV_RES_STORAGE,
    PCV_RES_BRIDGE
} PcvResKind;

#define PCV_RES_ERROR (pcv_res_error_quark())
                                                                                       
GQuark pcv_res_error_quark(void);

                                                                                   
typedef enum {
    PCV_RES_ERROR_INVALID,
    PCV_RES_ERROR_NOT_FOUND,
    PCV_RES_ERROR_CONFLICT,
    PCV_RES_ERROR_IO
} PcvResError;

#define PCV_RES_DEFAULT_DB_PATH "/var/lib/purecvisor/resource_registry.db"

                                                         
const gchar *pcv_res_kind_to_string(PcvResKind kind);
gboolean pcv_res_kind_from_string(const gchar *s, PcvResKind *out);

                                                                   
gboolean pcv_res_name_is_valid(const gchar *name);

   
              
                                                                                                  
                                                                                              
                                                                                
   
typedef struct {
    gchar   *ref;
    guint64  dev;
    guint64  ino;
    guint64  gen;
    gboolean has_identity;
} PcvResNorm;

                                                                                     
gboolean pcv_res_normalize(PcvResKind kind, const gchar *value, PcvResNorm *out, GError **error);
void pcv_res_norm_clear(PcvResNorm *n);

                                                                                         
gboolean pcv_res_registry_open(const gchar *db_path, GError **error);
void pcv_res_registry_close(void);

   
                                                                                  
                                                                                     
   
gboolean pcv_res_register(PcvResKind kind, const gchar *name, const gchar *value, const gchar *actor,
                          gboolean *refreshed_out, GError **error);
gboolean pcv_res_unregister(const gchar *name, GError **error);
gboolean pcv_res_grant(const gchar *name, const gchar *tenant, GError **error);
                                  
gboolean pcv_res_revoke(const gchar *name, const gchar *tenant, GError **error);

                                                                                  
gboolean pcv_res_lookup(const gchar *name, PcvResKind *kind_out, gchar **ref_out, GError **error);
                                                                                               
JsonArray *pcv_res_list(gboolean filter_kind, PcvResKind kind, GError **error);

   
                                                                                                
                                                                     
   
gboolean pcv_res_tenant_allows(PcvResKind kind, const PcvResNorm *norm, const gchar *tenant);
                                                              
gboolean pcv_res_covered(PcvResKind kind, const gchar *ref);

                                                                                                 
typedef struct {
    guint64 dev;
    guint64 ino;
    guint64 gen;
} PcvResPin;

   
                                                                                          
                                                                   
                                                                                                   
                                                                     
                                                                                                                                                       
                                                                          
   
gboolean pcv_res_pin_matches(const gchar *path, const PcvResPin *pin);

   
                                                                                        
                                                                                            
   
typedef struct {
    PcvResKind kind;
    gchar     *name;
    gchar     *ref;
    GPtrArray *tenants;
    guint      index;
} PcvResImportItem;

   
                                                                                     
                                                                                    
              
                                                        
   
gboolean pcv_res_import(GPtrArray *items, const gchar *actor, guint *registered_out, guint *granted_out,
                        GError **error);

   
                                                                                                      
                                                                                         
                                                            
   
gboolean pcv_res_vm_counts_for_revoke(const gchar *owner_tenant, const gchar *revoked_tenant);

   
                                                                                                            
                                                                                     
                                                                                            
                                                                                                  
                                                      
                                                                                     
   
gchar *pcv_res_unregistered_suggest_name(PcvResKind kind, const gchar *ref, GHashTable *taken);

                                                                       
                                                                                                  
                                                                                            
                                                                                         
                                                           

   
                                                                                                    
                                                                                                               
                                                                                     
                                                                                                                      
   
gboolean pcv_res_pool_cidr_is_valid(const gchar *cidr);

   
                                                                                                 
                                                                                         
                                                                                            
                                                               
                                                                
   
const gchar *pcv_res_pool_conflict(GPtrArray *pool, const gchar *cidr);

   
                                                                                               
                                                                                                                    
                                                             
   
gboolean pcv_res_pool_set(const gchar *tenant, GPtrArray *cidrs, GError **error);

   
                                                                                                    
                                                                                                 
                               
   
gboolean pcv_res_pool_add(const gchar *tenant, const gchar *cidr, GError **error);

   
                                                                                                     
                                                                                   
   
GPtrArray *pcv_res_pool_list(const gchar *tenant);

                                                                                                           
JsonArray *pcv_res_pool_list_all(void);

G_END_DECLS

#endif                                     
