#include "modules/virt/vm_config_history_norm.h"

#include <json-glib/json-glib.h>
#include <libxml/parser.h>
#include <string.h>

#include "modules/virt/vm_owner.h"

static const gchar *const k_pcv_ns[] = {
    PCV_VM_OWNER_NS_URI, "urn:purecvisor:overlay:1", "urn:purecvisor:dpdk:1", "urn:purecvisor:qos:1",
    "urn:purecvisor:vpc", NULL};

static const gchar *const k_known_devices[] = {"disk", "interface", "hostdev", "graphics", "video", "channel",
                                                "rng", "tpm", "watchdog", NULL};

typedef struct {
    GTree        *map;
    const guint8 *salt;
    gsize         salt_len;
    gboolean      live;
} NormCtx;

static gboolean
_is_el(const xmlNode *n, const gchar *name)
{
    return n && n->type == XML_ELEMENT_NODE && !n->ns && n->name && xmlStrEqual(n->name, BAD_CAST name);
}

static xmlNode *
_child(const xmlNode *n, const gchar *name)
{
    for (xmlNode *c = n ? n->children : NULL; c; c = c->next) {
        if (_is_el(c, name))
            return c;
    }
    return NULL;
}

static gchar *
_attr(const xmlNode *n, const gchar *name)
{
    if (!n)
        return NULL;
    xmlChar *v = xmlGetNoNsProp((xmlNode *)n, BAD_CAST name);
    gchar *out = (v && *v) ? g_strdup((const gchar *)v) : NULL;
    xmlFree(v);
    return out;
}

static gchar *
_text(const xmlNode *n)
{
    if (!n)
        return NULL;
    xmlChar *v = xmlNodeGetContent((xmlNode *)n);
    gchar *out = v ? g_strstrip(g_strdup((const gchar *)v)) : NULL;
    xmlFree(v);
    if (out && !*out) {
        g_free(out);
        out = NULL;
    }
    return out;
}

static void
_put_node(NormCtx *c, const gchar *path, JsonNode *node)
{
    g_tree_replace(c->map, g_strdup(path), node);
}

static void
_put_str_take(NormCtx *c, const gchar *path, gchar *v)
{
    if (!v)
        return;
    JsonNode *n = json_node_new(JSON_NODE_VALUE);
    json_node_set_string(n, v);
    _put_node(c, path, n);
    g_free(v);
}

static void
_put_int(NormCtx *c, const gchar *path, gint64 v)
{
    JsonNode *n = json_node_new(JSON_NODE_VALUE);
    json_node_set_int(n, v);
    _put_node(c, path, n);
}

static void
_put_bool(NormCtx *c, const gchar *path, gboolean v)
{
    JsonNode *n = json_node_new(JSON_NODE_VALUE);
    json_node_set_boolean(n, v);
    _put_node(c, path, n);
}

static gint
_cmp_str(gconstpointer a, gconstpointer b)
{
    return strcmp(*(const gchar *const *)a, *(const gchar *const *)b);
}

static void
_put_list_take(NormCtx *c, const gchar *path, GPtrArray *items, gboolean sort)
{
    if (items->len > 0) {
        if (sort)
            g_ptr_array_sort(items, _cmp_str);
        JsonArray *arr = json_array_new();
        for (guint i = 0; i < items->len; i++)
            json_array_add_string_element(arr, g_ptr_array_index(items, i));
        JsonNode *n = json_node_new(JSON_NODE_ARRAY);
        json_node_take_array(n, arr);
        _put_node(c, path, n);
    }
    g_ptr_array_unref(items);
}

static void
_put_attr(NormCtx *c, const xmlNode *n, const gchar *attr, const gchar *prefix, const gchar *key)
{
    gchar *v = _attr(n, attr);
    if (!v)
        return;
    gchar *path = g_strdup_printf("%s.%s", prefix, key);
    _put_str_take(c, path, v);
    g_free(path);
}

static gint64
_to_kib(const gchar *value, const gchar *unit)
{
    if (!value)
        return -1;
    gchar *end = NULL;
    guint64 v = g_ascii_strtoull(value, &end, 10);
    if (!end || end == value || *end)
        return -1;
    const gchar *u = unit ? unit : "KiB";
    gdouble f = 1.0;
    if (!g_ascii_strcasecmp(u, "b") || !g_ascii_strcasecmp(u, "bytes"))
        f = 1.0 / 1024.0;
    else if (!strcmp(u, "KB"))
        f = 1000.0 / 1024.0;
    else if (!strcmp(u, "k") || !strcmp(u, "KiB"))
        f = 1.0;
    else if (!strcmp(u, "MB"))
        f = 1e6 / 1024.0;
    else if (!strcmp(u, "M") || !strcmp(u, "MiB"))
        f = 1024.0;
    else if (!strcmp(u, "GB"))
        f = 1e9 / 1024.0;
    else if (!strcmp(u, "G") || !strcmp(u, "GiB"))
        f = 1024.0 * 1024.0;
    else if (!strcmp(u, "TB"))
        f = 1e12 / 1024.0;
    else if (!strcmp(u, "T") || !strcmp(u, "TiB"))
        f = 1024.0 * 1024.0 * 1024.0;
    return (gint64)((gdouble)v * f);
}

static void
_put_kib(NormCtx *c, const xmlNode *n, const gchar *path)
{
    gchar *v = _text(n);
    gchar *u = _attr(n, "unit");
    gint64 kib = _to_kib(v, u);
    if (kib >= 0)
        _put_int(c, path, kib);
    g_free(v);
    g_free(u);
}

static void
_canon(GString *out, const xmlNode *n, gboolean strip)
{
    if (n->type == XML_TEXT_NODE || n->type == XML_CDATA_SECTION_NODE) {
        gchar *t = g_strstrip(g_strdup(n->content ? (const gchar *)n->content : ""));
        if (*t) {
            g_string_append_c(out, '"');
            g_string_append(out, t);
            g_string_append_c(out, '"');
        }
        g_free(t);
        return;
    }
    if (n->type != XML_ELEMENT_NODE)
        return;
    if (strip && (_is_el(n, "alias") || _is_el(n, "address")))
        return;
    g_string_append_c(out, '<');
    if (n->ns && n->ns->href)
        g_string_append_printf(out, "{%s}", (const gchar *)n->ns->href);
    g_string_append(out, (const gchar *)n->name);
    GPtrArray *attrs = g_ptr_array_new_with_free_func(g_free);
    for (xmlAttr *a = n->properties; a; a = a->next) {
        xmlChar *v = xmlNodeListGetString(n->doc, a->children, 1);
        g_ptr_array_add(attrs, g_strdup_printf("%s%s%s=%s", a->ns && a->ns->href ? (const gchar *)a->ns->href : "",
                                               a->ns ? ":" : "", (const gchar *)a->name, v ? (const gchar *)v : ""));
        xmlFree(v);
    }
    g_ptr_array_sort(attrs, _cmp_str);
    for (guint i = 0; i < attrs->len; i++) {
        g_string_append_c(out, ' ');
        g_string_append(out, g_ptr_array_index(attrs, i));
    }
    g_ptr_array_unref(attrs);
    g_string_append_c(out, '>');
    for (const xmlNode *ch = n->children; ch; ch = ch->next)
        _canon(out, ch, strip);
    g_string_append(out, "</>");
}

static void
_put_fp_take(NormCtx *c, const gchar *key, GString *buf)
{
    if (buf->len > 0) {
        gchar *path = g_strdup_printf("opaque.%s", key);
        _put_str_take(c, path, pcv_vm_cfg_fingerprint(c->salt, c->salt_len, (const guint8 *)buf->str, buf->len));
        g_free(path);
    }
    g_string_free(buf, TRUE);
}

static void
_walk_disk(NormCtx *c, const xmlNode *d, guint idx)
{
    gchar *dev = _attr(_child(d, "target"), "dev");
    gchar *p = dev ? g_strdup_printf("disk[%s]", dev) : g_strdup_printf("disk[#%u]", idx);
    _put_attr(c, d, "device", p, "device");
    _put_attr(c, _child(d, "target"), "bus", p, "bus");
    const xmlNode *drv = _child(d, "driver");
    _put_attr(c, drv, "type", p, "driver.type");
    _put_attr(c, drv, "cache", p, "cache");
    _put_attr(c, drv, "io", p, "io");
    _put_attr(c, drv, "discard", p, "discard");
    gchar *k = NULL;
    if (_child(d, "readonly")) {
        k = g_strdup_printf("%s.readonly", p);
        _put_bool(c, k, TRUE);
        g_free(k);
    }
    if (_child(d, "shareable")) {
        k = g_strdup_printf("%s.shareable", p);
        _put_bool(c, k, TRUE);
        g_free(k);
    }
    const xmlNode *src = _child(d, "source");
    gchar *sv = _attr(src, "file");
    if (!sv)
        sv = _attr(src, "dev");
    if (!sv) {
        gchar *pool = _attr(src, "pool"), *vol = _attr(src, "volume");
        if (pool || vol)
            sv = g_strdup_printf("%s/%s", pool ? pool : "", vol ? vol : "");
        g_free(pool);
        g_free(vol);
    }
    if (!sv) {
        gchar *proto = _attr(src, "protocol"), *name = _attr(src, "name");
        gchar *q = name ? strchr(name, '?') : NULL;
        if (q)
            *q = '\0';
        if (proto || name)
            sv = g_strdup_printf("%s:%s", proto ? proto : "", name ? name : "");
        g_free(proto);
        g_free(name);
    }
    k = g_strdup_printf("%s.source", p);
    _put_str_take(c, k, sv);
    g_free(k);
    if (_child(src, "auth") || _child(d, "auth")) {
        k = g_strdup_printf("%s.auth", p);
        _put_bool(c, k, TRUE);
        g_free(k);
    }
    const xmlNode *enc = _child(src, "encryption");
    if (!enc)
        enc = _child(d, "encryption");
    if (enc) {
        k = g_strdup_printf("%s.encrypted", p);
        _put_bool(c, k, TRUE);
        g_free(k);
        _put_attr(c, enc, "format", p, "encryption_format");
    }
    const xmlNode *io = _child(d, "iotune");
    for (const xmlNode *t = io ? io->children : NULL; t; t = t->next) {
        if (t->type != XML_ELEMENT_NODE || t->ns)
            continue;
        gchar *v = _text(t);
        gchar *end = NULL;
        gint64 n = v ? g_ascii_strtoll(v, &end, 10) : 0;
        if (v && end && !*end) {
            k = g_strdup_printf("%s.iotune.%s", p, (const gchar *)t->name);
            _put_int(c, k, n);
            g_free(k);
        }
        g_free(v);
    }
    g_free(dev);
    g_free(p);
}

static void
_walk_nic(NormCtx *c, const xmlNode *i, guint idx)
{
    gchar *mac = _attr(_child(i, "mac"), "address");
    if (mac) {
        gchar *low = g_ascii_strdown(mac, -1);
        g_free(mac);
        mac = low;
    }
    gchar *p = mac ? g_strdup_printf("nic[%s]", mac) : g_strdup_printf("nic[#%u]", idx);
    gchar *type = _attr(i, "type");
    _put_attr(c, i, "type", p, "type");
    const xmlNode *src = _child(i, "source");
    gchar *k = NULL;
    if (type && (!strcmp(type, "bridge") || !strcmp(type, "network") || !strcmp(type, "direct"))) {
        gchar *sv = _attr(src, "bridge");
        if (!sv)
            sv = _attr(src, "network");
        if (!sv)
            sv = _attr(src, "dev");
        k = g_strdup_printf("%s.source", p);
        _put_str_take(c, k, sv);
        g_free(k);
        _put_attr(c, src, "portgroup", p, "portgroup");
    } else if (type && !strcmp(type, "vhostuser")) {
        _put_attr(c, src, "path", p, "vhostuser");
    }
    _put_attr(c, _child(i, "script"), "path", p, "script");
    _put_attr(c, _child(i, "model"), "type", p, "model");
    _put_attr(c, _child(i, "mtu"), "size", p, "mtu");
    _put_attr(c, _child(i, "filterref"), "filter", p, "filterref");
    GPtrArray *tags = g_ptr_array_new_with_free_func(g_free);
    for (const xmlNode *t = _child(i, "vlan") ? _child(i, "vlan")->children : NULL; t; t = t->next) {
        if (!_is_el(t, "tag"))
            continue;
        gchar *id = _attr(t, "id"), *nm = _attr(t, "nativeMode");
        if (id)
            g_ptr_array_add(tags, nm ? g_strdup_printf("%s:%s", id, nm) : g_strdup(id));
        g_free(id);
        g_free(nm);
    }
    k = g_strdup_printf("%s.vlan", p);
    _put_list_take(c, k, tags, TRUE);
    g_free(k);
    const xmlNode *bw = _child(i, "bandwidth");
    const gchar *dirs[] = {"inbound", "outbound"};
    for (guint d = 0; d < 2; d++) {
        const xmlNode *b = _child(bw, dirs[d]);
        if (!b)
            continue;
        gchar *a = _attr(b, "average"), *pk = _attr(b, "peak"), *bu = _attr(b, "burst");
        k = g_strdup_printf("%s.bandwidth.%s", p, dirs[d]);
        _put_str_take(c, k, g_strdup_printf("%s/%s/%s", a ? a : "", pk ? pk : "", bu ? bu : ""));
        g_free(k);
        g_free(a);
        g_free(pk);
        g_free(bu);
    }
    g_free(type);
    g_free(mac);
    g_free(p);
}

static gchar *
_hostdev_str(const xmlNode *h)
{
    gchar *type = _attr(h, "type");
    gchar *managed = _attr(h, "managed");
    const xmlNode *src = _child(h, "source");
    gchar *core = NULL;
    if (type && !strcmp(type, "pci")) {
        const xmlNode *a = _child(src, "address");
        gchar *dom = _attr(a, "domain"), *bus = _attr(a, "bus"), *slot = _attr(a, "slot"), *fn = _attr(a, "function");
        core = g_strdup_printf("pci:%s:%s:%s.%s", dom ? dom : "", bus ? bus : "", slot ? slot : "", fn ? fn : "");
        g_free(dom);
        g_free(bus);
        g_free(slot);
        g_free(fn);
    } else if (type && !strcmp(type, "usb")) {
        gchar *v = _attr(_child(src, "vendor"), "id"), *pr = _attr(_child(src, "product"), "id");
        core = g_strdup_printf("usb:%s:%s", v ? v : "", pr ? pr : "");
        g_free(v);
        g_free(pr);
    } else {
        core = g_strdup(type ? type : "unknown");
    }
    gchar *out = g_strdup_printf("%s%s", core, (managed && !strcmp(managed, "yes")) ? ",managed" : "");
    g_free(core);
    g_free(type);
    g_free(managed);
    return out;
}

static gchar *
_graphics_str(const xmlNode *g)
{
    gchar *type = _attr(g, "type"), *listen = _attr(g, "listen"), *autoport = _attr(g, "autoport");
    if (!listen)
        listen = _attr(_child(g, "listen"), "address");
    gboolean auto_on = autoport && !strcmp(autoport, "yes");
    gchar *port = auto_on ? NULL : _attr(g, "port");
    gchar *out = g_strdup_printf("%s|%s|%s|%s", type ? type : "", listen ? listen : "", auto_on ? "auto" : "fixed",
                                 port ? port : "");
    g_free(type);
    g_free(listen);
    g_free(autoport);
    g_free(port);
    return out;
}

static void
_walk_devices(NormCtx *c, const xmlNode *devs)
{
    GPtrArray *hostdevs = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *graphics = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *videos = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *channels = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *others = g_ptr_array_new_with_free_func(g_free);
    GHashTable *other_counts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    guint disk_i = 0, nic_i = 0;
    for (const xmlNode *d = devs ? devs->children : NULL; d; d = d->next) {
        if (d->type != XML_ELEMENT_NODE)
            continue;
        if (_is_el(d, "disk")) {
            _walk_disk(c, d, disk_i++);
        } else if (_is_el(d, "interface")) {
            _walk_nic(c, d, nic_i++);
        } else if (_is_el(d, "hostdev")) {
            g_ptr_array_add(hostdevs, _hostdev_str(d));
        } else if (_is_el(d, "graphics")) {
            g_ptr_array_add(graphics, _graphics_str(d));
        } else if (_is_el(d, "video")) {
            gchar *m = _attr(_child(d, "model"), "type");
            if (m)
                g_ptr_array_add(videos, m);
        } else if (_is_el(d, "channel")) {
            gchar *n = _attr(_child(d, "target"), "name");
            if (n)
                g_ptr_array_add(channels, n);
        } else if (_is_el(d, "rng") || _is_el(d, "tpm") || _is_el(d, "watchdog")) {
            gchar *m = _attr(d, "model");
            _put_str_take(c, (const gchar *)d->name, m ? m : g_strdup("present"));
        } else if (!c->live) {
            gboolean known = FALSE;
            for (guint i = 0; k_known_devices[i]; i++)
                known = known || _is_el(d, k_known_devices[i]);
            if (!known) {
                GString *one = g_string_new(NULL);
                _canon(one, d, TRUE);
                g_ptr_array_add(others, g_string_free(one, FALSE));
                const gchar *nm = (const gchar *)d->name;
                guint prev = GPOINTER_TO_UINT(g_hash_table_lookup(other_counts, nm));
                g_hash_table_replace(other_counts, g_strdup(nm), GUINT_TO_POINTER(prev + 1));
            }
        }
    }
    _put_list_take(c, "hostdev", hostdevs, TRUE);
    _put_list_take(c, "graphics", graphics, TRUE);
    _put_list_take(c, "video.model", videos, TRUE);
    _put_list_take(c, "channel", channels, TRUE);
    if (!c->live) {
        GHashTableIter it;
        gpointer key, val;
        g_hash_table_iter_init(&it, other_counts);
        while (g_hash_table_iter_next(&it, &key, &val)) {
            gchar *path = g_strdup_printf("other_devices.%s", (const gchar *)key);
            _put_int(c, path, GPOINTER_TO_UINT(val));
            g_free(path);
        }
        g_ptr_array_sort(others, _cmp_str);
        GString *all = g_string_new(NULL);
        for (guint i = 0; i < others->len; i++)
            g_string_append(all, g_ptr_array_index(others, i));
        _put_fp_take(c, "other_devices", all);
        _put_int(c, "other_devices_total", others->len);
    }
    g_ptr_array_unref(others);
    g_hash_table_unref(other_counts);
}

static gboolean
_is_pcv_ns(const xmlNode *n)
{
    if (!n->ns || !n->ns->href)
        return FALSE;
    for (guint i = 0; k_pcv_ns[i]; i++) {
        if (xmlStrEqual(n->ns->href, BAD_CAST k_pcv_ns[i]))
            return TRUE;
    }
    return FALSE;
}

static void
_walk_pcv_meta(NormCtx *c, const xmlNode *m)
{
    if (xmlStrEqual(m->ns->href, BAD_CAST PCV_VM_OWNER_NS_URI))
        return;
    gchar *base = g_strdup_printf("pcv.%s", (const gchar *)m->name);
    for (xmlAttr *a = m->properties; a; a = a->next) {
        gchar *v = _attr(m, (const gchar *)a->name);
        gchar *path = g_strdup_printf("%s.%s", base, (const gchar *)a->name);
        _put_str_take(c, path, v);
        g_free(path);
    }
    guint idx = 0;
    for (const xmlNode *ch = m->children; ch; ch = ch->next) {
        if (ch->type != XML_ELEMENT_NODE)
            continue;
        gchar *id = _attr(ch, "id");
        gchar *cb = id ? g_strdup_printf("%s.%s[%s]", base, (const gchar *)ch->name, id)
                       : g_strdup_printf("%s.%s[#%u]", base, (const gchar *)ch->name, idx);
        idx++;
        for (xmlAttr *a = ch->properties; a; a = a->next) {
            gchar *path = g_strdup_printf("%s.%s", cb, (const gchar *)a->name);
            _put_str_take(c, path, _attr(ch, (const gchar *)a->name));
            g_free(path);
        }
        g_free(id);
        g_free(cb);
    }
    g_free(base);
}

static void
_walk_os(NormCtx *c, const xmlNode *os)
{
    const xmlNode *type = _child(os, "type");
    _put_str_take(c, "os.type", _text(type));
    _put_attr(c, type, "arch", "os", "arch");
    _put_attr(c, type, "machine", "os", "machine");
    _put_attr(c, os, "firmware", "os", "firmware");
    const xmlNode *loader = _child(os, "loader");
    _put_attr(c, loader, "type", "os", "loader.type");
    _put_attr(c, loader, "secure", "os", "loader.secure");
    _put_str_take(c, "os.loader.path", _text(loader));
    _put_str_take(c, "os.nvram.path", _text(_child(os, "nvram")));
    _put_str_take(c, "os.kernel", _text(_child(os, "kernel")));
    _put_str_take(c, "os.initrd", _text(_child(os, "initrd")));
    _put_str_take(c, "os.dtb", _text(_child(os, "dtb")));
    const xmlNode *cmd = _child(os, "cmdline");
    if (cmd) {
        GString *b = g_string_new(NULL);
        _canon(b, cmd, FALSE);
        _put_fp_take(c, "cmdline", b);
    }
    GPtrArray *boot = g_ptr_array_new_with_free_func(g_free);
    for (const xmlNode *b = os->children; b; b = b->next) {
        if (_is_el(b, "boot")) {
            gchar *dev = _attr(b, "dev");
            if (dev)
                g_ptr_array_add(boot, dev);
        }
    }
    _put_list_take(c, "os.boot_order", boot, FALSE);
}

static void
_walk_cputune(NormCtx *c, const xmlNode *t)
{
    const gchar *ints[] = {"shares", "period", "quota", NULL};
    for (guint i = 0; ints[i]; i++) {
        gchar *path = g_strdup_printf("cputune.%s", ints[i]);
        _put_str_take(c, path, _text(_child(t, ints[i])));
        g_free(path);
    }
    GPtrArray *pins = g_ptr_array_new_with_free_func(g_free);
    for (const xmlNode *p = t->children; p; p = p->next) {
        if (!_is_el(p, "vcpupin"))
            continue;
        gchar *v = _attr(p, "vcpu"), *s = _attr(p, "cpuset");
        g_ptr_array_add(pins, g_strdup_printf("%s:%s", v ? v : "", s ? s : ""));
        g_free(v);
        g_free(s);
    }
    _put_list_take(c, "cputune.vcpupin", pins, TRUE);
    _put_attr(c, _child(t, "emulatorpin"), "cpuset", "cputune", "emulatorpin");
}

static void
_walk_root_child(NormCtx *c, const xmlNode *n, GString *fp_qemu, GString *fp_sysinfo, GString *fp_meta,
                 GPtrArray *meta_ns)
{
    if (n->ns && n->ns->href && xmlStrEqual(n->ns->href, BAD_CAST PCV_VMCH_QEMU_NS_URI)) {
        _canon(fp_qemu, n, FALSE);
        return;
    }
    if (_is_el(n, "name")) {
        _put_str_take(c, "name", _text(n));
    } else if (_is_el(n, "uuid")) {
        gchar *u = _text(n);
        _put_str_take(c, "uuid", u ? g_ascii_strdown(u, -1) : NULL);
        g_free(u);
    } else if (_is_el(n, "title") || _is_el(n, "description") || _is_el(n, "launchSecurity")) {
        GString *b = g_string_new(NULL);
        _canon(b, n, FALSE);
        _put_fp_take(c, _is_el(n, "launchSecurity") ? "launch_security" : (const gchar *)n->name, b);
    } else if (_is_el(n, "sysinfo")) {
        _canon(fp_sysinfo, n, FALSE);
    } else if (_is_el(n, "metadata")) {
        for (const xmlNode *m = n->children; m; m = m->next) {
            if (m->type != XML_ELEMENT_NODE)
                continue;
            if (_is_pcv_ns(m)) {
                _walk_pcv_meta(c, m);
            } else {
                _canon(fp_meta, m, FALSE);
                g_ptr_array_add(meta_ns, g_strdup(m->ns && m->ns->href ? (const gchar *)m->ns->href : ""));
            }
        }
    } else if (_is_el(n, "vcpu")) {
        _put_str_take(c, "vcpu.max", _text(n));
        _put_attr(c, n, "current", "vcpu", "current");
        _put_attr(c, n, "placement", "vcpu", "placement");
        _put_attr(c, n, "cpuset", "vcpu", "cpuset");
    } else if (_is_el(n, "cpu")) {
        _put_attr(c, n, "mode", "cpu", "mode");
        _put_str_take(c, "cpu.model", _text(_child(n, "model")));
        const xmlNode *t = _child(n, "topology");
        if (t) {
            gchar *s = _attr(t, "sockets"), *d = _attr(t, "dies"), *co = _attr(t, "cores"), *th = _attr(t, "threads");
            _put_str_take(c, "cpu.topology", g_strdup_printf("%s/%s/%s/%s", s ? s : "", d ? d : "", co ? co : "",
                                                             th ? th : ""));
            g_free(s);
            g_free(d);
            g_free(co);
            g_free(th);
        }
    } else if (_is_el(n, "cputune")) {
        _walk_cputune(c, n);
    } else if (_is_el(n, "numatune")) {
        const xmlNode *m = _child(n, "memory");
        gchar *mode = _attr(m, "mode"), *ns = _attr(m, "nodeset");
        if (mode || ns)
            _put_str_take(c, "numatune.memory", g_strdup_printf("%s:%s", mode ? mode : "", ns ? ns : ""));
        g_free(mode);
        g_free(ns);
    } else if (_is_el(n, "memory")) {
        _put_kib(c, n, "memory.max_kib");
    } else if (_is_el(n, "currentMemory")) {
        if (!c->live)
            _put_kib(c, n, "memory.current_kib");
    } else if (_is_el(n, "maxMemory")) {
        _put_attr(c, n, "slots", "memory", "max_slots");
    } else if (_is_el(n, "memoryBacking")) {
        if (_child(n, "hugepages"))
            _put_bool(c, "memory_backing.hugepages", TRUE);
        if (_child(n, "locked"))
            _put_bool(c, "memory_backing.locked", TRUE);
        _put_attr(c, _child(n, "source"), "type", "memory_backing", "source");
    } else if (_is_el(n, "memtune")) {
        const gchar *keys[] = {"hard_limit", "soft_limit", "swap_hard_limit", NULL};
        for (guint i = 0; keys[i]; i++) {
            const xmlNode *k = _child(n, keys[i]);
            if (!k)
                continue;
            gchar *path = g_strdup_printf("memtune.%s", keys[i]);
            _put_kib(c, k, path);
            g_free(path);
        }
    } else if (_is_el(n, "os")) {
        _walk_os(c, n);
    } else if (_is_el(n, "devices")) {
        _walk_devices(c, n);
    }
}

static gboolean
_emit_member(gpointer key, gpointer value, gpointer data)
{
    GString *out = data;
    if (out->len > 1)
        g_string_append_c(out, ',');
    JsonNode *k = json_node_new(JSON_NODE_VALUE);
    json_node_set_string(k, key);
    gchar *ks = json_to_string(k, FALSE);
    gchar *vs = json_to_string(value, FALSE);
    g_string_append_printf(out, "%s:%s", ks, vs);
    g_free(ks);
    g_free(vs);
    json_node_unref(k);
    return FALSE;
}

static gchar *
_tree_json(GTree *t)
{
    GString *out = g_string_new("{");
    g_tree_foreach(t, _emit_member, out);
    g_string_append_c(out, '}');
    return g_string_free(out, FALSE);
}

static gint
_tree_cmp(gconstpointer a, gconstpointer b, gpointer ud)
{
    (void)ud;
    return strcmp(a, b);
}

static GTree *
_tree_new(void)
{
    return g_tree_new_full(_tree_cmp, NULL, g_free, (GDestroyNotify)json_node_unref);
}

static GTree *
_normalize_doc(const gchar *xml, gsize len, gboolean live, const guint8 *salt, gsize salt_len, gchar **owner_out,
               const gchar **err_code)
{
    if (!xml || len == 0) {
        *err_code = "parse";
        return NULL;
    }
    if (len > PCV_VMCH_XML_LIMIT) {
        *err_code = "oversize";
        return NULL;
    }
    xmlDocPtr doc = xmlReadMemory(xml, (int)len, "pcv-vmch.xml", NULL,
                                  XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    xmlNode *root = doc ? xmlDocGetRootElement(doc) : NULL;
    if (!root || !_is_el(root, "domain") || doc->intSubset || doc->extSubset) {
        if (doc)
            xmlFreeDoc(doc);
        *err_code = "parse";
        return NULL;
    }
    NormCtx c = {.map = _tree_new(), .salt = salt, .salt_len = salt_len, .live = live};
    GString *fp_qemu = g_string_new(NULL), *fp_sys = g_string_new(NULL), *fp_meta = g_string_new(NULL);
    GPtrArray *meta_ns = g_ptr_array_new_with_free_func(g_free);
    for (const xmlNode *n = root->children; n; n = n->next) {
        if (n->type == XML_ELEMENT_NODE)
            _walk_root_child(&c, n, fp_qemu, fp_sys, fp_meta, meta_ns);
    }
    _put_fp_take(&c, "qemu_commandline", fp_qemu);
    _put_fp_take(&c, "sysinfo", fp_sys);
    _put_fp_take(&c, "metadata_foreign", fp_meta);
    _put_list_take(&c, "opaque.metadata_foreign_ns", meta_ns, TRUE);
    gchar *owner = pcv_vm_owner_from_domain_node(root);
    if (owner)
        _put_str_take(&c, "owner", g_strdup(owner));
    if (owner_out)
        *owner_out = owner;
    else
        g_free(owner);
    xmlFreeDoc(doc);
    return c.map;
}

static gboolean
_live_excluded(const gchar *path)
{
    return !strcmp(path, "memory.current_kib") || !strcmp(path, "autostart") || g_str_has_prefix(path, "other_devices") ||
           !strcmp(path, "opaque.other_devices");
}

typedef struct {
    GTree *base;
    GTree *overlay;
} OverlayCtx;

static gboolean
_overlay_add(gpointer key, gpointer value, gpointer data)
{
    OverlayCtx *o = data;
    if (_live_excluded(key))
        return FALSE;
    JsonNode *b = g_tree_lookup(o->base, key);
    if (!b || !json_node_equal(b, value))
        g_tree_replace(o->overlay, g_strdup(key), json_node_copy(value));
    return FALSE;
}

static gboolean
_overlay_removed(gpointer key, gpointer value, gpointer data)
{
    OverlayCtx *o = data;
    (void)value;
    if (_live_excluded(key))
        return FALSE;
    JsonNode *live = g_tree_lookup(o->base, key);
    if (!live)
        g_tree_replace(o->overlay, g_strdup(key), json_node_new(JSON_NODE_NULL));
    return FALSE;
}

gboolean
pcv_vm_cfg_normalize(const gchar *inactive_xml, gsize inactive_len, const gchar *live_xml, gsize live_len,
                     gboolean autostart, const guint8 *salt, gsize salt_len, PcvVmCfgSnapshot *out,
                     const gchar **err_code)
{
    const gchar *err = NULL;
    memset(out, 0, sizeof *out);
    if (err_code)
        *err_code = NULL;
    gchar *owner = NULL;
    GTree *cfg = _normalize_doc(inactive_xml, inactive_len, FALSE, salt, salt_len, &owner, &err);
    GTree *live = NULL;
    if (cfg && live_xml) {
        live = _normalize_doc(live_xml, live_len, TRUE, salt, salt_len, NULL, &err);
        if (!live) {
            g_tree_destroy(cfg);
            cfg = NULL;
        }
    }
    if (!cfg) {
        g_free(owner);
        if (err_code)
            *err_code = err ? err : "parse";
        return FALSE;
    }
    NormCtx c = {.map = cfg};
    _put_bool(&c, "autostart", autostart);
    JsonNode *n = g_tree_lookup(cfg, "name");
    out->name = g_strdup(n ? json_node_get_string(n) : "");
    n = g_tree_lookup(cfg, "uuid");
    out->uuid = g_strdup(n ? json_node_get_string(n) : "");
    out->owner = owner;
    out->config_json = _tree_json(cfg);
    out->config_digest = pcv_vm_cfg_sha256_hex(out->config_json);
    JsonNode *od = g_tree_lookup(cfg, "other_devices_total");
    out->other_device_count = od ? (gint)json_node_get_int(od) : 0;
    if (live) {
        GTree *overlay = _tree_new();
        OverlayCtx o = {.base = cfg, .overlay = overlay};
        g_tree_foreach(live, _overlay_add, &o);
        OverlayCtx r = {.base = live, .overlay = overlay};
        g_tree_foreach(cfg, _overlay_removed, &r);
        out->live_overlay_json = _tree_json(overlay);
        out->live_digest = pcv_vm_cfg_sha256_hex(out->live_overlay_json);
        g_tree_destroy(overlay);
        g_tree_destroy(live);
    }
    g_tree_destroy(cfg);
    return TRUE;
}

void
pcv_vm_cfg_snapshot_clear(PcvVmCfgSnapshot *s)
{
    if (!s)
        return;
    g_free(s->config_json);
    g_free(s->config_digest);
    g_free(s->live_overlay_json);
    g_free(s->live_digest);
    g_free(s->owner);
    g_free(s->name);
    g_free(s->uuid);
    memset(s, 0, sizeof *s);
}

gchar *
pcv_vm_cfg_fingerprint(const guint8 *salt, gsize salt_len, const guint8 *bytes, gsize len)
{
    static const guint8 empty = 0;
    gchar *full = g_compute_hmac_for_data(G_CHECKSUM_SHA256, salt ? salt : &empty, salt ? salt_len : 0,
                                          bytes ? bytes : &empty, bytes ? len : 0);
    full[32] = '\0';
    return full;
}

gchar *
pcv_vm_cfg_sha256_hex(const gchar *text)
{
    return g_compute_checksum_for_string(G_CHECKSUM_SHA256, text ? text : "", -1);
}

static GTree *
_json_to_tree(const gchar *json)
{
    GTree *t = _tree_new();
    if (!json)
        return t;
    JsonParser *p = json_parser_new();
    if (!json_parser_load_from_data(p, json, -1, NULL) || !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(p))) {
        g_object_unref(p);
        g_tree_destroy(t);
        return NULL;
    }
    JsonObject *o = json_node_get_object(json_parser_get_root(p));
    GList *members = json_object_get_members(o);
    for (GList *m = members; m; m = m->next)
        g_tree_replace(t, g_strdup(m->data), json_node_copy(json_object_get_member(o, m->data)));
    g_list_free(members);
    g_object_unref(p);
    return t;
}

typedef struct {
    GTree *before;
    GTree *after;
    GTree *paths;
} DiffCtx;

static gboolean
_collect_path(gpointer key, gpointer value, gpointer data)
{
    (void)value;
    g_tree_replace(((DiffCtx *)data)->paths, g_strdup(key), NULL);
    return FALSE;
}

gchar *
pcv_vm_cfg_diff(const gchar *before_json, const gchar *after_json, guint max_paths, guint *count_out)
{
    return pcv_vm_cfg_diff_filtered(before_json, after_json, max_paths, PCV_VMCH_DIFF_ALL, count_out);
}

typedef struct {
    DiffCtx  *d;
    GString  *out;
    guint     flags;
    guint     max;
    guint     count;
    guint     emitted;
} DiffWalk;

static gboolean
_diff_one(gpointer key, gpointer value, gpointer data)
{
    (void)value;
    DiffWalk *w = data;
    const gchar *path = key;
    JsonNode *b = g_tree_lookup(w->d->before, path);
    JsonNode *a = g_tree_lookup(w->d->after, path);
    gboolean b_null = !b || JSON_NODE_HOLDS_NULL(b), a_null = !a || JSON_NODE_HOLDS_NULL(a);
    if ((b_null && a_null) || (!b_null && !a_null && json_node_equal(b, a)))
        return FALSE;
    if ((w->flags & PCV_VMCH_DIFF_IGNORE_OPAQUE) && g_str_has_prefix(path, "opaque."))
        return FALSE;
    if ((w->flags & PCV_VMCH_DIFF_IGNORE_NEW_PATHS) && !b)
        return FALSE;
    w->count++;
    if (w->max && w->emitted >= w->max)
        return FALSE;
    JsonNode *pk = json_node_new(JSON_NODE_VALUE);
    json_node_set_string(pk, path);
    gchar *ps = json_to_string(pk, FALSE);
    gchar *bs = b_null ? g_strdup("null") : json_to_string(b, FALSE);
    gchar *as = a_null ? g_strdup("null") : json_to_string(a, FALSE);
    g_string_append_printf(w->out, "%s{\"after\":%s,\"before\":%s,\"path\":%s}", w->emitted ? "," : "", as, bs, ps);
    w->emitted++;
    g_free(ps);
    g_free(bs);
    g_free(as);
    json_node_unref(pk);
    return FALSE;
}

gchar *
pcv_vm_cfg_diff_filtered(const gchar *before_json, const gchar *after_json, guint max_paths, guint flags,
                         guint *count_out)
{
    if (count_out)
        *count_out = 0;
    GTree *b = _json_to_tree(before_json);
    GTree *a = _json_to_tree(after_json);
    if (!b || !a) {
        if (b)
            g_tree_destroy(b);
        if (a)
            g_tree_destroy(a);
        return NULL;
    }
    DiffCtx d = {.before = b, .after = a, .paths = g_tree_new_full(_tree_cmp, NULL, g_free, NULL)};
    g_tree_foreach(b, _collect_path, &d);
    g_tree_foreach(a, _collect_path, &d);
    DiffWalk w = {.d = &d, .out = g_string_new("["), .flags = flags, .max = max_paths};
    g_tree_foreach(d.paths, _diff_one, &w);
    g_string_append_c(w.out, ']');
    if (count_out)
        *count_out = w.count;
    g_tree_destroy(d.paths);
    g_tree_destroy(b);
    g_tree_destroy(a);
    return g_string_free(w.out, FALSE);
}

gboolean
pcv_vm_cfg_path_is_host_path(const gchar *path)
{
    if (!path)
        return FALSE;
    static const gchar *const exact[] = {"os.loader.path", "os.nvram.path", "os.kernel", "os.initrd", "os.dtb", NULL};
    for (guint i = 0; exact[i]; i++) {
        if (!strcmp(path, exact[i]))
            return TRUE;
    }
    return g_str_has_prefix(path, "nic[") && (g_str_has_suffix(path, "].script") || g_str_has_suffix(path, "].vhostuser"));
}
