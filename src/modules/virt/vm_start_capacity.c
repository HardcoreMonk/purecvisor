









#include "vm_start_capacity.h"
#include <errno.h>
#include <libvirt/virterror.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define HUGEPOOL "hugepages-2048kB"
#define HUGEPAGE_BYTES ((guint64)2097152)

typedef enum { NOT_REQUESTED, DEFERRED, REQUIRED, DENIED } RequirementState;
typedef struct {
    guint64 pages;
    gint host_node;
    const gchar *reason;
} Requirement;
typedef struct { guint64 total, free, reserved, surplus; } Pool;


static xmlNodePtr child(xmlNodePtr parent, const gchar *name)
{
    for (xmlNodePtr node = parent ? parent->children : NULL; node; node = node->next)
        if (node->type == XML_ELEMENT_NODE && !node->ns &&
            xmlStrEqual(node->name, (const xmlChar *)name)) return node;
    return NULL;
}


static guint children_named(xmlNodePtr parent, const gchar *name)
{
    guint count = 0;
    for (xmlNodePtr node = parent ? parent->children : NULL; node; node = node->next)
        if (node->type == XML_ELEMENT_NODE && !node->ns &&
            xmlStrEqual(node->name, (const xmlChar *)name)) count++;
    return count;
}


static gboolean decimal(const gchar *text, guint64 *value)
{
    if (!text || !*text) return FALSE;
    for (const gchar *p = text; *p; p++) if (!g_ascii_isdigit(*p)) return FALSE;
    errno = 0;
    gchar *end = NULL;
    *value = g_ascii_strtoull(text, &end, 10);
    return errno != ERANGE && end && !*end;
}


static gboolean size_bytes(const gchar *text, const gchar *unit, guint64 *bytes)
{
    guint64 value, factor = 0;
    if (!decimal(text, &value) || !value) return FALSE;
    gchar *normalized = g_ascii_strup(unit ? unit : "KiB", -1);
    const struct { const gchar *name; guint64 factor; } units[] = {
        {"B", 1}, {"BYTES", 1}, {"K", 1024}, {"KIB", 1024}, {"KB", 1000},
        {"M", 1048576}, {"MIB", 1048576}, {"MB", 1000000},
        {"G", 1073741824}, {"GIB", 1073741824}, {"GB", 1000000000},
        {"T", ((guint64)1 << 40)}, {"TIB", ((guint64)1 << 40)}, {"TB", 1000000000000}
    };
    for (guint i = 0; i < G_N_ELEMENTS(units); i++)
        if (g_str_equal(normalized, units[i].name)) { factor = units[i].factor; break; }
    g_free(normalized);
    if (!factor || value > G_MAXUINT64 / factor) return FALSE;
    *bytes = value * factor;
    return TRUE;
}


static RequirementState requirement(const gchar *xml, Requirement *out, GError **error)
{
    *out = (Requirement){ .host_node = -1 };
    gsize length = xml ? strlen(xml) : 0;
    xmlDocPtr doc = length && length <= INT_MAX ? xmlReadMemory(
        xml, (gint)length, NULL, NULL, XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING) : NULL;
    xmlNodePtr root = doc ? xmlDocGetRootElement(doc) : NULL;
    if (!root || root->ns || !xmlStrEqual(root->name, (const xmlChar *)"domain") ||
        doc->intSubset || doc->extSubset) {
        if (doc) xmlFreeDoc(doc);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "hugepage preflight: invalid domain XML");
        return DENIED;
    }
    RequirementState state = NOT_REQUESTED;
    xmlNodePtr backing = child(root, "memoryBacking");
    xmlNodePtr huge = child(backing, "hugepages");
    xmlChar *page_size = NULL, *page_unit = NULL, *page_nodes = NULL;
    xmlChar *memory = NULL, *memory_unit = NULL, *nodeset = NULL, *mode = NULL, *placement = NULL;
    if (!huge) goto done;
    state = DEFERRED;
    out->reason = "default or mixed page size";
    if (children_named(huge, "page") != 1) goto done;
    xmlNodePtr page = child(huge, "page");


    page_size = xmlGetProp(page, (const xmlChar *)"size");
    page_unit = xmlGetProp(page, (const xmlChar *)"unit");
    page_nodes = xmlGetProp(page, (const xmlChar *)"nodeset");
    guint64 bytes = 0;
    if (page_nodes || !size_bytes((const gchar *)page_size, (const gchar *)page_unit, &bytes) ||
        bytes != HUGEPAGE_BYTES) goto done;
    out->reason = "guest NUMA or memory hotplug";
    xmlNodePtr numa = child(root, "numatune");
    if (child(child(root, "cpu"), "numa") || child(numa, "memnode") ||
        child(child(root, "devices"), "memory")) goto done;
    out->reason = "automatic or complex host NUMA policy";
    xmlNodePtr vcpu = child(root, "vcpu");
    placement = xmlGetProp(vcpu, (const xmlChar *)"placement");
    if (placement && xmlStrEqual(placement, (const xmlChar *)"auto")) goto done;
    xmlFree(placement); placement = NULL;
    xmlNodePtr numa_memory = child(numa, "memory");
    if (numa_memory) {
        mode = xmlGetProp(numa_memory, (const xmlChar *)"mode");
        placement = xmlGetProp(numa_memory, (const xmlChar *)"placement");
        nodeset = xmlGetProp(numa_memory, (const xmlChar *)"nodeset");
        guint64 node = 0;
        if ((mode && !xmlStrEqual(mode, (const xmlChar *)"strict")) ||
            (placement && !xmlStrEqual(placement, (const xmlChar *)"static")) ||
            !decimal((const gchar *)nodeset, &node) || node > 4095) goto done;
        out->host_node = (gint)node;
    }
    state = DENIED;
    out->reason = NULL;
    if (children_named(root, "memory") != 1 || children_named(root, "numatune") > 1 ||
        children_named(root, "memoryBacking") != 1) goto invalid_memory;
    xmlNodePtr memory_node = child(root, "memory");
    memory = xmlNodeGetContent(memory_node);
    memory_unit = xmlGetProp(memory_node, (const xmlChar *)"unit");
    if (!memory || !size_bytes(g_strstrip((gchar *)memory), (const gchar *)memory_unit, &bytes))
        goto invalid_memory;


    out->pages = bytes / HUGEPAGE_BYTES + (bytes % HUGEPAGE_BYTES != 0);
    state = REQUIRED;
    goto done;
invalid_memory:
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "hugepage preflight: invalid or ambiguous domain memory");
done:
    xmlFree(page_size); xmlFree(page_unit); xmlFree(page_nodes);
    xmlFree(memory); xmlFree(memory_unit); xmlFree(nodeset); xmlFree(mode); xmlFree(placement);
    xmlFreeDoc(doc);
    return state;
}


static gboolean counter(const gchar *base, const gchar *name, guint64 *value, GError **error)
{
    gchar *path = g_build_filename(base, name, NULL), *text = NULL;
    gboolean ok = g_file_get_contents(path, &text, NULL, error);
    if (ok && (!decimal(g_strstrip(text), value) || *value > G_MAXINT64)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "hugepage preflight: invalid counter %s", path);
        ok = FALSE;
    }
    g_free(text); g_free(path);
    return ok;
}


static gboolean read_pool(const gchar *base, gboolean global, Pool *out, GError **error)
{
    *out = (Pool){0};
    if (!counter(base, "nr_hugepages", &out->total, error) ||
        !counter(base, "free_hugepages", &out->free, error) ||
        !counter(base, "surplus_hugepages", &out->surplus, error) ||
        (global && !counter(base, "resv_hugepages", &out->reserved, error))) return FALSE;
    if (out->free > out->total + out->surplus) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "hugepage preflight: free exceeds total plus surplus");
        return FALSE;
    }
    return TRUE;
}


static gboolean equal_pool(const Pool *a, const Pool *b)
{
    return a->total == b->total && a->free == b->free &&
           a->reserved == b->reserved && a->surplus == b->surplus;
}


static gboolean capacity(const Requirement *req, const gchar *sysfs_root, GError **error)
{
    gchar *global_path = g_build_filename(sysfs_root, "kernel/mm/hugepages", HUGEPOOL, NULL);
    gchar *node_relative = req->host_node >= 0 ? g_strdup_printf(
        "devices/system/node/node%d/hugepages", req->host_node) : NULL;
    gchar *node_path = node_relative ? g_build_filename(sysfs_root, node_relative, HUGEPOOL, NULL) : NULL;
    Pool before, after, node_before = {0}, node_after = {0};



    gboolean ok = read_pool(global_path, TRUE, &before, error) &&
        (!node_path || (read_pool(node_path, FALSE, &node_before, error) &&
                        read_pool(node_path, FALSE, &node_after, error))) &&
        read_pool(global_path, TRUE, &after, error);
    if (!ok) goto done;
    if (!equal_pool(&before, &after) || (node_path &&
        (!equal_pool(&node_before, &node_after) || node_before.free > before.free))) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "hugepage preflight: counter changed or global/node mismatch");
        ok = FALSE;
        goto done;
    }
    guint64 free_pages = node_path ? node_before.free : before.free;
    guint64 available = free_pages > before.reserved ? free_pages - before.reserved : 0;
    if (req->pages > available) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
                    "hugepage capacity insufficient: need=%" G_GUINT64_FORMAT
                    " pages, available=%" G_GUINT64_FORMAT " pages, host_node=%d",
                    req->pages, available, req->host_node);
        ok = FALSE;
    }
done:
    g_free(global_path); g_free(node_relative); g_free(node_path);
    return ok;
}


static gboolean check_domain(virDomainPtr domain, const gchar *root, GError **error)
{
    char *xml = virDomainGetXMLDesc(domain, VIR_DOMAIN_XML_INACTIVE);
    if (!xml) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "hugepage preflight: domain XML read failed");
        return FALSE;
    }
    Requirement req;
    RequirementState state = requirement(xml, &req, error);
    free(xml);
    if (state == DENIED) return FALSE;
    if (state == REQUIRED) {
        gint saved = virDomainHasManagedSaveImage(domain, 0);
        if (saved < 0) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "hugepage preflight: managed-save query failed");
            return FALSE;
        }
        if (!saved) return capacity(&req, root, error);
        req.reason = "managed-save restore";
        state = DEFERRED;
    }
    if (state == DEFERRED)
        g_warning("[vm.start] hugepage capacity deferred: %s; native validation remains", req.reason);
    return TRUE;
}


gboolean pcv_vm_start_with_capacity(virDomainPtr *domain_io, const gchar *sysfs_root,
                                     PcvVmStartPrepare prepare, gpointer prepare_data, GError **error)
{
    if (!domain_io || !*domain_io || !sysfs_root || !prepare) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "hugepage preflight: invalid start arguments");
        return FALSE;
    }


    if (!check_domain(*domain_io, sysfs_root, error)) return FALSE;
    if (!prepare(domain_io, prepare_data, error)) {
        if (error && !*error)
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "VM start preparation failed");
        return FALSE;
    }
    if (!*domain_io) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "VM start preparation lost domain");
        return FALSE;
    }
    if (!check_domain(*domain_io, sysfs_root, error)) return FALSE;
    if (virDomainCreate(*domain_io) < 0) {
        virErrorPtr native_error = virGetLastError();
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to start VM: %s",
                    native_error && native_error->message ? native_error->message : "unknown error");
        return FALSE;
    }
    return TRUE;
}
