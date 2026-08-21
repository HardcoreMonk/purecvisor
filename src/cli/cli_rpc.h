   
                  
                          
  
                           
                                                   
                                                    
                                        
  
                                             
  
                                                 
                                                  
                                                        
  
                                                     
                                                                 
                                                              
                                                      
                                                        
   
#ifndef CLI_RPC_H                         
#define CLI_RPC_H

#include <stdbool.h>                                   
#include <glib.h>                                                
#include <json-glib/json-glib.h>                                  
#include "purecvisor/version.h"                                    

                                             
                                            
#define DAEMON_SOCK_PATH "/var/run/purecvisor/daemon.sock"
#define PCVCTL_VERSION   PCV_PRODUCT_VERSION                                 

                                                                  
                                                      
                                                                
                                                            
#define CYBER_CYAN    "\x1b[38;5;51m"
#define CYBER_PINK    "\x1b[38;5;198m"
#define CYBER_YELLOW  "\x1b[38;5;226m"
#define CYBER_GREEN   "\x1b[38;5;46m"
#define CYBER_RED     "\x1b[38;5;196m"
#define CYBER_BLUE    "\x1b[38;5;33m"
#define CYBER_DIM     "\x1b[38;5;240m"
#define CYBER_RESET   "\x1b[0m"                                     
#define CYBER_BOLD    "\x1b[1m"                       

                                                                      
                                               
typedef enum {
    FMT_TABLE = 0,                                            
    FMT_JSON,                                  
    FMT_PLAIN,                                  
    FMT_CSV,                           
} OutputFormat;

                                     
                                                        
typedef enum {
    PCV_CLI_EXIT_OK      = 0,                        
    PCV_CLI_EXIT_RUNTIME = 1,                           
    PCV_CLI_EXIT_USAGE   = 2,                         
} PcvCliExitStatus;

                                                                   
                                                         
typedef struct {
    OutputFormat  fmt;                                             
    bool          interactive;                           
    bool          batch;                               
    bool          no_color;                                         
    bool          verbose;                               
    const char   *socket_path;                                         
    PcvCliExitStatus command_status;                            
} PcvCtx;

                                      
                                                              
extern PcvCtx g_ctx;

                                                  
                                             
                                                           
                                                       
const char *cc(const char *code);
const char *ce(const char *code);

                                                                 
void pcv_cli_command_begin(void);
                                                             
void pcv_cli_command_mark_success(void);
                                                  
void pcv_cli_command_mark_runtime_failure(void);
                                                 
PcvCliExitStatus pcv_cli_command_status(void);

   
                                                            
  
                                                    
                                               
  
                        
                                                                     
                                                    
                                                          
   
gchar *purectl_send_request(const gchar *method,
                            JsonObject  *params_obj,
                            GError     **error);

#endif                
