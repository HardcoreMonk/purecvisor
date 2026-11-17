                                                                                                
                                                                                                        
                                                                     
                                                                                                                                                 
                                 
                                                                                     
                                                                                                    
                                                                   
                                                                                
                                                                                
                                                            
                                                                                    
                                                                                                              
                                                                                      
                                                                                    
                                                                              
                                                                                                               
                                
                                                                           
                                             
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "modules/plugin/pcv_plugin_api.h"

                                                                                      
#ifndef PCV_PROBE_NAME
#define PCV_PROBE_NAME "pcv-test-api"
#endif
                                                                                                
#ifndef PCV_PROBE_ABI
#define PCV_PROBE_ABI PCV_PLUGIN_ABI_VERSION
#endif
static const PcvPluginMeta META = { PCV_PROBE_NAME, "1.0", "tests", PCV_PROBE_ABI };
static PcvPluginRegistry *g_reg;                                                            

                                                   
                                                        
static void mark(const char *line)
{
    const char *p = getenv("PCV_TEST_PLUGIN_MARK");
    FILE *f = p ? fopen(p, "a") : NULL;
    if (f) { fprintf(f, "%s\n", line); fclose(f); }
}

                                                                     
static void late(JsonObject *params, const gchar *rpc_id, gpointer server, GSocketConnection *c)
{
    (void)params;
    pcv_plugin_respond_error(rpc_id, server, c, 1999, "late handler must not be registered");
}

                                              
static void echo(JsonObject *params, const gchar *rpc_id, gpointer server, GSocketConnection *c)
{
    pcv_plugin_registry_add(g_reg, "pcvtestapi.late", late);                                    
    JsonObject *o = json_object_new();
    json_object_set_string_member(o, "plugin", "pcv-test-api");
    if (params && json_object_has_member(params, "msg") && JSON_NODE_HOLDS_VALUE(json_object_get_member(params, "msg")))
        json_object_set_string_member(o, "echo", json_object_get_string_member(params, "msg"));
    else
        json_object_set_null_member(o, "echo");
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, o);
    mark(pcv_plugin_respond_success(rpc_id, server, c, n) ? "echo sent=1" : "echo sent=0");
}

                                                 
static void fail(JsonObject *params, const gchar *rpc_id, gpointer server, GSocketConnection *c)
{
    (void)params;
    mark(pcv_plugin_respond_error(rpc_id, server, c, 1001, "pcvtestapi failure requested") ? "fail sent=1" : "fail sent=0");
}

                                                                                    
static void twice(JsonObject *params, const gchar *rpc_id, gpointer server, GSocketConnection *c)
{
    (void)params;
    gboolean a = pcv_plugin_respond_success(rpc_id, server, c, json_node_init_string(json_node_alloc(), "first"));
    gboolean b = pcv_plugin_respond_success(rpc_id, server, c, json_node_init_string(json_node_alloc(), "second"));
    gboolean e = pcv_plugin_respond_error(rpc_id, server, c, 1002, "third");
    char line[64];
    snprintf(line, sizeof line, "twice first=%d second=%d third=%d", a ? 1 : 0, b ? 1 : 0, e ? 1 : 0);
    mark(line);
}

                                                          
static void silent(JsonObject *params, const gchar *rpc_id, gpointer server, GSocketConnection *c)
{
    (void)params; (void)rpc_id; (void)server; (void)c;
    mark("silent returned");
}

                                                                      
static void zero(JsonObject *params, const gchar *rpc_id, gpointer server, GSocketConnection *c)
{
    (void)params;
    pcv_plugin_respond_error(rpc_id, server, c, 0, NULL);
}

                                                                          
static void info(JsonObject *params, const gchar *rpc_id, gpointer server, GSocketConnection *c)
{
    (void)params;
    JsonObject *o = json_object_new();
    json_object_set_string_member(o, "plugin", "pcv-test-api");
    json_object_set_boolean_member(o, "info", TRUE);
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, o);
    pcv_plugin_respond_success(rpc_id, server, c, n);
}

const PcvPluginMeta *pcv_plugin_get_meta(void) { return &META; }

void pcv_plugin_register(PcvPluginRegistry *reg)
{
    g_reg = reg;
    pcv_plugin_registry_add(reg, "pcvtestapi.echo", echo);
    pcv_plugin_registry_add(reg, "pcvtestapi.fail", fail);
    pcv_plugin_registry_add(reg, "pcvtestapi.twice", twice);
    pcv_plugin_registry_add(reg, "pcvtestapi.silent", silent);
    pcv_plugin_registry_add(reg, "pcvtestapi.zero", zero);
    pcv_plugin_registry_add(reg, "pcvtestapi.info", info);
}

void pcv_plugin_shutdown(void)
{
    char line[64];
    snprintf(line, sizeof line, "shutdown pid=%d", (int)getpid());
    mark(line);
    g_message("[pcv-test-api] shutdown callback pid=%d", (int)getpid());
}
