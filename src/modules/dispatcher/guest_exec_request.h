   
                             
                                                           
  
                           
                                                                                            
                                                                  
                                                                                     
                                                                   
                                                                 
  
                                                                 
                             
  
            
                                                                                                                        
  
                                   
   
#ifndef PURECVISOR_DISPATCHER_GUEST_EXEC_REQUEST_H
#define PURECVISOR_DISPATCHER_GUEST_EXEC_REQUEST_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

                                                                           
#define PCV_GUEST_EXEC_OSINFO_TIMEOUT_SEC 5
                                                              
                                                                           
#define PCV_GUEST_EXEC_BUDGET_SEC 15
                                                               
                                                            
#define PCV_GUEST_EXEC_POLL_INTERVAL_US (200 * 1000)

   
                                  
                                          
   
typedef enum {
    PCV_GUEST_SHELL_SH,
    PCV_GUEST_SHELL_POWERSHELL,
} PcvGuestShell;

   
                                                                                                             
                                                                                   
                                                                                                                              
                                                                                  
                                                                
   
gchar *pcv_guest_exec_request_json(PcvGuestShell shell, const gchar *command);

   
                                                                                                   
                                                                                      
                                                                            
                                                           
   
PcvGuestShell pcv_guest_exec_shell_for_osinfo(const gchar *osinfo_reply, gchar **os_id_out);

   
                                                                                            
                                                                                
                                                                                              
                                                                                                   
                              
                                                         
   
gchar *pcv_guest_exec_powershell_script(const gchar *command);

   
                                                                                                   
                                                                                                                   
                                                                               
                                                                
   
gboolean pcv_guest_exec_osinfo_error_is_fatal(int virerr_code);

   
                                                                                             
                                  
   
gchar *pcv_guest_exec_status_json(gint64 pid);

   
                                                                                                       
                                                                                  
                                                                                                      
                                                                            
                                          
   
gint pcv_guest_exec_call_timeout(gint64 start_us, gint64 now_us);

   
                                                                                               
                                                                       
                                 
   
gboolean pcv_guest_exec_may_poll(gint64 start_us, gint64 now_us);

   
                                                                                                
                                                                                     
                                                                                    
                                                                    
   
void pcv_guest_exec_set_output(JsonObject *res, const gchar *text_key, const gchar *b64_key,
                               const guchar *bytes, gsize len);

G_END_DECLS

#endif                                                 
