// include/purecvisor/core.h
#ifndef PURECVISOR_CORE_H
#define PURECVISOR_CORE_H

#include <glib.h>
#include <libvirt/libvirt.h>

// [기법: Opaque Pointer] 내부 구현 은닉 
typedef struct PvContext PvContext;

// [기법: X-Macro] 로그 레벨 정의 [cite: 37]
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

// Singleton Accessor
PvContext* pv_get_instance(void);

// Lifecycle API
int pv_init(void);
void pv_run(void);
void pv_cleanup(void);

// Logging
void pv_log(int level, const char *fmt, ...);

#endif // PURECVISOR_CORE_H