/* tests/test_security_group.c
 *
 * 대상 모듈: src/modules/network/security_group.c
 *
 * 이 테스트가 검증하는 것 (V5 회귀):
 *   pcv_security_group_rule_add()가 nft argv-reparse 인젝션 입력을 거부하는지.
 *   source/protocol/direction 값은 그대로 nft argv 요소가 되고 nft 가 argv 를
 *   이어붙여 재렉싱하므로, 검증 없이는 "1.2.3.4/32 accept; flush ruleset #" 같은
 *   source 가 flush ruleset 을 실행해 호스트 방화벽을 통째로 지운다.
 *   또한 그룹 이름(nft 체인 구성요소)이 pcv_validate_bridge_name 을 통과해야 함.
 *
 * 실행: sudo ./test_runner -p /security_group
 *
 * 외부 의존 (2026-07-04 재설계 이후):
 *   rule_add/create 는 nft 트랜잭션 성공 시에만 TRUE (spec §5 변이 순서).
 *   따라서 accept 경로는 root(+netns 격리, test_main.c) 전제 — 비 root 는 skip.
 *   거부 경로는 검증 단계에서 조기 반환하므로 권한 무관하게 성립한다.
 */

#include <glib.h>
#include <json-glib/json-glib.h>
#include <unistd.h>
#include <string.h>

#include "../src/utils/pcv_spawn.h"
#include "../src/modules/network/vm_vnet_cache.h"
#include <sqlite3.h>   /* [R8] FK-위반 write-fail 주입용 제2 연결 */

/* security_group.c 는 공개 헤더가 없어 dispatcher.c 처럼 extern 으로 선언한다.
 * (create 정의부의 [[nodiscard]] 는 여기서 생략 — 별도 TU) */
extern gboolean pcv_security_group_create(const gchar *name, const gchar *description);
extern gboolean pcv_security_group_rule_add(const gchar *name, JsonObject *rule);
extern gboolean pcv_security_group_delete(const gchar *name);
extern gboolean pcv_security_group_apply_to_vm(const gchar *vm, const gchar *sg_name);
extern gboolean pcv_security_group_detach_vm(const gchar *vm, const gchar *sg_name);
extern gboolean pcv_security_group_vm_is_bound(const gchar *vm);
extern gboolean pcv_security_group_rule_remove(const gchar *name, gint64 rule_id);
extern void pcv_security_group_resync_all(void);

#define SGV5_GROUP "sgv5"

/* 테스트 그룹이 존재하도록 보장한다. 최초 호출은 TRUE, 같은 러너 프로세스의
 * 이후 호출은 이미 존재하여 FALSE 를 반환한다 — 단, create 는 nft 트랜잭션
 * 성공 시에만 그룹을 남기므로(spec §5 롤백) 비 root 환경에서는 그룹이 생성
 * 되지 않을 수 있다. 거부 경로 테스트는 조기 반환이라 이와 무관하게 성립한다.
 * (-p 로 단일 케이스만 실행해도 그룹이 준비되도록 각 테스트에서 호출) */
static void _ensure_group(void) {
    gboolean created = pcv_security_group_create(SGV5_GROUP, "V5 injection regression");
    (void)created;
}

/* direction/protocol/source/port 를 갖는 규칙 JsonObject 를 만든다.
 * NULL 인자는 해당 멤버를 생략한다. */
static JsonObject *_rule(const gchar *direction, const gchar *protocol,
                         const gchar *source, gint port) {
    JsonObject *o = json_object_new();
    if (direction) json_object_set_string_member(o, "direction", direction);
    if (protocol)  json_object_set_string_member(o, "protocol", protocol);
    if (source)    json_object_set_string_member(o, "source", source);
    if (port > 0)  json_object_set_int_member(o, "port", port);
    return o;
}

/* ── 인젝션/무효 입력 거부 ─────────────────────────────────────── */

/* source 에 nft 인젝션 payload → flush ruleset 시도. 반드시 거부. */
static void test_sg_reject_source_injection(void) {
    _ensure_group();
    JsonObject *rule = _rule("ingress", "tcp",
                             "1.2.3.4/32 accept; flush ruleset #", 80);
    g_assert_false(pcv_security_group_rule_add(SGV5_GROUP, rule));
    json_object_unref(rule);
}

/* protocol 에 argv-reparse payload "tcp; drop". 반드시 거부. */
static void test_sg_reject_protocol_injection(void) {
    _ensure_group();
    JsonObject *rule = _rule("ingress", "tcp; drop", "10.0.0.0/24", 80);
    g_assert_false(pcv_security_group_rule_add(SGV5_GROUP, rule));
    json_object_unref(rule);
}

/* 화이트리스트 밖 프로토콜 "sctp". 반드시 거부. */
static void test_sg_reject_protocol_unknown(void) {
    _ensure_group();
    JsonObject *rule = _rule("ingress", "sctp", "10.0.0.0/24", 80);
    g_assert_false(pcv_security_group_rule_add(SGV5_GROUP, rule));
    json_object_unref(rule);
}

/* direction 이 ingress/egress 가 아님. 반드시 거부. */
static void test_sg_reject_direction(void) {
    _ensure_group();
    JsonObject *rule = _rule("bogus", "tcp", "10.0.0.0/24", 80);
    g_assert_false(pcv_security_group_rule_add(SGV5_GROUP, rule));
    json_object_unref(rule);
}

/* 그룹 이름이 nft 체인 구성요소로 안전하지 않음(공백/세미콜론). 생성 거부. */
static void test_sg_reject_bad_group_name(void) {
    gboolean created = pcv_security_group_create("sg; flush", "bad name");
    g_assert_false(created);
}

/* ── 정상 입력 수용 ────────────────────────────────────────────── */

/* 검증을 모두 통과하는 규칙 — nft 트랜잭션까지 성공해야 TRUE (신 계약).
 * 비 root 는 nft 가 항상 실패하므로 skip (make test 는 root+netns 로 실행됨). */
static void test_sg_accept_clean(void) {
    if (geteuid() != 0) {
        g_test_skip("root(+netns) 필요 — nft 성공이 TRUE 의 전제");
        return;
    }
    _ensure_group();
    JsonObject *rule = _rule("ingress", "tcp", "10.0.0.0/24", 80);
    g_assert_true(pcv_security_group_rule_add(SGV5_GROUP, rule));
    json_object_unref(rule);
}

/* ── 통합 (root + netns): 실제 nft 룰셋 상태 검증 ─────────────────
 * test_main.c 가 root 실행 시 전용 netns 로 격리하므로 여기서의 nft 변경은
 * 호스트에 도달할 수 없다. 격리가 없다면 test_main.c 가 이미 _exit(1) 했다. */
static void test_sg_nft_state_integration(void) {
    if (geteuid() != 0) {
        g_test_skip("root(+netns) 필요");
        return;
    }
    /* 그룹 생성 + 규칙 → bridge pcv_sg 에 스코프 체인 생성 */
    (void)pcv_security_group_create("sgint", "integration");
    JsonObject *rule = _rule("ingress", "tcp", "10.9.8.0/24", 8080);
    g_assert_true(pcv_security_group_rule_add("sgint", rule));
    json_object_unref(rule);

    gchar *out = NULL;
    const gchar *ls[] = {"nft", "list", "table", "bridge", "pcv_sg", NULL};
    g_assert_true(pcv_spawn_sync(ls, &out, NULL, NULL));
    g_assert_nonnull(strstr(out, "ingress-dispatch"));
    g_assert_nonnull(strstr(out, "egress-dispatch"));
    g_assert_nonnull(strstr(out, "baseline-in"));
    g_assert_nonnull(strstr(out, "sg-sgint-in"));
    g_assert_nonnull(strstr(out, "10.9.8.0/24"));
    /* 사고 회귀 가드 — 실제 커널에 로드된 룰셋 레벨 */
    g_assert_null(strstr(out, "hook input"));
    g_assert_null(strstr(out, "hook output"));
    /* 바인딩 없음 → 디스패치에 drop 이 없어야 한다 (비바인딩 무영향).
     * [M-4] 주의: 이 단언은 프로세스 전역 g_sg_map 에 vnet-resolvable 바인딩이
     * 없다는 전제에 의존한다. 향후 실 vnet 바인딩을 남기는 테스트가 이보다 앞서
     * 등록되면 깨질 수 있으므로, 그때는 per-test teardown(그룹 삭제) 또는
     * "sgint" 체인에 스코프한 단언으로 바꿔야 한다. */
    g_assert_null(strstr(out, "drop"));
    g_free(out);

    /* 그룹 삭제 → 체인 소멸 */
    g_assert_true(pcv_security_group_delete("sgint"));
    out = NULL;
    g_assert_true(pcv_spawn_sync(ls, &out, NULL, NULL));
    g_assert_null(strstr(out, "sg-sgint-in"));
    g_free(out);
}

/* ── I-2: _rebuild_dispatch 가 virsh 대신 캐시를 읽는지 (root+netns) ──
 * C1 fix 이후 apply_to_vm 은 바인딩 시 캐시를 evict 하므로, 캐시 읽기 증명은
 * evict 하지 않는 rebuild 경로(rule.add)로 한다: 바인딩 후 캐시를 다시 채우고,
 * rule.add 가 트리거하는 rebuild 가 그 캐시를 읽어 vnetCACHED 를 디스패치에
 * 내보내는지 확인한다. 실도메인이 없으니 virsh 로는 절대 안 나옴 → 비-tautology. */
static void test_sg_dispatch_reads_cache(void) {
    if (geteuid() != 0) { g_test_skip("root(+netns) 필요"); return; }

    (void)pcv_security_group_create("sgcache", "I-2 cache proof");
    JsonObject *r1 = _rule("ingress", "tcp", "10.7.0.0/24", 443);
    g_assert_true(pcv_security_group_rule_add("sgcache", r1));
    json_object_unref(r1);

    /* 바인딩: apply 는 evict → ghost-vm 은 실도메인이 아니라 lazy-miss 로 빈 해석 →
     * 이 시점 디스패치에 없음. (C1 fix 로 apply 가 캐시를 신뢰하지 않음을 전제) */
    g_assert_true(pcv_security_group_apply_to_vm("ghost-vm", "sgcache"));

    /* 이제 캐시에 가짜 vnet 을 넣고, evict 하지 않는 rebuild(rule.add)를 트리거한다.
     * rebuild 가 virsh 가 아니라 캐시를 읽으면 vnetCACHED 가 디스패치에 등장한다
     * (실도메인 없으니 virsh 로는 절대 안 나옴 → 비-tautology). */
    GPtrArray *fake = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(fake, g_strdup("vnetCACHED"));
    pcv_vm_vnet_cache_put("ghost-vm", fake);
    g_ptr_array_unref(fake);

    JsonObject *r2 = _rule("ingress", "udp", "10.7.0.0/24", 53);
    g_assert_true(pcv_security_group_rule_add("sgcache", r2));   /* rebuild, evict 없음 */
    json_object_unref(r2);

    gchar *out = NULL;
    const gchar *ls[] = {"nft", "list", "table", "bridge", "pcv_sg", NULL};
    g_assert_true(pcv_spawn_sync(ls, &out, NULL, NULL));
    g_assert_nonnull(strstr(out, "vnetCACHED"));   /* 캐시 경로 증명 */
    g_free(out);

    (void)pcv_security_group_detach_vm("ghost-vm", "sgcache");
    pcv_vm_vnet_cache_evict("ghost-vm");
    g_assert_true(pcv_security_group_delete("sgcache"));
}

/* ── R8: rule_add 가 DB 쓰기 실패 시 fail-closed 롤백하는지 (root+netns) ──
 * FK 위반으로 write-fail 을 결정적으로 주입한다: 그룹 생성(부모 행 INSERT) 후
 * 제2 sqlite 연결로 security_groups 부모 행을 삭제하면(인메모리 그룹은 g_sg_map 에
 * 유지), 이어지는 rule_add 의 sg_rules INSERT 가 FK 위반(SQLITE_CONSTRAINT)으로
 * 실패한다. foreign_keys=ON 은 프로덕션 연결(security_group.c _sg_db_init)에 이미
 * 설정됨. 수정 전: step 미검사 → stale/0 db_id → rule_add 가 TRUE 반환·규칙 잔존.
 * 수정 후: step-검사 → -1 → 보상 롤백 → FALSE + nft 에 그 규칙 부재. */
static void test_sg_rule_add_rolls_back_on_db_write_fail(void) {
    if (geteuid() != 0) { g_test_skip("root(+netns) 필요"); return; }

    /* SG_DB_PATH 는 security_group.c 내부 #define — 테스트는 경로를 하드코딩한다
     * (안정 경로; 드리프트 시 이 테스트가 크게 실패해 알린다). */
    const char *db_path = "/var/lib/purecvisor/security_groups.db";

    g_assert_true(pcv_security_group_create("sgr8", "R8 write-fail"));

    /* 제2 연결로 부모 행 삭제 → 인메모리 그룹은 유지, DB 부모만 사라짐 */
    sqlite3 *aux = NULL;
    g_assert_cmpint(sqlite3_open(db_path, &aux), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_exec(aux, "DELETE FROM security_groups WHERE name='sgr8';",
                                 NULL, NULL, NULL), ==, SQLITE_OK);
    sqlite3_close(aux);

    /* rule_add: nft group script 재빌드 성공 → sg_rules INSERT 가 FK 위반으로 실패 */
    JsonObject *rule = _rule("ingress", "tcp", "10.55.44.0/24", 8080);
    gboolean added = pcv_security_group_rule_add("sgr8", rule);
    json_object_unref(rule);
    g_assert_false(added);   /* 수정 전엔 TRUE → 여기서 실패(원하는 fail-before) */

    /* nft 에 그 규칙(고유 CIDR)이 없어야 한다 — 보상 revert 로 되돌려짐.
     * rule_add 가 line 697 에서 nft 를 먼저 적용하므로 bridge pcv_sg 테이블은 존재. */
    gchar *out = NULL;
    const gchar *ls[] = {"nft", "list", "table", "bridge", "pcv_sg", NULL};
    if (pcv_spawn_sync(ls, &out, NULL, NULL)) {
        g_assert_null(strstr(out ? out : "", "10.55.44.0/24"));
        g_free(out);
    }

    /* 정리: 인메모리 그룹 제거 (DB 부모 행은 이미 삭제됨 → _sg_db_delete_group no-op) */
    g_assert_true(pcv_security_group_delete("sgr8"));
}

/* ── I-2/C1 회귀: apply_to_vm 이 바인딩 시점에 stale 캐시를 evict 하는지 ──
 * 가짜 stale vnet 을 캐시에 심어두고 바인딩하면, apply 의 evict + lazy-miss 재해석
 * (ghost 는 실도메인 아님 → 빈 결과)으로 stale vnet 이 디스패치에 나오면 안 된다.
 * evict 가 없으면(회귀 시) stale vnet 이 디스패치에 남아 fail-open. */
static void test_sg_apply_evicts_stale(void) {
    if (geteuid() != 0) { g_test_skip("root(+netns) 필요"); return; }

    (void)pcv_security_group_create("sgstale", "C1 regression");
    JsonObject *rule = _rule("ingress", "tcp", "10.8.0.0/24", 22);
    g_assert_true(pcv_security_group_rule_add("sgstale", rule));
    json_object_unref(rule);

    /* 바인딩 전에 stale 캐시를 심는다 (언바인딩 상태 재시작 잔재 시뮬레이션) */
    GPtrArray *stale = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(stale, g_strdup("vnetSTALE"));
    pcv_vm_vnet_cache_put("ghost2-vm", stale);
    g_ptr_array_unref(stale);

    g_assert_true(pcv_security_group_apply_to_vm("ghost2-vm", "sgstale"));

    gchar *out = NULL;
    const gchar *ls[] = {"nft", "list", "table", "bridge", "pcv_sg", NULL};
    g_assert_true(pcv_spawn_sync(ls, &out, NULL, NULL));
    /* apply 가 evict 했으므로 stale 은 디스패치에 없어야 한다 (fail-open 방지) */
    g_assert_null(strstr(out, "vnetSTALE"));
    g_free(out);

    (void)pcv_security_group_detach_vm("ghost2-vm", "sgstale");
    pcv_vm_vnet_cache_evict("ghost2-vm");
    g_assert_true(pcv_security_group_delete("sgstale"));
}

/* ── I-2: vm_is_bound 게이트 (root 불필요 — virsh/nft 미도달) ──
 * 바인딩 여부만 판정. 인젝션 거부 테스트처럼 nft 성공과 무관하게 성립. */
static void test_sg_vm_is_bound(void) {
    (void)pcv_security_group_create("sgbind", "bind gate");
    g_assert_false(pcv_security_group_vm_is_bound("vm-unbound"));
    g_assert_false(pcv_security_group_vm_is_bound(NULL));

    /* apply_to_vm 은 nft(root) 실패 시 롤백되어 바인딩이 안 남을 수 있으므로,
     * 게이트 자체는 바인딩 유무 판정만 검증한다. 비 root 에서 apply 가 롤백되면
     * is_bound 는 FALSE 여야 하고, root 에서 성공하면 TRUE 여야 한다. */
    gboolean applied = pcv_security_group_apply_to_vm("vm-gate", "sgbind");
    g_assert_cmpint(pcv_security_group_vm_is_bound("vm-gate"), ==, applied);

    if (applied) {
        (void)pcv_security_group_detach_vm("vm-gate", "sgbind");
        g_assert_false(pcv_security_group_vm_is_bound("vm-gate"));
    }
    (void)pcv_security_group_delete("sgbind");
}

/* ── M-3: rule_remove 는 유효하지 않은 rule_id(<=0)를 거부 (계약 고정) ──
 * sqlite rowid 는 ≥1 이므로 0/음수는 영속화된 규칙일 수 없다. 0 은 in-flight
 * 규칙(db_id==0, rule_add 가 nft 반영 중)만 잘못 steal 하는 벡터라 진입부에서
 * 거부한다. (취약 상태는 root+동시성 없이 못 만들므로 이 테스트는 계약을 잠그는
 * 회귀 가드다 — strict RED 아님.) 거부는 nft 이전이라 root 불필요. */
static void test_sg_rule_remove_rejects_nonpositive_id(void) {
    g_assert_false(pcv_security_group_rule_remove("sgv5", 0));
    g_assert_false(pcv_security_group_rule_remove("sgv5", -1));
    g_assert_false(pcv_security_group_rule_remove(NULL, 5));   /* 기존 NULL 가드 대조 */
}

/* ── I2-R1: resync_all 의 keep-old-on-empty (root+netns) ──
 * ghost-rsync 는 실도메인이 아니라 virsh 가 빈 결과를 낸다. resync 가 빈 결과에
 * 캐시를 지우면(clear/evict) dispatch 에서 vnetRSYNC 가 사라지고, keep-old 면
 * 유지된다. vnetRSYNC 가 nft 에 남아있으면 keep-old-on-empty 계약이 성립한다
 * (transient/off virsh 실패로 fail-open 을 만들지 않는다는 증명). */
static void test_sg_resync_keeps_cache_on_empty(void) {
    if (geteuid() != 0) { g_test_skip("root(+netns) 필요"); return; }

    (void)pcv_security_group_create("sgrsync", "I2-R1 resync");
    JsonObject *rule = _rule("ingress", "tcp", "10.6.0.0/24", 22);
    g_assert_true(pcv_security_group_rule_add("sgrsync", rule));
    json_object_unref(rule);

    /* 바인딩(apply 는 C1 fix 로 evict → ghost virsh empty → 이 시점 dispatch 없음) */
    g_assert_true(pcv_security_group_apply_to_vm("ghost-rsync", "sgrsync"));

    /* 캐시에 가짜 vnet 주입 (라이프사이클 이벤트로 채워진 상태 시뮬레이션) */
    GPtrArray *fake = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(fake, g_strdup("vnetRSYNC"));
    pcv_vm_vnet_cache_put("ghost-rsync", fake);
    g_ptr_array_unref(fake);

    /* resync: ghost-rsync virsh empty → keep-old → 캐시 vnetRSYNC 유지 → dispatch 등장 */
    pcv_security_group_resync_all();

    gchar *out = NULL;
    const gchar *ls[] = {"nft", "list", "table", "bridge", "pcv_sg", NULL};
    g_assert_true(pcv_spawn_sync(ls, &out, NULL, NULL));
    g_assert_nonnull(strstr(out, "vnetRSYNC"));   /* keep-old-on-empty 증명 */
    g_free(out);

    (void)pcv_security_group_detach_vm("ghost-rsync", "sgrsync");
    pcv_vm_vnet_cache_evict("ghost-rsync");
    g_assert_true(pcv_security_group_delete("sgrsync"));
}

/* ── I2-R3: detach 가 fully-unbound VM 의 dormant 캐시를 회수 (root+netns) ──
 * 캐시에 가짜 vnet 을 넣고 바인딩 후 detach → 그 VM 이 어떤 그룹에도 안 묶이므로
 * 캐시 엔트리가 evict 되어야 한다. (회수 없으면 잔존 → cache_get 이 non-NULL). */
static void test_sg_detach_reaps_cache(void) {
    if (geteuid() != 0) { g_test_skip("root(+netns) 필요"); return; }

    (void)pcv_security_group_create("sgreap", "I2-R3 detach reap");
    JsonObject *rule = _rule("ingress", "tcp", "10.5.0.0/24", 22);
    g_assert_true(pcv_security_group_rule_add("sgreap", rule));
    json_object_unref(rule);

    g_assert_true(pcv_security_group_apply_to_vm("ghost-reap", "sgreap"));
    GPtrArray *fake = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(fake, g_strdup("vnetREAP"));
    pcv_vm_vnet_cache_put("ghost-reap", fake);
    g_ptr_array_unref(fake);

    g_assert_true(pcv_security_group_detach_vm("ghost-reap", "sgreap"));

    GPtrArray *after = pcv_vm_vnet_cache_get("ghost-reap");
    g_assert_null(after);   /* [R3] fully-unbound → 회수됨 */

    (void)pcv_security_group_delete("sgreap");
}

/* ── I2-R3: 다른 그룹에 남아있으면 detach 가 캐시를 유지 (root+netns) ──
 * VM 을 두 그룹에 바인딩 후 한 그룹만 detach → 여전히 다른 그룹에 묶여 있으므로
 * is_bound=TRUE → evict 안 함 → 캐시 유지. */
static void test_sg_detach_retains_multigroup(void) {
    if (geteuid() != 0) { g_test_skip("root(+netns) 필요"); return; }

    (void)pcv_security_group_create("sgmulti-a", "R3 multi a");
    (void)pcv_security_group_create("sgmulti-b", "R3 multi b");
    JsonObject *ra = _rule("ingress", "tcp", "10.5.1.0/24", 80);
    g_assert_true(pcv_security_group_rule_add("sgmulti-a", ra));
    json_object_unref(ra);
    JsonObject *rb = _rule("ingress", "tcp", "10.5.2.0/24", 443);
    g_assert_true(pcv_security_group_rule_add("sgmulti-b", rb));
    json_object_unref(rb);

    g_assert_true(pcv_security_group_apply_to_vm("ghost-multi", "sgmulti-a"));
    g_assert_true(pcv_security_group_apply_to_vm("ghost-multi", "sgmulti-b"));
    GPtrArray *fake = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(fake, g_strdup("vnetMULTI"));
    pcv_vm_vnet_cache_put("ghost-multi", fake);
    g_ptr_array_unref(fake);

    g_assert_true(pcv_security_group_detach_vm("ghost-multi", "sgmulti-a"));

    GPtrArray *after = pcv_vm_vnet_cache_get("ghost-multi");
    g_assert_nonnull(after);   /* [R3] sgmulti-b 에 여전히 바인딩 → 유지 */
    g_ptr_array_unref(after);

    (void)pcv_security_group_detach_vm("ghost-multi", "sgmulti-b");
    pcv_vm_vnet_cache_evict("ghost-multi");
    (void)pcv_security_group_delete("sgmulti-a");
    (void)pcv_security_group_delete("sgmulti-b");
}

/* ── I2-R3: delete 가 그 그룹에만 묶였던 VM 의 dormant 캐시를 회수 (root+netns) ── */
static void test_sg_delete_reaps_cache(void) {
    if (geteuid() != 0) { g_test_skip("root(+netns) 필요"); return; }

    (void)pcv_security_group_create("sgdelreap", "I2-R3 delete reap");
    JsonObject *rule = _rule("ingress", "tcp", "10.5.3.0/24", 22);
    g_assert_true(pcv_security_group_rule_add("sgdelreap", rule));
    json_object_unref(rule);

    g_assert_true(pcv_security_group_apply_to_vm("ghost-del", "sgdelreap"));
    GPtrArray *fake = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(fake, g_strdup("vnetDEL"));
    pcv_vm_vnet_cache_put("ghost-del", fake);
    g_ptr_array_unref(fake);

    g_assert_true(pcv_security_group_delete("sgdelreap"));

    GPtrArray *after = pcv_vm_vnet_cache_get("ghost-del");
    g_assert_null(after);   /* [R3] 그룹 삭제로 fully-unbound → 회수됨 */
}

/* ── I2-R3: delete 가 다른 그룹에 남아있는 VM 의 캐시는 유지 (root+netns) ──
 * VM 을 두 그룹에 바인딩 후 한 그룹만 delete → 여전히 다른 그룹에 묶여 있으므로
 * is_bound=TRUE → evict 안 함 → 캐시 유지. */
static void test_sg_delete_retains_multigroup(void) {
    if (geteuid() != 0) { g_test_skip("root(+netns) 필요"); return; }

    (void)pcv_security_group_create("sgdelm-a", "R3 del multi a");
    (void)pcv_security_group_create("sgdelm-b", "R3 del multi b");
    JsonObject *ra = _rule("ingress", "tcp", "10.5.4.0/24", 80);
    g_assert_true(pcv_security_group_rule_add("sgdelm-a", ra));
    json_object_unref(ra);
    JsonObject *rb = _rule("ingress", "tcp", "10.5.5.0/24", 443);
    g_assert_true(pcv_security_group_rule_add("sgdelm-b", rb));
    json_object_unref(rb);

    g_assert_true(pcv_security_group_apply_to_vm("ghost-delm", "sgdelm-a"));
    g_assert_true(pcv_security_group_apply_to_vm("ghost-delm", "sgdelm-b"));
    GPtrArray *fake = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(fake, g_strdup("vnetDELM"));
    pcv_vm_vnet_cache_put("ghost-delm", fake);
    g_ptr_array_unref(fake);

    g_assert_true(pcv_security_group_delete("sgdelm-a"));

    GPtrArray *after = pcv_vm_vnet_cache_get("ghost-delm");
    g_assert_nonnull(after);   /* [R3] sgdelm-b 에 여전히 바인딩 → 유지 */
    g_ptr_array_unref(after);

    (void)pcv_security_group_detach_vm("ghost-delm", "sgdelm-b");
    pcv_vm_vnet_cache_evict("ghost-delm");
    (void)pcv_security_group_delete("sgdelm-b");
}

/* ── 등록 ──────────────────────────────────────────────────────── */

void test_security_group_register(void) {
    g_test_add_func("/security_group/reject_source_injection",   test_sg_reject_source_injection);
    g_test_add_func("/security_group/reject_protocol_injection", test_sg_reject_protocol_injection);
    g_test_add_func("/security_group/reject_protocol_unknown",   test_sg_reject_protocol_unknown);
    g_test_add_func("/security_group/reject_direction",          test_sg_reject_direction);
    g_test_add_func("/security_group/reject_bad_group_name",     test_sg_reject_bad_group_name);
    g_test_add_func("/security_group/accept_clean",              test_sg_accept_clean);
    g_test_add_func("/security_group/nft_state_integration",     test_sg_nft_state_integration);
    g_test_add_func("/security_group/dispatch_reads_cache",      test_sg_dispatch_reads_cache);
    g_test_add_func("/security_group/apply_evicts_stale",        test_sg_apply_evicts_stale);
    g_test_add_func("/security_group/vm_is_bound",               test_sg_vm_is_bound);
    g_test_add_func("/security_group/rule_remove_rejects_nonpositive_id",
                    test_sg_rule_remove_rejects_nonpositive_id);
    g_test_add_func("/security_group/resync_keeps_cache_on_empty",
                    test_sg_resync_keeps_cache_on_empty);
    g_test_add_func("/security_group/detach_reaps_cache",       test_sg_detach_reaps_cache);
    g_test_add_func("/security_group/detach_retains_multigroup", test_sg_detach_retains_multigroup);
    g_test_add_func("/security_group/delete_reaps_cache",        test_sg_delete_reaps_cache);
    g_test_add_func("/security_group/delete_retains_multigroup", test_sg_delete_retains_multigroup);
    g_test_add_func("/security_group/rule_add_rolls_back_on_db_write_fail",
                    test_sg_rule_add_rolls_back_on_db_write_fail);
}
