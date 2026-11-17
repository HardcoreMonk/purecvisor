   
                            
                                                                                       
  
                           
                                                                                                  
                                                                      
                                                          
                                                                                               
                                                        
                                                                                               
                                                                                        
                                                                                               
                                                                                          
                                                                                      
                                                                               
                                                                 
                                                                                          
                                                            
                                                                         
  
                                                                      
                                                       
   
#include "modules/virt/vm_boot_autostart.h"

#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <net/if.h>                 
#include <string.h>

#include "api/vm_batch_queue.h"
#include "utils/pcv_validate.h"
#include "modules/resource/resource_domain_refs.h"

                                                  
static const gchar *const DECISION_NAMES[] = {
    [PCV_BOOT_DECIDE_RECORD_ONLY] = "record_only",
    [PCV_BOOT_DECIDE_SAME_BOOT]   = "same_boot",
    [PCV_BOOT_DECIDE_ACT]         = "act",
    [PCV_BOOT_DECIDE_DISABLED]    = "disabled",
    [PCV_BOOT_DECIDE_ERROR]       = "error",
};

                                           
                                     
const gchar *pcv_boot_autostart_decision_name(PcvBootDecision d)
{
    if ((guint)d >= G_N_ELEMENTS(DECISION_NAMES))
        return "error";
    return DECISION_NAMES[d];
}

                           
                                           
gboolean pcv_boot_autostart_read_boot_id_at(const gchar *path, gchar **boot_id_out, GError **error)
{
    g_return_val_if_fail(path != NULL && boot_id_out != NULL, FALSE);
    *boot_id_out = NULL;
    gchar *text = NULL;
    if (!g_file_get_contents(path, &text, NULL, error))
        return FALSE;
    g_strstrip(text);
                                                                   
    if (!g_uuid_string_is_valid(text)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "%s: boot_id is not a UUID", path);
        g_free(text);
        return FALSE;
    }
    *boot_id_out = text;
    return TRUE;
}

                           
                                          
void pcv_boot_autostart_marker_clear(PcvBootMarker *m)
{
    if (!m)
        return;
    g_free(m->boot_id);
    g_free(m->decision);
    g_strfreev(m->candidates);
    *m = (PcvBootMarker){ .state = PCV_BOOT_MARKER_ABSENT };
}

                                                                           
static gboolean node_is_int(JsonNode *n)
{
    return n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_INT64;
}

                                       
static gboolean node_is_string(JsonNode *n)
{
    return n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING;
}

                                                          
                                                  
static gboolean marker_from_root(JsonNode *root, PcvBootMarker *out)
{
    if (!root || !JSON_NODE_HOLDS_OBJECT(root))
        return FALSE;
    JsonObject *o = json_node_get_object(root);
    JsonNode *ver = json_object_get_member(o, "version");
    JsonNode *bid = json_object_get_member(o, "boot_id");
    JsonNode *dec = json_object_get_member(o, "decision");
    JsonNode *cands = json_object_get_member(o, "candidates");
    if (!node_is_int(ver) || json_node_get_int(ver) != 1)
        return FALSE;
    if (!node_is_string(bid) || !g_uuid_string_is_valid(json_node_get_string(bid)))
        return FALSE;
    if (!node_is_string(dec))
        return FALSE;
    if (!cands || !JSON_NODE_HOLDS_ARRAY(cands))
        return FALSE;
    JsonArray *arr = json_node_get_array(cands);
    guint n = json_array_get_length(arr);
    gchar **list = g_new0(gchar *, n + 1);
    for (guint i = 0; i < n; i++) {
        JsonNode *e = json_array_get_element(arr, i);
        if (!node_is_string(e)) {
            g_strfreev(list);
            return FALSE;
        }
        list[i] = g_strdup(json_node_get_string(e));
    }
    out->state = PCV_BOOT_MARKER_OK;
    out->boot_id = g_strdup(json_node_get_string(bid));
    out->decision = g_strdup(json_node_get_string(dec));
    out->candidates = list;
    return TRUE;
}

                                                               
                                                     
void pcv_boot_autostart_marker_read_at(const gchar *path, PcvBootMarker *out)
{
    g_return_if_fail(path != NULL && out != NULL);
    *out = (PcvBootMarker){ .state = PCV_BOOT_MARKER_ABSENT };
    g_autoptr(GError) err = NULL;
    g_autofree gchar *text = NULL;
    gsize len = 0;
    if (!g_file_get_contents(path, &text, &len, &err)) {
                                                                             
        if (!g_error_matches(err, G_FILE_ERROR, G_FILE_ERROR_NOENT))
            out->state = PCV_BOOT_MARKER_CORRUPT;
        return;
    }
    g_autoptr(JsonParser) parser = json_parser_new();
    if (len == 0 || !json_parser_load_from_data(parser, text, (gssize)len, NULL)
        || !marker_from_root(json_parser_get_root(parser), out))
        out->state = PCV_BOOT_MARKER_CORRUPT;
}

                                                   
                                        
gboolean pcv_boot_autostart_marker_write_at(const gchar *path, const gchar *boot_id, PcvBootDecision decision,
                                            const gchar *const *candidates, GError **error)
{
    g_return_val_if_fail(path != NULL, FALSE);
    if (!boot_id || !g_uuid_string_is_valid(boot_id)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "boot_id is not a UUID");
        return FALSE;
    }
    g_autoptr(GDateTime) now = g_date_time_new_now_utc();
    g_autofree gchar *at = g_date_time_format_iso8601(now);

    g_autoptr(JsonBuilder) b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "version");
    json_builder_add_int_value(b, 1);
    json_builder_set_member_name(b, "boot_id");
    json_builder_add_string_value(b, boot_id);
    json_builder_set_member_name(b, "decision");
    json_builder_add_string_value(b, pcv_boot_autostart_decision_name(decision));
    json_builder_set_member_name(b, "decided_at");
    json_builder_add_string_value(b, at);
    json_builder_set_member_name(b, "candidates");
    json_builder_begin_array(b);
    for (guint i = 0; candidates && candidates[i]; i++)
        json_builder_add_string_value(b, candidates[i]);
    json_builder_end_array(b);
    json_builder_end_object(b);

    g_autoptr(JsonNode) root = json_builder_get_root(b);
    g_autoptr(JsonGenerator) gen = json_generator_new();
    json_generator_set_root(gen, root);
    g_autofree gchar *text = json_generator_to_data(gen, NULL);
                                                            
    return g_file_set_contents_full(path, text, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600, error);
}

                                                                                           
                                                                         
PcvBootDecision pcv_boot_autostart_decide(gboolean enabled, const gchar *current_boot_id,
                                          const PcvBootMarker *marker, gboolean *write_now_out)
{
    gboolean write_now = FALSE;
    PcvBootDecision d;
    PcvBootMarkerState st = marker ? marker->state : PCV_BOOT_MARKER_ABSENT;

    if (!current_boot_id) {
        d = PCV_BOOT_DECIDE_ERROR;                                                  
    } else if (st == PCV_BOOT_MARKER_OK && g_strcmp0(marker->boot_id, current_boot_id) == 0) {
        d = PCV_BOOT_DECIDE_SAME_BOOT;                                       
    } else if (!enabled) {
        d = PCV_BOOT_DECIDE_DISABLED;                                            
        write_now = TRUE;
    } else if (st != PCV_BOOT_MARKER_OK) {
        d = PCV_BOOT_DECIDE_RECORD_ONLY;                                                
        write_now = TRUE;
    } else {
        d = PCV_BOOT_DECIDE_ACT;                                                         
    }
    if (write_now_out)
        *write_now_out = write_now;
    return d;
}


                                                                    

                                                  
static const gchar *const CLASS_NAMES[] = {
    [PCV_BOOT_CLASS_CANDIDATE]      = "candidate",
    [PCV_BOOT_CLASS_NOT_PCV]        = "not_pcv",
    [PCV_BOOT_CLASS_BRIDGE_MISSING] = "skipped_bridge_missing",
    [PCV_BOOT_CLASS_STATE_REASON]   = "skipped_state_reason",
    [PCV_BOOT_CLASS_ALREADY_ACTIVE] = "already_active",
    [PCV_BOOT_CLASS_XML_ERROR]      = "xml_error",
};

                                               
                                     
const gchar *pcv_boot_autostart_class_name(PcvBootClass c)
{
    if ((guint)c >= G_N_ELEMENTS(CLASS_NAMES))
        return "xml_error";
    return CLASS_NAMES[c];
}

                                                                                        
                                                          
gboolean pcv_boot_autostart_iface_exists_at(const gchar *sysfs_net_root, const gchar *name)
{
    if (!sysfs_net_root || !name || !*name || strchr(name, '/') || strcmp(name, ".") == 0 || strcmp(name, "..") == 0
        || strlen(name) >= IFNAMSIZ)
        return FALSE;
    g_autofree gchar *path = g_build_filename(sysfs_net_root, name, NULL);
    return g_file_test(path, G_FILE_TEST_IS_DIR);
}

                                                                                                       
                                  
                                           
gboolean pcv_boot_autostart_reason_allowed(int shutoff_reason)
{
    return shutoff_reason == VIR_DOMAIN_SHUTOFF_FAILED;
}

                                                                              
                                                                                
gboolean pcv_boot_autostart_bridge_in_set(const PcvBootBridgeOracle *o, const gchar *bridge)
{
    if (!o || !bridge || !*bridge)
        return FALSE;
    if (o->default_bridge && g_strcmp0(o->default_bridge, bridge) == 0)
        return TRUE;                                                                  
                                                                                                    
                                                                                      
    if (o->physical_mode && pcv_validate_bridge_name(bridge)) {
        g_autofree gchar *mode = NULL;
        g_autoptr(GError) err = NULL;
        if (o->physical_mode(bridge, &mode, &err)) {
                                                                                           
            if (mode != NULL)
                return TRUE;
        } else {
            g_warning("[boot-autostart] physical bridge state of '%s' unreadable — not PCV-managed: %s",
                      bridge, err ? err->message : "unknown");
        }
    }
    if (o->vpc_managed && o->vpc_managed(bridge))
        return TRUE;
    return FALSE;
}

                                                              
                                                                         
                          
PcvBootClass pcv_boot_autostart_classify(int state, int reason, const gchar *inactive_xml,
                                         const PcvBootBridgeOracle *o, gchar **detail_out)
{
    if (detail_out)
        *detail_out = NULL;
    if (state != VIR_DOMAIN_SHUTOFF) {
        if (detail_out) *detail_out = g_strdup_printf("state=%d", state);
        return PCV_BOOT_CLASS_ALREADY_ACTIVE;
    }
    if (!pcv_boot_autostart_reason_allowed(reason)) {
        if (detail_out) *detail_out = g_strdup_printf("reason=%d", reason);
        return PCV_BOOT_CLASS_STATE_REASON;
    }
    PcvResDomainRefs refs = { 0 };
    g_autoptr(GError) err = NULL;
    if (!inactive_xml || !pcv_res_domain_refs_parse(inactive_xml, &refs, &err)) {
        if (detail_out) *detail_out = g_strdup(err ? err->message : "no definition XML");
        pcv_res_domain_refs_clear(&refs);
        return PCV_BOOT_CLASS_XML_ERROR;
    }
                                                                                         
    g_autoptr(GPtrArray) bridges = g_ptr_array_new();
    gboolean any_in_set = FALSE;
    for (guint i = 0; refs.refs && i < refs.refs->len; i++) {
        const PcvResRef *r = g_ptr_array_index(refs.refs, i);
        if (r->kind != PCV_RES_BRIDGE || !r->ref || !*r->ref)
            continue;
        g_ptr_array_add(bridges, r->ref);
        if (pcv_boot_autostart_bridge_in_set(o, r->ref))
            any_in_set = TRUE;
    }
    PcvBootClass k = PCV_BOOT_CLASS_CANDIDATE;
    if (!any_in_set) {
        k = PCV_BOOT_CLASS_NOT_PCV;
        if (detail_out) {
            g_ptr_array_add(bridges, NULL);
            *detail_out = bridges->len > 1 ? g_strjoinv(",", (gchar **)bridges->pdata) : g_strdup("no bridge");
        }
    } else {
        for (guint i = 0; i < bridges->len; i++) {
            const gchar *br = g_ptr_array_index(bridges, i);
                                                                      
            if (!o->iface_exists || !o->iface_exists(br)) {
                k = PCV_BOOT_CLASS_BRIDGE_MISSING;
                if (detail_out) *detail_out = g_strdup(br);
                break;
            }
        }
    }
    pcv_res_domain_refs_clear(&refs);
    return k;
}


                                                                

                                                   
                                                               
PcvBootClass pcv_boot_autostart_recheck(int state, int reason)
{
    if (state != VIR_DOMAIN_SHUTOFF)
        return PCV_BOOT_CLASS_ALREADY_ACTIVE;
    if (!pcv_boot_autostart_reason_allowed(reason))
        return PCV_BOOT_CLASS_STATE_REASON;
    return PCV_BOOT_CLASS_CANDIDATE;
}

                                                                   
                                                                         
                                                    
struct PcvBootRun {
    PcvBootRunOps    ops;
    PcvVmBatchQueue *queue;
    PcvBootRunTally  tally;
    guint            n;
    guint            settled;
    guint            depth;
    gboolean         finished;
};

                                                                 
static void run_settle(PcvBootRun *run, guint *field, const gchar *result)
{
    (*field)++;
    run->settled++;
    if (run->ops.count)
        run->ops.count(result, run->ops.user_data);
}

                                                        
                                                  
static gboolean run_finish_if_done(PcvBootRun *run)
{
    if (run->depth > 0 || run->finished || run->settled < run->n)
        return FALSE;
    run->finished = TRUE;
    if (run->ops.finished)
        run->ops.finished(&run->tally, run->ops.user_data);
    g_free(run);
    return TRUE;
}

                                                                           
                                                                  
static gboolean run_q_submit(const gchar *vm, const gchar *actor, PcvVmBatchQueue *queue, gpointer user_data,
                             gchar **reason)
{
    (void)actor;                                          
    PcvBootRun *run = user_data;
    run->queue = queue;
    int state = -1, why = -1;
                                                                              
    if (run->ops.recheck && run->ops.recheck(vm, &state, &why, run->ops.user_data)) {
        PcvBootClass k = pcv_boot_autostart_recheck(state, why);
        if (k != PCV_BOOT_CLASS_CANDIDATE) {
            g_message("[boot-autostart] skip '%s': %s (recheck state=%d reason=%d)", vm,
                      pcv_boot_autostart_class_name(k), state, why);
            if (k == PCV_BOOT_CLASS_ALREADY_ACTIVE)
                run_settle(run, &run->tally.already_active, "already_active");
            else
                run_settle(run, &run->tally.skipped_state_reason, "skipped_state_reason");
            pcv_vm_batch_queue_item_done(queue);
            return TRUE;
        }
    }
    gchar *msg = NULL;
    PcvBootSubmit r = run->ops.submit(vm, run, &msg, run->ops.user_data);
    switch (r) {
    case PCV_BOOT_SUBMIT_ASYNC:
        g_free(msg);
        return TRUE;                                                                          
    case PCV_BOOT_SUBMIT_ALREADY_ACTIVE:
        g_free(msg);
        run_settle(run, &run->tally.already_active, "already_active");
        pcv_vm_batch_queue_item_done(queue);
        return TRUE;
    case PCV_BOOT_SUBMIT_LOCK_BUSY:
                                                                                                
        g_message("[boot-autostart] skip '%s': lock_busy (%s)", vm, msg ? msg : "-");
        g_free(msg);
        run_settle(run, &run->tally.lock_busy, "lock_busy");
        pcv_vm_batch_queue_item_done(queue);
        return TRUE;
    case PCV_BOOT_SUBMIT_REJECTED:
    default:
        *reason = msg;                                                                     
        return FALSE;
    }
}

                                                                   
static void run_q_report(const gchar *vm, const gchar *actor, const gchar *reason, gpointer user_data)
{
    (void)actor;
    PcvBootRun *run = user_data;
    g_warning("[boot-autostart] '%s' not started: %s", vm, reason ? reason : "-");
    if (run->ops.report)
        run->ops.report(vm, reason, run->ops.user_data);
    run_settle(run, &run->tally.rejected, "rejected");
}

                                                                               
                                                           
void pcv_boot_autostart_run_queue(const gchar *const *vms, guint n, const PcvBootRunOps *ops)
{
    g_return_if_fail(ops != NULL && ops->submit != NULL);
    PcvBootRun *run = g_new0(PcvBootRun, 1);
    run->ops = *ops;
    run->n = vms ? n : 0;
    if (run->n == 0) {
        run_finish_if_done(run);
        return;
    }
    const PcvVmBatchQueueOps qops = {
        .submit = run_q_submit,
        .report = run_q_report,
        .terminating = ops->terminating,
        .user_data = run,
        .max_in_flight = 1,
        .slots = NULL,
        .actor = NULL,
    };
    run->depth++;
    pcv_vm_batch_queue_run(vms, run->n, &qops);
    run->depth--;
    run_finish_if_done(run);
}

                                           
                                                            
void pcv_boot_autostart_item_done(PcvBootRun *run, gboolean ok)
{
    g_return_if_fail(run != NULL && run->queue != NULL);
    if (ok)
        run_settle(run, &run->tally.started, "started");
    else
        run_settle(run, &run->tally.failed, "failed");
    run->depth++;
    pcv_vm_batch_queue_item_done(run->queue);                                                  
    run->depth--;
    run_finish_if_done(run);
}
