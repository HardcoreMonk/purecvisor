


































































#include "process_monitor.h"
#include "utils/pcv_log.h"
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <ctype.h>
#include <unistd.h>






















#define PROC_LOG_DOM      "proc_mon"


#define PROC_INTERVAL_SEC 20


#define PROC_MAX          512





typedef enum {
    PROC_HOST      = 0,
    PROC_VM        = 1,
    PROC_CONTAINER = 2,
    PROC_SYSTEM    = 3
} ProcType;


static const gchar *_proc_type_str[] = { "host", "vm", "container", "system" };









typedef struct {





    gint     pid;










    gchar    comm[64];












    gchar    state;








    guint64  utime;






    guint64  stime;











    glong    rss_pages;








    guint64  io_rd_bytes;






    guint64  io_wr_bytes;











    gdouble  cpu_percent;






    gchar    cgroup[256];






    ProcType type;
} ProcInfo;







static struct {

    GThread    *thread;


    gboolean    running;


    gboolean    initialized;






    GMutex      mu;





    ProcInfo    procs[PROC_MAX];


    gint        count;







    glong       page_size;









    glong       clk_tck;








    gint64      prev_time_us;



















    GHashTable *prev_ticks;
} G = {0};












static void
_get_process_cgroup(gint pid, gchar *buf, gsize bufsize)
{
    gchar path[64];
    g_snprintf(path, sizeof(path), "/proc/%d/cgroup", pid);

    gchar *content = NULL;
    if (!g_file_get_contents(path, &content, NULL, NULL)) {
        g_strlcpy(buf, "/", bufsize);
        return;
    }


    gchar **lines = g_strsplit(content, "\n", -1);
    gboolean found = FALSE;
    for (gint i = 0; lines[i]; i++) {
        if (g_str_has_prefix(lines[i], "0::")) {
            g_strlcpy(buf, lines[i] + 3, bufsize);
            found = TRUE;
            break;
        }
    }


    if (!found && lines[0]) {
        const gchar *last_colon = strrchr(lines[0], ':');
        if (last_colon)
            g_strlcpy(buf, last_colon + 1, bufsize);
        else
            g_strlcpy(buf, "/", bufsize);
    } else if (!found) {
        g_strlcpy(buf, "/", bufsize);
    }

    g_strfreev(lines);
    g_free(content);
}














static ProcType
_classify_process(const gchar *cgroup, const gchar *comm)
{
    if (strstr(cgroup, "/lxc/") || strstr(cgroup, "/lxc.payload."))
        return PROC_CONTAINER;
    if (strstr(cgroup, "/machine.slice/") || strstr(comm, "qemu"))
        return PROC_VM;
    if (strstr(cgroup, ".service"))
        return PROC_SYSTEM;
    return PROC_HOST;
}































































static void
_parse_proc_stat(gint pid, ProcInfo *p)
{
    gchar path[64];
    g_snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE *f = fopen(path, "r");
    if (!f) return;


    gchar buf[1024];
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return; }
    fclose(f);







    gchar *start = strchr(buf, '(');
    gchar *end = strrchr(buf, ')');
    if (!start || !end || end <= start) return;


    gsize clen = (gsize)(end - start - 1);
    if (clen >= sizeof(p->comm)) clen = sizeof(p->comm) - 1;
    memcpy(p->comm, start + 1, clen);
    p->comm[clen] = '\0';































    gchar *rest = end + 2;
    gchar state;
    gint ppid;
    unsigned long utime, stime;
    long rss;





    int n = sscanf(rest,
        "%c %d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu %*d %*d %*d %*d %*d %*d %*u %*u %ld",
        &state, &ppid, &utime, &stime, &rss);
    if (n >= 5) {
        p->pid = pid;
        p->state = state;
        p->utime = utime;
        p->stime = stime;
        p->rss_pages = rss;
    }
}



































static void
_parse_proc_io(gint pid, ProcInfo *p)
{
    gchar path[64];
    g_snprintf(path, sizeof(path), "/proc/%d/io", pid);
    FILE *f = fopen(path, "r");
    if (!f) return;

    gchar line[128];
    while (fgets(line, sizeof(line), f)) {
        guint64 val;
        if (sscanf(line, "read_bytes: %lu", &val) == 1)
            p->io_rd_bytes = val;
        else if (sscanf(line, "write_bytes: %lu", &val) == 1)
            p->io_wr_bytes = val;
    }
    fclose(f);
}









static gint
_sort_by_cpu(gconstpointer a, gconstpointer b)
{
    const ProcInfo *pa = a, *pb = b;
    if (pb->cpu_percent > pa->cpu_percent) return 1;
    if (pb->cpu_percent < pa->cpu_percent) return -1;
    return 0;
}






























































static void
_collect_processes(void)
{
    DIR *d = opendir("/proc");
    if (!d) return;

    ProcInfo tmp[PROC_MAX];
    gint count = 0;


    gint64 now_us = g_get_monotonic_time();





    gdouble dt_sec = (G.prev_time_us > 0)
        ? (gdouble)(now_us - G.prev_time_us) / G_USEC_PER_SEC
        : 0.0;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && count < PROC_MAX) {

        if (!isdigit((unsigned char)ent->d_name[0])) continue;
        gint pid = atoi(ent->d_name);
        if (pid <= 0) continue;

        ProcInfo *p = &tmp[count];
        memset(p, 0, sizeof(*p));


        _parse_proc_stat(pid, p);
        if (p->pid == 0) continue;


        _parse_proc_io(pid, p);


        _get_process_cgroup(pid, p->cgroup, sizeof(p->cgroup));
        p->type = _classify_process(p->cgroup, p->comm);










        guint64 total_ticks = p->utime + p->stime;
        gpointer prev_val = g_hash_table_lookup(G.prev_ticks, GINT_TO_POINTER(pid));
        if (prev_val && dt_sec > 0.0) {
            guint64 prev_ticks = GPOINTER_TO_SIZE(prev_val);
            guint64 delta = (total_ticks >= prev_ticks) ? total_ticks - prev_ticks : 0;
            p->cpu_percent = 100.0 * (gdouble)delta / ((gdouble)G.clk_tck * dt_sec);
        }

        g_hash_table_insert(G.prev_ticks, GINT_TO_POINTER(pid), GSIZE_TO_POINTER(total_ticks));
        count++;
    }
    closedir(d);













    qsort(tmp, count, sizeof(ProcInfo), _sort_by_cpu);






    g_mutex_lock(&G.mu);
    memcpy(G.procs, tmp, count * sizeof(ProcInfo));
    G.count = count;
    g_mutex_unlock(&G.mu);


    G.prev_time_us = now_us;
}













static gpointer
_proc_thread(gpointer data)
{
    (void)data;
    PCV_LOG_INFO(PROC_LOG_DOM, "Process monitor started (interval=%ds)", PROC_INTERVAL_SEC);

    while (G.running) {
        _collect_processes();
        g_usleep(PROC_INTERVAL_SEC * G_USEC_PER_SEC);
    }

    PCV_LOG_INFO(PROC_LOG_DOM, "Process monitor stopped");
    return NULL;
}












































static JsonObject *
_proc_to_json(const ProcInfo *p)
{
    JsonObject *obj = json_object_new();
    json_object_set_int_member   (obj, "pid",       p->pid);
    json_object_set_string_member(obj, "comm",      p->comm);
    gchar state_str[2] = {p->state, '\0'};
    json_object_set_string_member(obj, "state",     state_str);
    json_object_set_double_member(obj, "cpu_percent",p->cpu_percent);
    json_object_set_double_member(obj, "mem_mb",
        (gdouble)p->rss_pages * G.page_size / (1024.0 * 1024.0));
    json_object_set_int_member   (obj, "rss_kb",    p->rss_pages * (G.page_size / 1024));
    json_object_set_int_member   (obj, "io_rd_bytes",p->io_rd_bytes);
    json_object_set_int_member   (obj, "io_wr_bytes",p->io_wr_bytes);
    json_object_set_string_member(obj, "type",      _proc_type_str[p->type]);
    json_object_set_string_member(obj, "cgroup",    p->cgroup);
    return obj;
}















void
pcv_process_monitor_init(void)
{
    g_mutex_init(&G.mu);
    G.page_size = sysconf(_SC_PAGESIZE);
    G.clk_tck = sysconf(_SC_CLK_TCK);
    if (G.clk_tck <= 0) G.clk_tck = 100;
    G.prev_ticks = g_hash_table_new(g_direct_hash, g_direct_equal);
    G.running = TRUE;
    G.initialized = TRUE;
    G.thread = g_thread_new("proc-monitor", _proc_thread, NULL);
}














void
pcv_process_monitor_shutdown(void)
{
    if (!G.initialized) return;
    G.running = FALSE;
    if (G.thread) {
        g_thread_join(G.thread);
        G.thread = NULL;
    }
    if (G.prev_ticks) {
        g_hash_table_destroy(G.prev_ticks);
        G.prev_ticks = NULL;
    }
    g_mutex_clear(&G.mu);
    G.initialized = FALSE;
}













JsonArray *
pcv_process_monitor_get_top(gint n)
{
    JsonArray *arr = json_array_new();
    g_mutex_lock(&G.mu);
    gint limit = (n > 0 && n < G.count) ? n : G.count;
    for (gint i = 0; i < limit; i++)
        json_array_add_object_element(arr, _proc_to_json(&G.procs[i]));
    g_mutex_unlock(&G.mu);
    return arr;
}









JsonArray *
pcv_process_monitor_get_all(void)
{
    return pcv_process_monitor_get_top(G.count);
}







static gint
_parse_type_filter(const gchar *type_str)
{
    if (!type_str || !*type_str) return -1;
    if (g_strcmp0(type_str, "host") == 0)      return PROC_HOST;
    if (g_strcmp0(type_str, "vm") == 0)        return PROC_VM;
    if (g_strcmp0(type_str, "container") == 0) return PROC_CONTAINER;
    if (g_strcmp0(type_str, "system") == 0)    return PROC_SYSTEM;
    return -1;
}












JsonArray *
pcv_process_monitor_get_filtered(gint n, const gchar *type_str)
{
    gint type_filter = _parse_type_filter(type_str);


    if (type_filter < 0)
        return (n > 0) ? pcv_process_monitor_get_top(n) : pcv_process_monitor_get_all();

    JsonArray *arr = json_array_new();
    g_mutex_lock(&G.mu);
    gint added = 0;
    for (gint i = 0; i < G.count; i++) {
        if ((gint)G.procs[i].type != type_filter) continue;
        json_array_add_object_element(arr, _proc_to_json(&G.procs[i]));
        added++;
        if (n > 0 && added >= n) break;
    }
    g_mutex_unlock(&G.mu);
    return arr;
}
