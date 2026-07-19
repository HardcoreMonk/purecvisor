#ifndef PURECVISOR_UPDATE_CHECK_H
#define PURECVISOR_UPDATE_CHECK_H

#include <glib.h>

typedef struct {
    gboolean enabled;
    char     current[32];
    char     latest[32];
    char     url[256];
    gboolean update_available;
    gint64   checked_at;
    char     state[16];
} PcvUpdateStatus;

void pcv_update_check_init(void);

PcvUpdateStatus pcv_update_check_get(void);

gboolean pcv_update_check_compare(const char *current, const char *latest,
                                  gboolean *update_available);

gboolean pcv_update_check_parse_release(const char *json, gssize len,
                                        char **tag_out, char **url_out);

#endif
