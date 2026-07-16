/* ==========================================================================
 * src/modules/lxc/lxc_owner.h
 * PureCVisor — 컨테이너 operator owner-scope 소유자 저장소 (B1 IDOR 시정)
 *
 * [파일 역할]
 *   VM은 소유자(pcv:owner)를 libvirt domain XML metadata에 저장하지만, 컨테이너는
 *   libvirt domain이 없으므로 per-container 파일
 *     <container_path>/<name>/purecvisor.owner
 *   에 소유자 subject를 저장한다. 기존 purecvisor.meta(이미지 태그) 포맷을 건드리지
 *   않도록 owner는 별도 파일에 둔다.
 *
 * [왜 별도 TU인가]
 *   경로 규칙(pcv_config_get_container_path 기반)과 파일 read/write만 담는
 *   self-contained 순수 TU라 test_runner도 링크할 수 있다(vm_batch_policy.c와 동일
 *   추출 패턴). lxc_driver.c(liblxc 의존, DAEMON 전용) 링크 없이 owner 저장소의
 *   효과 테스트가 가능하다.
 *
 * [소비자]
 *   - handler_container.c : create 성공 시 pcv_lxc_stamp_owner()로 소유자 기록
 *   - dispatcher.c        : owner-scope 게이트에서 pcv_lxc_read_owner()로 소유자 조회
 * ========================================================================== */

#ifndef PURECVISOR_LXC_OWNER_H
#define PURECVISOR_LXC_OWNER_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * @brief 컨테이너 소유자 subject 기록 (create 성공 경로에서 호출)
 * @param name       컨테이너 이름
 * @param owner_sub  소유자 subject 문자열 (NULL/빈 문자열이면 무동작 후 FALSE)
 * @return TRUE 기록 성공, FALSE 입력 오류 또는 파일 쓰기 실패
 */
gboolean pcv_lxc_stamp_owner(const gchar *name, const gchar *owner_sub);

/**
 * @brief 컨테이너 소유자 subject 조회 (owner-scope 게이트에서 호출)
 * @param name 컨테이너 이름
 * @return 소유자 subject 문자열(호출자 g_free 필수), 파일 부재/빈 값이면 NULL
 */
gchar   *pcv_lxc_read_owner(const gchar *name);

G_END_DECLS

#endif /* PURECVISOR_LXC_OWNER_H */
