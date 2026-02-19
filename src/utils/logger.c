#include "logger.h"
#include <stdio.h>
#include <time.h>

// 간단한 로그 핸들러: 시간 + 레벨 + 메시지 출력
static void _log_handler(const gchar *log_domain,
                         GLogLevelFlags log_level,
                         const gchar *message,
                         gpointer user_data) {
    time_t now;
    struct tm *tm_info;
    char time_buf[20];
    const char *level_str = "INFO";
    const char *color = "\033[0m"; // Reset

    time(&now);
    tm_info = localtime(&now);
    strftime(time_buf, 20, "%H:%M:%S", tm_info);

    if (log_level & G_LOG_LEVEL_ERROR) {
        level_str = "ERROR";
        color = "\033[1;31m"; // Red
    } else if (log_level & G_LOG_LEVEL_CRITICAL) {
        level_str = "CRIT";
        color = "\033[1;35m"; // Magenta
    } else if (log_level & G_LOG_LEVEL_WARNING) {
        level_str = "WARN";
        color = "\033[1;33m"; // Yellow
    } else if (log_level & G_LOG_LEVEL_MESSAGE) {
        level_str = "MSG";
        color = "\033[1;32m"; // Green
    } else if (log_level & G_LOG_LEVEL_INFO) {
        level_str = "INFO";
        color = "\033[1;34m"; // Blue
    } else if (log_level & G_LOG_LEVEL_DEBUG) {
        level_str = "DEBUG";
        color = "\033[1;30m"; // Gray
    }

    fprintf(stdout, "%s[%s] %s%s: %s\033[0m\n",
            color, time_buf, level_str,
            log_domain ? log_domain : "",
            message);
    fflush(stdout);
}

void purecvisor_logger_init(void) {
    // GLib의 기본 로그 핸들러를 커스텀 핸들러로 교체
    g_log_set_default_handler(_log_handler, NULL);
}