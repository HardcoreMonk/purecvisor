/**
 * @file snapshot_verify_probe.h
 * @brief backup.snapshot.verify 존재-검증 프로브 공개 인터페이스
 *
 * 아키텍처 위치:
 *   dispatcher.c 의 _snapshot_verify_worker 가 호출하는 순수 존재-판정 프로브.
 *   원래 dispatcher.c 인라인 정의였으나, 데몬(dispatcher.c)과 test_runner 가
 *   **둘 다** 링크해 유닛 테스트가 실 프로덕션 함수를 호출할 수 있도록 작은
 *   링크 가능한 TU 로 추출했다(I-2 시정, ADR-0025 반사실 확보).
 *
 * 의존: <glib.h> + src/utils/pcv_spawn.h (pcv_spawn_sync) 뿐.
 */
#ifndef PURECVISOR_SNAPSHOT_VERIFY_PROBE_H
#define PURECVISOR_SNAPSHOT_VERIFY_PROBE_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * pcv_snapshot_verify_probe:
 * @snap: ZFS 스냅샷 이름(예: "pool/vm@snap"). NULL/빈 문자열이면 FALSE.
 *
 * `zfs list -t snapshot -H -o name <snap>` 를 argv 배열(셸 미경유)로 실행하고
 * 종료코드로 존재 여부를 판정한다(exit 0 == 존재).
 * [블로킹] pcv_spawn_sync 는 GTask 워커 스레드에서만 호출한다.
 *
 * Returns: 스냅샷이 존재하면 TRUE, 아니면 FALSE.
 */
gboolean pcv_snapshot_verify_probe(const gchar *snap);

G_END_DECLS

#endif /* PURECVISOR_SNAPSHOT_VERIFY_PROBE_H */
