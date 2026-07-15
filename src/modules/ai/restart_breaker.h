/* src/modules/ai/restart_breaker.h
 *
 * AF-1 후속: self-healing VM 재시작 서킷 브레이커 (VM(uuid) 단위)
 *
 * [배경]
 *   self_healing.c 의 restart 실배선(_vm_restart_worker)은 virDomainCreate() 를
 *   워커 스레드에서 비동기로 수행한다. 따라서 create 실패는 트리거 시점의
 *   동기 경로(정책 쿨다운/정책-레벨 브레이커)로 되먹임되지 않고, 정책 쿨다운
 *   (기본 300s)만 지나면 같은 고장 VM 을 무한 재시도한다 (감사 로드맵
 *   "반복 재시작 실패 시 계층4 미개방").
 *
 * [이 모듈의 책임]
 *   VM(uuid) 별로 재시작 실패를 카운트하여, 연속 실패가 임계값에 도달하면
 *   해당 VM 의 재시작 액션을 cooldown 동안 차단한다(OPEN). cooldown 경과 후
 *   1회 프로브(HALF_OPEN)를 허용하고, 프로브 성공 시 정상 복귀(CLOSED),
 *   실패 시 재차단(OPEN)한다. 성공/running-guard skip 은 카운터를 리셋한다.
 *
 * [왜 circuit_breaker.c 를 재사용하지 않는가 — 설계 근거]
 *   - virt/circuit_breaker.c 의 구동 API(cb_record_failure/success/is_open)는
 *     libvirt 커넥션 풀 전용 전역 싱글톤 g_cb 하나에만 작용한다. 재시작 실패를
 *     그곳에 기록하면 libvirt 커넥션 브레이커를 오염시켜(디스크 불량은 커넥션
 *     장애가 아님) 전체 libvirt 오퍼레이션을 막을 수 있다.
 *   - cb_get_named_state() per-resource 경로는 상태 조회 전용(구동 불가)이다.
 *   - 의미론도 다르다: 여기서는 임계값 3·고정 cooldown(기본 1800s)·[ai] 설정·
 *     half-open 1회 프로브. circuit_breaker 는 임계값 5·지수 백오프(ms)·[libvirt]
 *     설정·연속 3회 성공 복귀. 재사용은 프레임워크 신설이 되므로 회피한다.
 *   상태 어휘(CbState: CLOSED/OPEN/HALF_OPEN)만 일관성을 위해 재사용한다.
 *
 * [스레드 안전]
 *   내부 GHashTable + GMutex 로 모든 접근을 보호한다. rb_allow()는 정책 평가
 *   경로(self_healing G.mu 보유 중)에서, rb_record()는 워커 스레드(G.mu 미보유)
 *   에서 호출된다. 두 경로 모두 rb 전용 뮤텍스만 잡으므로 G.mu → rb-mu 단방향
 *   중첩만 발생한다(역순 없음 → 데드락 불가).
 */
#ifndef PURECVISOR_RESTART_BREAKER_H
#define PURECVISOR_RESTART_BREAKER_H

#include <glib.h>
#include "modules/virt/circuit_breaker.h"  /* CbState(CLOSED/OPEN/HALF_OPEN) 재사용 */

G_BEGIN_DECLS

/* ── 설정 기본값 ─────────────────────────────────────── */
#define RESTART_BREAKER_THRESHOLD_DEFAULT     3     /**< CLOSED→OPEN 연속 실패 임계값 */
#define RESTART_BREAKER_COOLDOWN_SEC_DEFAULT  1800  /**< OPEN 차단 시간(초) = HALF_OPEN 프로브 대기 */

/* ── 생명주기 ────────────────────────────────────────── */

/**
 * rb_init:
 * 브레이커 상태 테이블/뮤텍스를 초기화하고 임계값·cooldown 을 기본값으로 설정.
 * pcv_healing_init() 에서 1회 호출. rb_shutdown() 과 짝을 이뤄야 한다.
 * (설정 로드는 호출자가 rb_configure() 로 수행 — 이 모듈은 pcv_config 비의존.)
 */
void rb_init(void);

/**
 * rb_shutdown:
 * 상태 테이블/뮤텍스 해제. pcv_healing_shutdown() 에서 호출.
 */
void rb_shutdown(void);

/**
 * rb_configure:
 * 임계값(연속 실패)과 cooldown(초)을 설정한다.
 * threshold 는 1~50 으로 클램핑, cooldown_sec 는 0 이상으로 클램핑
 * (0 이면 OPEN 직후 즉시 HALF_OPEN 프로브 허용 — 테스트/공격적 복구용).
 */
void rb_configure(gint threshold, gint cooldown_sec);

gint rb_get_threshold(void);
gint rb_get_cooldown_sec(void);

/* ── 게이트/피드백 ───────────────────────────────────── */

/**
 * rb_allow:
 * 재시작 dispatch 직전 호출. 해당 uuid 브레이커가 재시작을 허용하는지 반환.
 *   CLOSED    → TRUE (통과)
 *   OPEN      → cooldown 미경과: FALSE(차단) / 경과: HALF_OPEN 전이 후 TRUE(프로브 1회)
 *   HALF_OPEN → 프로브 진행 중이면 FALSE(중복 프로브 차단)
 * NULL/빈 uuid 는 TRUE 를 반환한다(브레이커 미적용 — 기존 동작 보존).
 * 부수효과: OPEN→HALF_OPEN 전이 시 probe_in_flight 를 설정한다.
 */
[[nodiscard]] gboolean rb_allow(const gchar *uuid);

/**
 * rb_record:
 * 재시작 워커 결과를 브레이커에 되먹인다.
 *   success=TRUE  → 실패 카운터 리셋. HALF_OPEN 이면 CLOSED 복귀.
 *   success=FALSE → CLOSED: 카운터++ (임계값 도달 시 OPEN, cooldown 재무장).
 *                   HALF_OPEN: 프로브 실패 → 즉시 재-OPEN (cooldown 재무장).
 * NULL/빈 uuid 는 무시한다.
 */
void rb_record(const gchar *uuid, gboolean success);

/**
 * rb_release_probe:
 *   HALF_OPEN 프로브가 판정 없이 중단됐을 때(워커의 conn/도메인 조회 실패 =
 *   rb_feedback 0) 프로브 토큰을 실패 카운트 없이 회수한다. state 를 OPEN 으로
 *   되돌리고 cooldown 을 재무장하므로 다음 cooldown 경과 시 재프로브가 가능하다.
 *   프로브 진행 중이 아니면 no-op. 워커 스레드(G.mu 미보유)에서 호출한다.
 */
void rb_release_probe(const gchar *uuid);

/* ── 진단/테스트 조회 ────────────────────────────────── */

/** 현재 상태 조회 (미등록 uuid → CB_STATE_CLOSED). */
CbState rb_state(const gchar *uuid);

/** 현재 연속 실패 횟수 조회 (미등록 uuid → 0). */
gint rb_failure_count(const gchar *uuid);

G_END_DECLS

#endif /* PURECVISOR_RESTART_BREAKER_H */
