/**
 * @file ovs_overlay.h
 * @brief OVS VXLAN 오버레이 공개 인터페이스
 *
 * ====================================================================
 * [역할]
 *   handler_overlay.c 에서 overlay.* RPC 6개 처리 시 호출.
 *   main.c 에서 부팅 시 init/create 호출.
 *
 * [오버레이 네트워크 기본 개념 (주니어 참고)]
 *   오버레이(Overlay) 네트워크는 기존 물리 네트워크(Underlay) 위에
 *   가상의 L2 네트워크를 구성하는 기술이다.
 *
 *   [왜 필요한가?]
 *     3대의 물리 서버(Node1/2/3)에 분산된 VM들이 마치
 *     같은 스위치에 연결된 것처럼 통신해야 한다.
 *     라이브 마이그레이션 후에도 VM의 IP가 바뀌지 않아야 한다.
 *
 *   [VXLAN 터널 동작 원리]
 *     1. VM이 패킷을 보내면 OVS 브릿지가 수신
 *     2. OVS가 원본 L2 프레임을 UDP(포트 4789) 헤더로 감싸서 전송
 *     3. 상대 노드의 OVS가 UDP 헤더를 벗기고 원본 프레임을 전달
 *     이 과정이 투명하게 이루어져 VM은 터널의 존재를 모른다.
 *
 *   [VNI (VXLAN Network Identifier)]
 *     24비트 값으로 최대 약 1600만 개의 가상 네트워크를 식별.
 *     PureCVisor 기본값: VNI=100 (pcvoverlay0).
 *
 *   [에디션 경계]
 *     Single Edge: overlay 생성/삭제/조회 + 수동 peer add/remove
 *     Cluster    : 위 공용 코어 + auto_mesh 자동화
 *
 * [함수 분류]
 *   Lifecycle : init(로컬 터널 IP 설정), shutdown(인메모리 정리)
 *   CRUD      : create/delete/list/info (오버레이 네트워크 관리)
 *   Peer      : add_peer/remove_peer (수동 피어 관리)
 *   Cluster Auto: auto_mesh (클러스터 빌드 전용)
 *
 * [사용 순서]
 *   공용 코어: pcv_overlay_init() -> pcv_overlay_create() -> pcv_overlay_add_peer()
 *   클러스터 자동화: pcv_overlay_auto_mesh()는 클러스터 빌드에서만 사용.
 *
 * [반환값 규칙]
 *   gboolean 반환 함수: 실패 시 GError 설정, 호출자가 g_error_free.
 *   JsonArray/JsonObject 반환 함수: 호출자가 소유권을 가짐 (unref 필요).
 *
 * [Single Edge 수동 분산 기능]
 *   사용자는 피어 IP를 명시적으로 넣어 VXLAN 터널을 수동 구성할 수 있다.
 *   자동 풀메시와 클러스터 기반 오케스트레이션은 클러스터 빌드 전용이다.
 * ====================================================================
 */
#ifndef PURECVISOR_OVS_OVERLAY_H
#define PURECVISOR_OVS_OVERLAY_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* ---- Lifecycle -- 데몬 시작/종료 시 호출 ---- */

/**
 * pcv_overlay_init -- 오버레이 매니저 초기화
 * @local_tunnel_ip: 로컬 터널 엔드포인트 IP (eno2 대역, 예: "192.0.2.19")
 *
 * NULL 또는 빈 문자열이면 오버레이 기능을 비활성화한다.
 * daemon.conf [overlay] local_ip 키에서 읽어 main.c에서 호출.
 */
void pcv_overlay_init(const gchar *local_tunnel_ip);

/**
 * pcv_overlay_shutdown -- 오버레이 매니저 종료
 *
 * 인메모리 상태(이름, CIDR, 피어 목록)만 해제한다.
 * OVS 브릿지 자체는 삭제하지 않는다 (재부팅 시 OVS가 자체 복원).
 */
void pcv_overlay_shutdown(void);

/**
 * pcv_overlay_restore -- 부팅 시 영속화된 오버레이 메타를 스캔·재구성 (best-effort)
 *
 * OVERLAY_META_DIR(/var/lib/purecvisor/overlay)의 overlay-*.meta 파일을 파싱하여
 * pcv_overlay_create/add_peer 로 멱등 재적용한다. 개별 파일 실패는 WARN 후 계속하며
 * 부팅을 막지 않는다. pcv_overlay_init() 이후(OVS 가용) 호출해야 한다. (AF-N3)
 */
void pcv_overlay_restore(void);

/**
 * pcv_overlay_reconcile / 타이머 -- 부팅 시 OVS 미가용이었어도 주기 reconcile 로 최종 재적용.
 *
 * pcv_overlay_restore 가 --may-exist 로 멱등이라 재실행이 안전하다. 타이머는
 * security_group resync 선례를 복제(worker offload + shutdown g_source_remove). (NET-5)
 */
void pcv_overlay_reconcile(void);
void pcv_overlay_reconcile_timer_init(void);
void pcv_overlay_reconcile_timer_shutdown(void);

/* ---- Overlay network CRUD ---- */

/**
 * pcv_overlay_create -- OVS 오버레이 브릿지 생성 (멱등)
 * @name: 오버레이 브릿지 이름 (예: "pcvoverlay0")
 * @vni: VXLAN Network Identifier (기본: 100)
 * @cidr: 게이트웨이 IP/CIDR (예: "10.100.0.1/24"), NULL이면 IP 미할당
 * @error: 에러 반환 포인터
 * @return 성공 시 TRUE (이미 존재하면 TRUE 반환 = 멱등)
 */
gboolean    pcv_overlay_create(const gchar *name, gint vni, const gchar *cidr, GError **error);

/**
 * pcv_overlay_delete -- OVS 오버레이 삭제 (멱등)
 * @name: 삭제할 오버레이 이름
 * @error: 에러 반환 포인터
 * @return 성공 시 TRUE (미존재도 TRUE)
 */
gboolean    pcv_overlay_delete(const gchar *name, GError **error);

/**
 * pcv_overlay_list -- 등록된 오버레이 네트워크 목록 조회
 * @return (transfer full): [{name, vni, cidr, peer_count, active}, ...]
 */
JsonArray  *pcv_overlay_list(void);

/**
 * pcv_overlay_info -- 단일 오버레이 상세 정보 조회
 * @name: 오버레이 이름
 * @return (transfer full): {name, vni, cidr, local_tunnel_ip, peers:[...]}
 */
JsonObject *pcv_overlay_info(const gchar *name);

/* ---- VXLAN mesh management ---- */

/**
 * pcv_overlay_add_peer -- VXLAN 터널 포트 추가 (멱등)
 * @name: 오버레이 이름
 * @peer_tunnel_ip: 피어의 터널 IP (eno2 대역, 예: "192.0.2.20")
 * @error: 에러 반환 포인터
 *
 * ovs-vsctl로 VXLAN 포트를 추가한다. 이미 등록된 피어는 건너뜀.
 * @return 성공 시 TRUE
 */
gboolean pcv_overlay_add_peer(const gchar *name, const gchar *peer_tunnel_ip, GError **error);

/**
 * pcv_overlay_remove_peer -- VXLAN 터널 포트 제거 (멱등)
 * @name: 오버레이 이름
 * @peer_tunnel_ip: 제거할 피어 IP
 * @error: 에러 반환 포인터
 * @return 성공 시 TRUE
 */
gboolean pcv_overlay_remove_peer(const gchar *name, const gchar *peer_tunnel_ip, GError **error);

/**
 * pcv_overlay_auto_mesh -- CSV 피어 목록으로 VXLAN 풀 메시 자동 구성 (클러스터 빌드 전용)
 * @name: 오버레이 이름
 * @peers_csv: 쉼표 구분 피어 IP 목록 (예: "192.0.2.20,192.0.2.21")
 * @error: 에러 반환 포인터
 *
 * daemon.conf [overlay] peers 키에서 읽어 main.c 부팅 시 호출.
 * 자기 자신의 IP(local_ip)는 자동으로 건너뛴다.
 * @return 항상 TRUE (개별 피어 실패는 비치명적)
 */
gboolean pcv_overlay_auto_mesh(const gchar *name, const gchar *peers_csv, GError **error);

G_END_DECLS

#endif /* PURECVISOR_OVS_OVERLAY_H */
