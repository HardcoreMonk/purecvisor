   
                             
                                                                                              
                              
  
                           
                                                                                               
                                                         
                                                                                 
                                                                                   
                                                                                                        
                                                                                       
                                                                                        
                                                                                      
                                                                        
                                                                                     
                                                                                              
                                                                                          
                                                     
  
                                                                       
               
  
                                                                                 
                                                               
                                                                          
                                                                                     
                                                
   
#include <json-glib/json-glib.h>
#include <libvirt/virterror.h>
#include <string.h>

#include "modules/dispatcher/guest_exec_request.h"

                                           
                                        
static gchar *builder_to_string(JsonBuilder *builder)
{
    JsonNode *root = json_builder_get_root(builder);
    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, root);
    gchar *out = json_generator_to_data(gen, NULL);
    g_object_unref(gen);
    json_node_unref(root);
    g_object_unref(builder);
    return out;
}

                                                                              
                                       
static JsonBuilder *begin_command(const gchar *execute)
{
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "execute");
    json_builder_add_string_value(b, execute);
    json_builder_set_member_name(b, "arguments");
    json_builder_begin_object(b);
    return b;
}

                                                           
static gchar *end_command(JsonBuilder *b)
{
    json_builder_end_object(b);                  
    json_builder_end_object(b);             
    return builder_to_string(b);
}

                                                                                                 
                                        
                                                            
                                                                            
                                                                                           
                                                                             
                                                                     
                                                                                                          
                                                                              
                                                                        
                                     
static const gchar k_ps_head[] =
    "[Console]::OutputEncoding=[System.Text.Encoding]::UTF8\n"
    "$ProgressPreference='SilentlyContinue'\n"
    "$global:LASTEXITCODE=0\n"
    "$pcvErr=$false\n"
    "try{$pcvSb=[ScriptBlock]::Create([System.Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('";
static const gchar k_ps_tail[] =
    "')))}catch{$pcvX=$_.Exception;if($pcvX.InnerException){$pcvX=$pcvX.InnerException};"
    "[Console]::Error.WriteLine($pcvX.Message);exit 1}\n"
    "try{& $pcvSb 2>&1|ForEach-Object{if($_ -is [System.Management.Automation.ErrorRecord]){"
    "if(-not ($_.Exception -is [System.Management.Automation.RemoteException])){$pcvErr=$true};"
    "[Console]::Error.WriteLine($_.ToString())}else{$_}}|Out-String -Stream|ForEach-Object{[Console]::Out.WriteLine($_)}}"
    "catch{$pcvErr=$true;[Console]::Error.WriteLine($_.ToString())}\n"
    "if($LASTEXITCODE){exit $LASTEXITCODE}\n"
    "if($pcvErr){exit 1}\n"
    "exit 0";

                                                                                          
static const gchar *const k_ps_flags[] = { "-NoLogo", "-NoProfile", "-NonInteractive", "-EncodedCommand" };

                                                                             
gchar *pcv_guest_exec_powershell_script(const gchar *command)
{
    if (!command)
        return NULL;
    g_autofree gchar *b64 = g_base64_encode((const guchar *)command, strlen(command));
    return g_strconcat(k_ps_head, b64, k_ps_tail, NULL);
}

                                                                                            
static gchar *powershell_encoded(const gchar *command)
{
    g_autofree gchar *script = pcv_guest_exec_powershell_script(command);
    gsize n = 0;
    g_autofree gchar *utf16 = script ? g_convert(script, -1, "UTF-16LE", "UTF-8", NULL, &n, NULL) : NULL;
    return utf16 ? g_base64_encode((const guchar *)utf16, n) : NULL;
}

                                                                                                  
                                                                  
gchar *pcv_guest_exec_request_json(PcvGuestShell shell, const gchar *command)
{
    if (!command || (shell != PCV_GUEST_SHELL_SH && shell != PCV_GUEST_SHELL_POWERSHELL))
        return NULL;
    g_autofree gchar *encoded = shell == PCV_GUEST_SHELL_POWERSHELL ? powershell_encoded(command) : NULL;
    if (shell == PCV_GUEST_SHELL_POWERSHELL && !encoded)
        return NULL;
    JsonBuilder *b = begin_command("guest-exec");
    json_builder_set_member_name(b, "path");
    json_builder_add_string_value(b, shell == PCV_GUEST_SHELL_SH ? "/bin/sh" : "powershell.exe");
    json_builder_set_member_name(b, "arg");
    json_builder_begin_array(b);
    if (shell == PCV_GUEST_SHELL_SH) {
        json_builder_add_string_value(b, "-c");
        json_builder_add_string_value(b, command);
    } else {
        for (gsize i = 0; i < G_N_ELEMENTS(k_ps_flags); i++)
            json_builder_add_string_value(b, k_ps_flags[i]);
        json_builder_add_string_value(b, encoded);
    }
    json_builder_end_array(b);
    json_builder_set_member_name(b, "capture-output");
    json_builder_add_boolean_value(b, TRUE);
    return end_command(b);
}

                                                                         
gchar *pcv_guest_exec_status_json(gint64 pid)
{
    JsonBuilder *b = begin_command("guest-exec-status");
    json_builder_set_member_name(b, "pid");
    json_builder_add_int_value(b, pid);
    return end_command(b);
}

                                                                            
                                                               
PcvGuestShell pcv_guest_exec_shell_for_osinfo(const gchar *osinfo_reply, gchar **os_id_out)
{
    if (os_id_out)
        *os_id_out = NULL;
    if (!osinfo_reply)
        return PCV_GUEST_SHELL_SH;
    PcvGuestShell shell = PCV_GUEST_SHELL_SH;
    JsonParser *parser = json_parser_new();
    if (json_parser_load_from_data(parser, osinfo_reply, -1, NULL)) {
        JsonNode *root = json_parser_get_root(parser);
        JsonObject *obj = (root && JSON_NODE_HOLDS_OBJECT(root)) ? json_node_get_object(root) : NULL;
        JsonNode *ret = obj ? json_object_get_member(obj, "return") : NULL;
        JsonObject *ret_obj = (ret && JSON_NODE_HOLDS_OBJECT(ret)) ? json_node_get_object(ret) : NULL;
        JsonNode *id = ret_obj ? json_object_get_member(ret_obj, "id") : NULL;
        if (id && JSON_NODE_HOLDS_VALUE(id) && json_node_get_value_type(id) == G_TYPE_STRING) {
            const gchar *s = json_node_get_string(id);
            if (g_strcmp0(s, "mswindows") == 0)
                shell = PCV_GUEST_SHELL_POWERSHELL;
            if (os_id_out)
                *os_id_out = g_strdup(s);
        }
    }
    g_object_unref(parser);
    return shell;
}

                                                                                 
                                      
gboolean pcv_guest_exec_osinfo_error_is_fatal(int virerr_code)
{
    switch (virerr_code) {
    case VIR_ERR_AGENT_UNRESPONSIVE:                                
    case VIR_ERR_AGENT_COMMAND_TIMEOUT:                     
    case VIR_ERR_OPERATION_TIMEOUT:                                 
    case VIR_ERR_AGENT_UNSYNCED:                                 
        return TRUE;
    default:
        return FALSE;
    }
}

                                                                               
#define GUEST_EXEC_CALL_TIMEOUT_MAX_SEC 30

                                                                  
static gint64 budget_left_us(gint64 start_us, gint64 now_us)
{
    gint64 elapsed = now_us > start_us ? now_us - start_us : 0;
    return (gint64)PCV_GUEST_EXEC_BUDGET_SEC * G_USEC_PER_SEC - elapsed;
}

                                                                            
                                  
gint pcv_guest_exec_call_timeout(gint64 start_us, gint64 now_us)
{
    gint64 left_sec = budget_left_us(start_us, now_us) / G_USEC_PER_SEC;
    return (gint)MAX(1, MIN(GUEST_EXEC_CALL_TIMEOUT_MAX_SEC, left_sec));
}

                                                              
gboolean pcv_guest_exec_may_poll(gint64 start_us, gint64 now_us)
{
    return budget_left_us(start_us, now_us) >= PCV_GUEST_EXEC_POLL_INTERVAL_US;
}

                                                                                     
                                                                          
                                                                         
void pcv_guest_exec_set_output(JsonObject *res, const gchar *text_key, const gchar *b64_key,
                               const guchar *bytes, gsize len)
{
    const gchar *src = bytes ? (const gchar *)bytes : "";
    gsize n = bytes ? len : 0;
    g_autofree gchar *text = g_utf8_make_valid(src, (gssize)n);
    json_object_set_string_member(res, text_key, text);
    if (n > 0 && !g_utf8_validate_len(src, n, NULL)) {
        g_autofree gchar *b64 = g_base64_encode(bytes, n);
        json_object_set_string_member(res, b64_key, b64);
    }
}
