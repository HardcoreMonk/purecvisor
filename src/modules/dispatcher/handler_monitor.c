
#include "rpc_utils.h"
#include <gio/gio.h>
#include "api/uds_server.h"
#include "modules/virt/virt_conn_pool.h"
#include "purecvisor/pcv_handler_util.h"
#include "modules/daemons/telemetry.h"
#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>

#include <sys/sysinfo.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/statvfs.h>

static void silent_libvirt_error_func(void *userdata, virErrorPtr err) {

    (void)userdata; (void)err;
}

static gboolean
skip_proc_lines(FILE *stream, char *buf, gsize buf_size, guint lines)
{
    for (guint i = 0; i < lines; i++) {
        if (!fgets(buf, (int)buf_size, stream))
            return FALSE;
    }
    return TRUE;
}

extern virDomainPtr pure_virt_get_domain(virConnectPtr conn, const gchar *identifier);

void handle_monitor_metrics(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    const gchar *vm_id = json_object_get_string_member(params, "vm_id");
    if (!vm_id) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing parameter: vm_id");
        pure_uds_server_send_response(server, connection, err); g_free(err); return;
    }

    virConnectPtr conn;
    PCV_REQUIRE_VIRT_CONN(conn, rpc_id, server, connection);
    virDomainPtr dom = pure_virt_get_domain(conn, vm_id);
    if (!dom) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "VM Entity not found");
        pure_uds_server_send_response(server, connection, err); g_free(err); virt_conn_pool_release(conn); return;
    }

    virDomainInfo info;
    if (virDomainGetInfo(dom, &info) < 0) {
        virErrorPtr libvirt_err = virGetLastError();
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, libvirt_err ? libvirt_err->message : "Failed to get metrics");
        pure_uds_server_send_response(server, connection, err); g_free(err);
        virDomainFree(dom); virt_conn_pool_release(conn); return;
    }

    const gchar *state_str = "UNKNOWN";
    switch (info.state) {
        case VIR_DOMAIN_RUNNING: state_str = "RUNNING"; break;
        case VIR_DOMAIN_BLOCKED: state_str = "BLOCKED"; break;
        case VIR_DOMAIN_PAUSED:  state_str = "PAUSED"; break;
        case VIR_DOMAIN_SHUTDOWN:state_str = "SHUTDOWN"; break;
        case VIR_DOMAIN_SHUTOFF: state_str = "SHUTOFF"; break;
        case VIR_DOMAIN_CRASHED: state_str = "CRASHED"; break;
    }

    JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
    JsonObject *res_obj = json_object_new();

    json_object_set_string_member(res_obj, "state", state_str);
    json_object_set_int_member(res_obj, "vcpu", info.nrVirtCpu);
    json_object_set_double_member(res_obj, "mem_max_mb", info.maxMem / 1024.0);
    json_object_set_double_member(res_obj, "mem_used_mb", info.memory / 1024.0);
    json_object_set_int_member(res_obj, "cpu_time_ns", info.cpuTime);

    {
        extern VmMetrics* get_vm_metrics(const gchar *vm_id);
        char uuid_buf[VIR_UUID_STRING_BUFLEN];
        if (virDomainGetUUIDString(dom, uuid_buf) == 0) {
            VmMetrics *net = get_vm_metrics(uuid_buf);
            if (net) {
                JsonObject *net_obj = json_object_new();
                json_object_set_int_member(net_obj, "rx_bytes",   (gint64)net->rx_bytes);
                json_object_set_int_member(net_obj, "tx_bytes",   (gint64)net->tx_bytes);
                json_object_set_int_member(net_obj, "rx_packets", (gint64)net->rx_packets);
                json_object_set_int_member(net_obj, "tx_packets", (gint64)net->tx_packets);
                json_object_set_int_member(net_obj, "rx_errs",    (gint64)net->rx_errs);
                json_object_set_int_member(net_obj, "tx_errs",    (gint64)net->tx_errs);
                json_object_set_int_member(net_obj, "rx_drop",    (gint64)net->rx_drop);
                json_object_set_int_member(net_obj, "tx_drop",    (gint64)net->tx_drop);
                json_object_set_object_member(res_obj, "net", net_obj);
            }
        }
    }

    json_node_take_object(res_node, res_obj);

    gchar *resp = pure_rpc_build_success_response(rpc_id, res_node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);

    virDomainFree(dom);
    virt_conn_pool_release(conn);
}

gchar *handle_monitor_fleet(JsonObject *params, GError **error) {

    virSetErrorFunc(NULL, silent_libvirt_error_func);

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Libvirt connection failed.");
        return NULL;
    }

    virDomainStatsRecordPtr *stats = NULL;
    unsigned int stats_flags = VIR_DOMAIN_STATS_STATE | VIR_DOMAIN_STATS_CPU_TOTAL | VIR_DOMAIN_STATS_VCPU | VIR_DOMAIN_STATS_BALLOON | VIR_DOMAIN_STATS_BLOCK | VIR_DOMAIN_STATS_INTERFACE;

    int count = virConnectGetAllDomainStats(conn, stats_flags, &stats, 0);
    if (count < 0) {
        virt_conn_pool_release(conn);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to fetch fleet stats.");
        return NULL;
    }

    JsonArray *fleet_array = json_array_new();

    for (int i = 0; i < count; i++) {
        virDomainStatsRecordPtr rec = stats[i];
        JsonObject *vm_obj = json_object_new();

        json_object_set_string_member(vm_obj, "name", virDomainGetName(rec->dom));

        int state = VIR_DOMAIN_NOSTATE;
        int reason = 0;
        virDomainGetState(rec->dom, &state, &reason, 0);

        unsigned int vcpu = 0;
        unsigned long long cpu_time_ns = 0;
        unsigned long long mem_curr = 0, mem_max = 0, mem_unused = 0, mem_usable = 0;
        unsigned long long rd_bytes = 0, wr_bytes = 0, rx_bytes = 0, tx_bytes = 0;
        int has_balloon_unused = 0;
        int has_balloon_usable = 0;

        for (int j = 0; j < rec->nparams; j++) {
            virTypedParameterPtr p = &rec->params[j];
            if      (strcmp(p->field, "vcpu.current")    == 0) vcpu       = p->value.ui;
            else if (strcmp(p->field, "cpu.time")         == 0) cpu_time_ns = p->value.ul;
            else if (strcmp(p->field, "balloon.current") == 0) mem_curr   = p->value.ul;
            else if (strcmp(p->field, "balloon.maximum") == 0) mem_max    = p->value.ul;
            else if (strcmp(p->field, "balloon.unused")  == 0) { mem_unused = p->value.ul; has_balloon_unused = 1; }
            else if (strcmp(p->field, "balloon.usable")  == 0) { mem_usable = p->value.ul; has_balloon_usable = 1; }
            else if (strstr(p->field, ".rd.bytes")) rd_bytes += p->value.ul;
            else if (strstr(p->field, ".wr.bytes")) wr_bytes += p->value.ul;
            else if (strstr(p->field, ".rx.bytes")) rx_bytes += p->value.ul;
            else if (strstr(p->field, ".tx.bytes")) tx_bytes += p->value.ul;
        }

        double mem_used_mb = has_balloon_usable
            ? (double)(mem_curr > mem_usable ? mem_curr - mem_usable : 0) / 1024.0
            : has_balloon_unused
            ? (double)(mem_curr > mem_unused ? mem_curr - mem_unused : 0) / 1024.0
            : -1.0;

        char vm_ip[64] = "N/A";
        if (state == VIR_DOMAIN_RUNNING) {
            virDomainInterfacePtr *ifaces = NULL;

            int iface_cnt = virDomainInterfaceAddresses(rec->dom, &ifaces,
                                VIR_DOMAIN_INTERFACE_ADDRESSES_SRC_LEASE, 0);

            if (iface_cnt <= 0)
                iface_cnt = virDomainInterfaceAddresses(rec->dom, &ifaces,
                                VIR_DOMAIN_INTERFACE_ADDRESSES_SRC_AGENT, 0);

            if (iface_cnt > 0 && ifaces) {

                for (int k = 0; k < iface_cnt && vm_ip[0] == 'N'; k++) {
                    if (!ifaces[k]) continue;
                    if (g_strcmp0(ifaces[k]->name, "lo") == 0) continue;
                    for (unsigned int a = 0; a < ifaces[k]->naddrs; a++) {
                        if (ifaces[k]->addrs[a].type == VIR_IP_ADDR_TYPE_IPV4) {
                            strncpy(vm_ip, ifaces[k]->addrs[a].addr, sizeof(vm_ip)-1);
                            break;
                        }
                    }
                }
                for (int k = 0; k < iface_cnt; k++)
                    if (ifaces[k]) virDomainInterfaceFree(ifaces[k]);
                free(ifaces);
            }

            if (g_strcmp0(vm_ip, "N/A") == 0) {
                char mac_lower[18] = {0};

                char *xml_tmp = virDomainGetXMLDesc(rec->dom, 0);
                if (xml_tmp) {
                    char raw_mac[18] = {0};
                    char *mp = strstr(xml_tmp, "<mac address='");
                    if (mp) sscanf(mp, "<mac address='%17[^']'", raw_mac);

                    for (int i = 0; raw_mac[i]; i++)
                        mac_lower[i] = (char)tolower((unsigned char)raw_mac[i]);
                    free(xml_tmp);
                }
                if (mac_lower[0]) {

                    FILE *arp = popen("arp -n 2>/dev/null", "r");
                    if (arp) {
                        char line[256];
                        while (fgets(line, sizeof(line), arp)) {
                            char ip_buf[64]={0}, hw[8]={0}, mac_buf[18]={0};
                            if (sscanf(line, "%63s %7s %17s", ip_buf, hw, mac_buf) == 3) {

                                char mac_cmp[18]={0};
                                for (int i=0; mac_buf[i]; i++)
                                    mac_cmp[i]=(char)tolower((unsigned char)mac_buf[i]);
                                if (g_strcmp0(mac_cmp, mac_lower) == 0) {
                                    strncpy(vm_ip, ip_buf, sizeof(vm_ip)-1);
                                    break;
                                }
                            }
                        }
                        pclose(arp);
                    }
                }
            }
        }
        json_object_set_string_member(vm_obj, "ip", vm_ip);

        char mac_addr[32]    = "N/A";
        char net_source[128] = "N/A";
        char net_model[32]   = "N/A";
        char disk_path[256]  = "N/A";
        char disk_size[32]   = "N/A";
        char disk_bus[16]    = "N/A";
        char cdrom_path[256] = "(empty)";
        char vnc_port[16]    = "N/A";
        char vm_uuid[64]     = "N/A";

        char uuid_buf[VIR_UUID_STRING_BUFLEN];
        if (virDomainGetUUIDString(rec->dom, uuid_buf) == 0)
            strncpy(vm_uuid, uuid_buf, sizeof(vm_uuid)-1);

        char *xml = virDomainGetXMLDesc(rec->dom, 0);
        if (xml) {

            char *mac_ptr = strstr(xml, "<mac address='");
            if (mac_ptr) sscanf(mac_ptr, "<mac address='%17[^']'", mac_addr);

            char *iface_ptr = strstr(xml, "<interface type='");
            if (iface_ptr) {
                char iface_type[32] = {0};
                sscanf(iface_ptr, "<interface type='%31[^']'", iface_type);
                char *source_ptr = strstr(iface_ptr, "<source ");
                if (source_ptr) {
                    char src_attr[32]={0}, src_val[64]={0};
                    if (sscanf(source_ptr, "<source %31[^=]='%63[^']'", src_attr, src_val)==2)
                        snprintf(net_source, sizeof(net_source), "%s (%s)", src_val, iface_type);
                    else
                        strncpy(net_source, iface_type, sizeof(net_source)-1);
                }

                char *model_ptr = strstr(iface_ptr, "<model type='");
                if (model_ptr) sscanf(model_ptr, "<model type='%31[^']'", net_model);
            }

            char *disk_ptr = strstr(xml, "<disk type=");
            while (disk_ptr) {

                char *dev_ptr = strstr(disk_ptr, "device='");
                char dev_type[16] = {0};
                if (dev_ptr) sscanf(dev_ptr, "device='%15[^']'", dev_type);

                if (g_strcmp0(dev_type, "cdrom") == 0) {

                    char *src_ptr = strstr(disk_ptr, "<source file='");
                    if (src_ptr) sscanf(src_ptr, "<source file='%255[^']'", cdrom_path);
                    else strncpy(cdrom_path, "(empty)", sizeof(cdrom_path)-1);
                } else if (g_strcmp0(disk_path, "N/A") == 0) {

                    char *src_ptr = strstr(disk_ptr, "<source file='");
                    if (!src_ptr) src_ptr = strstr(disk_ptr, "<source dev='");
                    if (src_ptr) {
                        sscanf(src_ptr, "<source file='%255[^']'", disk_path);
                        if (g_strcmp0(disk_path, "N/A") == 0)
                            sscanf(src_ptr, "<source dev='%255[^']'", disk_path);
                    }

                    char *tgt_ptr = strstr(disk_ptr, "<target dev=");
                    if (tgt_ptr) {
                        char tgt_bus[16]={0};
                        sscanf(tgt_ptr, "<target dev='%*[^']' bus='%15[^']'", tgt_bus);
                        if (tgt_bus[0]) strncpy(disk_bus, tgt_bus, sizeof(disk_bus)-1);
                    }
                }

                char *next = strstr(disk_ptr+6, "<disk type=");
                if (!next) break;
                disk_ptr = next;
            }

            char *vnc_ptr = strstr(xml, "<graphics type='vnc'");
            if (vnc_ptr) {
                int vport = -1;
                sscanf(vnc_ptr, "<graphics type='vnc' port='%d'", &vport);
                if (vport == -1) snprintf(vnc_port, sizeof(vnc_port), "auto");
                else             snprintf(vnc_port, sizeof(vnc_port), ":%d", vport);
            }

            if (g_strcmp0(disk_path, "N/A") != 0) {
                off_t bytes = 0;
                struct stat st;
                if (stat(disk_path, &st) == 0) {
                    if (S_ISREG(st.st_mode)) {

                        bytes = st.st_size;
                    } else if (S_ISBLK(st.st_mode)) {

                        int fd = open(disk_path, O_RDONLY | O_NONBLOCK);
                        if (fd >= 0) {
                            bytes = lseek(fd, 0, SEEK_END);
                            if (bytes < 0) bytes = 0;
                            close(fd);
                        }
                    }
                }
                if (bytes > 0) {
                    double gb = (double)bytes / (1024.0*1024.0*1024.0);
                    if (gb >= 1.0) snprintf(disk_size, sizeof(disk_size), "%.0f GB", gb);
                    else           snprintf(disk_size, sizeof(disk_size), "%.0f MB", gb*1024.0);
                }
            }
            free(xml);
        }
        json_object_set_string_member(vm_obj, "mac",        mac_addr);
        json_object_set_string_member(vm_obj, "net_source", net_source);
        json_object_set_string_member(vm_obj, "net_model",  net_model);
        json_object_set_string_member(vm_obj, "disk_path",  disk_path);
        json_object_set_string_member(vm_obj, "disk_size",  disk_size);
        json_object_set_string_member(vm_obj, "disk_bus",   disk_bus);
        json_object_set_string_member(vm_obj, "cdrom_path", cdrom_path);
        json_object_set_string_member(vm_obj, "vnc_port",   vnc_port);
        json_object_set_string_member(vm_obj, "uuid",       vm_uuid);

        const char *state_str = "UNKNOWN";

        switch (state) {
            case VIR_DOMAIN_RUNNING: state_str = "RUNNING"; break;
            case VIR_DOMAIN_PAUSED: state_str = "PAUSED"; break;
            case VIR_DOMAIN_SHUTDOWN: state_str = "SHUTDOWN"; break;
            case VIR_DOMAIN_SHUTOFF: state_str = "OFFLINE"; break;
            case VIR_DOMAIN_CRASHED: state_str = "CRASHED"; break;
        }
        json_object_set_string_member(vm_obj, "state", state_str);

        json_object_set_int_member(vm_obj, "vcpu", vcpu);
        json_object_set_int_member(vm_obj, "cpu_time_ns", (gint64)cpu_time_ns);
        json_object_set_double_member(vm_obj, "mem_used_mb", mem_used_mb);
        json_object_set_double_member(vm_obj, "mem_max_mb", (double)mem_max / 1024.0);
        json_object_set_int_member(vm_obj, "disk_rd_bytes", rd_bytes);
        json_object_set_int_member(vm_obj, "disk_wr_bytes", wr_bytes);
        json_object_set_int_member(vm_obj, "net_rx_bytes", rx_bytes);
        json_object_set_int_member(vm_obj, "net_tx_bytes", tx_bytes);

        int autostart = 0;
        virDomainGetAutostart(rec->dom, &autostart);
        int persistent = virDomainIsPersistent(rec->dom);
        json_object_set_boolean_member(vm_obj, "autostart",  autostart ? TRUE : FALSE);
        json_object_set_boolean_member(vm_obj, "persistent", persistent > 0 ? TRUE : FALSE);

        json_array_add_object_element(fleet_array, vm_obj);
    }

    virDomainStatsRecordListFree(stats);

    JsonObject *host_obj = json_object_new();

    int cpu_cores = sysconf(_SC_NPROCESSORS_ONLN);
    json_object_set_int_member(host_obj, "cpus", cpu_cores);
    char cpu_model[128] = "Unknown Architecture";
    FILE *f_cpu = fopen("/proc/cpuinfo", "r");
    if (f_cpu) {
        char line[256];
        while (fgets(line, sizeof(line), f_cpu)) {
            if (strncmp(line, "model name", 10) == 0) {
                char *colon = strchr(line, ':');
                if (colon) { strncpy(cpu_model, colon + 2, sizeof(cpu_model)-1); cpu_model[strcspn(cpu_model, "\n")] = 0; break; }
            }
        }
        fclose(f_cpu);
    }
    json_object_set_string_member(host_obj, "cpu_model", cpu_model);

    unsigned long long u=0, n=0, s=0, i=0, io=0, irq=0, soft=0, steal=0;
    JsonArray *cores_array = json_array_new();
    FILE *f_stat = fopen("/proc/stat", "r");
    if (f_stat) {
        char line[256];
        while (fgets(line, sizeof(line), f_stat)) {
            if (strncmp(line, "cpu", 3) == 0) {
                if (line[3] == ' ') {

                    sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &u, &n, &s, &i, &io, &irq, &soft, &steal);
                    json_object_set_int_member(host_obj, "cpu_total_ticks", u+n+s+i+io+irq+soft+steal);
                    json_object_set_int_member(host_obj, "cpu_idle_ticks", i+io);

                    json_object_set_int_member(host_obj, "cpu_user_ticks",    u);
                    json_object_set_int_member(host_obj, "cpu_nice_ticks",    n);
                    json_object_set_int_member(host_obj, "cpu_system_ticks",  s);
                    json_object_set_int_member(host_obj, "cpu_iowait_ticks",  io);
                    json_object_set_int_member(host_obj, "cpu_irq_ticks",     irq);
                    json_object_set_int_member(host_obj, "cpu_softirq_ticks", soft);
                    json_object_set_int_member(host_obj, "cpu_steal_ticks",   steal);
                } else if (line[3] >= '0' && line[3] <= '9') {

                    int core_id; sscanf(line, "cpu%d %llu %llu %llu %llu %llu %llu %llu %llu", &core_id, &u, &n, &s, &i, &io, &irq, &soft, &steal);
                    JsonObject *core_obj = json_object_new();
                    json_object_set_int_member(core_obj, "id", core_id);
                    json_object_set_int_member(core_obj, "total", u+n+s+i+io+irq+soft+steal);
                    json_object_set_int_member(core_obj, "idle", i+io);
                    json_object_set_int_member(core_obj, "user", u);
                    json_object_set_int_member(core_obj, "system", s);
                    json_object_set_int_member(core_obj, "iowait", io);
                    json_object_set_int_member(core_obj, "steal", steal);
                    json_array_add_object_element(cores_array, core_obj);
                }
            }
        }
        fclose(f_stat);
    }
    json_object_set_array_member(host_obj, "cores", cores_array);

    FILE *f_up = fopen("/proc/uptime", "r");
    if (f_up) {
        double up_sec = 0;
        if (fscanf(f_up, "%lf", &up_sec) == 1)
            json_object_set_double_member(host_obj, "uptime_secs", up_sec);
        fclose(f_up);
    }

    FILE *f_la = fopen("/proc/loadavg", "r");
    if (f_la) {
        double l1 = 0, l5 = 0, l15 = 0;
        if (fscanf(f_la, "%lf %lf %lf", &l1, &l5, &l15) == 3) {
            json_object_set_double_member(host_obj, "load_1", l1);
            json_object_set_double_member(host_obj, "load_5", l5);
            json_object_set_double_member(host_obj, "load_15", l15);
        }
        fclose(f_la);
    }

    {
        int milli = 0;
        FILE *f_temp = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
        if (f_temp) { if (fscanf(f_temp, "%d", &milli) != 1) milli = 0; fclose(f_temp); }
        if (milli <= 0) {

            GDir *hdir = g_dir_open("/sys/class/hwmon", 0, NULL);
            if (hdir) {
                const gchar *ent;
                while ((ent = g_dir_read_name(hdir)) != NULL) {
                    gchar *npath = g_strdup_printf("/sys/class/hwmon/%s/name", ent);
                    gchar *chip = NULL; gsize len = 0;
                    if (g_file_get_contents(npath, &chip, &len, NULL)) {
                        g_strstrip(chip);
                        if (g_strcmp0(chip, "k10temp") == 0 || g_strcmp0(chip, "coretemp") == 0) {
                            gchar *tpath = g_strdup_printf("/sys/class/hwmon/%s/temp1_input", ent);
                            FILE *tf = fopen(tpath, "r");
                            if (tf) { if (fscanf(tf, "%d", &milli) != 1) milli = 0; fclose(tf); }
                            g_free(tpath);
                            g_free(chip); g_free(npath);
                            break;
                        }
                        g_free(chip);
                    }
                    g_free(npath);
                }
                g_dir_close(hdir);
            }
        }
        if (milli > 0)
            json_object_set_double_member(host_obj, "cpu_temp_c", milli / 1000.0);
    }

    unsigned long long mem_tot_kb = 0, mem_free_kb = 0, mem_avail_kb = 0;
    unsigned long long buffers_kb = 0, cached_kb = 0;
    unsigned long long swap_tot_kb = 0, swap_free_kb = 0;
    unsigned long long slab_kb = 0, sreclaimable_kb = 0;
    FILE *f_mem = fopen("/proc/meminfo", "r");
    if(f_mem) {
        char line[256];
        while(fgets(line, sizeof(line), f_mem)) {
            if(sscanf(line, "MemTotal: %llu kB", &mem_tot_kb) == 1) continue;
            if(sscanf(line, "MemFree: %llu kB", &mem_free_kb) == 1) continue;
            if(sscanf(line, "MemAvailable: %llu kB", &mem_avail_kb) == 1) continue;
            if(sscanf(line, "Buffers: %llu kB", &buffers_kb) == 1) continue;
            if(sscanf(line, "Cached: %llu kB", &cached_kb) == 1) continue;
            if(sscanf(line, "SwapTotal: %llu kB", &swap_tot_kb) == 1) continue;
            if(sscanf(line, "SwapFree: %llu kB", &swap_free_kb) == 1) continue;
            if(sscanf(line, "Slab: %llu kB", &slab_kb) == 1) continue;
            if(sscanf(line, "SReclaimable: %llu kB", &sreclaimable_kb) == 1) continue;
        }
        fclose(f_mem);
    }
    unsigned long long mem_used_kb = mem_tot_kb - mem_avail_kb;
    json_object_set_double_member(host_obj, "mem_total_gb", (double)mem_tot_kb / 1048576.0);
    json_object_set_double_member(host_obj, "mem_used_gb", (double)mem_used_kb / 1048576.0);
    json_object_set_double_member(host_obj, "mem_avail_gb", (double)mem_avail_kb / 1048576.0);
    json_object_set_double_member(host_obj, "mem_free_gb", (double)mem_free_kb / 1048576.0);
    json_object_set_double_member(host_obj, "mem_percent", mem_tot_kb > 0 ? ((double)mem_used_kb / mem_tot_kb) * 100.0 : 0.0);
    json_object_set_double_member(host_obj, "mem_buffers_mb", (double)buffers_kb / 1024.0);
    json_object_set_double_member(host_obj, "mem_cached_mb", (double)cached_kb / 1024.0);
    json_object_set_double_member(host_obj, "swap_total_gb", (double)swap_tot_kb / 1048576.0);
    json_object_set_double_member(host_obj, "swap_used_gb", (double)(swap_tot_kb - swap_free_kb) / 1048576.0);

    json_object_set_double_member(host_obj, "mem_slab_mb", (double)slab_kb / 1024.0);
    json_object_set_double_member(host_obj, "mem_sreclaimable_mb", (double)sreclaimable_kb / 1024.0);

    struct statvfs vfs;
    if (statvfs("/", &vfs) == 0) {
        unsigned long long total_disk = (unsigned long long)vfs.f_blocks * vfs.f_frsize;
        unsigned long long free_disk = (unsigned long long)vfs.f_bfree * vfs.f_frsize;
        unsigned long long used_disk = total_disk - free_disk;
        json_object_set_double_member(host_obj, "disk_total_gb", (double)total_disk / 1073741824.0);
        json_object_set_double_member(host_obj, "disk_used_gb", (double)used_disk / 1073741824.0);
        json_object_set_double_member(host_obj, "disk_percent", total_disk > 0 ? ((double)used_disk / total_disk) * 100.0 : 0.0);
    }

    FILE *f_net = fopen("/proc/net/dev", "r");
    unsigned long long host_rx = 0, host_tx = 0;
    char host_iface[32] = "N/A";
    if (f_net) {
        char line[256];
        if (skip_proc_lines(f_net, line, sizeof(line), 2)) {
            while (fgets(line, sizeof(line), f_net)) {
                char iface[32]; unsigned long long rx, tx;
                if (sscanf(line, " %31[^:]: %llu %*u %*u %*u %*u %*u %*u %*u %llu", iface, &rx, &tx) == 3) {
                    if (strncmp(iface, "lo", 2) != 0) {
                        strncpy(host_iface, iface, sizeof(host_iface)-1);
                        host_rx = rx; host_tx = tx; break;
                    }
                }
            }
        }
        fclose(f_net);
    }
    json_object_set_string_member(host_obj, "net_iface", host_iface);
    json_object_set_int_member(host_obj, "net_rx_bytes", host_rx);
    json_object_set_int_member(host_obj, "net_tx_bytes", host_tx);

    {
        FILE *f_nd = fopen("/proc/net/dev", "r");
        unsigned long long rx_errs=0, tx_errs=0, rx_drop=0, tx_drop=0;
        unsigned long long rx_pkts=0, tx_pkts=0;
        if (f_nd) {
            char ndl[512];
            if (skip_proc_lines(f_nd, ndl, sizeof(ndl), 2)) {
                while (fgets(ndl, sizeof(ndl), f_nd)) {
                    char nif[32];
                    unsigned long long rb, rp, re, rd, tb, tp, te, td;
                    if (sscanf(ndl, " %31[^:]: %llu %llu %llu %llu %*u %*u %*u %*u %llu %llu %llu %llu",
                               nif, &rb, &rp, &re, &rd, &tb, &tp, &te, &td) == 9) {
                        char *ns = nif; while (*ns == ' ') ns++;
                        if (strncmp(ns, "lo", 2) == 0) continue;
                        rx_pkts += rp; tx_pkts += tp;
                        rx_errs += re; tx_errs += te;
                        rx_drop += rd; tx_drop += td;
                    }
                }
            }
            fclose(f_nd);
        }
        json_object_set_int_member(host_obj, "net_rx_packets", rx_pkts);
        json_object_set_int_member(host_obj, "net_tx_packets", tx_pkts);
        json_object_set_int_member(host_obj, "net_rx_errs", rx_errs);
        json_object_set_int_member(host_obj, "net_tx_errs", tx_errs);
        json_object_set_int_member(host_obj, "net_rx_drop", rx_drop);
        json_object_set_int_member(host_obj, "net_tx_drop", tx_drop);
    }

    {
        FILE *f_ds = fopen("/proc/diskstats", "r");
        unsigned long long h_rd_ios=0, h_wr_ios=0, h_rd_bytes=0, h_wr_bytes=0, h_io_ticks=0;
        if (f_ds) {
            char dsl[512];
            while (fgets(dsl, sizeof(dsl), f_ds)) {
                unsigned int maj, min;
                char ddev[64];
                unsigned long long rd_io, rd_m, rd_sec, rd_t, wr_io, wr_m, wr_sec, wr_t, io_n, io_t, io_w;
                int dn = sscanf(dsl, " %u %u %63s %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                    &maj, &min, ddev, &rd_io, &rd_m, &rd_sec, &rd_t, &wr_io, &wr_m, &wr_sec, &wr_t, &io_n, &io_t, &io_w);
                if (dn < 14) continue;

                size_t dl = strlen(ddev);
                int keep = 0;
                if (strncmp(ddev,"sd",2)==0 && dl==3) keep=1;
                else if (strncmp(ddev,"vd",2)==0 && dl==3) keep=1;
                else if (strncmp(ddev,"nvme",4)==0 && !strchr(ddev,'p')) keep=1;
                if (!keep) continue;
                h_rd_ios   += rd_io;   h_wr_ios   += wr_io;
                h_rd_bytes += rd_sec*512; h_wr_bytes += wr_sec*512;
                h_io_ticks += io_t;
            }
            fclose(f_ds);
        }
        json_object_set_int_member(host_obj, "disk_rd_bytes", h_rd_bytes);
        json_object_set_int_member(host_obj, "disk_wr_bytes", h_wr_bytes);
        json_object_set_int_member(host_obj, "disk_rd_ios",   h_rd_ios);
        json_object_set_int_member(host_obj, "disk_wr_ios",   h_wr_ios);
        json_object_set_int_member(host_obj, "disk_io_ticks_ms", h_io_ticks);
    }

    virt_conn_pool_release(conn);

    JsonObject *rpc_resp = json_object_new();
    json_object_set_string_member(rpc_resp, "jsonrpc", "2.0");
    json_object_set_string_member(rpc_resp, "id", "monitor-fleet");

    JsonObject *result_obj = json_object_new();
    json_object_set_array_member(result_obj, "fleet", fleet_array);
    json_object_set_object_member(result_obj, "host", host_obj);
    json_object_set_object_member(rpc_resp, "result", result_obj);

    JsonNode *root_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(root_node, rpc_resp);

    gchar *response_str = json_to_string(root_node, FALSE);
    json_node_free(root_node);

    return response_str;
}
