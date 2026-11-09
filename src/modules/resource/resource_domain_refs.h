                                                                             
                                              
                                                                                               
  
                           
                                                                                       
                                                                                          
                       
                                                                                                           
                                                                                                              
                                                                                      
                                                                                        
                                                                                       
                                                                                            
                                                                                           
                                                                        
  
                                                                                  
                                                     
                                                                                
#ifndef PURECVISOR_RESOURCE_DOMAIN_REFS_H
#define PURECVISOR_RESOURCE_DOMAIN_REFS_H

#include <glib.h>

#include "modules/resource/resource_registry.h"

G_BEGIN_DECLS

                                   
typedef struct {
    PcvResKind kind;
    gchar     *ref;
} PcvResRef;

                                                                                                
typedef struct {
    gchar     *owner;
    GPtrArray *refs;
} PcvResDomainRefs;

   
                                                                                          
                                                                        
                                               
   
gboolean pcv_res_domain_refs_parse(const gchar *xml, PcvResDomainRefs *out, GError **error);

   
                                                                                                
                                               
   
void pcv_res_domain_refs_merge(PcvResDomainRefs *into, const PcvResDomainRefs *from);

                                                                                 
void pcv_res_domain_refs_clear(PcvResDomainRefs *r);

   
                                                                                        
                                       
                                              
   
gboolean pcv_res_domain_refs_uses(const PcvResDomainRefs *r, PcvResKind kind, const gchar *item_ref);

   
                                                                                                           
                                                                                         
                                                                                          
                                                                                              
                                                            
                                                                   
   
void pcv_res_domain_refs_canonicalize(PcvResDomainRefs *r);

                                                                                                         
typedef enum {
    PCV_RES_DOM_ACTIVE,
    PCV_RES_DOM_INACTIVE,
    PCV_RES_DOM_GONE,
    PCV_RES_DOM_UNKNOWN
} PcvResDomState;

   
                                                                                                                 
                                                                                                               
                                                                                   
                                                            
                                                              
   
PcvResDomState pcv_res_domain_active_state(int is_active_rc, gboolean no_domain_error);

G_END_DECLS

#endif                                        
