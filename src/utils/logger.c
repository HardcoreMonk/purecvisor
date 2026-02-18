#include "logger.h"
#include <stdio.h>

void purecvisor_logger_init(void) {
    // GLib 기본 로거 사용 (stdout/stderr)
    // 필요 시 파일 로깅 등을 여기에 추가
    g_setenv("G_MESSAGES_DEBUG", "all", TRUE); 
}