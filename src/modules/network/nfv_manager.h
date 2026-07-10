/**
 * @file nfv_manager.h
 * @brief 네트워크 기능 가상화 (NFV) 공개 인터페이스 -- 가상 LB + FW 정책 + 서비스 체인
 *
 * ====================================================================
 * [역할]
 *   handler_overlay.c 에서 NFV 관련 RPC를 처리할 때 호출.
 *   OVN lb-add/acl-add 기반으로 가상 네트워크 기능을 관리한다.
 *
 * [NFV 기본 개념 (주니어 참고)]
 *   NFV(Network Function Virtualization)는 전용 네트워크 하드웨어
 *   (로드밸런서, 방화벽, IDS 등)를 소프트웨어로 가상화하는 기술.
 *
 *   [로드 밸런서 (LB)]
 *     OVN 네이티브 LB를 사용. VIP(Virtual IP)로 들어오는 트래픽을
 *     여러 백엔드 서버에 분산시킨다. OVN의 conntrack 기반 구현.
 *     예: web-lb VIP 10.0.0.100:80 -> 10.0.0.10:80, 10.0.0.11:80
 *
 *   [방화벽 정책 (FW Policy)]
 *     이름 기반 ACL 정책 세트. JSON 파일로 영속화.
 *     실제 ACL 규칙 적용은 ovn_manager의 pcv_ovn_acl_add()로 별도 수행.
 *     이 모듈은 정책 "정의"의 CRUD만 담당한다.
 *
 *   [서비스 체인 (Service Chain)]
 *     패킷이 통과해야 할 VNF(Virtual Network Function)의 순서를 정의.
 *     예: FW -> IDS -> LB 순서로 패킷 처리.
 *     steps 배열에 각 단계의 VNF 설정을 JSON으로 저장.
 *
 * [Graceful Degradation]
 *   LB 기능: pcv_ovn_is_available() 확인 후 동작.
 *     OVN 미설치 시 lb_create -> GError, lb_list -> 빈 배열.
 *   FW/Chain: JSON 파일 기반이므로 OVN 없이도 CRUD 가능.
 *
 * [파일 영속화 경로]
 *   /var/run/purecvisor/nfv-policy-<name>.json  -- FW 정책
 *   /var/run/purecvisor/nfv-chain-<name>.json   -- 서비스 체인
 *   주의: /var/run/ 은 tmpfs 이므로 재부팅 시 휘발.
 *         영구 보존이 필요하면 /var/lib/ 으로 이동 검토.
 *
 * [의존 모듈]
 *   ovn_manager.h  -- OVN 가용성 확인 (pcv_ovn_is_available)
 *   pcv_spawn.h    -- ovn-nbctl 명령 실행
 * ====================================================================
 */
#ifndef PURECVISOR_NFV_MANAGER_H
#define PURECVISOR_NFV_MANAGER_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* ---- Lifecycle ---- */
void pcv_nfv_init(void);      /* NFV 매니저 초기화. 데몬 시작 시 호출. */
void pcv_nfv_shutdown(void);  /* NFV 매니저 종료. 현재 정리할 상태 없음. */

/* ---- LB (Load Balancer) -- OVN 네이티브 LB 래퍼 ---- */

/**
 * pcv_nfv_lb_create -- OVN 로드 밸런서 생성
 * @name: LB 이름 (예: "web-lb")
 * @vip: Virtual IP 주소 (예: "10.0.0.100")
 * @port: 서비스 포트 (예: 80)
 * @backends: 백엔드 목록 (예: "10.0.0.10:80,10.0.0.11:80")
 */
gboolean    pcv_nfv_lb_create(const gchar *name, const gchar *vip, gint port,
                               const gchar *backends, GError **error);

/** pcv_nfv_lb_delete -- OVN LB 삭제 (멱등) */
gboolean    pcv_nfv_lb_delete(const gchar *name, GError **error);

/** pcv_nfv_lb_list -- OVN LB 목록 조회. @return [{entry: "..."}, ...] */
JsonArray  *pcv_nfv_lb_list(void);

/* ---- FW Policy (방화벽 정책 세트) -- JSON 파일 기반 메타데이터 CRUD ---- */

/** pcv_nfv_fw_policy_create -- 방화벽 정책 세트 생성 (JSON 파일) */
gboolean    pcv_nfv_fw_policy_create(const gchar *name, const gchar *sw, GError **error);

/** pcv_nfv_fw_policy_delete -- 방화벽 정책 세트 삭제 (멱등) */
gboolean    pcv_nfv_fw_policy_delete(const gchar *name, GError **error);

/** pcv_nfv_fw_policy_list -- 등록된 정책 목록 (파일 시스템 스캔) */
JsonArray  *pcv_nfv_fw_policy_list(const gchar *sw);

/* ---- Service Chain (서비스 체인) -- JSON 파일 기반 VNF 순서 정의 ---- */

/**
 * pcv_nfv_chain_create -- 서비스 체인 생성
 * @name: 체인 이름
 * @steps_json: (nullable) VNF 단계 JSON 배열 문자열 (NULL이면 빈 배열)
 */
gboolean    pcv_nfv_chain_create(const gchar *name, const gchar *steps_json, GError **error);

/** pcv_nfv_chain_delete -- 서비스 체인 삭제 (멱등) */
gboolean    pcv_nfv_chain_delete(const gchar *name, GError **error);

/** pcv_nfv_chain_list -- 등록된 서비스 체인 목록 (파일 시스템 스캔) */
JsonArray  *pcv_nfv_chain_list(void);

G_END_DECLS
#endif
