











#include "modules/backup/backup_send_estimate.h"

#include <string.h>

gint64
pcv_backup_parse_send_estimate(const gchar *text)
{
    if (!text || !*text) return 0;

    gint64 estimate = 0;
    gchar **lines = g_strsplit(text, "\n", -1);
    for (guint i = 0; lines[i]; i++) {
        gchar *line = g_strstrip(lines[i]);
        if (!g_str_has_prefix(line, "size")) continue;

        const gchar *value = line + strlen("size");
        if (*value != '\t' && !g_ascii_isspace(*value)) continue;
        while (*value && g_ascii_isspace(*value)) value++;
        if (!g_ascii_isdigit(*value)) continue;

        gchar *end = NULL;
        guint64 parsed = g_ascii_strtoull(value, &end, 10);
        while (end && *end && g_ascii_isspace(*end)) end++;
        if (end && *end == '\0' && parsed > 0 && parsed <= G_MAXINT64) {
            estimate = (gint64)parsed;
            break;
        }
    }
    g_strfreev(lines);
    return estimate;
}
