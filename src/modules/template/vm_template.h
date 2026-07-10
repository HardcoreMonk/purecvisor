/**
 * @file vm_template.h
 * @brief VM 템플릿 관리 모듈 공개 헤더 — JSON 파일 기반 CRUD
 *
 * [파일 역할]
 *   VM 생성 시 사전 정의된 리소스 프로필(vCPU, 메모리, 디스크, OS, cloud-init)을
 *   템플릿으로 저장/조회/삭제하는 CRUD 인터페이스를 선언합니다.
 *   1 템플릿 = 1 JSON 파일 (/etc/purecvisor/templates/<name>.json)
 *
 * [아키텍처 위치]
 *   main.c -> pcv_vm_template_init() / shutdown()
 *   handler_template.c -> pcv_vm_template_create/delete/get/list()
 *   vm.create 핸들러 -> pcv_vm_template_get() -> 반환값으로 VM 생성 파라미터 구성
 *
 * [주요 자료구조]
 *   PcvVmTemplate — VM 리소스 프로필 구조체
 *     name, vcpu, memory_mb, disk_gb (필수)
 *     os_variant, iso_path, network_bridge, cloud_init_user_data, description (선택)
 *
 * [내장 프리셋 (init 시 파일 미존재 시만 자동 생성)]
 *   ubuntu-small   — 2 vCPU,  2 GB,  20 GB (개발/테스트)
 *   ubuntu-medium  — 4 vCPU,  4 GB,  40 GB (일반 서비스)
 *   ubuntu-large   — 8 vCPU,  8 GB,  80 GB (고성능)
 *
 * [메모리 관리]
 *   - PcvVmTemplate의 모든 gchar* 멤버는 동적 할당
 *   - pcv_vm_template_free()로 구조체 + 멤버 일괄 해제
 *   - pcv_vm_template_list() 반환 GPtrArray: g_ptr_array_unref()로 해제
 */

#ifndef PCV_VM_TEMPLATE_H
#define PCV_VM_TEMPLATE_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * PcvVmTemplate:
 * VM 생성 시 사용하는 리소스 프로필 구조체.
 * 모든 gchar* 멤버는 동적 할당이며 pcv_vm_template_free()로 해제합니다.
 */
typedef struct {
    gchar  *name;                   /**< 템플릿 이름 (예: "ubuntu-small"), PK 역할 */
    gint    vcpu;                   /**< vCPU 수 (예: 2, 4, 8)                    */
    gint    memory_mb;              /**< 메모리 MB (예: 2048, 4096)               */
    gint    disk_gb;                /**< 디스크 크기 GB — ZFS zvol 크기            */
    gchar  *os_variant;             /**< OS 종류 ("ubuntu24.04", "debian12" 등)   */
    gchar  *iso_path;               /**< cloud image 경로 (nullable)              */
    gchar  *network_bridge;         /**< 기본 브릿지 (nullable, 기본: pcvbr0)     */
    gchar  *cloud_init_user_data;   /**< cloud-init user-data YAML (nullable)     */
    gchar  *description;            /**< 사람이 읽을 수 있는 설명 (nullable)       */
} PcvVmTemplate;

/**
 * 모듈 초기화 — 디렉터리 생성 + 내장 프리셋 자동 생성
 * main.c 또는 데몬 시작 시 호출.
 */
void pcv_vm_template_init(void);

/**
 * 모듈 종료 — 현재 no-op (향후 캐시 해제 등)
 */
void pcv_vm_template_shutdown(void);

/* ── CRUD ───────────────────────────────────────────────────────────── */

/**
 * 템플릿 생성(저장). 동명 파일이 있으면 에러 반환.
 * @return TRUE on success
 */
gboolean pcv_vm_template_create(PcvVmTemplate *tmpl, GError **error);

/**
 * 템플릿 삭제.
 * 멱등성: 파일이 없어도 TRUE 반환.
 */
gboolean pcv_vm_template_delete(const gchar *name, GError **error);

/**
 * 이름으로 단일 템플릿 조회.
 * @return 새로 할당된 PcvVmTemplate* (caller가 pcv_vm_template_free 필요), 없으면 NULL
 */
PcvVmTemplate *pcv_vm_template_get(const gchar *name);

/**
 * 전체 템플릿 목록 조회.
 * @return GPtrArray of PcvVmTemplate* (pcv_vm_template_free로 해제)
 */
GPtrArray *pcv_vm_template_list(void);

/**
 * PcvVmTemplate 해제.
 */
void pcv_vm_template_free(PcvVmTemplate *t);

G_END_DECLS

#endif /* PCV_VM_TEMPLATE_H */
