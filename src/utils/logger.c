/**
 * @file logger.c
 * @brief 레거시 로깅 어댑터 — purecvisor_logger_init() → pcv_log_init() 위임
 *
 * Sprint C-1에서 pcv_log.c 도입 시 기존 main.c의 purecvisor_logger_init() 호출을
 * 변경하지 않고 새 로깅 시스템으로 전환하기 위한 thin wrapper입니다.
 *
 * [아키텍처 위치]
 *   main.c → purecvisor_logger_init() → pcv_log_init()
 *
 * [존재 이유]
 *   초기 버전 main.c가 purecvisor_logger_init()를 호출하고 있었기 때문에
 *   호환성 유지 목적으로 남아 있습니다. 신규 코드에서는 pcv_log_init()을 직접 호출합니다.
 *
 *   이 패턴은 API 이름 변경 시 기존 호출자를 점진적으로 마이그레이션할 때
 *   사용하는 일반적인 "어댑터 패턴"입니다.
 *
 * [주의사항]
 *   - 이 파일은 단 1개 함수만 포함하는 호환성 래퍼입니다
 *   - 실제 로깅 로직은 pcv_log.c에 있습니다
 *   - 향후 main.c에서 pcv_log_init()을 직접 호출하도록 전환되면 삭제 가능
 */

#include "logger.h"
#include "pcv_log.h"

/**
 * purecvisor_logger_init - 레거시 로깅 초기화 함수
 *
 * 내부적으로 pcv_log_init()을 호출합니다.
 * main.c 호환성 유지 목적의 thin wrapper입니다.
 * 새 코드에서는 pcv_log_init()을 직접 사용하세요.
 */
void purecvisor_logger_init(void) {
    pcv_log_init();
}
