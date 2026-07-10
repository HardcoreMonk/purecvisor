/**
 * @file network_firewall_host.h
 * @brief 호스트 방화벽(UFW/iptables-DROP) 자동 공존 공개 인터페이스 (VP-6)
 *
 * ====================================================================
 * 책임:
 *   purecvisor 관리형 NAT 네트워크가 UFW/iptables-DROP 정책을 쓰는 호스트에서
 *   운영자 개입 없이 동작하도록, 게스트 포워딩 + 호스트 DHCP/DNS(67/53) 경로를
 *   호스트 방화벽에 자동으로 뚫어 준다. 기존 network_firewall.c 는 자기
 *   `table inet purecvisor` nftables 규칙만 전담하고, 이 파일은 "호스트가
 *   이미 갖고 있는 방화벽"과의 공존만 담당한다 (관심사 분리).
 *
 * 직접 만지는 외부 시스템:
 *   - ufw 바이너리 (route allow / allow 룰 삽입·삭제, 영구)
 *   - iptables(-nft) FORWARD/INPUT 체인 (-I/-D, 비영구 → 재부팅 시 소멸)
 *   - /etc/ufw/ufw.conf (ENABLED 파싱, 읽기 전용)
 *   - firewall-cmd (감지만; 실개입은 비범위)
 *
 * 읽는 순서 / entry point:
 *   pcv_host_fw_detect()  → 현재 호스트 방화벽 상태 판정 (스폰/파일 접근)
 *   pcv_host_fw_plan()    → 상태별 실행 명령 문자열 목록 생성 (순수 함수)
 *   pcv_host_fw_integrate()/_remove() → detect→plan→pcv_spawn 실행
 *   배선: network_firewall.c 의 setup_nat/teardown 말미 (config 가드).
 *
 * 실패 시 사용자 영향:
 *   integrate 실패(부분/전체) 시 게스트가 DHCP 주소를 못 받거나 인터넷이
 *   불통이 될 수 있다. 단, 개별 명령 실패는 soft(경고 후 계속)이며 setup_nat
 *   본 반환값에는 영향을 주지 않는다 (방화벽 공존은 best-effort 부가 기능).
 *
 * 관련 문서 / 검증:
 *   docs/superpowers/specs/2026-07-06-vp6-host-firewall-coexist-design.md
 *   유닛 검증: make test_runner && ./test_runner -p /network/host_fw_plan_ufw_add
 * ====================================================================
 */
#ifndef PURECVISOR_NETWORK_FIREWALL_HOST_H
#define PURECVISOR_NETWORK_FIREWALL_HOST_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * PcvHostFwState:
 * 호스트에 이미 존재하는 방화벽 정책의 종류.
 *
 * @PCV_HOST_FW_OPEN:          개입 불요 (FORWARD ACCEPT, UFW 비활성 등)
 * @PCV_HOST_FW_UFW:           UFW 활성 — ufw route/allow 룰로 공존
 * @PCV_HOST_FW_IPTABLES_DROP: iptables FORWARD 기본 정책 DROP — -I 룰로 공존
 * @PCV_HOST_FW_FIREWALLD:     firewalld 활성 — 실개입 비범위(감지·경고만)
 */
typedef enum {
    PCV_HOST_FW_OPEN = 0,
    PCV_HOST_FW_UFW,
    PCV_HOST_FW_IPTABLES_DROP,
    PCV_HOST_FW_FIREWALLD,
} PcvHostFwState;

/**
 * pcv_host_fw_detect:
 * 현재 호스트 방화벽 상태를 판정한다. (파일시스템/외부 명령 접근 — 비순수)
 *
 * 판정 순서:
 *   1. UFW:           `ufw` 바이너리 존재 + /etc/ufw/ufw.conf 의 `ENABLED=yes`
 *   2. IPTABLES_DROP: (UFW 아님) `iptables -S FORWARD` 첫 줄 `-P FORWARD DROP`
 *   3. FIREWALLD:     (둘 다 아님) `firewall-cmd` 존재 + `--state` 활성
 *   4. OPEN:          그 외
 *
 * 스폰/파일 접근 실패는 crash 없이 OPEN 으로 폴백한다 (best-effort 판정).
 * [중요] 내부적으로 pcv_spawn_sync 를 쓰므로 GTask 워커 스레드에서 호출.
 */
PcvHostFwState pcv_host_fw_detect(void);

/**
 * pcv_host_fw_plan:
 * @st:     호스트 방화벽 상태 (테스트는 이 값을 직접 주입한다)
 * @bridge: 대상 브릿지 인터페이스명 (예: "pcvbr0"). NULL 이면 빈 목록.
 * @remove: TRUE 면 제거(대칭) 명령, FALSE 면 추가 명령
 *
 * 상태별로 실행할 명령을 "공백으로 조인한 문자열"의 목록으로 생성한다.
 * **순수 함수**: 파일시스템/스폰 접근 없음 — 유닛 테스트 대상.
 * iptables 의 `-C` 존재 검사는 여기 넣지 않고 executor 책임으로 둔다
 * (plan 은 -I/-D 명령만 생성).
 *
 * 반환: 요소가 gchar* 인 GPtrArray (free_func=g_free). 호출자가
 *       g_ptr_array_unref() 로 해제. OPEN/FIREWALLD 는 길이 0 배열.
 */
GPtrArray *pcv_host_fw_plan(PcvHostFwState st, const gchar *bridge, gboolean remove);

/**
 * pcv_host_fw_integrate:
 * @bridge: 대상 브릿지 인터페이스명
 * @error:  (nullable) 부분/전체 실패 시 설정. 호출자가 g_error_free.
 *
 * detect→plan→pcv_spawn 실행으로 호스트 방화벽에 공존 룰을 추가한다.
 * iptables 명령은 `-C` 가드로 멱등 삽입(이미 있으면 skip). 개별 명령 실패는
 * WARN 후 계속하되, 하나라도 실패하면 FALSE 를 반환한다(나머지는 그대로 시도).
 * FIREWALLD 감지 시 경고 + audit(result=skipped) 후 TRUE.
 * [중요] 블로킹 — GTask 워커 스레드에서만 호출.
 */
gboolean pcv_host_fw_integrate(const gchar *bridge, GError **error);

/**
 * pcv_host_fw_remove:
 * @bridge: 대상 브릿지 인터페이스명
 * @error:  (nullable) 부분/전체 실패 시 설정. 호출자가 g_error_free.
 *
 * integrate 의 대칭. UFW 는 `--force delete`, iptables 는 `-D`(-C 가드로
 * 이미 없으면 skip)로 공존 룰을 제거한다. 실패 처리 규약은 integrate 와 동일.
 * [중요] 블로킹 — GTask 워커 스레드에서만 호출.
 */
gboolean pcv_host_fw_remove(const gchar *bridge, GError **error);

G_END_DECLS

#endif /* PURECVISOR_NETWORK_FIREWALL_HOST_H */
