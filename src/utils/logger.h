/**
 * @file logger.h
 * @brief 레거시 로깅 어댑터 헤더 — purecvisor_logger_init() 선언
 *
 * Sprint C-1에서 pcv_log.c 도입 시 기존 호출 코드 호환성을 위해 유지합니다.
 * purecvisor_logger_init()은 pcv_log_init()의 thin wrapper입니다.
 * 신규 코드에서는 pcv_log.h를 직접 include하세요.
 *
 * [아키텍처 위치]
 *   main.c → purecvisor_logger_init() → pcv_log_init() (logger.c에서 위임)
 *
 * [로그 확인]
 *   journalctl -u purecvisorsd -f 또는 journalctl -u purecvisormd -f        (실시간)
 *   journalctl -u purecvisorsd --since "1 hour ago" | jq
 *   journalctl -u purecvisormd --since "1 hour ago" | jq  (JSON 파싱)
 *   cat /var/log/purecvisor/audit.log   (감사 로그)
 */
#ifndef PURECVISOR_LOGGER_H
#define PURECVISOR_LOGGER_H

#include <glib.h>

/**
 * purecvisor_logger_init:
 * 레거시 로깅 초기화 함수. pcv_log_init()으로 위임합니다.
 * 새 코드에서는 pcv_log_init()을 직접 호출하세요.
 */
void purecvisor_logger_init(void);

#endif // PURECVISOR_LOGGER_H
