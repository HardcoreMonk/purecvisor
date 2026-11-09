                                                
                           
                                                                     
                                                             
                                                              
                                                            
                                                      
                                                  
                                                                   
   
#include "vm_inspection.h"

#include <libvirt/virterror.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <string.h>

GQuark pcv_vm_inspection_error_quark(void)
{
    return g_quark_from_static_string("pcv-vm-inspection-error");
}

                                                      
                                                    
gboolean pcv_vm_inspection_uuid_valid(const gchar *uuid)
{
    if (!uuid || !g_uuid_string_is_valid(uuid)) return FALSE;
    for (const gchar *p = uuid; *p; p++)
        if (*p >= 'A' && *p <= 'F') return FALSE;
    return TRUE;
}

                                                                    
static gboolean named(xmlNodePtr n, const gchar *name)
{
    return n && n->type == XML_ELEMENT_NODE && !n->ns &&
           xmlStrEqual(n->name, BAD_CAST name);
}

static xmlNodePtr child(xmlNodePtr parent, const gchar *name)
{
    xmlNodePtr found = NULL;
    for (xmlNodePtr n = parent ? parent->children : NULL; n; n = n->next) {
        if (!named(n, name)) continue;
        if (found) return NULL;
        found = n;
    }
    return found;
}

                                                                       
static gchar *property(xmlNodePtr n, const gchar *key)
{
    xmlChar *raw = n ? xmlGetProp(n, BAD_CAST key) : NULL;
    gchar *copy = raw ? g_strdup((const gchar *)raw) : NULL;
    xmlFree(raw);
    return copy;
}

                                                         
                                                     
static xmlDocPtr configuration(const gchar *raw, const gchar *uuid)
{
    if (!raw || strnlen(raw, PCV_VM_INSPECTION_XML_LIMIT + 1) >
                   PCV_VM_INSPECTION_XML_LIMIT ||
        strstr(raw, "<!DOCTYPE") || strstr(raw, "<!ENTITY")) return NULL;
    xmlDocPtr doc = xmlReadMemory(raw, (int)strlen(raw), "inspection.xml", NULL,
                                 XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    xmlNodePtr root = doc ? xmlDocGetRootElement(doc) : NULL;
    if (!named(root, "domain") || doc->intSubset || doc->extSubset) goto invalid;
    xmlNodePtr id = child(root, "uuid");
    xmlChar *content = id ? xmlNodeGetContent(id) : NULL;
    gboolean matches = content && g_strcmp0((const gchar *)content, uuid) == 0;
    xmlFree(content);
    if (matches) return doc;
invalid:
    if (doc) xmlFreeDoc(doc);
    return NULL;
}

                                                        
static JsonObject *empty_facts(void)
{
    JsonObject *facts = json_object_new();
    const gchar *unknown[] = {"configuration_stability", "power", "architecture",
        "firmware", "secure_boot", "disk_enumeration"};
    for (guint i = 0; i < G_N_ELEMENTS(unknown); i++)
        json_object_set_string_member(facts, unknown[i], "unknown");
    json_object_set_null_member(facts, "configuration_sha256");
    JsonObject *os = json_object_new();
    json_object_set_string_member(os, "family", "unknown");
    json_object_set_string_member(os, "id", "unknown");
    json_object_set_string_member(os, "version", "unknown");
    json_object_set_object_member(facts, "os", os);
    json_object_set_array_member(facts, "disks", json_array_new());
    return facts;
}

                                                              
static const gchar *power_name(int state)
{
    switch (state) {
    case VIR_DOMAIN_RUNNING: case VIR_DOMAIN_BLOCKED: return "running";
    case VIR_DOMAIN_SHUTOFF: return "stopped";
    case VIR_DOMAIN_PAUSED: case VIR_DOMAIN_PMSUSPENDED: return "paused";
    case VIR_DOMAIN_SHUTDOWN: return "transitioning";
    default: return "unknown";
    }
}

                                                                   
static gboolean valid_target(const gchar *value)
{
    if (!value || !*value || strlen(value) > 63) return FALSE;
    for (const gchar *p = value; *p; p++)
        if (!g_ascii_isalnum(*p) && *p != '_' && *p != '-') return FALSE;
    return TRUE;
}

                                                    
static guint boot_order(xmlNodePtr device)
{
    g_autofree gchar *value = property(child(device, "boot"), "order");
    if (!value || !*value || strlen(value) > 5) return 0;
    for (const gchar *p = value; *p; p++) if (!g_ascii_isdigit(*p)) return 0;
    guint64 order = g_ascii_strtoull(value, NULL, 10);
    return order > 0 && order <= 65535 ? (guint)order : 0;
}

                                                               
                                                                      
static gboolean boot_roles_known(xmlNodePtr root, xmlNodePtr devices)
{
    xmlNodePtr os = child(root, "os");
    for (xmlNodePtr n = os ? os->children : NULL; n; n = n->next)
        if (named(n, "boot") || named(n, "kernel") || named(n, "init")) return FALSE;
    if (child(root, "bootloader")) return FALSE;
    g_autoptr(GHashTable) seen = g_hash_table_new(g_direct_hash, g_direct_equal);
    gboolean any = FALSE;
    for (xmlNodePtr n = devices ? devices->children : NULL; n; n = n->next) {
        guint count = 0;
        for (xmlNodePtr b = n->children; b; b = b->next)
            if (named(b, "boot")) count++;
        if (!count) continue;
        guint order = boot_order(n);
        if (count != 1 || !order || g_hash_table_contains(seen, GUINT_TO_POINTER(order)))
            return FALSE;
        any = TRUE;
        g_hash_table_add(seen, GUINT_TO_POINTER(order));
    }
    return any;
}

                                                       
                                                                
static JsonObject *disk_facts(xmlNodePtr node, const gchar *uuid,
                              const gchar *target, guint ordinal, gboolean boot_known)
{
    JsonObject *disk = json_object_new();
    g_autofree gchar *input = g_strdup_printf("%s:%s", uuid, target);
    g_autofree gchar *digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, input, -1);
    g_autofree gchar *id = g_strconcat("disk-", digest, NULL);
    json_object_set_string_member(disk, "disk_id", id);
    json_object_set_int_member(disk, "ordinal", ordinal);
    if (boot_known) json_object_set_boolean_member(disk, "boot", boot_order(node) > 0);
    else json_object_set_null_member(disk, "boot");

    g_autofree gchar *format = property(child(node, "driver"), "type");
    const gchar *supported[] = {"raw", "qcow2", "vmdk", "vhd", "vhdx"};
    const gchar *known = "unknown";
    for (guint i = 0; i < G_N_ELEMENTS(supported); i++)
        if (g_strcmp0(format, supported[i]) == 0) known = supported[i];
    json_object_set_string_member(disk, "format", known);
    json_object_set_null_member(disk, "virtual_bytes");
    json_object_set_null_member(disk, "allocated_bytes");
                                                                    
    json_object_set_string_member(disk, "backing_chain",
        child(child(node, "backingStore"), "source") ? "present" : "unknown");
    json_object_set_string_member(disk, "encryption",
        child(node, "encryption") || child(child(node, "source"), "encryption")
            ? "encrypted" : "unknown");
    return disk;
}

                                                                     
                                                          
static JsonObject *parse_facts(xmlDocPtr doc, const gchar *uuid, GPtrArray *targets)
{
    JsonObject *facts = empty_facts();
    xmlNodePtr root = xmlDocGetRootElement(doc), os = child(root, "os");
    g_autofree gchar *arch = property(child(os, "type"), "arch");
    if (g_strcmp0(arch, "x86_64") == 0 || g_strcmp0(arch, "aarch64") == 0 ||
        g_strcmp0(arch, "i386") == 0)
        json_object_set_string_member(facts, "architecture", arch);
    g_autofree gchar *firmware = property(os, "firmware");
    g_autofree gchar *loader = property(child(os, "loader"), "type");
    if (g_strcmp0(firmware, "efi") == 0 ||
        (!firmware && g_strcmp0(loader, "pflash") == 0))
        json_object_set_string_member(facts, "firmware", "uefi");
    else if (g_strcmp0(firmware, "bios") == 0 && g_strcmp0(loader, "pflash") != 0) {
        json_object_set_string_member(facts, "firmware", "bios");
        json_object_set_string_member(facts, "secure_boot", "disabled");
    }
                                                                            
    xmlNodePtr devices = child(root, "devices");
    if (!devices) return facts;
    gboolean complete = TRUE, known_boot = boot_roles_known(root, devices);
    guint encountered = 0;
    JsonArray *disks = json_object_get_array_member(facts, "disks");
    g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (xmlNodePtr n = devices->children; n; n = n->next) {
        if (!named(n, "disk")) continue;
        g_autofree gchar *device = property(n, "device");
        if (g_strcmp0(device, "cdrom") == 0 || g_strcmp0(device, "floppy") == 0) continue;
        if (++encountered > PCV_VM_INSPECTION_DISK_LIMIT) {
            json_object_set_array_member(facts, "disks", json_array_new());
            g_ptr_array_set_size(targets, 0);
            complete = FALSE;
            break;
        }
        g_autofree gchar *target = property(child(n, "target"), "dev");
        if ((g_strcmp0(device, "disk") != 0 && g_strcmp0(device, "lun") != 0) ||
            !valid_target(target) || g_hash_table_contains(seen, target)) {
            complete = FALSE;
            continue;
        }
        g_hash_table_add(seen, g_strdup(target));
        json_array_add_object_element(disks, disk_facts(n, uuid, target, targets->len, known_boot));
        g_ptr_array_add(targets, g_strdup(target));
    }
    json_object_set_string_member(facts, "disk_enumeration", complete ? "complete" : "partial");
    return facts;
}

                                                            
static const gchar *read_failure(void)
{
    return virGetLastErrorCode() == VIR_ERR_NO_SUPPORT ? "UNSUPPORTED" : "READ_FAILED";
}

                                                            
static gboolean expired(GCancellable *cancel, gint64 deadline)
{
    return (cancel && g_cancellable_is_cancelled(cancel)) || g_get_monotonic_time() >= deadline;
}

                                                      
static void unavailable(JsonObject *report, const gchar *failure)
{
    json_object_set_string_member(report, "outcome", "unavailable");
    json_object_set_string_member(report, "failure", failure);
    json_object_set_null_member(report, "facts");
}

                                                               
static void bytes_member(JsonObject *disk, const gchar *key, unsigned long long value)
{
    if (!value || value > 9999999999999999999ULL) return;
    g_autofree gchar *text = g_strdup_printf("%llu", value);
    json_object_set_string_member(disk, key, text);
}

                                                                   
                                                               
                                                             
JsonNode *pcv_vm_inspection_collect(virConnectPtr conn, const gchar *uuid,
                                   GCancellable *cancel, gint64 deadline_us, GError **error)
{
    if (!pcv_vm_inspection_uuid_valid(uuid)) {
        g_set_error_literal(error, pcv_vm_inspection_error_quark(), PCV_VM_INSPECTION_INVALID,
                            "canonical UUID required");
        return NULL;
    }
    gint64 started = g_get_real_time() / G_USEC_PER_SEC;
    JsonObject *report = json_object_new();
    json_object_set_string_member(report, "contract", "pcv-vm-inspection-v1");
    json_object_set_string_member(report, "producer_build", PCV_VM_INSPECTION_BUILD);
    json_object_set_string_member(report, "configuration_view", "inactive-redacted");
    json_object_set_string_member(report, "uuid", uuid);
    json_object_set_int_member(report, "started_at", started);
    unavailable(report, "READ_FAILED");
    virDomainPtr domain = NULL;
    xmlDocPtr doc = NULL;
    gchar *before = NULL, *after = NULL;
    JsonObject *facts = NULL;
    GPtrArray *targets = g_ptr_array_new_with_free_func(g_free);
    int state_before = VIR_DOMAIN_NOSTATE, state_after = VIR_DOMAIN_NOSTATE, reason = 0;
    int active_before = -1, active_after = -1;

    if (expired(cancel, deadline_us)) goto timeout;
    if (!conn) goto done;
                                                          
    virResetLastError();
    domain = virDomainLookupByUUIDString(conn, uuid);
    if (!domain) {
        if (virGetLastErrorCode() == VIR_ERR_NO_DOMAIN) {
            json_object_set_string_member(report, "outcome", "not_observed");
            json_object_set_null_member(report, "failure");
        } else unavailable(report, read_failure());
        goto done;
    }
    if (expired(cancel, deadline_us)) goto timeout;
    char observed_uuid[VIR_UUID_STRING_BUFLEN];
    if (virDomainGetUUIDString(domain, observed_uuid) < 0 || strcmp(observed_uuid, uuid) != 0)
        goto done;
    if (expired(cancel, deadline_us)) goto timeout;
    int persistent = virDomainIsPersistent(domain);
    if (persistent != 1) {
        unavailable(report, persistent == 0 ? "UNSUPPORTED" : read_failure());
        goto done;
    }
    if (expired(cancel, deadline_us)) goto timeout;
    active_before = virDomainIsActive(domain);
    if (expired(cancel, deadline_us)) goto timeout;
    if (virDomainGetState(domain, &state_before, &reason, 0) < 0) state_before = VIR_DOMAIN_NOSTATE;
    if (expired(cancel, deadline_us)) goto timeout;
    before = virDomainGetXMLDesc(domain, VIR_DOMAIN_XML_INACTIVE);
    if (!before) { unavailable(report, read_failure()); goto done; }
    if (expired(cancel, deadline_us)) goto timeout;
    doc = configuration(before, uuid);
    if (!doc) { unavailable(report, "UNSUPPORTED"); goto done; }
    facts = parse_facts(doc, uuid, targets);
    JsonArray *disks = json_object_get_array_member(facts, "disks");
                                                              
                                                               
    for (guint i = 0; active_before == 0 && i < targets->len; i++) {
        if (expired(cancel, deadline_us)) goto timeout;
        virDomainBlockInfo info = {0};
        if (virDomainGetBlockInfo(domain, g_ptr_array_index(targets, i), &info, 0) == 0) {
            JsonObject *disk = json_array_get_object_element(disks, i);
            bytes_member(disk, "virtual_bytes", info.capacity);
            bytes_member(disk, "allocated_bytes", info.allocation);
        }
    }
    if (expired(cancel, deadline_us)) goto timeout;
    after = virDomainGetXMLDesc(domain, VIR_DOMAIN_XML_INACTIVE);
    if (expired(cancel, deadline_us)) goto timeout;
    active_after = virDomainIsActive(domain);
    if (expired(cancel, deadline_us)) goto timeout;
    if (virDomainGetState(domain, &state_after, &reason, 0) < 0) state_after = VIR_DOMAIN_NOSTATE;
    if (!after || strnlen(after, PCV_VM_INSPECTION_XML_LIMIT + 1) > PCV_VM_INSPECTION_XML_LIMIT ||
        strcmp(before, after) != 0 || active_before < 0 || active_after != active_before) {
                                                   
        const gchar *stability = after && strnlen(after, PCV_VM_INSPECTION_XML_LIMIT + 1) <= PCV_VM_INSPECTION_XML_LIMIT &&
            (strcmp(before, after) != 0 || (active_before >= 0 && active_after >= 0 &&
                                           active_after != active_before)) ? "changed" : "unknown";
        json_object_unref(facts);
        facts = empty_facts();
        json_object_set_string_member(facts, "configuration_stability", stability);
    } else {
        g_autofree gchar *digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, before, -1);
        json_object_set_string_member(facts, "configuration_stability", "stable");
        json_object_set_string_member(facts, "configuration_sha256", digest);
    }
    json_object_set_string_member(facts, "power",
        state_before == VIR_DOMAIN_NOSTATE || state_after == VIR_DOMAIN_NOSTATE ? "unknown" :
        state_before == state_after ? power_name(state_before) : "transitioning");
    json_object_set_string_member(report, "outcome", "observed");
    json_object_set_null_member(report, "failure");
    json_object_set_object_member(report, "facts", facts);
    facts = NULL;
    goto done;
timeout:
    unavailable(report, "TIMEOUT");
done:
                                                          
    if (expired(cancel, deadline_us)) unavailable(report, "TIMEOUT");
    if (facts) json_object_unref(facts);
    if (doc) xmlFreeDoc(doc);
    free(before); free(after);
    if (domain) virDomainFree(domain);
    g_ptr_array_unref(targets);
    gint64 finished = g_get_real_time() / G_USEC_PER_SEC;
    if (finished < started) {
        json_object_unref(report);
        g_set_error_literal(error, pcv_vm_inspection_error_quark(), PCV_VM_INSPECTION_READ_FAILED,
                            "inspection clock changed");
        return NULL;
    }
    json_object_set_int_member(report, "finished_at", finished);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, report);
    gsize length = 0;
    gchar *wire = json_to_string(node, FALSE);
    length = strlen(wire);
    g_free(wire);
    if (length > PCV_VM_INSPECTION_OUTPUT_LIMIT) unavailable(report, "UNSUPPORTED");
    return node;
}
