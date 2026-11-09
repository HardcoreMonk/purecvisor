   
                           
                                                                     
  
                           
                                                                         
                                                     
                                                           
  
                                                         
   
#include "vm_delete_status.h"

                                                                                          
                                            
   
gchar *
pcv_vm_delete_job_result(const gchar *name, gboolean already_absent,
                         const GPtrArray *warnings, const gchar *error)
{
    g_autoptr(JsonBuilder) b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "name");
    json_builder_add_string_value(b, name ? name : "");
    if (already_absent) {                              
        json_builder_set_member_name(b, "already_absent");
        json_builder_add_boolean_value(b, TRUE);
    }
    if (warnings && warnings->len > 0) {                                       
        json_builder_set_member_name(b, "warnings");
        json_builder_begin_array(b);
        for (guint i = 0; i < warnings->len; i++)
            json_builder_add_string_value(b, g_ptr_array_index(warnings, i));
        json_builder_end_array(b);
    }
    if (error) {
        json_builder_set_member_name(b, "error");
        json_builder_add_string_value(b, error);
    }
    json_builder_end_object(b);
    g_autoptr(JsonNode) root = json_builder_get_root(b);
    return json_to_string(root, FALSE);
}

                                                                     
                                                                      
static void
_lift_result(JsonObject *out, const gchar *result)
{
    if (!result)
        return;
    g_autoptr(JsonParser) parser = json_parser_new();
    if (!json_parser_load_from_data(parser, result, -1, NULL))
        return;                             
    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root))
        return;
    JsonObject *r = json_node_get_object(root);
    JsonNode *absent = json_object_get_member(r, "already_absent");
    if (absent && JSON_NODE_HOLDS_VALUE(absent) && json_node_get_value_type(absent) == G_TYPE_BOOLEAN
        && json_node_get_boolean(absent))
        json_object_set_boolean_member(out, "already_absent", TRUE);
    JsonNode *warnings = json_object_get_member(r, "warnings");
    if (warnings && JSON_NODE_HOLDS_ARRAY(warnings))
        json_object_set_member(out, "warnings", json_node_copy(warnings));
    JsonNode *error = json_object_get_member(r, "error");
    if (error && JSON_NODE_HOLDS_VALUE(error) && json_node_get_value_type(error) == G_TYPE_STRING)
        json_object_set_string_member(out, "error", json_node_get_string(error));
}

                                              
static void
_copy_string(JsonObject *out, JsonObject *job, const gchar *key)
{
    JsonNode *n = json_object_get_member(job, key);
    if (n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING)
        json_object_set_string_member(out, key, json_node_get_string(n));
}

                                                                                                 
                                                    
JsonObject *
pcv_vm_delete_status_build(const gchar *name, JsonObject *job)
{
    JsonObject *out = json_object_new();
    json_object_set_string_member(out, "name", name ? name : "");
    if (!job) {
        json_object_set_string_member(out, "status", "not_found");
        return out;
    }
                                                                                                     
    json_object_set_string_member(out, "status",
        json_object_get_string_member_with_default(job, "status", "unknown"));
    json_object_set_string_member(out, "job_id",
        json_object_get_string_member_with_default(job, "job_id", ""));
    json_object_set_int_member(out, "progress",
        json_object_get_int_member_with_default(job, "progress", 0));
    _copy_string(out, job, "detail");
    json_object_set_boolean_member(out, "outcome_known",
        json_object_get_boolean_member_with_default(job, "outcome_known", FALSE));
    _copy_string(out, job, "recovery_reason");
    json_object_set_int_member(out, "created_at",
        json_object_get_int_member_with_default(job, "created_at", 0));
    json_object_set_int_member(out, "updated_at",
        json_object_get_int_member_with_default(job, "updated_at", 0));
    _lift_result(out, json_object_get_string_member_with_default(job, "result", NULL));
    return out;
}
