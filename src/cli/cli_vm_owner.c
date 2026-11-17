   
                       
                                                                             
  
                           
                                                                                                                       
                                                                                                 
                              
                                                        
  
                                                                             
                    
   
#include "cli_vm_owner.h"
#include "cli_rpc.h"
#include "cli_output.h"

#include <glib.h>
#include <json-glib/json-glib.h>
#include <stdio.h>

                                          
static void
_owner_usage(void)
{
    printf("%sUsage: pcvctl vm owner <vm> <owner> [--expect <previous_owner>|--expect-none]%s\n",
           cc(CYBER_YELLOW), cc(CYBER_RESET));
}

                                                  
void
cmd_vm_owner(int argc, char *argv[])
{
    gboolean ok = argc >= 4 && argv[2][0] && argv[3][0];
    const gchar *expected = NULL;
    gboolean expect_none = FALSE;
    if (ok && argc == 5)
        ok = expect_none = g_strcmp0(argv[4], "--expect-none") == 0;
    else if (ok && argc == 6)
        ok = g_strcmp0(argv[4], "--expect") == 0 && argv[5][0] && (expected = argv[5]) != NULL;
    else if (ok && argc != 4)
        ok = FALSE;
    if (!ok) {
        _owner_usage();
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", argv[2]);
    json_object_set_string_member(params, "owner", argv[3]);
    if (expected)
        json_object_set_string_member(params, "expected_owner", expected);
    else if (expect_none)
        json_object_set_null_member(params, "expected_owner");
    GError *error = NULL;
    gchar *resp = purectl_send_request("vm.owner.set", params, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error);
        return;
    }
    if (resp) {
        print_action_response(resp, "VM_OWNER");
        g_free(resp);
    }
}

                                                          
                                                                     
                                                              
                                                            
void cmd_vm_inspect(int argc, char *argv[]) {
    gboolean valid = argc == 3 && g_uuid_string_is_valid(argv[2]);
    if (valid) {
        for (const gchar *p = argv[2]; *p; p++)
            if (g_ascii_isupper(*p)) valid = FALSE;
    }
    if (!valid) {
        g_printerr("Usage: pcvctl vm inspect <canonical-lowercase-uuid>\n");
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "uuid", argv[2]);
    GError *error = NULL;
    gchar *response = purectl_send_request("vm.inspect", params, &error);
    if (error) {
        g_printerr("VM inspection request failed: %s\n", error->message);
        g_error_free(error);
    }
    if (response) print_raw_response(response);
    g_free(response);
}
