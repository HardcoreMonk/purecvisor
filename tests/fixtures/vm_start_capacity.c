






#include <gio/gio.h>
#include <glib/gstdio.h>
#include <dlfcn.h>
#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "modules/virt/vm_start_capacity.h"

static const gchar *fixture_root;
static const gchar *xml_path;
static const gchar *effects_path;
static const gchar *fixture_mode;
static guint managed_queries;
static gboolean counter_changed;



gboolean g_file_get_contents(const gchar *filename, gchar **contents,
                            gsize *length, GError **error)
{
    typedef gboolean (*ReadFile)(const gchar *, gchar **, gsize *, GError **);
    ReadFile real_read = (ReadFile)dlsym(RTLD_NEXT, "g_file_get_contents");
    g_assert_nonnull(real_read);
    gboolean ok = real_read(filename, contents, length, error);
    if (ok && !counter_changed && g_strcmp0(fixture_mode, "counter-change") == 0 &&
        g_str_has_suffix(filename, "resv_hugepages")) {
        gchar *total = g_build_filename(fixture_root, "kernel/mm/hugepages",
                                        "hugepages-2048kB/nr_hugepages", NULL);
        g_assert_true(g_file_set_contents(total, "21\n", -1, NULL));
        g_free(total);
        counter_changed = TRUE;
    }
    return ok;
}


static void effect(const char *name, virDomainPtr domain)
{
    FILE *stream = fopen(effects_path, "a");
    if (!stream) abort();
    fprintf(stream, "%s %d\n", name, domain == (virDomainPtr)2 ? 2 : 1);
    fclose(stream);
}


char *virDomainGetXMLDesc(virDomainPtr domain G_GNUC_UNUSED, unsigned int flags)
{
    g_assert_cmpuint(flags, ==, VIR_DOMAIN_XML_INACTIVE);
    if (g_strcmp0(fixture_mode, "xml-error") == 0) return NULL;
    gchar *contents = NULL;
    if (!g_file_get_contents(xml_path, &contents, NULL, NULL)) return NULL;
    char *copy = strdup(contents);
    g_free(contents);
    return copy;
}


int virDomainHasManagedSaveImage(virDomainPtr domain G_GNUC_UNUSED,
                                unsigned int flags G_GNUC_UNUSED)
{
    managed_queries++;
    if (g_strcmp0(fixture_mode, "managed-save") == 0) return 1;
    if (g_strcmp0(fixture_mode, "managed-error") == 0) return -1;
    if (g_strcmp0(fixture_mode, "managed-late-error") == 0 && managed_queries > 1) return -1;
    return 0;
}


int virDomainCreate(virDomainPtr domain)
{
    effect("CREATE", domain);
    return g_strcmp0(fixture_mode, "native-error") == 0 ? -1 : 0;
}


virErrorPtr virGetLastError(void)
{
    static virError error = { .message = "fixture native error" };
    return &error;
}


static gboolean prepare(virDomainPtr *domain, gpointer data G_GNUC_UNUSED,
                        GError **error)
{
    effect("PREPARE", *domain);
    if (g_strcmp0(fixture_mode, "prepare-error") == 0) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "fixture prepare failed");
        return FALSE;
    }

    if (g_strcmp0(fixture_mode, "prepare-no-error") == 0) return FALSE;
    if (g_strcmp0(fixture_mode, "lost-domain") == 0) *domain = NULL;
    if (g_strcmp0(fixture_mode, "xml-lost") == 0) g_assert_cmpint(g_unlink(xml_path), ==, 0);
    if (g_strcmp0(fixture_mode, "memory-change") == 0)
        g_assert_true(g_file_set_contents(xml_path,
            "<domain><memory unit='MiB'>40</memory><memoryBacking><hugepages>"
            "<page size='2048' unit='KiB'/></hugepages></memoryBacking>"
            "<numatune><memory mode='strict' nodeset='0'/></numatune></domain>", -1, NULL));
    if (g_strcmp0(fixture_mode, "grow") == 0) {
        gchar *global = g_build_filename(fixture_root, "kernel/mm/hugepages",
                                         "hugepages-2048kB/free_hugepages", NULL);
        gchar *node = g_build_filename(fixture_root, "devices/system/node/node0/hugepages",
                                       "hugepages-2048kB/free_hugepages", NULL);
        g_assert_true(g_file_set_contents(global, "11\n", -1, NULL));
        g_assert_true(g_file_set_contents(node, "1\n", -1, NULL));
        g_free(global); g_free(node);
    }
    if (g_strcmp0(fixture_mode, "replace-domain") == 0) *domain = (virDomainPtr)2;
    return TRUE;
}


int main(int argc, char **argv)
{
    if (argc != 5) return 2;
    fixture_root = argv[1]; xml_path = argv[2]; effects_path = argv[3]; fixture_mode = argv[4];
    g_assert_true(g_file_set_contents(effects_path, "", 0, NULL));
    virDomainPtr domain = (virDomainPtr)1;
    GError *error = NULL;
    gboolean ok = pcv_vm_start_with_capacity(&domain, fixture_root, prepare, NULL, &error);
    if (error) g_print("ERROR %s\n", error->message);
    g_clear_error(&error);
    return ok ? 0 : 1;
}
