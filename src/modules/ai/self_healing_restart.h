/* src/modules/ai/self_healing_restart.h
 *
 * self-healing-restart 안전통제 결정 로직 (추출 seam)
 *
 * [배경]
 *   self_healing.c 의 _vm_restart_worker() 는 libvirt 커넥션/도메인 획득까지
 *   포함해 유닛테스트가 닿기 어렵다(libvirt 데몬·실 VM 의존). 결정 로직만
 *   ("이미 실행 중이면 재시작하지 않는다 / 아니면 기동을 시도한다") 이 파일로
 *   추출해 libvirt 비의존 소형 TU로 만들었다 — AIO-3/DISP-4 추출 패턴과 동일
 *   (효과 테스트가 빌드 표면(libvirt 헤더/링크)을 넓히지 않도록).
 *
 * [책임]
 *   running-guard: 도메인이 이미 실행 중(is_active>0)이면 재시작을 시도하지
 *   않고 "skipped" 로 반환한다. 아니면 create_fn(dom) 을 호출해 기동을 시도하고
 *   결과에 따라 "success"/"failed" 를 반환한다. 재시작 서킷 브레이커(AIO-2,
 *   restart_breaker.c) 되먹임 신호(rb_feedback)도 함께 계산한다.
 *
 * [호출자]
 *   self_healing.c 의 _vm_restart_worker() 가 virDomainIsActive()/virDomainCreate()
 *   를 정적 어댑터(_sh_domain_create)로 감싸 이 함수에 위임한다. executed 회계·
 *   감사 로그·WS 브로드캐스트·rb_record 되먹임은 호출자(self_healing.c) 책임 —
 *   이 함수는 순수 결정 로직만 담당한다.
 */
#ifndef PURECVISOR_SELF_HEALING_RESTART_H
#define PURECVISOR_SELF_HEALING_RESTART_H

#include <glib.h>

G_BEGIN_DECLS

/* PCV_SAFETY_CONTROL: self-healing-restart — 워커 스레드에서 실제 virDomainCreate로
 * VM 재시작 실배선 (AF-1, 결정 로직 추출 seam) */
/**
 * pcv_healing_restart_decide:
 * @is_active:   virDomainIsActive() 결과. >0 이면 도메인이 이미 실행 중.
 * @create_fn:   도메인 기동 시도 콜백. 0 반환 시 성공. running-guard 가 걸리면
 *               호출되지 않는다.
 * @dom:         create_fn 에 그대로 전달되는 불투명 포인터(실사용: virDomainPtr).
 * @rb_feedback: (out) 재시작 서킷 브레이커(restart_breaker.c) 되먹임 신호.
 *               +1 = 성공/skip, -1 = create 실패.
 *
 * Returns: "skipped" (is_active>0, create_fn 미호출) /
 *          "success" (create_fn(dom)==0) /
 *          "failed"  (그 외).
 */
const gchar *pcv_healing_restart_decide(int is_active,
                                        int (*create_fn)(gpointer dom), gpointer dom,
                                        gint *rb_feedback);

G_END_DECLS

#endif /* PURECVISOR_SELF_HEALING_RESTART_H */
