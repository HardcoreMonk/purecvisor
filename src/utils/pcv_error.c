/**
 * @file pcv_error.c
 * @brief GError 도메인 quark 정의 — PureCVisor 에러 도메인 등록
 *
 * Sprint C-1(GIO P2)에서 도입된 에러 도메인 정의 파일입니다.
 * GLib GError 시스템에서 에러 출처를 구분하기 위해 quark를 등록합니다.
 *
 * [아키텍처 위치]
 *   모든 모듈에서 GError를 생성할 때 이 quark을 사용합니다:
 *     g_set_error(error, pcv_vm_error_quark(), PCV_VM_ERROR_NOT_FOUND, "VM not found");
 *
 *   에러 처리 흐름:
 *     핸들러 → GError 생성 (이 파일의 quark 사용)
 *     → dispatcher.c → JSON-RPC 에러 응답으로 변환 (-32000 Server error)
 *     → REST 서버 → HTTP 상태 코드로 변환 (404, 500 등)
 *
 * [등록된 도메인]
 *   pcv_vm_error       : VM 관련 에러 (생성/시작/중지/삭제 실패)
 *   pcv_lxc_error      : LXC 컨테이너 관련 에러
 *   pcv_validate_error : 입력값 검증 실패 에러
 *
 * [GError quark 동작 원리]
 *   GQuark는 문자열을 정수 ID로 변환하는 GLib의 인터닝(interning) 시스템입니다.
 *   "pcv-vm-error" 문자열 → 고유 정수 ID (GQuark)
 *   정수 비교는 문자열 비교보다 빠르므로, 에러 도메인 구분이 효율적입니다.
 *
 *   G_DEFINE_QUARK(pcv-vm-error, pcv_vm_error) 매크로는
 *   pcv_vm_error_quark() 함수를 자동 생성합니다.
 *   이 함수는 "pcv-vm-error" 문자열의 GQuark 값을 반환합니다.
 *
 * [사용 패턴]
 *   // 에러 생성 (핸들러에서)
 *   g_set_error(error, pcv_vm_error_quark(), PCV_VM_ERROR_NOT_FOUND,
 *               "VM '%s' not found", name);
 *
 *   // 에러 확인 (호출자에서)
 *   if (g_error_matches(error, pcv_vm_error_quark(), PCV_VM_ERROR_NOT_FOUND)) {
 *       // 404 Not Found 응답
 *   }
 *
 * [에러 코드 enum 위치]
 *   include/purecvisor/pcv_error.h에 PCV_VM_ERROR_*, PCV_LXC_ERROR_* 등이 정의됩니다.
 *
 * [새 에러 도메인 추가 방법]
 *   1. 이 파일에 G_DEFINE_QUARK(pcv-xxx-error, pcv_xxx_error) 추가
 *   2. include/purecvisor/pcv_error.h에 GQuark pcv_xxx_error_quark(void) 선언
 *   3. include/purecvisor/pcv_error.h에 에러 코드 enum 추가
 */

#include "purecvisor/pcv_error.h"

/*
 * G_DEFINE_QUARK 매크로 확장 예시:
 *
 * G_DEFINE_QUARK(pcv-vm-error, pcv_vm_error) 는 다음 함수를 생성합니다:
 *
 *   GQuark pcv_vm_error_quark(void) {
 *       return g_quark_from_static_string("pcv-vm-error");
 *   }
 *
 * g_quark_from_static_string()은 정적 문자열의 GQuark을 반환합니다.
 * 최초 호출 시 문자열을 인터닝하고, 이후 호출에서는 캐시된 값을 반환합니다.
 */

/** VM 에러 도메인: VM 생성/시작/중지/삭제/마이그레이션 실패 */
G_DEFINE_QUARK(pcv-vm-error, pcv_vm_error)

/** LXC 에러 도메인: 컨테이너 생성/시작/중지/exec 실패 */
G_DEFINE_QUARK(pcv-lxc-error, pcv_lxc_error)

/** 검증 에러 도메인: 입력값 검증 실패 (pcv_validate.c에서 사용) */
G_DEFINE_QUARK(pcv-validate-error, pcv_validate_error)
