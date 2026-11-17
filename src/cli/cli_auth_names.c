   
                         
                                                                                                              
  
                           
                                                                                                  
                                                                                      
                                                                                       
                                                                                                  
                                                                                                  
                                                                                               
                                                             
  
                                                                           
                                                                                  
   
#include "cli_auth_names.h"
#include "cli_rpc.h"
#include "cli_output.h"

#include <glib.h>
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <string.h>

                                                                                         
                                                                
                           
static void
_print_result(gchar *resp, GError *error, const gchar *action)
{
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error);
        return;
    }
    print_action_response(resp, action);
    g_free(resp);
}

   
                   
                                                                                               
               
  
                                                           
                                                                                  
   
void
cmd_auth_create(int argc, char *argv[])
{
    const char *pos[3] = { NULL, NULL, NULL };
    int npos = 0;
    gboolean reuse = FALSE;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--reuse-deleted-name") == 0)
            reuse = TRUE;
        else if (npos < 3)
            pos[npos++] = argv[i];
    }
    if (npos < 3) {
        printf("%sUsage: pcvctl auth create <username> <password> <role> [--reuse-deleted-name]%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();                                         
    json_object_set_string_member(params, "username", pos[0]);
    json_object_set_string_member(params, "password", pos[1]);
    json_object_set_string_member(params, "role",     pos[2]);
    if (reuse)                                                                  
        json_object_set_boolean_member(params, "reuse_deleted_name", TRUE);
    GError *error = NULL;
    gchar *resp = purectl_send_request("auth.user.create", params, &error);
    _print_result(resp, error, "AUTH_USER_CREATE");
}

                                                          
void
cmd_auth_retire_name(int argc, char *argv[])
{
    if (argc != 3) {
        printf("%sUsage: pcvctl auth retire-name <name>%s\n", cc(CYBER_YELLOW), cc(CYBER_RESET));
        printf("Example: pcvctl auth retire-name former-operator\n");
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "username", argv[2]);
    GError *error = NULL;
    gchar *resp = purectl_send_request("auth.deleted_name.add", params, &error);
    _print_result(resp, error, "AUTH_RETIRE_NAME");
}
