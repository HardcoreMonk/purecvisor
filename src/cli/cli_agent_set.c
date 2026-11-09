   
                        
                                                                                                                   
  
                           
                                                                                                                   
                                                                                   
                                                                                                    
                                                                                       
                                                     
                                                                                
                                                                            
  
                                                                          
                                                                            
   
#include "cli_agent_set.h"
#include "cli_rpc.h"
#include "cli_output.h"
#include "modules/ai/ai_provider.h"

#include <glib.h>
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <string.h>

                                                       
static void
_agent_set_usage(void)
{
    printf("%sUsage: pcvctl agent set --provider <claude|openai|gemini|ollama>"
           " [--api_key KEY] [--model NAME] [--endpoint URL]%s\n"
           "%s  At least one value is required. A key enables the provider; --api_key \"\" disables it.%s\n",
           cc(CYBER_YELLOW), cc(CYBER_RESET), cc(CYBER_DIM), cc(CYBER_RESET));
}

                                                                               
static void
_agent_set_reject(const char *reason)
{
    g_printerr("%s[!] agent set: %s%s\n", ce(CYBER_RED), reason, ce(CYBER_RESET));
    _agent_set_usage();
}

                                                                                 
static const char *
_agent_provider_canonical(const char *name)
{
    for (gint j = 0; j < PCV_AI_PROVIDER_COUNT; j++) {
        const char *known = pcv_ai_provider_name((PcvAiProvider)j);
        if (g_ascii_strcasecmp(name, known) == 0)
            return known;
    }
    return NULL;
}

void
cmd_agent_set(int argc, char *argv[])
{
    const char *provider = NULL, *api_key = NULL, *model = NULL, *endpoint = NULL;
    for (int i = 2; i < argc; i++) {
        const char **slot = NULL;
        if (g_strcmp0(argv[i], "--provider") == 0)
            slot = &provider;
        else if (g_strcmp0(argv[i], "--api_key") == 0 || g_strcmp0(argv[i], "--api-key") == 0)
            slot = &api_key;
        else if (g_strcmp0(argv[i], "--model") == 0)
            slot = &model;
        else if (g_strcmp0(argv[i], "--endpoint") == 0)
            slot = &endpoint;
        else if (g_strcmp0(argv[i], "--enabled") == 0) {
            _agent_set_reject("--enabled is not supported: a key enables the provider, --api_key \"\" disables it");
            return;
        } else if (g_strcmp0(argv[i], "--weight") == 0) {
            _agent_set_reject("--weight is not supported: provider weights are fixed by the daemon");
            return;
        } else {
            _agent_set_reject("unknown argument");
            return;
        }
        if (i + 1 >= argc) {
            _agent_set_reject("option needs a value");
            return;
        }
        *slot = argv[++i];
    }
    if (!provider) {
        _agent_set_usage();
        return;
    }
    const char *name = _agent_provider_canonical(provider);
    if (!name) {
        _agent_set_reject("unknown provider");
        return;
    }
    if (!api_key && !model && !endpoint) {
        _agent_set_reject("nothing to set");
        return;
    }
    if ((model && !*model) || (endpoint && !*endpoint)) {
        _agent_set_reject("--model and --endpoint must not be empty");
        return;
    }
    if (pcv_ai_key_is_masked(api_key)) {                                      
        _agent_set_reject("--api_key looks like a masked or placeholder value");
        return;
    }
    if (api_key && !*api_key && (model || endpoint)) {                            
        _agent_set_reject("--api_key \"\" disables the provider and cannot be combined with other values");
        return;
    }

    JsonObject *entry = json_object_new();
    json_object_set_string_member(entry, "name", name);
    if (api_key)  json_object_set_string_member(entry, "api_key", api_key);
    if (model)    json_object_set_string_member(entry, "model", model);
    if (endpoint) json_object_set_string_member(entry, "endpoint", endpoint);
    JsonArray *list = json_array_new();
    json_array_add_object_element(list, entry);
    JsonObject *params = json_object_new();                                          
    json_object_set_array_member(params, "providers", list);
    GError *error = NULL;
    gchar *resp = purectl_send_request("agent.config.set", params, &error);
    if (error) {                                                       
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error);
        return;
    }
    print_action_response(resp, "AGENT_CONFIG_SET");
    g_free(resp);
}
