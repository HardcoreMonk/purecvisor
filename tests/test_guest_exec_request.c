   
                                  
                                                          
  
                           
                                                                                        
                                                                                        
                                                                
                                                                                         
                                                                       
                                                 
                                                                                           
                                                                                      
                                                 
                                                
  
                                                                     
                   
                                                          
   
#include <glib.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <libvirt/virterror.h>

#include "modules/dispatcher/guest_exec_request.h"

                                                              
                                  
static JsonObject *parse_request(JsonParser *parser, const gchar *json)
{
    g_assert_nonnull(json);
    GError *err = NULL;
    gboolean ok = json_parser_load_from_data(parser, json, -1, &err);
    g_assert_no_error(err);
    g_assert_true(ok);
    JsonNode *root = json_parser_get_root(parser);
    g_assert_true(root && JSON_NODE_HOLDS_OBJECT(root));
    return json_node_get_object(root);
}

                                                                      
                                                   
static void check_sh_roundtrip(const gchar *label, const gchar *command)
{
    g_test_message("sh case: %s", label);
    g_autofree gchar *json = pcv_guest_exec_request_json(PCV_GUEST_SHELL_SH, command);
    JsonParser *parser = json_parser_new();
    JsonObject *obj = parse_request(parser, json);
    g_assert_cmpstr(json_object_get_string_member(obj, "execute"), ==, "guest-exec");
    JsonObject *args = json_object_get_object_member(obj, "arguments");
    g_assert_nonnull(args);
    g_assert_cmpstr(json_object_get_string_member(args, "path"), ==, "/bin/sh");
    g_assert_true(json_object_get_boolean_member(args, "capture-output"));
    JsonArray *argv = json_object_get_array_member(args, "arg");
    g_assert_nonnull(argv);
    g_assert_cmpuint(json_array_get_length(argv), ==, 2);
    g_assert_cmpstr(json_array_get_string_element(argv, 0), ==, "-c");
    g_assert_cmpstr(json_array_get_string_element(argv, 1), ==, command);
    g_object_unref(parser);
}

                                                                              
                                          
static void test_json_sh_roundtrip(void)
{
    check_sh_roundtrip("plain", "uname -a");
    check_sh_roundtrip("double_quote", "echo \"hello world\"");
    check_sh_roundtrip("backslash", "printf 'a\\\\b\\n'");
    check_sh_roundtrip("newline", "echo one\necho two");
    check_sh_roundtrip("tab", "printf 'a\tb'");
    check_sh_roundtrip("control_0x01", "echo \x01 done");
    check_sh_roundtrip("hangul", "echo 한글 출력");
    check_sh_roundtrip("mixed", "cat <<'EOF'\n\"q\" \\ \t\x01 한글\nEOF");

    gchar *big = g_malloc(1025);                                          
    for (int i = 0; i < 1024; i++)
        big[i] = (i % 64 == 63) ? '\n' : (gchar)('a' + (i % 26));
    big[1024] = '\0';
    check_sh_roundtrip("1024_bytes", big);
    g_free(big);

    g_assert_null(pcv_guest_exec_request_json(PCV_GUEST_SHELL_SH, NULL));
}

                                             
                                                  
static void test_status_json(void)
{
    const gint64 pids[] = { 0, 3, 4242, G_GINT64_CONSTANT(2147483648) };
    for (guint i = 0; i < G_N_ELEMENTS(pids); i++) {
        g_autofree gchar *json = pcv_guest_exec_status_json(pids[i]);
        JsonParser *parser = json_parser_new();
        JsonObject *obj = parse_request(parser, json);
        g_assert_cmpstr(json_object_get_string_member(obj, "execute"), ==, "guest-exec-status");
        JsonObject *args = json_object_get_object_member(obj, "arguments");
        g_assert_nonnull(args);
        JsonNode *pid = json_object_get_member(args, "pid");
        g_assert_nonnull(pid);
        g_assert_cmpint(json_node_get_value_type(pid), ==, G_TYPE_INT64);
        g_assert_cmpint(json_node_get_int(pid), ==, pids[i]);
        g_object_unref(parser);
    }
}

                                                  
typedef struct {
    const gchar  *label;
    const gchar  *reply;
    PcvGuestShell shell;
    const gchar  *os_id;                         
} OsinfoCase;

                                                               
                                                        
static void test_shell_for_osinfo_table(void)
{
    static const OsinfoCase cases[] = {
        { "windows", "{\"return\":{\"id\":\"mswindows\",\"name\":\"Microsoft Windows\",\"version-id\":\"11\"}}",
          PCV_GUEST_SHELL_POWERSHELL, "mswindows" },
        { "ubuntu", "{\"return\":{\"id\":\"ubuntu\",\"name\":\"Ubuntu\"}}", PCV_GUEST_SHELL_SH, "ubuntu" },
        { "no_id", "{\"return\":{}}", PCV_GUEST_SHELL_SH, NULL },
        { "error_reply", "{\"error\":{\"class\":\"CommandNotFound\",\"desc\":\"The command guest-get-osinfo has not been found\"}}",
          PCV_GUEST_SHELL_SH, NULL },
        { "null", NULL, PCV_GUEST_SHELL_SH, NULL },
        { "broken_json", "{\"return\":{\"id\":\"mswindows\"", PCV_GUEST_SHELL_SH, NULL },
        { "upper_case", "{\"return\":{\"id\":\"MSWINDOWS\"}}", PCV_GUEST_SHELL_SH, "MSWINDOWS" },
        { "trailing_space", "{\"return\":{\"id\":\"mswindows \"}}", PCV_GUEST_SHELL_SH, "mswindows " },
        { "id_number", "{\"return\":{\"id\":7}}", PCV_GUEST_SHELL_SH, NULL },
        { "return_not_object", "{\"return\":\"mswindows\"}", PCV_GUEST_SHELL_SH, NULL },
    };
    for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
        const OsinfoCase *c = &cases[i];
        g_test_message("osinfo case: %s", c->label);
        gchar *os_id = (gchar *)"untouched";
        PcvGuestShell shell = pcv_guest_exec_shell_for_osinfo(c->reply, &os_id);
        g_assert_cmpint(shell, ==, c->shell);
        g_assert_cmpstr(os_id, ==, c->os_id);
        g_free(os_id);
        g_assert_cmpint(pcv_guest_exec_shell_for_osinfo(c->reply, NULL), ==, c->shell);                       
    }
}

                                                                                      
static gchar *decode_encoded_command(const gchar *b64)
{
    gsize n = 0;
    g_autofree guchar *utf16 = g_base64_decode(b64, &n);
    g_assert_cmpuint(n % 2, ==, 0);
    GError *err = NULL;
    gchar *script = g_convert((const gchar *)utf16, (gssize)n, "UTF-8", "UTF-16LE", NULL, NULL, &err);
    g_assert_no_error(err);
    return script;
}

                                                                             
static gchar *embedded_command(const gchar *script)
{
    const gchar *open = strstr(script, "FromBase64String('");
    g_assert_nonnull(open);
    open += strlen("FromBase64String('");
    const gchar *close = strchr(open, '\'');
    g_assert_nonnull(close);
    g_autofree gchar *b64 = g_strndup(open, (gsize)(close - open));
    gsize n = 0;
    guchar *raw = g_base64_decode(b64, &n);
    gchar *cmd = g_strndup((const gchar *)raw, n);
    g_free(raw);
    return cmd;
}

                                                                             
static gchar *canonical_wrapper(const gchar *command)
{
    g_autofree gchar *b64 = g_base64_encode((const guchar *)command, strlen(command));
    return g_strconcat(
        "[Console]::OutputEncoding=[System.Text.Encoding]::UTF8\n"
        "$ProgressPreference='SilentlyContinue'\n"
        "$global:LASTEXITCODE=0\n"
        "$pcvErr=$false\n"
        "try{$pcvSb=[ScriptBlock]::Create([System.Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('", b64,
        "')))}catch{$pcvX=$_.Exception;if($pcvX.InnerException){$pcvX=$pcvX.InnerException};"
        "[Console]::Error.WriteLine($pcvX.Message);exit 1}\n"
        "try{& $pcvSb 2>&1|ForEach-Object{if($_ -is [System.Management.Automation.ErrorRecord]){"
        "if(-not ($_.Exception -is [System.Management.Automation.RemoteException])){$pcvErr=$true};"
        "[Console]::Error.WriteLine($_.ToString())}else{$_}}|Out-String -Stream|ForEach-Object{[Console]::Out.WriteLine($_)}}"
        "catch{$pcvErr=$true;[Console]::Error.WriteLine($_.ToString())}\n"
        "if($LASTEXITCODE){exit $LASTEXITCODE}\n"
        "if($pcvErr){exit 1}\n"
        "exit 0", NULL);
}

                                                                                        
                                                                                     
                                                                                                  
                                                                        
static void test_json_powershell(void)
{
    static const gchar *commands[] = {
        "Write-Output '}'; exit 0 #pcv",
        "Write-Output 'it''s quoted'",
        "Write-Output \"say \\\"hi\\\" to $env:COMPUTERNAME\"",
        "Write-Output first-line\nWrite-Output second-line\tTAB",
        "Write-Output '한글 출력 확인'",
        "cmd /c dir \"C:\\Program Files\" /b",
    };
    static const gchar *flags[] = { "-NoLogo", "-NoProfile", "-NonInteractive", "-EncodedCommand" };
    for (guint i = 0; i < G_N_ELEMENTS(commands); i++) {
        const gchar *cmd = commands[i];
        g_test_message("powershell case %u", i);
        g_autofree gchar *json = pcv_guest_exec_request_json(PCV_GUEST_SHELL_POWERSHELL, cmd);
        JsonParser *parser = json_parser_new();
        JsonObject *obj = parse_request(parser, json);
        g_assert_cmpstr(json_object_get_string_member(obj, "execute"), ==, "guest-exec");
        JsonObject *args = json_object_get_object_member(obj, "arguments");
        g_assert_cmpstr(json_object_get_string_member(args, "path"), ==, "powershell.exe");
        g_assert_true(json_object_get_boolean_member(args, "capture-output"));
        JsonArray *argv = json_object_get_array_member(args, "arg");
        g_assert_cmpuint(json_array_get_length(argv), ==, 5);
        for (guint k = 0; k < G_N_ELEMENTS(flags); k++)
            g_assert_cmpstr(json_array_get_string_element(argv, k), ==, flags[k]);
        g_autofree gchar *script = decode_encoded_command(json_array_get_string_element(argv, 4));
        g_autofree gchar *direct = pcv_guest_exec_powershell_script(cmd);
        g_assert_cmpstr(script, ==, direct);

        g_assert_null(strstr(script, cmd));                                                 
        g_autofree gchar *inner = embedded_command(script);
        g_assert_cmpstr(inner, ==, cmd);                                              
        g_assert_true(g_str_has_prefix(script, "[Console]::OutputEncoding=[System.Text.Encoding]::UTF8\n"));                 
        g_assert_nonnull(strstr(script, "[ScriptBlock]::Create("));
        g_assert_nonnull(strstr(script, "$pcvX.InnerException"));
        g_assert_nonnull(strstr(script, "-is [System.Management.Automation.ErrorRecord]"));
        g_assert_nonnull(strstr(script, "[Console]::Error.WriteLine($_.ToString())"));
                                                                                                     
        g_assert_nonnull(strstr(script, "{if(-not ($_.Exception -is [System.Management.Automation.RemoteException]))"
                                        "{$pcvErr=$true};[Console]::Error.WriteLine($_.ToString())}"));
                                                                                    
        const gchar *outer_try = strstr(script, "try{& $pcvSb");
        const gchar *outer_catch = strstr(script, "}catch{$pcvErr=$true;");
        g_assert_nonnull(outer_try);
        g_assert_nonnull(outer_catch);
        g_assert_true(outer_try < outer_catch);
        const gchar *lastexit = strstr(script, "if($LASTEXITCODE){exit $LASTEXITCODE}");
        const gchar *cmdlet_err = strstr(script, "if($pcvErr){exit 1}");
        g_assert_nonnull(lastexit);
        g_assert_nonnull(cmdlet_err);
        g_assert_true(lastexit < cmdlet_err);                                                         
        g_autofree gchar *canon = canonical_wrapper(cmd);
        g_assert_cmpstr(script, ==, canon);
        g_object_unref(parser);
    }
    g_assert_null(pcv_guest_exec_powershell_script(NULL));
    g_assert_null(pcv_guest_exec_request_json(PCV_GUEST_SHELL_POWERSHELL, NULL));
}

                                                                                      
                                                     
static void test_powershell_cmdline_bound(void)
{
    gchar big[1025];
    for (int i = 0; i < 1024; i++)
        big[i] = (gchar)('a' + (i % 26));
    big[1024] = '\0';
    g_autofree gchar *json = pcv_guest_exec_request_json(PCV_GUEST_SHELL_POWERSHELL, big);
    JsonParser *parser = json_parser_new();
    JsonObject *args = json_object_get_object_member(parse_request(parser, json), "arguments");
    gsize total = strlen(json_object_get_string_member(args, "path"));
    JsonArray *argv = json_object_get_array_member(args, "arg");
    for (guint i = 0; i < json_array_get_length(argv); i++)
        total += 1 + strlen(json_array_get_string_element(argv, i));                   
    g_test_message("powershell command line for 1024-byte command: %" G_GSIZE_FORMAT " chars", total);
    g_assert_cmpuint(total, <, 32767);
    g_object_unref(parser);
}

                                                                      
static void test_osinfo_error_fatal_table(void)
{
    g_assert_cmpint(VIR_ERR_AGENT_UNRESPONSIVE, ==, 86);                                       
    g_assert_cmpint(VIR_ERR_AGENT_COMMAND_TIMEOUT, ==, 112);
    g_assert_cmpint(VIR_ERR_OPERATION_TIMEOUT, ==, 68);
    g_assert_cmpint(VIR_ERR_AGENT_UNSYNCED, ==, 97);
    g_assert_true(pcv_guest_exec_osinfo_error_is_fatal(VIR_ERR_AGENT_UNRESPONSIVE));
    g_assert_true(pcv_guest_exec_osinfo_error_is_fatal(VIR_ERR_AGENT_COMMAND_TIMEOUT));
    g_assert_true(pcv_guest_exec_osinfo_error_is_fatal(VIR_ERR_OPERATION_TIMEOUT));
    g_assert_true(pcv_guest_exec_osinfo_error_is_fatal(VIR_ERR_AGENT_UNSYNCED));
    g_assert_false(pcv_guest_exec_osinfo_error_is_fatal(VIR_ERR_AGENT_COMMAND_FAILED));                           
    g_assert_false(pcv_guest_exec_osinfo_error_is_fatal(VIR_ERR_INTERNAL_ERROR));
    g_assert_false(pcv_guest_exec_osinfo_error_is_fatal(VIR_ERR_OK));
    g_assert_false(pcv_guest_exec_osinfo_error_is_fatal(-1));
}

                                                        
typedef struct {
    const gchar *label;
    gint64       elapsed_us;                   
    gint         timeout;
    gboolean     may_poll;
} BudgetCase;

                                                                                                
                                                             
static void test_budget_table(void)
{
    static const BudgetCase cases[] = {
        { "start",            0,                          15, TRUE  },
        { "half_second",      500 * 1000,                 14, TRUE  },                         
        { "ten_seconds",      10 * G_USEC_PER_SEC,         5, TRUE  },
        { "fourteen_and_half", 14 * G_USEC_PER_SEC + 500 * 1000, 1, TRUE },                                      
        { "poll_boundary",    14 * G_USEC_PER_SEC + 800 * 1000, 1, TRUE },                      
        { "poll_stop",        14 * G_USEC_PER_SEC + 850 * 1000, 1, FALSE },                          
        { "over_budget",      20 * G_USEC_PER_SEC,         1, FALSE },
        { "clock_backwards",  -5 * G_USEC_PER_SEC,        15, TRUE  },                 
    };
    const gint64 start = G_GINT64_CONSTANT(1000000000);
    g_assert_cmpint(PCV_GUEST_EXEC_BUDGET_SEC, ==, 15);
    g_assert_cmpint(PCV_GUEST_EXEC_POLL_INTERVAL_US, ==, 200 * 1000);
    for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
        const BudgetCase *c = &cases[i];
        g_test_message("budget case: %s", c->label);
        g_assert_cmpint(pcv_guest_exec_call_timeout(start, start + c->elapsed_us), ==, c->timeout);
        g_assert_cmpint(pcv_guest_exec_may_poll(start, start + c->elapsed_us), ==, c->may_poll);
    }
}

                                                  
typedef struct {
    const gchar  *label;
    const guchar *bytes;
    gsize         len;
    const gchar  *text;                             
    gsize         text_len;
    const gchar  *b64;                             
} OutputCase;

                                                                                       
                                                                                   
static void test_output_bytes_table(void)
{
    static const guchar utf8[] = "hello 한글\n";
    static const guchar cp949[] = { 0xc7, 0xd1, 0xb1, 0xdb };                           
    static const guchar with_nul[] = { 'a', 0x00, 'b' };
    static const OutputCase cases[] = {
        { "utf8", utf8, sizeof(utf8) - 1, "hello 한글\n", sizeof(utf8) - 1, NULL },
        { "cp949", cp949, sizeof(cp949), "\xef\xbf\xbd\xd1\xb1\xef\xbf\xbd", 8, "x9Gx2w==" },                            
        { "nul", with_nul, sizeof(with_nul), "a\xef\xbf\xbd" "b", 5, "YQBi" },                          
        { "empty", NULL, 0, "", 0, NULL },
    };
    for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
        const OutputCase *c = &cases[i];
        g_test_message("output case: %s", c->label);
        JsonObject *res = json_object_new();
        pcv_guest_exec_set_output(res, "stdout", "stdout_b64", c->bytes, c->len);
        JsonNode *node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(node, res);
        g_autofree gchar *wire = json_to_string(node, FALSE);
        json_node_unref(node);
        JsonParser *parser = json_parser_new();
        JsonObject *back = parse_request(parser, wire);                                      
        const gchar *text = json_object_get_string_member(back, "stdout");
        g_assert_nonnull(text);
        g_assert_true(g_utf8_validate(text, -1, NULL));
        g_assert_cmpuint(strlen(text), ==, c->text_len);                                       
        g_assert_cmpmem(text, strlen(text), c->text, c->text_len);
        if (c->b64)
            g_assert_cmpstr(json_object_get_string_member(back, "stdout_b64"), ==, c->b64);
        else
            g_assert_false(json_object_has_member(back, "stdout_b64"));
        g_object_unref(parser);
    }
}

                                                                      
void test_guest_exec_request_register(void)
{
    g_test_add_func("/guest_exec/request/json_sh_roundtrip", test_json_sh_roundtrip);
    g_test_add_func("/guest_exec/request/status_json", test_status_json);
    g_test_add_func("/guest_exec/request/shell_for_osinfo_table", test_shell_for_osinfo_table);
    g_test_add_func("/guest_exec/request/json_powershell", test_json_powershell);
    g_test_add_func("/guest_exec/request/powershell_cmdline_bound", test_powershell_cmdline_bound);
    g_test_add_func("/guest_exec/request/osinfo_error_fatal_table", test_osinfo_error_fatal_table);
    g_test_add_func("/guest_exec/request/budget_table", test_budget_table);
    g_test_add_func("/guest_exec/request/output_bytes_table", test_output_bytes_table);
}
