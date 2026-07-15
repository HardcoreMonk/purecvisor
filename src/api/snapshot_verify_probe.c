/**
 * @file snapshot_verify_probe.c
 * @brief backup.snapshot.verify 존재-검증 프로브 구현
 *
 * ADR-0025(배선=완료): 기존 스텁은 zfs 명령 문자열만 조립하고 `(void)check`
 * 후 하드코딩 `exists:true` 를 반환했다("보고성공 무동작"). 이 프로브는 실제로
 *   zfs list -t snapshot -H -o name <snap>
 * 를 argv 배열(셸 미경유 — 인젝션 구조적 안전)로 실행하고, 종료코드로 존재
 * 여부를 판정한다(exit 0 == 존재; backup_scheduler.c:pcv_backup_verify 동일 관례).
 * [블로킹] pcv_spawn_sync 는 GTask 워커 스레드에서만 호출한다.
 *
 * (I-2 시정: 데몬과 test_runner 가 둘 다 링크하는 TU 로 추출 — 유닛 테스트가
 *  손복제 대신 이 실 함수를 호출한다.)
 */
#include "snapshot_verify_probe.h"
#include "../utils/pcv_spawn.h"

gboolean pcv_snapshot_verify_probe(const gchar *snap)
{
    if (!snap || !*snap)
        return FALSE;
    const gchar *argv[] = {
        "zfs", "list", "-t", "snapshot", "-H", "-o", "name", snap, NULL
    };
    /* exit 0 == 스냅샷 존재. stdout/stderr 불필요(→ /dev/null). */
    return pcv_spawn_sync(argv, NULL, NULL, NULL);
}
