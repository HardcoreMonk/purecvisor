   
                      
                                                 
  
                                                              
                                                                    
  
                           
                                                                        
                                                       
                                                                  
                                                
  
                       
                                                       
                                                     
   
#pragma once

#include <gio/gio.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

typedef struct _UdsServer UdsServer;

   
                           
                                                                    
                                                                  
  
                                                           
   
void handle_vpc_list(JsonObject *, const gchar *, UdsServer *, GSocketConnection *);
void handle_vpc_get(JsonObject *, const gchar *, UdsServer *, GSocketConnection *);
void handle_vpc_subnet_list(JsonObject *, const gchar *, UdsServer *, GSocketConnection *);
void handle_vpc_attachment_list(JsonObject *, const gchar *, UdsServer *, GSocketConnection *);
void handle_vpc_service_list(JsonObject *, const gchar *, UdsServer *, GSocketConnection *);
void handle_vpc_status(JsonObject *, const gchar *, UdsServer *, GSocketConnection *);
void handle_vpc_backend_list(JsonObject *, const gchar *, UdsServer *, GSocketConnection *);

   
                           
                                                                   
                                                                        
                                                                          
  
                                                      
                                      
   
void handle_vpc_create(JsonObject *, const gchar *, UdsServer *, GSocketConnection *);
void handle_vpc_delete(JsonObject *, const gchar *, UdsServer *, GSocketConnection *);
void handle_vpc_egress_set(JsonObject *, const gchar *, UdsServer *, GSocketConnection *);
void handle_vpc_subnet_create(JsonObject *, const gchar *, UdsServer *, GSocketConnection *);
void handle_vpc_subnet_delete(JsonObject *, const gchar *, UdsServer *, GSocketConnection *);
void handle_vpc_attachment_create(JsonObject *, const gchar *, UdsServer *, GSocketConnection *);
void handle_vpc_attachment_delete(JsonObject *, const gchar *, UdsServer *, GSocketConnection *);
void handle_vpc_service_publish(JsonObject *, const gchar *, UdsServer *, GSocketConnection *);
void handle_vpc_service_unpublish(JsonObject *, const gchar *, UdsServer *, GSocketConnection *);
void handle_vpc_reconcile(JsonObject *, const gchar *, UdsServer *, GSocketConnection *);

G_END_DECLS
