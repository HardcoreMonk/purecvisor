   
                             
                                                                                                          
  
                           
                                                                                           
                                                                                             
                                                                        
                                                                            
                                                                                           
  
                                                                    
                                                                  
   
#include "cli_security_close.h"
#include "cli_rpc.h"
#include "cli_output.h"

#include <glib.h>
#include <json-glib/json-glib.h>
#include <stdio.h>

                                                       
static void
_close_usage(void)
{
    printf("%sUsage: pcvctl security close <event_id> --reason TEXT%s\n",
           cc(CYBER_YELLOW), cc(CYBER_RESET));
}

void
cmd_security_close(int argc, char *argv[])
{
    const char *reason = NULL;
    for (int i = 3; i < argc; i++) {
        if (g_strcmp0(argv[i], "--reason") == 0 && i + 1 < argc)
            reason = argv[++i];
    }
                                                                                  
    if (argc < 3 || argv[2][0] == '-' || !reason) {
        _close_usage();
        return;
    }
    JsonObject *params = json_object_new();                                          
    json_object_set_string_member(params, "event_id", argv[2]);
    json_object_set_string_member(params, "reason", reason);
    GError *error = NULL;
    gchar *resp = purectl_send_request("security.event.close", params, &error);
    if (error) {                                                       
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error);
        return;
    }
    print_action_response(resp, "SECURITY_CLOSE");
    g_free(resp);
}
