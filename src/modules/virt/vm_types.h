/**
 * @file vm_types.h
 * @brief VM 생성을 위한 공유 설정 DTO (Data Transfer Object)
 *
 * ──────────────────────────────────────────────────────────────
 * [아키텍처에서의 위치]
 *   디스패처 핸들러(handler_vm_start.c 등)에서 RPC 파라미터를 파싱한 후
 *   이 구조체에 담아 vm_manager 계층으로 전달합니다.
 *
 *   요청 흐름:
 *     JSON-RPC params → handler_vm_start.c (파싱)
 *       → PureCVisorVmConfig 구조체에 저장
 *       → vm_manager.c (VM 생성 로직)
 *       → vm_config_builder.c (libvirt XML 생성)
 *
 * [vm_config_builder.h의 PureCVisorVmConfig와의 차이]
 *   - 이 파일의 PureCVisorVmConfig: 단순 데이터 컨테이너 (DTO)
 *     → 필드를 직접 읽고 쓸 수 있는 투명(transparent) 구조체
 *     → 핸들러에서 JSON 파라미터를 담는 용도
 *
 *   - vm_config_builder.h의 PureCVisorVmConfig: 불투명(opaque) 빌더 객체
 *     → pcv_vm_config_new() / pcv_vm_config_set_*() 빌더 패턴으로 조작
 *     → libvirt XML을 생성하는 용도
 *
 *   현재 두 구조체가 같은 이름(PureCVisorVmConfig)을 사용하므로
 *   동시에 include하면 타입 충돌이 발생합니다.
 *   실제로 vm_manager.c는 vm_config_builder.h만 include하고,
 *   디스패처 핸들러에서는 이 파일 또는 개별 파라미터를 사용합니다.
 *
 * [메모리 관리]
 *   - name, iso_path는 gchar* 동적 할당 문자열
 *   - 구조체 사용 후 각 문자열 필드를 g_free()로 해제해야 함
 *   - 보통 핸들러 함수 스코프 내에서 스택 할당 후 사용
 * ──────────────────────────────────────────────────────────────
 */
#pragma once

#include <glib.h>

/**
 * @brief VM 생성 시 필요한 설정 파라미터를 담는 DTO 구조체
 *
 * vm_manager.c 및 vm_config_builder.c에서 공통으로 사용한다.
 * JSON-RPC의 vm.create / vm.start 요청 파라미터와 1:1 매핑된다.
 */
typedef struct {
    gchar *name;          /**< VM 이름 — 영숫자/하이픈/언더스코어만 허용 (pcv_validate_vm_name 검증) */
    guint vcpu;           /**< 가상 CPU 개수 (1 이상, 호스트 물리 코어 수 이하 권장) */
    guint memory_mb;      /**< 메모리 크기 (MB 단위, 최소 256MB 권장, cloud-init 기본 2048) */
    guint disk_size_gb;   /**< 디스크 크기 (GB 단위, 0이면 기본 50GB, ZFS zvol로 생성) */
    gchar *iso_path;      /**< 설치 ISO 파일 경로 (nullable — NULL이면 CDROM 미연결) */
} PureCVisorVmConfig;
