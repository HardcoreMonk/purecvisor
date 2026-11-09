                           
                                                                             
                                                                    
                       
                                                       
                                                                
#include "modules/virt/vm_start_capacity.h"
#include <glib/gstdio.h>
#include <libvirt/virterror.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    gboolean huge, owner, ok, done, change;
    gint prepares, creates;
    GError *error;
} Trial;
static GMutex mu;
static GCond changed;
static gboolean entered, release_owner;
static const gchar *mode, *root;

static void barrier(void)
{
    g_mutex_lock(&mu);
    entered = TRUE;
    g_cond_broadcast(&changed);
    gint64 end = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (!release_owner)
        g_assert_true(g_cond_wait_until(&changed, &mu, end));
    g_mutex_unlock(&mu);
}

char *virDomainGetXMLDesc(virDomainPtr domain, unsigned int flags)
{
    g_assert_cmpuint(flags, ==, VIR_DOMAIN_XML_INACTIVE);
    Trial *trial = (Trial *)domain;
    return strdup(trial->huge ?
        "<domain><memory unit='MiB'>8</memory><memoryBacking><hugepages>"
        "<page size='2048' unit='KiB'/></hugepages></memoryBacking></domain>" :
        "<domain><memory unit='MiB'>8</memory></domain>");
}

int virDomainHasManagedSaveImage(virDomainPtr domain G_GNUC_UNUSED,
                                unsigned int flags G_GNUC_UNUSED) { return 0; }

static void consume_pool(void)
{
    gchar *path = g_build_filename(root, "kernel/mm/hugepages/hugepages-2048kB/free_hugepages", NULL);
    g_assert_true(g_file_set_contents(path, "0\n", -1, NULL));
    g_free(path);
}

int virDomainCreate(virDomainPtr domain)
{
    Trial *trial = (Trial *)domain;
    trial->creates++;
    if (trial->owner && g_str_has_prefix(mode, "native")) barrier();
    if (trial->owner && g_str_equal(mode, "consume")) consume_pool();
    return trial->owner && g_str_equal(mode, "native-failure") ? -1 : 0;
}

virErrorPtr virGetLastError(void)
{
    static virError error = { .message = "fixture native failure" };
    return &error;
}

static gboolean prepare(virDomainPtr *domain, gpointer data G_GNUC_UNUSED, GError **error)
{
    Trial *trial = (Trial *)*domain;
    trial->prepares++;
    if (trial->change) trial->huge = TRUE;
    if (trial->owner && !g_str_has_prefix(mode, "native")) barrier();
    if (trial->owner && g_str_equal(mode, "prepare-failure")) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "fixture prepare failure");
        return FALSE;
    }
    return TRUE;
}

static gpointer attempt(gpointer data)
{
    Trial *trial = data;
    virDomainPtr domain = (virDomainPtr)trial;
    trial->ok = pcv_vm_start_with_capacity(&domain, root, prepare, NULL, &trial->error);
    g_mutex_lock(&mu);
    trial->done = TRUE;
    g_cond_broadcast(&changed);
    g_mutex_unlock(&mu);
    return NULL;
}

int main(int argc, char **argv)
{
    g_assert_cmpint(argc, ==, 3);
    root = argv[1]; mode = argv[2];
    Trial owner = { .huge = TRUE, .owner = TRUE };
    Trial second = { .huge = !g_str_equal(mode, "ordinary") && !g_str_equal(mode, "change"),
                     .change = g_str_equal(mode, "change") };
    GThread *first_thread = g_thread_new("owner", attempt, &owner);
    g_mutex_lock(&mu);
    gint64 end = g_get_monotonic_time() + 3 * G_TIME_SPAN_SECOND;
    while (!entered) g_assert_true(g_cond_wait_until(&changed, &mu, end));
    g_mutex_unlock(&mu);
    GThread *second_thread = g_thread_new("contender", attempt, &second);
    g_mutex_lock(&mu);
    end = g_get_monotonic_time() + G_TIME_SPAN_SECOND;
    while (!second.done) g_assert_true(g_cond_wait_until(&changed, &mu, end));
                                                                                               
    release_owner = TRUE;
    g_cond_broadcast(&changed);
    g_mutex_unlock(&mu);
    g_thread_join(second_thread); g_thread_join(first_thread);

    gboolean ordinary = g_str_equal(mode, "ordinary");
    g_assert_cmpint(second.ok, ==, ordinary);
    g_assert_cmpint(second.creates, ==, ordinary ? 1 : 0);
    g_assert_cmpint(second.prepares, ==, ordinary || second.change ? 1 : 0);
    if (!ordinary) g_assert_error(second.error, G_IO_ERROR, G_IO_ERROR_BUSY);
    gboolean fail = g_str_has_suffix(mode, "failure");
    g_assert_cmpint(owner.ok, ==, !fail);
    if (fail) g_assert_nonnull(owner.error);

    Trial retry = { .huge = TRUE };
    attempt(&retry);
    if (g_str_equal(mode, "consume")) {
        g_assert_false(retry.ok);
        g_assert_error(retry.error, G_IO_ERROR, G_IO_ERROR_NO_SPACE);
        g_assert_cmpint(retry.prepares, ==, 0);
        g_assert_cmpint(retry.creates, ==, 0);
        gchar *path = g_build_filename(root, "kernel/mm/hugepages/hugepages-2048kB/free_hugepages", NULL);
        g_assert_true(g_file_set_contents(path, "10\n", -1, NULL));
        g_free(path);
        Trial recovered = { .huge = TRUE };
        attempt(&recovered);
        g_assert_true(recovered.ok);
        g_assert_cmpint(recovered.creates, ==, 1);
        g_clear_error(&recovered.error);
    } else {
        g_assert_true(retry.ok);
        g_assert_cmpint(retry.prepares, ==, 1);
        g_assert_cmpint(retry.creates, ==, 1);
    }
    printf("PASS %s: contender prepare=%d create=%d retry=%d\n", mode,
           second.prepares, second.creates, retry.ok);
    g_clear_error(&owner.error); g_clear_error(&second.error); g_clear_error(&retry.error);
    g_cond_clear(&changed); g_mutex_clear(&mu);
    return 0;
}
