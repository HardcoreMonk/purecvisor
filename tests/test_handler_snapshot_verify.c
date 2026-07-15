/**
 * @file test_handler_snapshot_verify.c
 * @brief backup.snapshot.verify 존재-검증 프로브 유닛 테스트 (ADR-0025)
 *
 * 대상: src/api/snapshot_verify_probe.c  pcv_snapshot_verify_probe()
 *
 * ============================================================================
 *  이 테스트가 검증하는 것 (존재-판정 계약)
 * ============================================================================
 *  기존 스텁은 zfs 명령 문자열만 조립하고 `(void)check` 후 하드코딩
 *  `exists:true` 를 반환했다("보고성공 무동작", ADR-0025 가 겨냥하는 클래스).
 *  실 프로브는
 *    zfs list -t snapshot -H -o name <snap>
 *  를 pcv_spawn_sync(argv, ...) 로 실행하고 **종료코드**로 존재 여부를
 *  판정한다(exit 0 == 존재; backup_scheduler.c:pcv_backup_verify 동일 관례).
 *
 *  프로브는 데몬(dispatcher.c)과 test_runner 가 **둘 다** 링크하는
 *  src/api/snapshot_verify_probe.c 로 추출됐다(I-2 시정). 따라서 이 테스트는
 *  손복제(byte-동일 재현) 대신 **실 프로덕션 함수** pcv_snapshot_verify_probe()
 *  를 직접 호출해 존재-판정 계약을 검증한다 — 프로덕션을 스텁으로 되돌리면
 *  이 테스트가 실제로 red 가 된다(반사실 확보).
 *  진짜 end-to-end 반사실(하드코딩 true vs 실 zfs, 데몬 RPC 왕복)은
 *  mock-zfs 통합 효과테스트(계획 Task 2)가 실증한다.
 *
 *  반사실(counterfactual): 명백히 존재하지 않는 스냅샷 → FALSE 여야 한다.
 *  하드코딩 스텁(항상 TRUE)으로 되돌리면 test_probe_nonexistent_is_false 가
 *  red 가 된다. 실 zfs 가 있는 호스트는 exit 1, 없는 호스트는 spawn 실패 —
 *  어느 경로든 프로브는 FALSE(결정적, 실 ZFS 풀 불요).
 *
 * 실행: sudo ./test_runner -p /handler_snapshot_verify
 * 외부 의존: pcv_spawn_sync (링크됨) — 실 UDS/libvirt/데몬 불필요.
 * ============================================================================
 */
#include <glib.h>
#include "../src/utils/pcv_spawn.h"
#include "../src/api/snapshot_verify_probe.h"

static void ensure_spawn(void) {
    static gboolean initialized = FALSE;
    if (!initialized) { pcv_spawn_launcher_init(); initialized = TRUE; }
}

/* 명백히 존재하지 않는 스냅샷 → FALSE. 스텁(하드코딩 true)이면 red. */
static void test_probe_nonexistent_is_false(void) {
    ensure_spawn();
    gboolean exists =
        pcv_snapshot_verify_probe("nonexistent-pcv-test-pool-XYZ/vm@snap-does-not-exist");
    g_assert_false(exists);
}

/* NULL / 빈 문자열 안전성 — 크래시 없이 FALSE. */
static void test_probe_null_safe(void) {
    ensure_spawn();
    g_assert_false(pcv_snapshot_verify_probe(NULL));
    g_assert_false(pcv_snapshot_verify_probe(""));
}

void test_handler_snapshot_verify_register(void) {
    g_test_add_func("/handler_snapshot_verify/probe_nonexistent_is_false",
                    test_probe_nonexistent_is_false);
    g_test_add_func("/handler_snapshot_verify/probe_null_safe",
                    test_probe_null_safe);
}
