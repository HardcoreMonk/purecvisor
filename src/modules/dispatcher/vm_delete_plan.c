   
                         
                                                        
  
                           
                                                                                                     
                                                                                              
                                                                
                                                                    
  
                                                                           
                                                                
   
#include "modules/dispatcher/vm_delete_plan.h"

#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <string.h>

#define ZVOL_PREFIX "/dev/zvol/"

                                                                      
static xmlDocPtr
_read_domain(const gchar *xml, GError **error)
{
    xmlDocPtr doc = xml ? xmlReadMemory(xml, (int)strlen(xml), "vm-delete-plan.xml", NULL,
                                        XML_PARSE_NONET | XML_PARSE_NOBLANKS |
                                        XML_PARSE_NOERROR | XML_PARSE_NOWARNING) : NULL;
    xmlNodePtr root = doc ? xmlDocGetRootElement(doc) : NULL;
    if (!root || xmlStrcmp(root->name, BAD_CAST "domain") != 0) {
        if (doc)
            xmlFreeDoc(doc);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "VM XML unavailable or invalid; storage plan blocked");
        return NULL;
    }
    return doc;
}

                                             
static xmlNodePtr
_child(xmlNodePtr parent, const gchar *name)
{
    for (xmlNodePtr cur = parent ? parent->children : NULL; cur; cur = cur->next)
        if (cur->type == XML_ELEMENT_NODE && xmlStrcmp(cur->name, BAD_CAST name) == 0)
            return cur;
    return NULL;
}

                                                                          
static gchar *
_source_path(xmlNodePtr source)
{
    if (!source)
        return NULL;
    for (const gchar *const *attr = (const gchar *const[]){ "file", "dev", NULL }; *attr; attr++) {
        xmlChar *value = xmlGetProp(source, BAD_CAST *attr);
        if (value) {
            gchar *path = *value ? g_strdup((const gchar *)value) : NULL;
            xmlFree(value);
            if (path)
                return path;
        }
    }
    return NULL;
}

                                                                                 
static gboolean
_is_hard_disk(xmlNodePtr disk)
{
    xmlChar *device = xmlGetProp(disk, BAD_CAST "device");
    gboolean yes = !device || xmlStrcmp(device, BAD_CAST "disk") == 0;
    xmlFree(device);
    return yes;
}

                                                                
static gboolean
_path_is_plain(const gchar *path)
{
    gchar **parts = g_strsplit(path, "/", -1);
    gboolean plain = TRUE;
    for (gint i = 1; parts[i] && plain; i++)                                  
        plain = *parts[i] && g_strcmp0(parts[i], ".") != 0 && g_strcmp0(parts[i], "..") != 0;
    g_strfreev(parts);
    return plain;
}

                                                                             
static gboolean
_claim_if_owned(const gchar *path, const gchar *vm_name, PcvVmDeleteStoragePlan *plan)
{
    if (!g_path_is_absolute(path) || !_path_is_plain(path))
        return FALSE;
    if (g_str_has_prefix(path, ZVOL_PREFIX)) {
        const gchar *dataset = path + strlen(ZVOL_PREFIX);
        const gchar *leaf = strrchr(dataset, '/');
        if (!leaf || g_strcmp0(leaf + 1, vm_name) != 0)
            return FALSE;
        plan->kind = PCV_VM_DELETE_STORAGE_ZVOL;
        plan->path = g_strdup(path);
        plan->dataset = g_strdup(dataset);
        return TRUE;
    }
    if (g_str_has_prefix(path, "/dev/"))
        return FALSE;                                   
    g_autofree gchar *base = g_path_get_basename(path);
    for (const gchar *const *ext = (const gchar *const[]){ ".qcow2", ".raw", ".img", NULL }; *ext; ext++) {
        g_autofree gchar *expected = g_strconcat(vm_name, *ext, NULL);
        if (g_strcmp0(base, expected) == 0) {
            plan->kind = PCV_VM_DELETE_STORAGE_FILE;
            plan->path = g_strdup(path);
            return TRUE;
        }
    }
    return FALSE;
}

                                                                           
static void
_add_kept(PcvVmDeleteStoragePlan *plan, const gchar *path)
{
    const gchar *shown = g_str_has_prefix(path, ZVOL_PREFIX) ? path + strlen(ZVOL_PREFIX) : path;
    for (guint i = 0; i < plan->kept->len; i++)
        if (g_strcmp0(g_ptr_array_index(plan->kept, i), shown) == 0)
            return;
    g_ptr_array_add(plan->kept, g_strdup(shown));
}

                                                                            
static void
_add_kept_pathless(PcvVmDeleteStoragePlan *plan, xmlNodePtr disk)
{
    xmlChar *type = xmlGetProp(disk, BAD_CAST "type");
    xmlChar *target = _child(disk, "target") ? xmlGetProp(_child(disk, "target"), BAD_CAST "dev") : NULL;
    g_autofree gchar *label = g_strdup_printf("%s disk %s", type ? (const gchar *)type : "unknown",
                                              target ? (const gchar *)target : "(no target)");
    xmlFree(type);
    xmlFree(target);
    _add_kept(plan, label);
}

                                   
static void
_plan_init(PcvVmDeleteStoragePlan *plan)
{
    memset(plan, 0, sizeof(*plan));
    plan->kept = g_ptr_array_new_with_free_func(g_free);
}

                                                                              
                                                                       
gboolean
pcv_vm_delete_plan_from_xml(const gchar *domain_xml, const gchar *vm_name,
                            PcvVmDeleteStoragePlan *plan, GError **error)
{
    _plan_init(plan);
    xmlDocPtr doc = _read_domain(domain_xml, error);
    if (!doc) {
        pcv_vm_delete_plan_clear(plan);
        return FALSE;
    }
    xmlNodePtr devices = _child(xmlDocGetRootElement(doc), "devices");
    gboolean first_seen = FALSE;
    for (xmlNodePtr disk = devices ? devices->children : NULL; disk; disk = disk->next) {
        if (disk->type != XML_ELEMENT_NODE || xmlStrcmp(disk->name, BAD_CAST "disk") != 0 || !_is_hard_disk(disk))
            continue;
        xmlNodePtr source = _child(disk, "source");
        g_autofree gchar *path = _source_path(source);
        gboolean is_first = !first_seen;
        first_seen = TRUE;
        if (path) {
            if (!(is_first && vm_name && _claim_if_owned(path, vm_name, plan)))
                _add_kept(plan, path);
        } else if (source) {
            _add_kept_pathless(plan, disk);
        }
    }
    xmlFreeDoc(doc);
    return TRUE;
}

                                                                         
                                                      
gboolean
pcv_vm_delete_plan_add_kept_from_xml(const gchar *domain_xml, PcvVmDeleteStoragePlan *plan, GError **error)
{
    xmlDocPtr doc = _read_domain(domain_xml, error);
    if (!doc)
        return FALSE;
    xmlNodePtr devices = _child(xmlDocGetRootElement(doc), "devices");
    for (xmlNodePtr disk = devices ? devices->children : NULL; disk; disk = disk->next) {
        if (disk->type != XML_ELEMENT_NODE || xmlStrcmp(disk->name, BAD_CAST "disk") != 0 || !_is_hard_disk(disk))
            continue;
        g_autofree gchar *path = _source_path(_child(disk, "source"));
        if (path && g_strcmp0(path, plan->path) != 0)
            _add_kept(plan, path);
    }
    xmlFreeDoc(doc);
    return TRUE;
}

                                                                   
                                         
void
pcv_vm_delete_plan_for_absent(const gchar *zvol_pool, const gchar *vm_name, PcvVmDeleteStoragePlan *plan)
{
    _plan_init(plan);
    plan->kind = PCV_VM_DELETE_STORAGE_ZVOL;
    plan->dataset = g_strdup_printf("%s/%s", zvol_pool, vm_name);
    plan->path = g_strconcat(ZVOL_PREFIX, plan->dataset, NULL);
}

                                                       
void
pcv_vm_delete_plan_clear(PcvVmDeleteStoragePlan *plan)
{
    if (!plan)
        return;
    g_free(plan->path);
    g_free(plan->dataset);
    g_clear_pointer(&plan->kept, g_ptr_array_unref);
    memset(plan, 0, sizeof(*plan));
}

                                    
static void
_disk_ref_free(gpointer data)
{
    PcvVmDeleteDiskRef *ref = data;
    g_free(ref->path);
    g_free(ref->format);
    g_free(ref);
}

                                                     
static void
_add_ref(GPtrArray *refs, xmlNodePtr source, const gchar *format)
{
    gchar *path = _source_path(source);
    if (!path)
        return;
    PcvVmDeleteDiskRef *ref = g_new0(PcvVmDeleteDiskRef, 1);
    ref->path = path;
    ref->format = g_strdup(format);
    g_ptr_array_add(refs, ref);
}

                                                                         
static void
_collect_chain(GPtrArray *refs, xmlNodePtr parent)
{
    for (xmlNodePtr cur = parent->children; cur; cur = cur->next) {
        if (cur->type != XML_ELEMENT_NODE)
            continue;
        if (xmlStrcmp(cur->name, BAD_CAST "backingStore") == 0 || xmlStrcmp(cur->name, BAD_CAST "mirror") == 0) {
            _add_ref(refs, _child(cur, "source"), NULL);
            _collect_chain(refs, cur);
        }
    }
}

                                                             
                                             
GPtrArray *
pcv_vm_delete_xml_disk_refs(const gchar *domain_xml, GError **error)
{
    xmlDocPtr doc = _read_domain(domain_xml, error);
    if (!doc)
        return NULL;
    GPtrArray *refs = g_ptr_array_new_with_free_func(_disk_ref_free);
    xmlNodePtr devices = _child(xmlDocGetRootElement(doc), "devices");
    for (xmlNodePtr disk = devices ? devices->children : NULL; disk; disk = disk->next) {
        if (disk->type != XML_ELEMENT_NODE || xmlStrcmp(disk->name, BAD_CAST "disk") != 0)
            continue;
        xmlNodePtr driver = _child(disk, "driver");
        xmlChar *format = driver ? xmlGetProp(driver, BAD_CAST "type") : NULL;
        _add_ref(refs, _child(disk, "source"), (const gchar *)format);
        xmlFree(format);
        _collect_chain(refs, disk);
    }
    xmlFreeDoc(doc);
    return refs;
}

                                                                               
GPtrArray *
pcv_vm_delete_parse_snapshot_clones(const gchar *zfs_output)
{
    GPtrArray *clones = g_ptr_array_new_with_free_func(g_free);
    g_auto(GStrv) lines = g_strsplit(zfs_output ? zfs_output : "", "\n", -1);
    for (gint i = 0; lines[i]; i++) {
        const gchar *tab = strchr(lines[i], '\t');
        if (!tab)
            continue;
        g_auto(GStrv) names = g_strsplit(tab + 1, ",", -1);
        for (gint j = 0; names[j]; j++) {
            g_strstrip(names[j]);
            if (*names[j] && g_strcmp0(names[j], "-") != 0)
                g_ptr_array_add(clones, g_strdup(names[j]));
        }
    }
    return clones;
}

                                                    
                                            
GPtrArray *
pcv_vm_delete_parse_backing_chain(const gchar *qemu_img_json, GError **error)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    if (!qemu_img_json || !json_parser_load_from_data(parser, qemu_img_json, -1, error)) {
        if (error && !*error)
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "qemu-img output is missing");
        return NULL;
    }
    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_ARRAY(root)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "qemu-img output is not an array");
        return NULL;
    }
    GPtrArray *files = g_ptr_array_new_with_free_func(g_free);
    JsonArray *chain = json_node_get_array(root);
    for (guint i = 0; i < json_array_get_length(chain); i++) {
        JsonNode *item = json_array_get_element(chain, i);
        if (!JSON_NODE_HOLDS_OBJECT(item))
            continue;
        JsonObject *image = json_node_get_object(item);
        const gchar *file = json_object_get_string_member_with_default(image, "full-backing-filename", NULL);
        if (!file)
            file = json_object_get_string_member_with_default(image, "backing-filename", NULL);
        if (file && *file)
            g_ptr_array_add(files, g_strdup(file));
    }
    return files;
}
