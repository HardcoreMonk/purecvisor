/**
 * @file pcv_error.h
 * @brief PureCVisor 전용 GError 도메인 및 에러 코드 정의
 *
 * ====================================================================
 *  파일 역할
 * ====================================================================
 *  GLib의 GError 메커니즘을 활용한 모듈별 전용 에러 도메인을 정의한다.
 *  기존 G_IO_ERROR(GLib 범용 도메인)를 사용하면 서로 다른 모듈에서
 *  동일 에러 코드가 다른 의미를 갖는 충돌이 발생할 수 있으므로,
 *  PCV_VM_ERROR, PCV_LXC_ERROR, PCV_VALIDATE_ERROR 3개 전용 도메인으로
 *  분리하여 에러 출처를 명확히 구분한다.
 *
 * ====================================================================
 *  아키텍처 위치
 * ====================================================================
 *  공개 헤더(include/purecvisor/)에 속하며, 데몬 내부 모듈과
 *  테스트 코드 양쪽에서 포함한다. 핸들러, 코어 모듈, 드라이버 등
 *  GError를 반환하는 모든 함수에서 이 헤더의 도메인/코드를 사용한다.
 *
 * ====================================================================
 *  주요 흐름 (에러 전파 패턴)
 * ====================================================================
 *  1. 하위 모듈에서 g_set_error()로 에러 설정:
 *       g_set_error(error, PCV_VM_ERROR, PCV_VM_ERR_NOT_FOUND,
 *                   "VM '%s' not found", name);
 *
 *  2. 호출자가 g_error_matches()로 에러 종류 판별:
 *       if (g_error_matches(err, PCV_LXC_ERROR, PCV_LXC_ERR_CMD_FAILED))
 *
 *  3. 디스패처가 에러를 JSON-RPC 에러 응답으로 변환하여 클라이언트에 전송.
 *
 * ====================================================================
 *  정의된 도메인 (3개)
 * ====================================================================
 *  - PCV_VM_ERROR      : VM 관리 관련 (NOT_FOUND, BUSY, LIBVIRT_FAILED 등)
 *  - PCV_LXC_ERROR     : LXC 컨테이너 관련 (NOT_RUNNING, CMD_FAILED 등)
 *  - PCV_VALIDATE_ERROR: 입력값 검증 관련 (NAME, PATH, RANGE 등)
 *
 * ====================================================================
 *  주의사항
 * ====================================================================
 *  - GError 사용 후 반드시 g_error_free(error)로 해제할 것.
 *  - 새 에러 도메인 추가 시: GQuark 함수 선언 + enum 정의 + .c에서 quark 등록.
 *  - G_BEGIN_DECLS / G_END_DECLS 매크로로 C++ 호환성 보장.
 */

#ifndef PURECVISOR_ERROR_H
#define PURECVISOR_ERROR_H

#include <glib.h>

G_BEGIN_DECLS

/* ══════════════════════════════════════════════════════
 * PCV_VM_ERROR — VM 관리 관련 에러
 * ══════════════════════════════════════════════════════*/

#define PCV_VM_ERROR   (pcv_vm_error_quark())
GQuark pcv_vm_error_quark(void);

typedef enum {
    PCV_VM_ERR_NOT_FOUND       = 1,  /**< 지정한 VM이 존재하지 않음              */
    PCV_VM_ERR_ALREADY_EXISTS  = 2,  /**< 동일 이름의 VM이 이미 존재             */
    PCV_VM_ERR_BUSY            = 3,  /**< VM이 다른 작업 중 (lock 점유)           */
    PCV_VM_ERR_INVALID_STATE   = 4,  /**< 현재 상태에서 해당 작업 불가            */
    PCV_VM_ERR_LIBVIRT_FAILED  = 5,  /**< libvirt API 호출 실패                  */
    PCV_VM_ERR_XML_FAILED      = 6,  /**< VM XML 구성 또는 파싱 실패             */
    PCV_VM_ERR_INTERNAL        = 7,  /**< 내부 오류 (상세 사유 메시지 참조)       */
} PcvVmErrorCode;

/* ══════════════════════════════════════════════════════
 * PCV_LXC_ERROR — LXC 컨테이너 관련 에러
 * ══════════════════════════════════════════════════════*/

#define PCV_LXC_ERROR  (pcv_lxc_error_quark())
GQuark pcv_lxc_error_quark(void);

typedef enum {
    PCV_LXC_ERR_NOT_FOUND      = 1,  /**< 컨테이너가 존재하지 않음              */
    PCV_LXC_ERR_ALREADY_EXISTS = 2,  /**< 동일 이름의 컨테이너 이미 존재         */
    PCV_LXC_ERR_NOT_RUNNING    = 3,  /**< 작업에 실행 중인 컨테이너 필요         */
    PCV_LXC_ERR_CMD_FAILED     = 4,  /**< 외부 명령(zfs/lxc-*) 실행 실패        */
    PCV_LXC_ERR_CONFIG_FAILED  = 5,  /**< liblxc config 적용 실패               */
    PCV_LXC_ERR_INTERNAL       = 6,  /**< 내부 오류                              */
} PcvLxcErrorCode;

/* ══════════════════════════════════════════════════════
 * PCV_VALIDATE_ERROR — 입력값 검증 관련 에러
 * ══════════════════════════════════════════════════════*/

#define PCV_VALIDATE_ERROR (pcv_validate_error_quark())
GQuark pcv_validate_error_quark(void);

typedef enum {
    PCV_VALIDATE_ERR_NAME      = 1,  /**< VM/컨테이너 이름 형식 오류             */
    PCV_VALIDATE_ERR_SNAP_NAME = 2,  /**< 스냅샷 이름 형식 오류                  */
    PCV_VALIDATE_ERR_PATH      = 3,  /**< 경로 형식/순회 오류                    */
    PCV_VALIDATE_ERR_BRIDGE    = 4,  /**< 브리지 이름 형식 오류                  */
    PCV_VALIDATE_ERR_RANGE     = 5,  /**< 수치 범위 초과 (memory/vcpu/disk)      */
    PCV_VALIDATE_ERR_IMAGE     = 6,  /**< 컨테이너 이미지 형식 오류              */
    PCV_VALIDATE_ERR_CMD       = 7,  /**< exec 명령어 형식 오류                  */
} PcvValidateErrorCode;

/* ── 방어적 유틸리티 매크로 ─────────────────────────────────── */

/**
 * PCV_STRDUP_PRINTF:
 * g_strdup_printf 래퍼 — OOM 시 g_warning 후 NULL 반환.
 * GLib에서 g_strdup_printf는 실질적으로 abort 하지만
 * 정적 분석 도구/코드 리뷰용 방어 가드.
 *
 * 사용법:
 *   gchar *key = PCV_STRDUP_PRINTF("%s/%s", prefix, name);
 *   if (!key) return;
 */
#define PCV_STRDUP_PRINTF(fmt, ...) \
    ({ gchar *_s = g_strdup_printf((fmt), __VA_ARGS__); \
       if (G_UNLIKELY(!_s)) \
           g_warning("g_strdup_printf returned NULL at %s:%d", __FILE__, __LINE__); \
       _s; })

G_END_DECLS

#endif /* PURECVISOR_ERROR_H */
