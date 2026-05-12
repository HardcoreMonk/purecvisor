
















































#include "ebpf_telemetry.h"
#include <libvirt/libvirt.h>
#include <string.h>
#include <stdio.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <dirent.h>
#include "utils/pcv_log.h"
#include "prometheus_exporter.h"
#include "modules/ai/anomaly_detector.h"
#include "modules/ai/workload_predict.h"
#include "modules/virt/virt_conn_pool.h"
#include "modules/virt/circuit_breaker.h"
#include "modules/core/vm_state.h"
#include "utils/pcv_spawn.h"
#include "utils/pcv_tls.h"
#include "utils/pcv_worker_pool.h"
#include "modules/audit/pcv_audit.h"
#include "modules/storage/zfs_driver.h"




















#define EBPF_LOG_DOM       "ebpf_telem"
#define EBPF_INTERVAL_SEC  5
#define EBPF_MAX_VMS       64























typedef struct {
    gchar    name[64];

    gdouble  cpu_percent;
    guint64  cpu_time_ns;

    guint64  mem_max_kb;
    guint64  mem_used_kb;

    guint64  disk_rd_bytes;
    guint64  disk_wr_bytes;
    guint64  disk_rd_reqs;
    guint64  disk_wr_reqs;

    guint64  net_rx_bytes;
    guint64  net_tx_bytes;
    guint64  net_rx_pkts;
    guint64  net_tx_pkts;

    gint     state;


} VmExtMetrics;


















typedef struct {






    gdouble  cpu_percent;
    gdouble  cpu_user;
    gdouble  cpu_system;
    gdouble  cpu_nice;
    gdouble  cpu_iowait;
    gdouble  cpu_steal;
    gdouble  cpu_irq;
    gdouble  cpu_softirq;
    gdouble  cpu_idle;






    guint64  mem_total_kb;
    guint64  mem_avail_kb;
    guint64  mem_free_kb;
    guint64  mem_buffers_kb;
    guint64  mem_cached_kb;
    guint64  mem_slab_kb;
    guint64  mem_sreclaimable_kb;
    guint64  swap_total_kb;
    guint64  swap_free_kb;
    guint64  pgfault;
    guint64  pgmajfault;






    gdouble  load_1m;
    gdouble  load_5m;
    gdouble  load_15m;






    guint64  net_rx_bytes;
    guint64  net_tx_bytes;
    guint64  net_rx_packets;
    guint64  net_tx_packets;
    guint64  net_rx_errs;
    guint64  net_tx_errs;
    guint64  net_rx_drop;
    guint64  net_tx_drop;







    guint64  disk_rd_bytes;
    guint64  disk_wr_bytes;
    guint64  disk_rd_ios;
    guint64  disk_wr_ios;
    guint64  disk_io_ticks_ms;
} HostMetrics;














static struct {
    GThread      *thread;
    gboolean      running;
    VmExtMetrics  vms[EBPF_MAX_VMS];
    gint          vm_count;
    HostMetrics   host;
    GMutex        mu;
    gboolean      initialized;
} G = {0};
































static void
_collect_host_cpu(HostMetrics *h)
{
    static guint64 prev[8] = {0};
    static guint64 prev_total = 0;
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return;

    char buf[256];
    if (fgets(buf, sizeof(buf), f)) {
        unsigned long long user, nice, sys, idle, iowait, irq, softirq, steal;
        if (sscanf(buf, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &user, &nice, &sys, &idle, &iowait, &irq, &softirq, &steal) == 8) {
            guint64 cur[8] = {user, nice, sys, idle, iowait, irq, softirq, steal};
            guint64 total = user + nice + sys + idle + iowait + irq + softirq + steal;

            if (prev_total == 0) {

                prev_total = total;
                memcpy(prev, cur, sizeof(prev));
                h->cpu_percent = 0.0;
            } else {
                guint64 dt = total - prev_total;
                if (dt > 0) {

#define _CPU_PCT(idx) (100.0 * (gdouble)(cur[idx] - prev[idx]) / (gdouble)dt)
                    h->cpu_user    = _CPU_PCT(0);
                    h->cpu_nice    = _CPU_PCT(1);
                    h->cpu_system  = _CPU_PCT(2);
                    h->cpu_idle    = _CPU_PCT(3);
                    h->cpu_iowait  = _CPU_PCT(4);
                    h->cpu_irq     = _CPU_PCT(5);
                    h->cpu_softirq = _CPU_PCT(6);
                    h->cpu_steal   = _CPU_PCT(7);
#undef _CPU_PCT

                    h->cpu_percent = 100.0 - h->cpu_idle - h->cpu_iowait;
                }
                prev_total = total;
                memcpy(prev, cur, sizeof(prev));
            }
        }
    }
    fclose(f);
}






















static void
_collect_host_mem(HostMetrics *h)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return;

    char line[128];
    while (fgets(line, sizeof(line), f)) {
        guint64 val;
        if (sscanf(line, "MemTotal: %lu kB", &val) == 1)
            h->mem_total_kb = val;
        else if (sscanf(line, "MemAvailable: %lu kB", &val) == 1)
            h->mem_avail_kb = val;
        else if (sscanf(line, "MemFree: %lu kB", &val) == 1)
            h->mem_free_kb = val;
        else if (sscanf(line, "Buffers: %lu kB", &val) == 1)
            h->mem_buffers_kb = val;
        else if (sscanf(line, "Cached: %lu kB", &val) == 1)
            h->mem_cached_kb = val;
        else if (sscanf(line, "Slab: %lu kB", &val) == 1)
            h->mem_slab_kb = val;
        else if (sscanf(line, "SReclaimable: %lu kB", &val) == 1)
            h->mem_sreclaimable_kb = val;
        else if (sscanf(line, "SwapTotal: %lu kB", &val) == 1)
            h->swap_total_kb = val;
        else if (sscanf(line, "SwapFree: %lu kB", &val) == 1)
            h->swap_free_kb = val;
    }
    fclose(f);





    f = fopen("/proc/vmstat", "r");
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        guint64 val;
        if (sscanf(line, "pgfault %lu", &val) == 1)
            h->pgfault = val;
        else if (sscanf(line, "pgmajfault %lu", &val) == 1)
            h->pgmajfault = val;
    }
    fclose(f);
}












static void
_collect_host_load(HostMetrics *h)
{
    FILE *f = fopen("/proc/loadavg", "r");
    if (!f) return;
    double l1, l5, l15;
    if (fscanf(f, "%lf %lf %lf", &l1, &l5, &l15) == 3) {
        h->load_1m  = l1;
        h->load_5m  = l5;
        h->load_15m = l15;
    }
    fclose(f);
}
























static void
_collect_host_net(HostMetrics *h)
{
    FILE *f = fopen("/proc/net/dev", "r");
    if (!f) return;

    guint64 rx_total = 0, tx_total = 0;
    guint64 rx_pkt = 0, tx_pkt = 0;
    guint64 rx_err = 0, tx_err = 0;
    guint64 rx_drp = 0, tx_drp = 0;
    char line[512];

    if (!fgets(line, sizeof(line), f)) { fclose(f); return; }
    if (!fgets(line, sizeof(line), f)) { fclose(f); return; }

    while (fgets(line, sizeof(line), f)) {
        char iface[32];
        guint64 rb, rp, re, rd;
        guint64 tb, tp, te, td;




        if (sscanf(line,
            " %31[^:]: %lu %lu %lu %lu %*u %*u %*u %*u %lu %lu %lu %lu",
            iface, &rb, &rp, &re, &rd, &tb, &tp, &te, &td) == 9) {
            g_strstrip(iface);
            if (g_strcmp0(iface, "lo") == 0) continue;
            rx_total += rb; rx_pkt += rp; rx_err += re; rx_drp += rd;
            tx_total += tb; tx_pkt += tp; tx_err += te; tx_drp += td;
        }
    }
    fclose(f);
    h->net_rx_bytes   = rx_total;
    h->net_tx_bytes   = tx_total;
    h->net_rx_packets = rx_pkt;
    h->net_tx_packets = tx_pkt;
    h->net_rx_errs    = rx_err;
    h->net_tx_errs    = tx_err;
    h->net_rx_drop    = rx_drp;
    h->net_tx_drop    = tx_drp;
}







































static void
_collect_host_disk_io(HostMetrics *h)
{
    FILE *f = fopen("/proc/diskstats", "r");
    if (!f) return;

    guint64 rd_bytes = 0, wr_bytes = 0, rd_ios = 0, wr_ios = 0, io_ticks = 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        unsigned int major, minor;
        char dev[64];
        unsigned long long rd_io, rd_m, rd_sec, rd_t;
        unsigned long long wr_io, wr_m, wr_sec, wr_t;
        unsigned long long io_now, io_tk, io_wt;
        int n = sscanf(line,
            " %u %u %63s %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
            &major, &minor, dev,
            &rd_io, &rd_m, &rd_sec, &rd_t,
            &wr_io, &wr_m, &wr_sec, &wr_t,
            &io_now, &io_tk, &io_wt);
        if (n < 14) continue;

        size_t dlen = strlen(dev);
        if (dlen == 0) continue;
        gboolean keep = FALSE;
        if (strncmp(dev, "sd", 2) == 0 && dlen == 3) keep = TRUE;
        else if (strncmp(dev, "vd", 2) == 0 && dlen == 3) keep = TRUE;
        else if (strncmp(dev, "nvme", 4) == 0 && strstr(dev, "p") == NULL) keep = TRUE;
        if (!keep) continue;

        rd_ios   += rd_io;
        wr_ios   += wr_io;
        rd_bytes += rd_sec * 512;
        wr_bytes += wr_sec * 512;
        io_ticks += io_tk;
    }
    fclose(f);
    h->disk_rd_bytes   = rd_bytes;
    h->disk_wr_bytes   = wr_bytes;
    h->disk_rd_ios     = rd_ios;
    h->disk_wr_ios     = wr_ios;
    h->disk_io_ticks_ms = io_ticks;
}


































static void
_collect_node_cpu(void)
{
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return;

    char buf[512];
    while (fgets(buf, sizeof(buf), f)) {
        int cpuid;
        unsigned long long user, nice, sys, idle, iowait, irq, softirq, steal;
        if (pcv_ebpf_proc_stat_is_cpu_core_line(buf) &&
            sscanf(buf, "cpu%d %llu %llu %llu %llu %llu %llu %llu %llu",
                   &cpuid, &user, &nice, &sys, &idle, &iowait, &irq, &softirq, &steal) == 9) {
            char cpu[8];
            g_snprintf(cpu, sizeof(cpu), "%d", cpuid);
            char lbl[64];
#define _SET_CPU(mode, val) \
            g_snprintf(lbl, sizeof(lbl), "cpu=\"%s\",mode=\"" mode "\"", cpu); \
            pcv_prom_gauge_set_labels("node_cpu_seconds_total", lbl, (gdouble)(val) / 100.0);
            _SET_CPU("user",    user);
            _SET_CPU("nice",    nice);
            _SET_CPU("system",  sys);
            _SET_CPU("idle",    idle);
            _SET_CPU("iowait",  iowait);
            _SET_CPU("irq",     irq);
            _SET_CPU("softirq", softirq);
            _SET_CPU("steal",   steal);
#undef _SET_CPU
        }

        unsigned long long ctxt;
        if (sscanf(buf, "ctxt %llu", &ctxt) == 1)
            pcv_prom_gauge_set_labels("node_context_switches_total", "", (gdouble)ctxt);
        unsigned long long forks;
        if (sscanf(buf, "processes %llu", &forks) == 1)
            pcv_prom_gauge_set_labels("node_forks_total", "", (gdouble)forks);
        unsigned long long procs_running;
        if (sscanf(buf, "procs_running %llu", &procs_running) == 1)
            pcv_prom_gauge_set_labels("node_procs_running", "", (gdouble)procs_running);
        unsigned long long procs_blocked;
        if (sscanf(buf, "procs_blocked %llu", &procs_blocked) == 1)
            pcv_prom_gauge_set_labels("node_procs_blocked", "", (gdouble)procs_blocked);
    }
    fclose(f);
}





















static void
_collect_node_meminfo(void)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char key[64];
        unsigned long long val;
        if (sscanf(line, "%63[^:]: %llu kB", key, &val) == 2) {


            for (char *p = key; *p; p++) {
                if (*p == '(' || *p == ')') *p = '_';
            }
            char metric[128];
            g_snprintf(metric, sizeof(metric), "node_memory_%s_bytes", key);
            pcv_prom_gauge_set_labels(metric, "", (gdouble)val * 1024.0);
        }
    }
    fclose(f);
}




















static void
_collect_node_filesystem(void)
{
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) return;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char dev[128], mount[128], fstype[32];
        if (sscanf(line, "%127s %127s %31s", dev, mount, fstype) < 3)
            continue;

        if (dev[0] != '/' && strncmp(fstype, "zfs", 3) != 0)
            continue;

        if (strcmp(fstype, "tmpfs") == 0 || strcmp(fstype, "devtmpfs") == 0 ||
            strcmp(fstype, "proc") == 0 || strcmp(fstype, "sysfs") == 0)
            continue;

        struct statvfs vfs;
        if (statvfs(mount, &vfs) != 0) continue;

        gdouble total = (gdouble)vfs.f_blocks * vfs.f_frsize;
        gdouble free_b = (gdouble)vfs.f_bfree * vfs.f_frsize;
        gdouble avail = (gdouble)vfs.f_bavail * vfs.f_frsize;
        gdouble files = (gdouble)vfs.f_files;
        gdouble ffree = (gdouble)vfs.f_ffree;

        char lbl[256];
        g_snprintf(lbl, sizeof(lbl),
            "device=\"%s\",mountpoint=\"%s\",fstype=\"%s\"", dev, mount, fstype);

        pcv_prom_gauge_set_labels("node_filesystem_size_bytes", lbl, total);
        pcv_prom_gauge_set_labels("node_filesystem_free_bytes", lbl, free_b);
        pcv_prom_gauge_set_labels("node_filesystem_avail_bytes", lbl, avail);
        pcv_prom_gauge_set_labels("node_filesystem_files", lbl, files);
        pcv_prom_gauge_set_labels("node_filesystem_files_free", lbl, ffree);
        gdouble ro = (vfs.f_flag & ST_RDONLY) ? 1.0 : 0.0;
        pcv_prom_gauge_set_labels("node_filesystem_readonly", lbl, ro);
    }
    fclose(f);
}

























static void
_collect_node_diskstats(void)
{
    FILE *f = fopen("/proc/diskstats", "r");
    if (!f) return;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        unsigned int major, minor;
        char dev[64];
        unsigned long long rd_ios, rd_merges, rd_sectors, rd_ticks;
        unsigned long long wr_ios, wr_merges, wr_sectors, wr_ticks;
        unsigned long long io_now, io_ticks, io_wt;
        unsigned long long dc_ios = 0, dc_merges = 0, dc_sectors = 0, dc_ticks = 0;

        int n = sscanf(line,
            " %u %u %63s %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
            &major, &minor, dev,
            &rd_ios, &rd_merges, &rd_sectors, &rd_ticks,
            &wr_ios, &wr_merges, &wr_sectors, &wr_ticks,
            &io_now, &io_ticks, &io_wt,
            &dc_ios, &dc_merges, &dc_sectors, &dc_ticks);
        if (n < 14) continue;


        size_t dlen = strlen(dev);
        if (dlen > 0 && dev[dlen-1] >= '0' && dev[dlen-1] <= '9') {

            if (strncmp(dev, "nvme", 4) != 0 &&
                strncmp(dev, "dm-", 3) != 0 &&
                strncmp(dev, "zd", 2) != 0) {

                gboolean is_part = FALSE;
                for (int i = (int)dlen - 1; i >= 0 && dev[i] >= '0' && dev[i] <= '9'; i--) {
                    if (i > 0 && ((dev[i-1] >= 'a' && dev[i-1] <= 'z') || (dev[i-1] >= 'A' && dev[i-1] <= 'Z'))) {

                        if (i > 1) { is_part = TRUE; break; }
                    }
                }
                if (is_part) continue;
            }
        }

        char lbl[64];
        g_snprintf(lbl, sizeof(lbl), "device=\"%s\"", dev);

        pcv_prom_gauge_set_labels("node_disk_reads_completed_total", lbl, (gdouble)rd_ios);
        pcv_prom_gauge_set_labels("node_disk_reads_merged_total", lbl, (gdouble)rd_merges);
        pcv_prom_gauge_set_labels("node_disk_read_bytes_total", lbl, (gdouble)rd_sectors * 512.0);
        pcv_prom_gauge_set_labels("node_disk_read_time_seconds_total", lbl, (gdouble)rd_ticks / 1000.0);
        pcv_prom_gauge_set_labels("node_disk_writes_completed_total", lbl, (gdouble)wr_ios);
        pcv_prom_gauge_set_labels("node_disk_writes_merged_total", lbl, (gdouble)wr_merges);
        pcv_prom_gauge_set_labels("node_disk_written_bytes_total", lbl, (gdouble)wr_sectors * 512.0);
        pcv_prom_gauge_set_labels("node_disk_write_time_seconds_total", lbl, (gdouble)wr_ticks / 1000.0);
        pcv_prom_gauge_set_labels("node_disk_io_now", lbl, (gdouble)io_now);
        pcv_prom_gauge_set_labels("node_disk_io_time_seconds_total", lbl, (gdouble)io_ticks / 1000.0);
        pcv_prom_gauge_set_labels("node_disk_io_time_weighted_seconds_total", lbl, (gdouble)io_wt / 1000.0);
        if (n >= 18) {
            pcv_prom_gauge_set_labels("node_disk_discards_completed_total", lbl, (gdouble)dc_ios);
            pcv_prom_gauge_set_labels("node_disk_discards_merged_total", lbl, (gdouble)dc_merges);
            pcv_prom_gauge_set_labels("node_disk_discard_time_seconds_total", lbl, (gdouble)dc_ticks / 1000.0);
        }
    }
    fclose(f);
}
























static void
_collect_node_netdev(void)
{
    FILE *f = fopen("/proc/net/dev", "r");
    if (!f) return;

    char line[512];

    if (!fgets(line, sizeof(line), f)) { fclose(f); return; }
    if (!fgets(line, sizeof(line), f)) { fclose(f); return; }

    while (fgets(line, sizeof(line), f)) {
        char iface[32];

        unsigned long long rb, rp, re, rd, rfi, rfr, rc, rmu;
        unsigned long long tb, tp, te, td, tfi, tco, tcr, tcomp;
        if (sscanf(line,
            " %31[^:]: %llu %llu %llu %llu %llu %llu %llu %llu"
            " %llu %llu %llu %llu %llu %llu %llu %llu",
            iface, &rb, &rp, &re, &rd, &rfi, &rfr, &rc, &rmu,
            &tb, &tp, &te, &td, &tfi, &tco, &tcr, &tcomp) != 17)
            continue;

        g_strstrip(iface);
        char lbl[64];
        g_snprintf(lbl, sizeof(lbl), "device=\"%s\"", iface);

        pcv_prom_gauge_set_labels("node_network_receive_bytes_total", lbl, (gdouble)rb);
        pcv_prom_gauge_set_labels("node_network_receive_packets_total", lbl, (gdouble)rp);
        pcv_prom_gauge_set_labels("node_network_receive_errs_total", lbl, (gdouble)re);
        pcv_prom_gauge_set_labels("node_network_receive_drop_total", lbl, (gdouble)rd);
        pcv_prom_gauge_set_labels("node_network_receive_multicast_total", lbl, (gdouble)rmu);
        pcv_prom_gauge_set_labels("node_network_transmit_bytes_total", lbl, (gdouble)tb);
        pcv_prom_gauge_set_labels("node_network_transmit_packets_total", lbl, (gdouble)tp);
        pcv_prom_gauge_set_labels("node_network_transmit_errs_total", lbl, (gdouble)te);
        pcv_prom_gauge_set_labels("node_network_transmit_drop_total", lbl, (gdouble)td);
    }
    fclose(f);
}



















static void
_collect_node_vmstat(void)
{
    FILE *f = fopen("/proc/vmstat", "r");
    if (!f) return;

    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char key[64];
        unsigned long long val;
        if (sscanf(line, "%63s %llu", key, &val) == 2) {

            if (strcmp(key, "pgfault") == 0 || strcmp(key, "pgmajfault") == 0 ||
                strcmp(key, "pgpgin") == 0 || strcmp(key, "pgpgout") == 0 ||
                strcmp(key, "pswpin") == 0 || strcmp(key, "pswpout") == 0 ||
                strcmp(key, "oom_kill") == 0) {
                char metric[128];
                g_snprintf(metric, sizeof(metric), "node_vmstat_%s", key);
                pcv_prom_gauge_set_labels(metric, "", (gdouble)val);
            }
        }
    }
    fclose(f);
}





















static void
_collect_node_sockstat(void)
{
    FILE *f = fopen("/proc/net/sockstat", "r");
    if (!f) return;

    char line[256];
    while (fgets(line, sizeof(line), f)) {

        unsigned long long v;
        if (sscanf(line, "sockets: used %llu", &v) == 1)
            pcv_prom_gauge_set_labels("node_sockstat_sockets_used", "", (gdouble)v);

        unsigned long long inuse, orphan, tw, alloc, mem;
        if (sscanf(line, "TCP: inuse %llu orphan %llu tw %llu alloc %llu mem %llu",
                   &inuse, &orphan, &tw, &alloc, &mem) == 5) {
            pcv_prom_gauge_set_labels("node_sockstat_TCP_inuse", "", (gdouble)inuse);
            pcv_prom_gauge_set_labels("node_sockstat_TCP_orphan", "", (gdouble)orphan);
            pcv_prom_gauge_set_labels("node_sockstat_TCP_tw", "", (gdouble)tw);
            pcv_prom_gauge_set_labels("node_sockstat_TCP_alloc", "", (gdouble)alloc);
            pcv_prom_gauge_set_labels("node_sockstat_TCP_mem", "", (gdouble)mem);
        }
        unsigned long long udp_inuse, udp_mem;
        if (sscanf(line, "UDP: inuse %llu mem %llu", &udp_inuse, &udp_mem) == 2) {
            pcv_prom_gauge_set_labels("node_sockstat_UDP_inuse", "", (gdouble)udp_inuse);
            pcv_prom_gauge_set_labels("node_sockstat_UDP_mem", "", (gdouble)udp_mem);
        }
    }
    fclose(f);
}




























static void
_collect_node_pressure_file(const char *resource)
{
    char path[64];
    g_snprintf(path, sizeof(path), "/proc/pressure/%s", resource);
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char type[16];
        double avg10, avg60, avg300;
        unsigned long long total_us;
        if (sscanf(line, "%15s avg10=%lf avg60=%lf avg300=%lf total=%llu",
                   type, &avg10, &avg60, &avg300, &total_us) == 5) {
            if (strcmp(type, "some") == 0 || strcmp(type, "full") == 0) {
                char metric[128];
                g_snprintf(metric, sizeof(metric),
                    "node_pressure_%s_%s_seconds_total", resource, type);
                pcv_prom_gauge_set_labels(metric, "", (gdouble)total_us / 1e6);
            }
        }
    }
    fclose(f);
}


static void
_collect_node_pressure(void)
{
    _collect_node_pressure_file("cpu");
    _collect_node_pressure_file("io");
    _collect_node_pressure_file("memory");
}





















static void
_collect_node_hwmon(void)
{
    DIR *d = opendir("/sys/class/hwmon");
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;


        char name_path[256], chip_name[64] = "unknown";
        g_snprintf(name_path, sizeof(name_path), "/sys/class/hwmon/%s/name", ent->d_name);
        FILE *nf = fopen(name_path, "r");
        if (nf) {
            if (fgets(chip_name, sizeof(chip_name), nf))
                chip_name[strcspn(chip_name, "\n")] = 0;
            fclose(nf);
        }


        for (int i = 1; i <= 16; i++) {
            char tpath[256];
            g_snprintf(tpath, sizeof(tpath), "/sys/class/hwmon/%s/temp%d_input", ent->d_name, i);
            FILE *tf = fopen(tpath, "r");
            if (!tf) break;

            int milli = 0;
            if (fscanf(tf, "%d", &milli) == 1) {
                char lbl[128];
                g_snprintf(lbl, sizeof(lbl), "chip=\"%s\",sensor=\"temp%d\"", chip_name, i);
                pcv_prom_gauge_set_labels("node_hwmon_temp_celsius", lbl, (gdouble)milli / 1000.0);
            }
            fclose(tf);


            g_snprintf(tpath, sizeof(tpath), "/sys/class/hwmon/%s/temp%d_crit", ent->d_name, i);
            tf = fopen(tpath, "r");
            if (tf) {
                int crit = 0;
                if (fscanf(tf, "%d", &crit) == 1) {
                    char lbl[128];
                    g_snprintf(lbl, sizeof(lbl), "chip=\"%s\",sensor=\"temp%d\"", chip_name, i);
                    pcv_prom_gauge_set_labels("node_hwmon_temp_crit_celsius", lbl, (gdouble)crit / 1000.0);
                }
                fclose(tf);
            }
        }
    }
    closedir(d);
}



































static void
_collect_node_misc(void)
{

    struct sysinfo si;
    if (sysinfo(&si) == 0) {
        gdouble boot = (gdouble)(g_get_real_time() / G_USEC_PER_SEC) - (gdouble)si.uptime;
        pcv_prom_gauge_set_labels("node_boot_time_seconds", "", boot);
        pcv_prom_gauge_set_labels("node_time_seconds", "",
            (gdouble)(g_get_real_time() / G_USEC_PER_SEC));
    }


    FILE *f = fopen("/proc/loadavg", "r");
    if (f) {
        double l1, l5, l15;
        if (fscanf(f, "%lf %lf %lf", &l1, &l5, &l15) == 3) {
            pcv_prom_gauge_set_labels("node_load1", "", l1);
            pcv_prom_gauge_set_labels("node_load5", "", l5);
            pcv_prom_gauge_set_labels("node_load15", "", l15);
        }
        fclose(f);
    }


    f = fopen("/proc/uptime", "r");
    if (f) {
        double up;
        if (fscanf(f, "%lf", &up) == 1)
            pcv_prom_gauge_set_labels("node_uptime_seconds", "", up);
        fclose(f);
    }


    f = fopen("/proc/sys/kernel/random/entropy_avail", "r");
    if (f) {
        int ent;
        if (fscanf(f, "%d", &ent) == 1)
            pcv_prom_gauge_set_labels("node_entropy_available_bits", "", (gdouble)ent);
        fclose(f);
    }


    f = fopen("/proc/sys/fs/file-nr", "r");
    if (f) {
        unsigned long long allocated, unused, max_fd;
        if (fscanf(f, "%llu %llu %llu", &allocated, &unused, &max_fd) == 3) {
            pcv_prom_gauge_set_labels("node_filefd_allocated", "", (gdouble)allocated);
            pcv_prom_gauge_set_labels("node_filefd_maximum", "", (gdouble)max_fd);
        }
        fclose(f);
    }


    f = fopen("/proc/sys/net/netfilter/nf_conntrack_count", "r");
    if (f) {
        unsigned long long ct;
        if (fscanf(f, "%llu", &ct) == 1)
            pcv_prom_gauge_set_labels("node_nf_conntrack_entries", "", (gdouble)ct);
        fclose(f);
    }
    f = fopen("/proc/sys/net/netfilter/nf_conntrack_max", "r");
    if (f) {
        unsigned long long ct;
        if (fscanf(f, "%llu", &ct) == 1)
            pcv_prom_gauge_set_labels("node_nf_conntrack_entries_limit", "", (gdouble)ct);
        fclose(f);
    }


    f = fopen("/proc/net/arp", "r");
    if (f) {
        char line[256];
        int count = -1;
        while (fgets(line, sizeof(line), f)) count++;
        if (count > 0)
            pcv_prom_gauge_set_labels("node_arp_entries", "device=\"eno1\"", (gdouble)count);
        fclose(f);
    }


    DIR *d = opendir("/sys/class/net");
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.' || strcmp(ent->d_name, "lo") == 0) continue;
            char lbl[64];
            g_snprintf(lbl, sizeof(lbl), "device=\"%s\"", ent->d_name);


            char path[256];
            g_snprintf(path, sizeof(path), "/sys/class/net/%s/mtu", ent->d_name);
            FILE *mf = fopen(path, "r");
            if (mf) {
                int mtu;
                if (fscanf(mf, "%d", &mtu) == 1)
                    pcv_prom_gauge_set_labels("node_network_mtu_bytes", lbl, (gdouble)mtu);
                fclose(mf);
            }


            g_snprintf(path, sizeof(path), "/sys/class/net/%s/speed", ent->d_name);
            mf = fopen(path, "r");
            if (mf) {
                int speed;
                if (fscanf(mf, "%d", &speed) == 1 && speed > 0)
                    pcv_prom_gauge_set_labels("node_network_speed_bytes",
                        lbl, (gdouble)speed * 125000.0);
                fclose(mf);
            }


            g_snprintf(path, sizeof(path), "/sys/class/net/%s/carrier", ent->d_name);
            mf = fopen(path, "r");
            if (mf) {
                int carrier;
                if (fscanf(mf, "%d", &carrier) == 1)
                    pcv_prom_gauge_set_labels("node_network_carrier", lbl, (gdouble)carrier);
                fclose(mf);
            }
        }
        closedir(d);
    }
}
































static void
_collect_vm_metrics(void)
{
    virConnectPtr conn = virConnectOpen("qemu:///system");
    if (!conn) return;

    virDomainStatsRecordPtr *stats = NULL;
    unsigned int flags = VIR_CONNECT_GET_ALL_DOMAINS_STATS_RUNNING;
    gint nstats = virConnectGetAllDomainStats(conn,
        VIR_DOMAIN_STATS_STATE | VIR_DOMAIN_STATS_CPU_TOTAL |
        VIR_DOMAIN_STATS_BALLOON | VIR_DOMAIN_STATS_BLOCK |
        VIR_DOMAIN_STATS_INTERFACE,
        &stats, flags);

    if (nstats < 0) {
        virConnectClose(conn);
        return;
    }

    g_mutex_lock(&G.mu);
    G.vm_count = 0;

    for (gint i = 0; i < nstats && G.vm_count < EBPF_MAX_VMS; i++) {
        virDomainStatsRecordPtr rec = stats[i];
        const gchar *name = virDomainGetName(rec->dom);
        if (!name) continue;

        VmExtMetrics *vm = &G.vms[G.vm_count++];
        memset(vm, 0, sizeof(*vm));
        g_strlcpy(vm->name, name, sizeof(vm->name));


        for (gint j = 0; j < rec->nparams; j++) {
            virTypedParameterPtr p = &rec->params[j];

            if (g_strcmp0(p->field, "state.state") == 0)
                vm->state = p->value.i;
            else if (g_strcmp0(p->field, "cpu.time") == 0)
                vm->cpu_time_ns = p->value.ul;
            else if (g_strcmp0(p->field, "balloon.maximum") == 0)
                vm->mem_max_kb = p->value.ul;
            else if (g_strcmp0(p->field, "balloon.current") == 0)
                vm->mem_used_kb = p->value.ul;

            else if (g_str_has_prefix(p->field, "block.") &&
                     g_str_has_suffix(p->field, ".rd.bytes"))
                vm->disk_rd_bytes += p->value.ul;
            else if (g_str_has_prefix(p->field, "block.") &&
                     g_str_has_suffix(p->field, ".wr.bytes"))
                vm->disk_wr_bytes += p->value.ul;
            else if (g_str_has_prefix(p->field, "block.") &&
                     g_str_has_suffix(p->field, ".rd.reqs"))
                vm->disk_rd_reqs += p->value.ul;
            else if (g_str_has_prefix(p->field, "block.") &&
                     g_str_has_suffix(p->field, ".wr.reqs"))
                vm->disk_wr_reqs += p->value.ul;

            else if (g_str_has_prefix(p->field, "net.") &&
                     g_str_has_suffix(p->field, ".rx.bytes"))
                vm->net_rx_bytes += p->value.ul;
            else if (g_str_has_prefix(p->field, "net.") &&
                     g_str_has_suffix(p->field, ".tx.bytes"))
                vm->net_tx_bytes += p->value.ul;
            else if (g_str_has_prefix(p->field, "net.") &&
                     g_str_has_suffix(p->field, ".rx.pkts"))
                vm->net_rx_pkts += p->value.ul;
            else if (g_str_has_prefix(p->field, "net.") &&
                     g_str_has_suffix(p->field, ".tx.pkts"))
                vm->net_tx_pkts += p->value.ul;
        }
    }
    g_mutex_unlock(&G.mu);

    virDomainStatsRecordListFree(stats);
    virConnectClose(conn);
}



















static gpointer
_ebpf_thread(gpointer data)
{
    (void)data;
    PCV_LOG_INFO(EBPF_LOG_DOM, "eBPF telemetry thread started (interval=%ds)", EBPF_INTERVAL_SEC);

    while (G.running) {

        HostMetrics h = {0};
        _collect_host_cpu(&h);
        _collect_host_mem(&h);
        _collect_host_load(&h);
        _collect_host_net(&h);
        _collect_host_disk_io(&h);

        g_mutex_lock(&G.mu);
        G.host = h;
        g_mutex_unlock(&G.mu);


        _collect_vm_metrics();











        {
            g_mutex_lock(&G.mu);
            for (gint i = 0; i < G.vm_count; i++) {
                gchar lbl[128];
                g_snprintf(lbl, sizeof(lbl), "vm_name=\"%s\"", G.vms[i].name);
                pcv_prom_gauge_set_labels("purecvisor_vm_net_rx_bytes_total",
                                          lbl, (gdouble)G.vms[i].net_rx_bytes);
                pcv_prom_gauge_set_labels("purecvisor_vm_net_tx_bytes_total",
                                          lbl, (gdouble)G.vms[i].net_tx_bytes);
                pcv_prom_gauge_set_labels("purecvisor_vm_net_rx_packets_total",
                                          lbl, (gdouble)G.vms[i].net_rx_pkts);
                pcv_prom_gauge_set_labels("purecvisor_vm_net_tx_packets_total",
                                          lbl, (gdouble)G.vms[i].net_tx_pkts);
            }
            g_mutex_unlock(&G.mu);
        }


        _collect_node_cpu();
        _collect_node_meminfo();
        _collect_node_filesystem();
        _collect_node_diskstats();
        _collect_node_netdev();
        _collect_node_vmstat();
        _collect_node_sockstat();
        _collect_node_pressure();
        _collect_node_hwmon();
        _collect_node_misc();


        pcv_anomaly_evaluate();
        pcv_predict_evaluate();















        {
            guint idle = 0, total = 0, max = 0;
            virt_conn_pool_stats(&idle, &total, &max);
            pcv_prom_gauge_set_labels("purecvisor_connpool_idle", "", (gdouble)idle);
            pcv_prom_gauge_set_labels("purecvisor_connpool_active", "", (gdouble)(total - idle));
            pcv_prom_gauge_set_labels("purecvisor_connpool_max", "", (gdouble)max);
            pcv_prom_gauge_set_labels("purecvisor_connpool_wait_seconds", "",
                                      virt_conn_pool_wait_avg_seconds());
        }


        pcv_prom_gauge_set_labels("purecvisor_worker_pool_pending", "",
                                  (gdouble)pcv_worker_pool_get_pending());


        pcv_prom_gauge_set_labels("purecvisor_audit_queue_depth", "",
                                  (gdouble)pcv_audit_get_queue_depth());
        pcv_prom_gauge_set_labels("purecvisor_audit_dropped_total", "",
                                  (gdouble)pcv_audit_get_dropped_count());


        {
            static gint64 last_pool_check = 0;
            gint64 now_pool_us = g_get_monotonic_time();
            if (now_pool_us - last_pool_check >= 60 * G_USEC_PER_SEC) {
                last_pool_check = now_pool_us;
                ZfsPoolHealth zh;
                if (pcv_zfs_pool_health("pcvpool", &zh)) {
                    gdouble state_val = 0.0;
                    if (g_strcmp0(zh.state, "DEGRADED") == 0) state_val = 1.0;
                    else if (g_strcmp0(zh.state, "FAULTED") == 0) state_val = 2.0;
                    else if (g_strcmp0(zh.state, "UNAVAIL") == 0) state_val = 3.0;

                    pcv_prom_gauge_set_labels("purecvisor_zpool_state", "", state_val);
                    pcv_prom_gauge_set_labels("purecvisor_zpool_errors_read", "",
                                              (gdouble)zh.errors_read);
                    pcv_prom_gauge_set_labels("purecvisor_zpool_errors_write", "",
                                              (gdouble)zh.errors_write);
                    pcv_prom_gauge_set_labels("purecvisor_zpool_errors_cksum", "",
                                              (gdouble)zh.errors_cksum);
                    if (zh.scrub_age_sec >= 0) {
                        pcv_prom_gauge_set_labels("purecvisor_zpool_scrub_age_seconds", "",
                                                  (gdouble)zh.scrub_age_sec);
                    }
                    pcv_prom_gauge_set_labels("purecvisor_zpool_capacity_percent", "",
                                              zh.capacity_pct);
                }
            }
        }


        {
            static gint64 last_cap_record = 0;
            gint64 now_cap_us = g_get_monotonic_time();
            if (now_cap_us - last_cap_record >= (gint64)3600 * G_USEC_PER_SEC) {
                last_cap_record = now_cap_us;
                pcv_zfs_capacity_record("pcvpool");
            }
        }



















        {
            gchar *ka_out = NULL;
            const gchar *ka_argv[] = {"systemctl", "is-active", "keepalived", NULL};
            gboolean ka_running = pcv_spawn_sync(ka_argv, &ka_out, NULL, NULL);
            pcv_prom_gauge_set_labels("purecvisor_keepalived_active", "",
                                      ka_running ? 1.0 : 0.0);
            g_free(ka_out);


            gchar *ip_out = NULL;
            const gchar *ip_argv[] = {"ip", "-o", "addr", "show", "dev", "pcvbr0", NULL};
            if (pcv_spawn_sync(ip_argv, &ip_out, NULL, NULL) && ip_out) {
                gboolean has_vip = (g_strstr_len(ip_out, -1, "192.0.2.100") != NULL);
                pcv_prom_gauge_set_labels("purecvisor_keepalived_vip_owner", "",
                                          has_vip ? 1.0 : 0.0);

                pcv_prom_gauge_set_labels("purecvisor_keepalived_master", "",
                                          has_vip ? 1.0 : 0.0);
            }
            g_free(ip_out);
        }


        pcv_prom_gauge_set_labels("purecvisor_circuit_breaker_state", "",
                                  (gdouble)cb_get_state());
        pcv_prom_gauge_set_labels("purecvisor_circuit_breaker_failures_total", "",
                                  (gdouble)cb_get_failure_count());
        pcv_prom_gauge_set_labels("purecvisor_vm_locks_held", "",
                                  (gdouble)pcv_vm_state_get_lock_count());








        {
            gint64 cert_days = pcv_tls_get_cert_expiry_days();
            if (cert_days >= 0) {
                pcv_prom_gauge_set_labels("purecvisor_tls_cert_expiry_days", "",
                                          (gdouble)cert_days);
            }

            {
                static guint cert_check_counter = 0;
                if (++cert_check_counter >= 720) {
                    cert_check_counter = 0;
                    pcv_tls_check_expiry_warning();
                }
            }
        }

        g_usleep(EBPF_INTERVAL_SEC * G_USEC_PER_SEC);
    }

    PCV_LOG_INFO(EBPF_LOG_DOM, "eBPF telemetry thread stopped");
    return NULL;
}









void
pcv_ebpf_telemetry_init(void)
{
    g_mutex_init(&G.mu);
    G.running = TRUE;
    G.initialized = TRUE;
    G.thread = g_thread_new("ebpf-telem", _ebpf_thread, NULL);
    PCV_LOG_INFO(EBPF_LOG_DOM, "eBPF telemetry initialized");
}













void
pcv_ebpf_telemetry_shutdown(void)
{
    if (!G.initialized) return;
    G.running = FALSE;
    if (G.thread) {
        g_thread_join(G.thread);
        G.thread = NULL;
    }
    g_mutex_clear(&G.mu);
    G.initialized = FALSE;
}






















































static JsonObject *g_cached_host_obj = nullptr;
static gint64      g_cached_host_ts  = 0;
static GMutex      g_host_cache_mu;

JsonObject *
pcv_ebpf_telemetry_get_host(void)
{
    gint64 now = g_get_monotonic_time();
    g_mutex_lock(&g_host_cache_mu);
    if (g_cached_host_obj && (now - g_cached_host_ts) < 2 * G_USEC_PER_SEC) {
        JsonObject *ref = json_object_ref(g_cached_host_obj);
        g_mutex_unlock(&g_host_cache_mu);
        return ref;
    }
    g_mutex_unlock(&g_host_cache_mu);

    JsonObject *obj = json_object_new();
    g_mutex_lock(&G.mu);


    json_object_set_double_member(obj, "cpu_percent",    G.host.cpu_percent);
    json_object_set_double_member(obj, "cpu_user",       G.host.cpu_user);
    json_object_set_double_member(obj, "cpu_system",     G.host.cpu_system);
    json_object_set_double_member(obj, "cpu_nice",       G.host.cpu_nice);
    json_object_set_double_member(obj, "cpu_iowait",     G.host.cpu_iowait);
    json_object_set_double_member(obj, "cpu_steal",      G.host.cpu_steal);
    json_object_set_double_member(obj, "cpu_irq",        G.host.cpu_irq);
    json_object_set_double_member(obj, "cpu_softirq",    G.host.cpu_softirq);
    json_object_set_double_member(obj, "cpu_idle",       G.host.cpu_idle);


    json_object_set_int_member   (obj, "mem_total_kb",       G.host.mem_total_kb);
    json_object_set_int_member   (obj, "mem_avail_kb",       G.host.mem_avail_kb);
    json_object_set_int_member   (obj, "mem_free_kb",        G.host.mem_free_kb);
    json_object_set_int_member   (obj, "mem_buffers_kb",     G.host.mem_buffers_kb);
    json_object_set_int_member   (obj, "mem_cached_kb",      G.host.mem_cached_kb);
    json_object_set_int_member   (obj, "mem_slab_kb",        G.host.mem_slab_kb);
    json_object_set_int_member   (obj, "mem_sreclaimable_kb",G.host.mem_sreclaimable_kb);
    json_object_set_int_member   (obj, "swap_total_kb",      G.host.swap_total_kb);
    json_object_set_int_member   (obj, "swap_free_kb",       G.host.swap_free_kb);
    json_object_set_int_member   (obj, "pgfault",            G.host.pgfault);
    json_object_set_int_member   (obj, "pgmajfault",         G.host.pgmajfault);

    gdouble mem_pct = G.host.mem_total_kb > 0
        ? 100.0 * (1.0 - (gdouble)G.host.mem_avail_kb / (gdouble)G.host.mem_total_kb)
        : 0.0;
    json_object_set_double_member(obj, "mem_percent", mem_pct);


    json_object_set_double_member(obj, "load_1m",        G.host.load_1m);
    json_object_set_double_member(obj, "load_5m",        G.host.load_5m);
    json_object_set_double_member(obj, "load_15m",       G.host.load_15m);


    json_object_set_int_member   (obj, "net_rx_bytes",   G.host.net_rx_bytes);
    json_object_set_int_member   (obj, "net_tx_bytes",   G.host.net_tx_bytes);
    json_object_set_int_member   (obj, "net_rx_packets", G.host.net_rx_packets);
    json_object_set_int_member   (obj, "net_tx_packets", G.host.net_tx_packets);
    json_object_set_int_member   (obj, "net_rx_errs",    G.host.net_rx_errs);
    json_object_set_int_member   (obj, "net_tx_errs",    G.host.net_tx_errs);
    json_object_set_int_member   (obj, "net_rx_drop",    G.host.net_rx_drop);
    json_object_set_int_member   (obj, "net_tx_drop",    G.host.net_tx_drop);


    json_object_set_int_member   (obj, "disk_rd_bytes",    G.host.disk_rd_bytes);
    json_object_set_int_member   (obj, "disk_wr_bytes",    G.host.disk_wr_bytes);
    json_object_set_int_member   (obj, "disk_rd_ios",      G.host.disk_rd_ios);
    json_object_set_int_member   (obj, "disk_wr_ios",      G.host.disk_wr_ios);
    json_object_set_int_member   (obj, "disk_io_ticks_ms", G.host.disk_io_ticks_ms);

    g_mutex_unlock(&G.mu);


    g_mutex_lock(&g_host_cache_mu);
    if (g_cached_host_obj) json_object_unref(g_cached_host_obj);
    g_cached_host_obj = json_object_ref(obj);
    g_cached_host_ts  = g_get_monotonic_time();
    g_mutex_unlock(&g_host_cache_mu);

    return obj;
}












static JsonObject *
_vm_to_json(const VmExtMetrics *vm)
{
    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "name",          vm->name);
    json_object_set_int_member   (obj, "state",         vm->state);
    json_object_set_int_member   (obj, "cpu_time_ns",   vm->cpu_time_ns);
    json_object_set_int_member   (obj, "mem_max_kb",    vm->mem_max_kb);
    json_object_set_int_member   (obj, "mem_used_kb",   vm->mem_used_kb);
    json_object_set_int_member   (obj, "disk_rd_bytes", vm->disk_rd_bytes);
    json_object_set_int_member   (obj, "disk_wr_bytes", vm->disk_wr_bytes);
    json_object_set_int_member   (obj, "disk_rd_reqs",  vm->disk_rd_reqs);
    json_object_set_int_member   (obj, "disk_wr_reqs",  vm->disk_wr_reqs);
    json_object_set_int_member   (obj, "net_rx_bytes",  vm->net_rx_bytes);
    json_object_set_int_member   (obj, "net_tx_bytes",  vm->net_tx_bytes);
    json_object_set_int_member   (obj, "net_rx_pkts",   vm->net_rx_pkts);
    json_object_set_int_member   (obj, "net_tx_pkts",   vm->net_tx_pkts);
    return obj;
}














JsonObject *
pcv_ebpf_telemetry_get_vm(const gchar *vm_name)
{
    g_mutex_lock(&G.mu);
    for (gint i = 0; i < G.vm_count; i++) {
        if (g_strcmp0(G.vms[i].name, vm_name) == 0) {
            JsonObject *obj = _vm_to_json(&G.vms[i]);
            g_mutex_unlock(&G.mu);
            return obj;
        }
    }
    g_mutex_unlock(&G.mu);

    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "error", "VM not found in telemetry cache");
    return obj;
}













JsonArray *
pcv_ebpf_telemetry_get_all_vms(void)
{
    JsonArray *arr = json_array_new();
    g_mutex_lock(&G.mu);
    for (gint i = 0; i < G.vm_count; i++)
        json_array_add_object_element(arr, _vm_to_json(&G.vms[i]));
    g_mutex_unlock(&G.mu);
    return arr;
}
