
#ifndef PURECVISOR_CORE_H
#define PURECVISOR_CORE_H

#include <glib.h>
#include <libvirt/libvirt.h>

typedef struct PvContext PvContext;

#define LOG_LEVELS \
    X(LOG_INFO,  "[INFO] ") \
    X(LOG_WARN,  "[WARN] ") \
    X(LOG_ERR,   "[ERR ] ") \
    X(LOG_DEBUG, "[DBG ] ")

enum LogLevel {
#define X(name, str) name,
    LOG_LEVELS
#undef X
};

PvContext* pv_get_instance(void);

int pv_init(void);

void pv_run(void);

void pv_cleanup(void);

void pv_log(int level, const char *fmt, ...);

#endif
