/**
 * @file security_group.c
 * @brief 네트워크 보안 그룹 — SQLite 영속화 + nftables 적용 (bridge 스코프 디스패치)
 *
 * [파일 역할]
 *   VM에 적용할 방화벽 규칙을 그룹 단위로 관리합니다.
 *   SQLite WAL 기반 영속화 + nftables bridge pcv_sg 스코프 체인 적용.
 *
 * [nftables 체인 구조 — 2026-07-04 재설계]
 *   table bridge pcv_sg 하나만 사용한다 (호스트 input/output 훅 금지).
 *     ingress-dispatch (hook postrouting, policy accept)
 *     egress-dispatch  (hook prerouting,  policy accept)
 *       → 바인딩된 vnet 에만 "jump baseline → jump sg-<g> → drop"
 *     baseline-in/out : ARP/DHCP/ND/conntrack 공통 허용
 *     sg-<g>-in/out   : 그룹 accept 규칙 (terminal drop 없음 — 다중 그룹 합성)
 *   기본 정책: ingress deny / egress allow (egress 규칙 보유 시 whitelist 전환).
 *   상세: docs/superpowers/specs/2026-07-04-security-group-scoped-nft-design.md
 *
 * [주니어 참고 — SQLite 영속화가 왜 필요한가]
 *   nftables 규칙은 커널 메모리에만 존재하므로 재부팅 시 사라집니다.
 *   보안 그룹의 이름, 설명, 규칙, VM 바인딩을 SQLite DB에 저장하여
 *   데몬 재시작 시 pcv_security_group_restore()로 복원합니다.
 *   DB 경로: /var/lib/purecvisor/security_groups.db (WAL 모드)
 *
 * [RPC 메서드]
 *   security_group.create  — 보안 그룹 생성
 *   security_group.delete  — 보안 그룹 삭제
 *   security_group.list    — 보안 그룹 목록 조회
 *   security_group.rule.add    — 규칙 추가
 *   security_group.rule.remove — 규칙 삭제
 *   vm.security_group.set      — VM에 보안 그룹 적용
 *
 * [스레드 안전]
 *   GMutex로 보안 그룹 저장소 보호
 */
#include <glib.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <sqlite3.h>
#include "../../utils/pcv_spawn.h"
#include "../../utils/pcv_log.h"
#include "../../utils/pcv_validate.h"
#include "../../utils/pcv_config.h"       /* [I2-R1] resync interval config */
#include "../../utils/pcv_worker_pool.h"  /* [I2-R1] resync 워커 오프로드 */
#include "security_group.h"
#include "security_group_nft.h"
#include "vm_iface.h"
#include "vm_vnet_cache.h"
#include "../security/security_event.h"
#include "../security/security_store.h"

/* ── 타입 정의 ──────────────────────────────────────────────── */

/**
 * SgRule:
 * @direction:  "ingress" 또는 "egress"
 * @protocol:   "tcp", "udp", "icmp" 등
 * @port_start: 포트 시작 (0이면 전체)
 * @port_end:   포트 끝 (0이면 port_start와 동일)
 * @source:     소스 CIDR 또는 보안 그룹 참조
 * @db_id:      SQLite row id (삭제용)
 */
typedef struct {
    gchar  *direction;
    gchar  *protocol;
    gint    port_start;
    gint    port_end;
    gchar  *source;
    gint64  db_id;
} SgRule;

/**
 * SecurityGroup:
 * @name:        보안 그룹 이름 (고유 식별자)
 * @description: 설명
 * @rules:       GPtrArray of SgRule*
 * @vm_bindings: 이 그룹이 적용된 VM 목록 (GPtrArray of gchar*)
 */
typedef struct {
    gchar     *name;
    gchar     *description;
    GPtrArray *rules;
    GPtrArray *vm_bindings;
} SecurityGroup;

/* ── 전역 상태 ──────────────────────────────────────────────── */

static GHashTable *g_sg_map = nullptr;   /* name -> SecurityGroup* */
static GMutex      g_sg_mu;
static sqlite3    *g_sg_db  = nullptr;

#define SG_DB_PATH "/var/lib/purecvisor/security_groups.db"
#define SG_NFT_SCRIPT_PATH "/run/purecvisor/sg-ruleset.nft"
/* [R5] nft/modprobe 정상 sub-second — hung 락/모듈 bound (spawn-hardening) */
#define SG_SPAWN_TIMEOUT_SEC 30
static GMutex g_sg_nft_mu;   /* _nft_run_script 의 nft -f 실행만 직렬화 (spec §5 동시성) */
static GMutex g_sg_dispatch_mu;   /* _rebuild_dispatch 의 스냅샷→nft 실행을 원자 직렬화
                                     (동시 rebuild 의 lost-update / 일시적 fail-open 방어).
                                     락 순서: g_sg_dispatch_mu → g_sg_mu → g_vnet_cache_mu / g_sg_nft_mu
                                     (후자 둘은 최내측, 서로 안 겹침). */
static guint g_sg_resync_timer_id = 0;    /* [I2-R1] g_timeout source id */
static gint  g_sg_resync_inflight = 0;    /* [I2-R1] 중첩 방지 (g_atomic) */

/* ── 내부 헬퍼 ──────────────────────────────────────────────── */

static void
_sg_rule_free(gpointer data)
{
    SgRule *r = data;
    if (!r) return;
    g_free(r->direction);
    g_free(r->protocol);
    g_free(r->source);
    g_free(r);
}

static void
_sg_free(gpointer data)
{
    SecurityGroup *sg = data;
    if (!sg) return;
    g_free(sg->name);
    g_free(sg->description);
    if (sg->rules) g_ptr_array_unref(sg->rules);
    if (sg->vm_bindings) g_ptr_array_unref(sg->vm_bindings);
    g_free(sg);
}

static void
_ensure_init(void)
{
    if (!g_sg_map) {
        g_sg_map = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, _sg_free);
    }
}

/* ── SQLite 영속화 ─────────────────────────────────────────── */

static void _sg_db_init(void) {
    if (g_sg_db) return;
    int rc = sqlite3_open(SG_DB_PATH, &g_sg_db);
    if (rc != SQLITE_OK) {
        PCV_LOG_WARN("SG", "Cannot open SG database %s: %s", SG_DB_PATH, sqlite3_errmsg(g_sg_db));
        g_sg_db = nullptr;
        return;
    }
    sqlite3_exec(g_sg_db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(g_sg_db, "PRAGMA busy_timeout=3000;", NULL, NULL, NULL);

    const char *ddl =
        "CREATE TABLE IF NOT EXISTS security_groups ("
        "  name TEXT PRIMARY KEY, description TEXT);"
        "CREATE TABLE IF NOT EXISTS sg_rules ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  group_name TEXT NOT NULL,"
        "  direction TEXT NOT NULL DEFAULT 'ingress',"
        "  protocol TEXT NOT NULL DEFAULT 'tcp',"
        "  port_start INTEGER NOT NULL DEFAULT 0,"
        "  port_end INTEGER NOT NULL DEFAULT 0,"
        "  source TEXT NOT NULL DEFAULT '0.0.0.0/0',"
        "  FOREIGN KEY(group_name) REFERENCES security_groups(name) ON DELETE CASCADE);"
        "CREATE TABLE IF NOT EXISTS sg_vm_bindings ("
        "  group_name TEXT NOT NULL,"
        "  vm_name TEXT NOT NULL,"
        "  PRIMARY KEY(group_name, vm_name),"
        "  FOREIGN KEY(group_name) REFERENCES security_groups(name) ON DELETE CASCADE);";
    sqlite3_exec(g_sg_db, ddl, NULL, NULL, NULL);
    sqlite3_exec(g_sg_db, "PRAGMA foreign_keys=ON;", NULL, NULL, NULL);
}

/* [R10] void DB 뮤테이터 공통 종료 — step 실패를 삼키지 말고 WARN 후 finalize.
 * save_rule 은 rowid 를 읽어야 해 별도 처리(R8) — 여기 미포함. */
static void
_sg_db_exec_finalize(sqlite3_stmt *stmt, const char *what)
{
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE)
        PCV_LOG_WARN("SG", "%s 실패 (rc=%d): %s", what, rc, sqlite3_errmsg(g_sg_db));
    sqlite3_finalize(stmt);
}

static void _sg_db_save_group(const gchar *name, const gchar *desc) {
    if (!g_sg_db) return;
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(g_sg_db, "INSERT OR REPLACE INTO security_groups(name,description) VALUES(?,?)", -1, &stmt, NULL) != SQLITE_OK || !stmt) return;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, desc ? desc : "", -1, SQLITE_STATIC);
    _sg_db_exec_finalize(stmt, "security_groups INSERT");
}

static void _sg_db_delete_group(const gchar *name) {
    if (!g_sg_db) return;
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(g_sg_db, "DELETE FROM security_groups WHERE name=?", -1, &stmt, NULL) != SQLITE_OK || !stmt) return;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
    _sg_db_exec_finalize(stmt, "security_groups DELETE");
}

static gint64 _sg_db_save_rule(const gchar *group, const gchar *dir, const gchar *proto,
                               gint port_start, gint port_end, const gchar *source) {
    if (!g_sg_db) return -1;
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(g_sg_db,
        "INSERT INTO sg_rules(group_name,direction,protocol,port_start,port_end,source) VALUES(?,?,?,?,?,?)",
        -1, &stmt, NULL) != SQLITE_OK || !stmt) return -1;
    sqlite3_bind_text(stmt, 1, group, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, dir, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, proto, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 4, port_start);
    sqlite3_bind_int(stmt, 5, port_end);
    sqlite3_bind_text(stmt, 6, source, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        PCV_LOG_WARN("SG", "sg_rules INSERT 실패 (rc=%d): %s", rc, sqlite3_errmsg(g_sg_db));
        sqlite3_finalize(stmt);
        return -1;
    }
    gint64 row_id = sqlite3_last_insert_rowid(g_sg_db);
    sqlite3_finalize(stmt);
    return row_id;
}

static void _sg_db_delete_rule(gint64 rule_id) {
    if (!g_sg_db) return;
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(g_sg_db, "DELETE FROM sg_rules WHERE id=?", -1, &stmt, NULL) != SQLITE_OK || !stmt) return;
    sqlite3_bind_int64(stmt, 1, rule_id);
    _sg_db_exec_finalize(stmt, "sg_rules DELETE");
}

static void _sg_db_save_binding(const gchar *group, const gchar *vm) {
    if (!g_sg_db) return;
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(g_sg_db, "INSERT OR IGNORE INTO sg_vm_bindings(group_name,vm_name) VALUES(?,?)", -1, &stmt, NULL) != SQLITE_OK || !stmt) return;
    sqlite3_bind_text(stmt, 1, group, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, vm, -1, SQLITE_STATIC);
    _sg_db_exec_finalize(stmt, "sg_vm_bindings INSERT");
}

static void _sg_db_delete_binding(const gchar *group, const gchar *vm) {
    if (!g_sg_db) return;
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(g_sg_db,
            "DELETE FROM sg_vm_bindings WHERE group_name=? AND vm_name=?",
            -1, &stmt, NULL) != SQLITE_OK || !stmt) return;
    sqlite3_bind_text(stmt, 1, group, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, vm, -1, SQLITE_STATIC);
    _sg_db_exec_finalize(stmt, "sg_vm_bindings DELETE");
}

/* ── nftables 실행 계층 (신설계: table bridge pcv_sg) ────────────
 * 스크립트 생성은 security_group_nft.c (순수), 실행은 여기서만.
 * 모든 적용은 nft -f 단일 트랜잭션 — 부분 상태 없음. */

static gboolean
_nft_run_script(const gchar *script, GError **error)
{
    g_mutex_lock(&g_sg_nft_mu);
    g_mkdir_with_parents("/run/purecvisor", 0755);
    gboolean ok = g_file_set_contents(SG_NFT_SCRIPT_PATH, script, -1, error);
    if (ok) {
        const gchar *argv[] = {"nft", "-f", SG_NFT_SCRIPT_PATH, NULL};
        gchar *std_err = nullptr;
        ok = pcv_spawn_sync_timeout(argv, NULL, &std_err, SG_SPAWN_TIMEOUT_SEC, error);
        if (!ok)
            PCV_LOG_ERROR("SG", "nft -f 트랜잭션 실패: %s",
                          std_err ? std_err : "unknown");
        g_free(std_err);
    }
    g_mutex_unlock(&g_sg_nft_mu);
    return ok;
}

static gboolean
_nft_ensure(GError **error)
{
    /* ct state (baseline) 가 bridge family 에서 동작하려면 필요. 실패해도
     * 계속 — 이후 nft -f 가 거부하면 그 시점에 시끄럽게 실패한다 (spec §5). */
    const gchar *mp[] = {"modprobe", "nf_conntrack_bridge", NULL};
    if (!pcv_spawn_sync_timeout(mp, NULL, NULL, SG_SPAWN_TIMEOUT_SEC, NULL))
        PCV_LOG_WARN("SG", "modprobe nf_conntrack_bridge 실패 — ct 규칙 적용이 거부될 수 있음");
    gchar *script = pcv_sg_nft_build_ensure_script();
    gboolean ok = _nft_run_script(script, error);
    g_free(script);
    return ok;
}

/* 구설계 잔재 정리: inet purecvisor 의 sg-* base chain (구 input/output 훅 + drop).
 * 2026-07-04 장애의 원인 객체 — 데몬 시작 시 best-effort 제거. */
static void
_nft_teardown_legacy(void)
{
    const gchar *ls[] = {"nft", "list", "table", "inet", "purecvisor", NULL};
    gchar *out = nullptr;
    if (!pcv_spawn_sync_timeout(ls, &out, NULL, SG_SPAWN_TIMEOUT_SEC, NULL) || !out) {
        g_free(out);
        return;   /* 테이블 없음 = 정리 불필요 */
    }
    gchar **lines = g_strsplit(out, "\n", -1);
    for (gchar **l = lines; *l; l++) {
        gchar *t = g_strstrip(*l);
        if (!g_str_has_prefix(t, "chain sg-"))
            continue;
        gchar *name = g_strdup(t + strlen("chain "));
        gchar *sp = strchr(name, ' ');
        if (sp) *sp = '\0';
        const gchar *fl[]  = {"nft", "flush",  "chain", "inet", "purecvisor", name, NULL};
        const gchar *del[] = {"nft", "delete", "chain", "inet", "purecvisor", name, NULL};
        (void)pcv_spawn_sync_timeout(fl,  NULL, NULL, SG_SPAWN_TIMEOUT_SEC, NULL);
        (void)pcv_spawn_sync_timeout(del, NULL, NULL, SG_SPAWN_TIMEOUT_SEC, NULL);
        PCV_LOG_INFO("SG", "legacy chain 제거: inet purecvisor %s", name);
        g_free(name);
    }
    g_strfreev(lines);
    g_free(out);
}

/* SgRule → 빌더 뷰 복사 (g_sg_mu 보유 상태에서 호출, 잠금 밖에서 사용/해제) */
static GPtrArray *
_snapshot_rule_view(SecurityGroup *sg)
{
    GPtrArray *view = g_ptr_array_new();
    for (guint i = 0; i < sg->rules->len; i++) {
        SgRule *r = g_ptr_array_index(sg->rules, i);
        SgNftRule *n = g_new0(SgNftRule, 1);
        n->direction  = g_strdup(r->direction);
        n->protocol   = g_strdup(r->protocol);
        n->port_start = r->port_start;
        n->port_end   = r->port_end;
        n->source     = g_strdup(r->source);
        g_ptr_array_add(view, n);
    }
    return view;
}

static void
_rule_view_free(GPtrArray *view)
{
    for (guint i = 0; i < view->len; i++) {
        SgNftRule *n = g_ptr_array_index(view, i);
        g_free((gchar *)n->direction);
        g_free((gchar *)n->protocol);
        g_free((gchar *)n->source);
        g_free(n);
    }
    g_ptr_array_unref(view);
}

/* 전체 바인딩 상태 → 디스패치 재생성 (spec §4-3).
 * 1) g_sg_mu 아래 스냅샷 (spawn 금지 구간)
 * 2) 잠금 밖 vnet 해석 (virsh 블로킹)
 * 3) 단일 nft -f 트랜잭션
 * fail-closed: 디스패치 재적재는 "두 dispatch 체인 flush + 전체 재적재"를 하나의
 *   스크립트로 만들어 단일 _nft_run_script(=단일 nft -f) 로 실행한다. nft -f 는
 *   트랜잭션이므로 실패 시 디스패치 체인은 이전 상태 그대로 — 부분 적용 없음. */
static gboolean
_rebuild_dispatch_ex(const gchar *prefix, GError **error)
{
    /* 스냅샷→nft 실행 전체를 원자 직렬화
     * (락 순서: g_sg_dispatch_mu → g_sg_mu → g_vnet_cache_mu / g_sg_nft_mu,
     *  후자 둘은 최내측, 서로 안 겹침). */
    /* [M-6] prefix(그룹 체인 스크립트)가 주어지면 dispatch 스크립트 앞에 붙여
     * 같은 nft -f 트랜잭션으로 원자 실행한다 (그룹 체인 → dispatch jump 순서). */
    g_mutex_lock(&g_sg_dispatch_mu);
    g_mutex_lock(&g_sg_mu);
    _ensure_init();
    GHashTable *vm_map = g_hash_table_new_full(g_str_hash, g_str_equal,
                                               g_free, (GDestroyNotify)g_ptr_array_unref);
    GHashTable *egress_groups = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, g_sg_map);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        SecurityGroup *sg = v;
        for (guint i = 0; i < sg->rules->len; i++) {
            SgRule *r = g_ptr_array_index(sg->rules, i);
            if (g_strcmp0(r->direction, "egress") == 0) {
                g_hash_table_add(egress_groups, g_strdup(sg->name));
                break;
            }
        }
        for (guint i = 0; i < sg->vm_bindings->len; i++) {
            const gchar *vm = g_ptr_array_index(sg->vm_bindings, i);
            GPtrArray *groups = g_hash_table_lookup(vm_map, vm);
            if (!groups) {
                groups = g_ptr_array_new_with_free_func(g_free);
                g_hash_table_insert(vm_map, g_strdup(vm), groups);
            }
            g_ptr_array_add(groups, g_strdup(sg->name));
        }
    }
    g_mutex_unlock(&g_sg_mu);

    GPtrArray *bindings = g_ptr_array_new();
    g_hash_table_iter_init(&it, vm_map);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        const gchar *vm = k;
        GPtrArray *groups = v;
        gboolean egress = FALSE;
        for (guint j = 0; j < groups->len; j++) {
            if (g_hash_table_contains(egress_groups, g_ptr_array_index(groups, j))) {
                egress = TRUE;
                break;
            }
        }
        /* I-2: virsh sweep 제거 — 캐시 우선. 콜드 미스만 lazy 해석 + 적재 (fail-open 방지). */
        GPtrArray *ifaces = pcv_vm_vnet_cache_get(vm);
        if (!ifaces) {
            ifaces = pcv_vm_iface_list(vm);          /* 콜드: 1회 해석 */
            if (ifaces->len > 0)                     /* 실행 중일 때만 적재 (꺼진 VM 은 캐시 안 함) */
                pcv_vm_vnet_cache_put(vm, ifaces);
        }
        for (guint j = 0; j < ifaces->len; j++) {
            SgNftBinding *b = g_new0(SgNftBinding, 1);
            b->vnet            = g_strdup(g_ptr_array_index(ifaces, j));
            b->groups          = groups;   /* vm_map 소유 — 스크립트 생성까지만 참조 */
            b->egress_enforced = egress;
            g_ptr_array_add(bindings, b);
        }
        g_ptr_array_unref(ifaces);
    }

    gchar *dscript = pcv_sg_nft_build_dispatch_script(bindings);
    gchar *full = prefix ? g_strconcat(prefix, "\n", dscript, NULL)   /* 그룹 체인 → dispatch */
                         : g_strdup(dscript);
    gboolean ok = _nft_run_script(full, error);
    g_free(full);
    g_free(dscript);

    for (guint i = 0; i < bindings->len; i++) {
        SgNftBinding *b = g_ptr_array_index(bindings, i);
        g_free((gchar *)b->vnet);
        g_free(b);
    }
    g_ptr_array_unref(bindings);
    g_hash_table_destroy(vm_map);
    g_hash_table_destroy(egress_groups);
    g_mutex_unlock(&g_sg_dispatch_mu);
    return ok;
}

/* 기존 dispatch-only 경로 — 시그니처·동작 보존 (M-6 이전과 동일). */
static gboolean
_rebuild_dispatch(GError **error)
{
    return _rebuild_dispatch_ex(NULL, error);
}

/* ── 공개 API ───────────────────────────────────────────────── */

/**
 * pcv_security_group_create — 보안 그룹 생성
 *
 * @param name         보안 그룹 이름
 * @param description  설명 (NULL이면 빈 문자열)
 * @return TRUE 성공, FALSE 이미 존재 또는 nft 반영 실패
 */
[[nodiscard]] gboolean
pcv_security_group_create(const gchar *name, const gchar *description)
{
    if (!name || !*name) return FALSE;
    /* 그룹 이름은 nft 체인 구성요소(sg-<name>-in/-out) — V5 화이트리스트 유지 */
    if (!pcv_validate_bridge_name(name)) {
        PCV_LOG_WARN("SG", "Rejected security group create: invalid name '%s'", name);
        return FALSE;
    }
    _sg_db_init();

    g_mutex_lock(&g_sg_mu);
    _ensure_init();
    if (g_hash_table_contains(g_sg_map, name)) {
        g_mutex_unlock(&g_sg_mu);
        return FALSE;
    }
    SecurityGroup *sg = g_new0(SecurityGroup, 1);
    sg->name        = g_strdup(name);
    sg->description = g_strdup(description ? description : "");
    sg->rules       = g_ptr_array_new_with_free_func(_sg_rule_free);
    sg->vm_bindings = g_ptr_array_new_with_free_func(g_free);
    g_hash_table_insert(g_sg_map, sg->name, sg);
    g_mutex_unlock(&g_sg_mu);

    /* spec §5: nft 성공 후에만 영속화. 그룹 생성 = 빈 regular chain 골격만 —
     * 디스패치에 jump/drop 이 없으므로 어떤 트래픽에도 영향 없음 (spec §3 계약 1) */
    GError *error = nullptr;
    gchar *gscript = pcv_sg_nft_build_group_script(name, NULL);
    gboolean ok = _nft_ensure(&error) && _nft_run_script(gscript, &error);
    g_free(gscript);
    if (!ok) {
        PCV_LOG_ERROR("SG", "'%s' nft 반영 실패 — 생성 롤백: %s",
                      name, error ? error->message : "unknown");
        g_clear_error(&error);
        g_mutex_lock(&g_sg_mu);
        g_hash_table_remove(g_sg_map, name);   /* _sg_free 가 정리 */
        g_mutex_unlock(&g_sg_mu);
        return FALSE;
    }
    _sg_db_save_group(name, description);
    PCV_LOG_INFO("SG", "Created security group '%s' (bridge pcv_sg 스코프 체인)", name);
    return TRUE;
}

/**
 * pcv_security_group_delete — 보안 그룹 삭제
 *
 * 바인딩 소멸을 디스패치에 먼저 반영한 뒤 그룹 체인을 destroy 한다.
 * nft 반영 실패 시 인메모리 상태를 롤백하고 FALSE 를 반환한다 (spec §5).
 */
gboolean
pcv_security_group_delete(const gchar *name)
{
    if (!name) return FALSE;

    g_mutex_lock(&g_sg_mu);
    _ensure_init();
    SecurityGroup *sg = g_hash_table_lookup(g_sg_map, name);
    if (!sg) {
        g_mutex_unlock(&g_sg_mu);
        return FALSE;
    }
    g_hash_table_steal(g_sg_map, name);   /* 롤백 대비 소유권만 분리 */
    g_mutex_unlock(&g_sg_mu);

    /* [R3] steal 후 sg 유효할 때 VM 이름 스냅샷 (아래 _sg_free 가 vm_bindings 해제 전).
     * 성공 시 그 그룹에만 묶였던 VM 의 dormant 캐시를 회수한다. */
    GPtrArray *unbind_vms = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < sg->vm_bindings->len; i++)
        g_ptr_array_add(unbind_vms, g_strdup(g_ptr_array_index(sg->vm_bindings, i)));

    /* 바인딩 소멸 반영(디스패치) → 그룹 체인 destroy → 성공 시에만 DB 삭제 */
    GError *error = nullptr;
    gboolean ok = _rebuild_dispatch(&error);
    if (ok) {
        gchar *dscript = pcv_sg_nft_build_group_delete_script(name);
        ok = _nft_run_script(dscript, &error);
        g_free(dscript);
    }
    if (!ok) {
        PCV_LOG_ERROR("SG", "'%s' nft 삭제 실패 — 롤백: %s",
                      name, error ? error->message : "unknown");
        g_clear_error(&error);
        g_mutex_lock(&g_sg_mu);
        g_hash_table_insert(g_sg_map, sg->name, sg);
        g_mutex_unlock(&g_sg_mu);
        g_ptr_array_unref(unbind_vms);   /* [R3] 롤백 — 그룹 재삽입, evict 안 함 */
        return FALSE;
    }
    _sg_db_delete_group(name);
    _sg_free(sg);
    /* [R3] 삭제된 그룹에만 묶였던 VM 들의 dormant 캐시 엔트리 회수 */
    for (guint i = 0; i < unbind_vms->len; i++) {
        const gchar *rvm = g_ptr_array_index(unbind_vms, i);
        if (!pcv_security_group_vm_is_bound(rvm))
            pcv_vm_vnet_cache_evict(rvm);
    }
    g_ptr_array_unref(unbind_vms);
    PCV_LOG_INFO("SG", "Deleted security group '%s'", name);
    return TRUE;
}

/**
 * pcv_security_group_list — 모든 보안 그룹 목록 반환
 *
 * Returns: (transfer full): JsonArray* — 호출자가 json_array_unref() 필요
 */
JsonArray *
pcv_security_group_list(void)
{
    JsonArray *arr = json_array_new();

    g_mutex_lock(&g_sg_mu);
    _ensure_init();

    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, g_sg_map);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        SecurityGroup *sg = value;
        JsonObject *obj = json_object_new();
        json_object_set_string_member(obj, "name", sg->name);
        json_object_set_string_member(obj, "description", sg->description);
        json_object_set_int_member(obj, "rule_count", (gint64)sg->rules->len);

        JsonArray *vm_arr = json_array_new();
        for (guint i = 0; i < sg->vm_bindings->len; i++)
            json_array_add_string_element(vm_arr, g_ptr_array_index(sg->vm_bindings, i));
        json_object_set_array_member(obj, "vms", vm_arr);

        JsonArray *rule_arr = json_array_new();
        for (guint i = 0; i < sg->rules->len; i++) {
            SgRule *r = g_ptr_array_index(sg->rules, i);
            JsonObject *robj = json_object_new();
            json_object_set_string_member(robj, "direction", r->direction);
            json_object_set_string_member(robj, "protocol", r->protocol);
            json_object_set_int_member(robj, "port_start", r->port_start);
            json_object_set_int_member(robj, "port_end", r->port_end);
            json_object_set_string_member(robj, "source", r->source);
            json_object_set_int_member(robj, "id", r->db_id);
            json_array_add_object_element(rule_arr, robj);
        }
        json_object_set_array_member(obj, "rules", rule_arr);

        json_array_add_object_element(arr, obj);
    }
    g_mutex_unlock(&g_sg_mu);
    return arr;
}

/**
 * pcv_security_group_rule_add — 보안 그룹에 규칙 추가
 *
 * @param name  보안 그룹 이름
 * @param rule  규칙 파라미터 (direction, protocol, port, source)
 * @return TRUE 성공, FALSE 그룹 미존재
 */
gboolean
pcv_security_group_rule_add(const gchar *name, JsonObject *rule)
{
    if (!name || !rule) return FALSE;

    /* ── 입력 검증 (nft argv-reparse 인젝션 차단, V5) ─────────────────
     * source/protocol/direction 은 그대로 nft argv 요소가 되며, nft 는
     * argv 를 이어붙여 다시 렉싱하므로 "1.2.3.4/32 accept; flush ruleset #"
     * 같은 값이 들어오면 flush ruleset 이 실행되어 호스트 방화벽이 통째로
     * 지워진다. 락을 잡거나 힙을 할당하기 전에 JSON 값을 로컬로 추출·검증하고
     * 거부 경로에서 정리할 상태가 없도록 조기 반환한다. */
    const gchar *direction = json_object_has_member(rule, "direction")
        ? json_object_get_string_member(rule, "direction") : "ingress";
    const gchar *protocol = json_object_has_member(rule, "protocol")
        ? json_object_get_string_member(rule, "protocol") : "tcp";
    const gchar *source = json_object_has_member(rule, "source")
        ? json_object_get_string_member(rule, "source") : "0.0.0.0/0";
    gint port_start = json_object_has_member(rule, "port_start")
        ? (gint)json_object_get_int_member(rule, "port_start")
        : (json_object_has_member(rule, "port")
            ? (gint)json_object_get_int_member(rule, "port") : 0);
    gint port_end = json_object_has_member(rule, "port_end")
        ? (gint)json_object_get_int_member(rule, "port_end") : 0;

    /* direction: 정확히 "ingress" 또는 "egress" (nft in/out 체인 선택) */
    if (g_strcmp0(direction, "ingress") != 0 &&
        g_strcmp0(direction, "egress")  != 0) {
        PCV_LOG_WARN("SG", "Rejected rule for '%s': invalid direction '%s'",
                     name, direction ? direction : "(null)");
        return FALSE;
    }
    /* protocol: tcp|udp|icmp 화이트리스트 (argv-reparse 방지) */
    if (!pcv_validate_l4_proto(protocol)) {
        PCV_LOG_WARN("SG", "Rejected rule for '%s': invalid protocol '%s'",
                     name, protocol ? protocol : "(null)");
        return FALSE;
    }
    /* source: CIDR 형식만 (any-sentinel "0.0.0.0/0" 도 통과) */
    if (!pcv_validate_cidr(source)) {
        PCV_LOG_WARN("SG", "Rejected rule for '%s': invalid source CIDR '%s'",
                     name, source ? source : "(null)");
        return FALSE;
    }
    /* port: 지정된 경우 1..65535, 범위 지정 시 end >= start */
    if (port_start > 0 && !pcv_validate_port(port_start)) {
        PCV_LOG_WARN("SG", "Rejected rule for '%s': invalid port_start %d",
                     name, port_start);
        return FALSE;
    }
    if (port_end > 0 && !pcv_validate_port(port_end)) {
        PCV_LOG_WARN("SG", "Rejected rule for '%s': invalid port_end %d",
                     name, port_end);
        return FALSE;
    }
    if (port_start > 0 && port_end > 0 && port_end < port_start) {
        PCV_LOG_WARN("SG", "Rejected rule for '%s': port_end %d < port_start %d",
                     name, port_end, port_start);
        return FALSE;
    }

    /* [R4] SG DB 가 열려 있어야 규칙을 영속화할 수 있다. DB 미가용(degraded)이면
     * db_id 를 얻지 못해 M-3 가드(rule_id<=0 거부)로 rule_remove 불가한 orphan
     * 규칙이 생긴다. 메모리·nft 를 건드리기 전에 조기 거부한다(fail-closed, no orphan). */
    _sg_db_init();   /* 멱등 — 이미 열렸으면 no-op */
    if (!g_sg_db) {
        PCV_LOG_WARN("SG", "Rejected rule_add for '%s': SG DB unavailable (degraded)", name);
        return FALSE;
    }

    g_mutex_lock(&g_sg_mu);
    _ensure_init();
    SecurityGroup *sg = g_hash_table_lookup(g_sg_map, name);
    if (!sg) {
        g_mutex_unlock(&g_sg_mu);
        return FALSE;
    }

    SgRule *r = g_new0(SgRule, 1);
    r->direction  = g_strdup(direction);
    r->protocol   = g_strdup(protocol);
    r->port_start = port_start;
    r->port_end   = port_end;
    r->source     = g_strdup(source);
    g_ptr_array_add(sg->rules, r);

    GPtrArray *view = _snapshot_rule_view(sg);
    g_mutex_unlock(&g_sg_mu);

    /* 그룹 체인 재적재 + 디스패치 재생성 (egress whitelist 전환 반영).
     * spec §5: 실패 시 in-memory 롤백, DB 미기록. */
    /* [M-6] 그룹 체인 + dispatch 를 단일 nft -f 트랜잭션으로 — 부분실패(그룹 체인만
     * 적용되고 dispatch 실패) 발산 제거. 실패 시 커널도 함께 롤백된다. */
    GError *error = nullptr;
    gchar *gscript = pcv_sg_nft_build_group_script(name, view);
    gboolean ok = _rebuild_dispatch_ex(gscript, &error);
    g_free(gscript);
    _rule_view_free(view);

    if (!ok) {
        PCV_LOG_ERROR("SG", "rule_add '%s' nft 반영 실패 — 롤백: %s",
                      name, error ? error->message : "unknown");
        g_clear_error(&error);
        g_mutex_lock(&g_sg_mu);
        SecurityGroup *sg2 = g_hash_table_lookup(g_sg_map, name);
        if (sg2) g_ptr_array_remove(sg2->rules, r);   /* _sg_rule_free 가 해제 */
        g_mutex_unlock(&g_sg_mu);
        return FALSE;
    }
    /* nft 성공 → DB 영속화. r->db_id 쓰기는 r 이 여전히 살아있을 때만 (동시
     * pcv_security_group_delete 가 nft 창 동안 r 을 해제할 수 있으므로 UAF 방어).
     * DB 저장 값은 r 이 아니라 로컬(name/direction/protocol/source/port_*)에서 취한다. */
    gint64 db_id = _sg_db_save_rule(name, direction, protocol,
                                    port_start, port_end, source);

    if (db_id < 1) {
        /* [R8] DB 저장 실패(디스크 full/I/O 오류/FK 등) — nft 는 이미 적용됨.
         * db_id 불량인 채 두면 M-3 가드로 rule_remove 불가한 orphan 이 된다.
         * 메모리에서 r 을 제거하고 nft 를 그 규칙 없이 재빌드(보상 트랜잭션)해
         * 규칙을 원천 되돌린다(fail-closed, no orphan). */
        PCV_LOG_ERROR("SG", "rule_add '%s' DB 저장 실패 — nft 롤백", name);
        g_mutex_lock(&g_sg_mu);
        SecurityGroup *sgc = g_hash_table_lookup(g_sg_map, name);
        GPtrArray *rb_view = NULL;
        if (sgc && g_ptr_array_find(sgc->rules, r, NULL)) {
            g_ptr_array_remove(sgc->rules, r);      /* _sg_rule_free 가 해제 */
            rb_view = _snapshot_rule_view(sgc);     /* r 제거 후 스냅샷(deep copy) */
        }
        g_mutex_unlock(&g_sg_mu);
        if (rb_view) {                               /* r 이 살아있었을 때만 revert */
            GError *rberr = nullptr;
            gchar *rbscript = pcv_sg_nft_build_group_script(name, rb_view);
            if (!_rebuild_dispatch_ex(rbscript, &rberr)) {
                PCV_LOG_ERROR("SG", "rule_add '%s' 보상 revert 실패"
                              "(이중 결함 — 다음 변이/restore 가 자가치유): %s",
                              name, rberr ? rberr->message : "unknown");
                g_clear_error(&rberr);
            }
            g_free(rbscript);
            _rule_view_free(rb_view);
        }
        return FALSE;
    }

    g_mutex_lock(&g_sg_mu);
    SecurityGroup *sg3 = g_hash_table_lookup(g_sg_map, name);
    if (sg3 && g_ptr_array_find(sg3->rules, r, NULL)) {
        r->db_id = db_id;
    } else {
        /* 그룹/규칙이 nft 창 동안 삭제됨 — 방금 넣은 DB 행 정리 (고아 방지) */
        _sg_db_delete_rule(db_id);
    }
    g_mutex_unlock(&g_sg_mu);
    PCV_LOG_INFO("SG", "Applied rule to '%s': %s %s port %d-%d from %s",
        name, direction, protocol, port_start, port_end, source);
    return TRUE;
}

/**
 * pcv_security_group_apply_to_vm — VM에 보안 그룹 바인딩
 *
 * @param vm  VM 이름
 * @param sg  보안 그룹 이름
 * @return TRUE 성공, FALSE 그룹 미존재
 */
gboolean
pcv_security_group_apply_to_vm(const gchar *vm, const gchar *sg_name)
{
    if (!vm || !sg_name) return FALSE;
    /* vm 은 virsh domiflist argv 로 흘러간다 — V11 화이트리스트 게이트 */
    if (!pcv_validate_vm_name(vm)) {
        PCV_LOG_WARN("SG", "Rejected bind: invalid vm '%s'", vm);
        return FALSE;
    }

    g_mutex_lock(&g_sg_mu);
    _ensure_init();
    SecurityGroup *sg = g_hash_table_lookup(g_sg_map, sg_name);
    if (!sg) {
        g_mutex_unlock(&g_sg_mu);
        return FALSE;
    }
    for (guint i = 0; i < sg->vm_bindings->len; i++) {
        if (g_strcmp0(g_ptr_array_index(sg->vm_bindings, i), vm) == 0) {
            g_mutex_unlock(&g_sg_mu);
            return TRUE;   /* 멱등 */
        }
    }
    g_ptr_array_add(sg->vm_bindings, g_strdup(vm));
    g_mutex_unlock(&g_sg_mu);

    /* I-2/C1: 바인딩 시점에 캐시를 무효화해 항상 fresh 해석을 강제한다. 언바인딩
     * 상태에서 재시작된 VM(sync_vm 의 evict 는 bound 게이트 뒤라 미도달)의 stale
     * vnet 이 재바인딩 시 그대로 쓰여 무필터가 되는 fail-open 을 막는다. 이어지는
     * _rebuild_dispatch 의 lazy-miss 가 live vnet 을 재해석·적재한다 (bind 당 O(1)). */
    pcv_vm_vnet_cache_evict(vm);

    GError *error = nullptr;
    if (!_rebuild_dispatch(&error)) {
        PCV_LOG_ERROR("SG", "bind '%s'→VM '%s' nft 반영 실패 — 롤백: %s",
                      sg_name, vm, error ? error->message : "unknown");
        g_clear_error(&error);
        g_mutex_lock(&g_sg_mu);
        SecurityGroup *sg2 = g_hash_table_lookup(g_sg_map, sg_name);
        if (sg2) {
            for (guint i = 0; i < sg2->vm_bindings->len; i++) {
                if (g_strcmp0(g_ptr_array_index(sg2->vm_bindings, i), vm) == 0) {
                    g_ptr_array_remove_index(sg2->vm_bindings, i);
                    break;
                }
            }
        }
        g_mutex_unlock(&g_sg_mu);
        /* [R3] 롤백으로 vm 이 fully-unbound 됐는데 _rebuild_dispatch 의 lazy-miss 가
         * 캐시를 재적재했을 수 있으므로 dormant 엔트리를 회수한다 (detach/delete 와 대칭).
         * is_bound 는 g_sg_mu 를 잡으므로 반드시 위 unlock 뒤에 둔다. */
        if (!pcv_security_group_vm_is_bound(vm))
            pcv_vm_vnet_cache_evict(vm);
        return FALSE;
    }
    _sg_db_save_binding(sg_name, vm);
    PCV_LOG_INFO("SG", "Applied security group '%s' to VM '%s'", sg_name, vm);
    return TRUE;
}

/**
 * pcv_security_group_rule_remove — 보안 그룹에서 규칙 삭제 (db_id 기반)
 *
 * @param name    보안 그룹 이름
 * @param rule_id SQLite row id
 * @return TRUE 성공, FALSE 그룹 미존재 또는 규칙 미존재
 */
gboolean
pcv_security_group_rule_remove(const gchar *name, gint64 rule_id)
{
    if (!name) return FALSE;
    if (rule_id <= 0) {   /* [M-3] sqlite rowid 는 ≥1 — 0/음수는 유효 규칙 아님.
                             * in-flight 규칙(db_id==0)을 잘못 steal 하는 것 방지. */
        PCV_LOG_WARN("SG", "Rejected rule_remove for '%s': invalid rule_id %ld",
                     name, (long)rule_id);
        return FALSE;
    }

    g_mutex_lock(&g_sg_mu);
    _ensure_init();
    SecurityGroup *sg = g_hash_table_lookup(g_sg_map, name);
    if (!sg) {
        g_mutex_unlock(&g_sg_mu);
        return FALSE;
    }
    SgRule *stolen = nullptr;
    for (guint i = 0; i < sg->rules->len; i++) {
        SgRule *r = g_ptr_array_index(sg->rules, i);
        if (r->db_id == rule_id) {
            stolen = g_ptr_array_steal_index(sg->rules, i);   /* 롤백 대비 소유권 분리 */
            break;
        }
    }
    if (!stolen) {
        g_mutex_unlock(&g_sg_mu);
        return FALSE;
    }
    GPtrArray *view = _snapshot_rule_view(sg);
    g_mutex_unlock(&g_sg_mu);

    /* [M-6] 그룹 체인 + dispatch 를 단일 nft -f 트랜잭션으로 (rule_add 와 대칭). */
    GError *error = nullptr;
    gchar *gscript = pcv_sg_nft_build_group_script(name, view);
    gboolean ok = _rebuild_dispatch_ex(gscript, &error);
    g_free(gscript);
    _rule_view_free(view);

    if (!ok) {
        PCV_LOG_ERROR("SG", "rule_remove '%s'/%ld nft 반영 실패 — 롤백: %s",
                      name, (long)rule_id, error ? error->message : "unknown");
        g_clear_error(&error);
        g_mutex_lock(&g_sg_mu);
        SecurityGroup *sg2 = g_hash_table_lookup(g_sg_map, name);
        if (sg2) g_ptr_array_add(sg2->rules, stolen);
        else     _sg_rule_free(stolen);   /* 그룹이 사라졌으면 해제만 */
        g_mutex_unlock(&g_sg_mu);
        return FALSE;
    }
    _sg_db_delete_rule(rule_id);
    _sg_rule_free(stolen);
    PCV_LOG_INFO("SG", "Removed rule %ld from '%s'", (long)rule_id, name);
    return TRUE;
}

/* [M-10→B-2] evidence 가드는 security_event 공용 함수로 이동 (역직렬화 site 와
 * 대칭성 완결). 이 래퍼는 기존 호출부 호환용 얇은 위임. */
static void
_sg_set_evidence(gchar *dst, gsize dstsz, const gchar *ejstr)
{
    pcv_security_event_set_evidence(dst, dstsz, ejstr);
}

/* [M-7] restore 시 nft 테이블 준비 실패 = 부팅 시 바인딩된 모든 VM 이 미필터
 * (fleet-wide fail-open) 인데 ERROR 로그만으로는 운영자가 놓치기 쉽다. sync_vm
 * 실패와 동일하게 CRITICAL security_event 로 surface 한다. restore 는 첫 보안 RPC
 * 이전(store lazy-open 전)이라 pcv_security_store_ensure_open() 으로 store 를 먼저
 * 연다. target 은 특정 VM 이 아니라 host(fleet). */
static void
_emit_sg_restore_failure_event(gint group_count, const GError *err)
{
    (void)pcv_security_store_ensure_open();   /* 부팅 경로: store 가 아직 안 열렸을 수 있음 */

    PcvSecurityEvent ev = {0};
    ev.timestamp   = g_get_real_time() / G_USEC_PER_SEC;
    ev.source      = PCV_SECURITY_SOURCE_PCV_AUDIT;
    ev.type        = PCV_SECURITY_EVENT_AUDIT_PATTERN;
    ev.severity    = PCV_SECURITY_SEVERITY_CRIT;
    ev.confidence  = 100;
    ev.target_kind = PCV_SECURITY_TARGET_HOST;
    ev.status      = PCV_SECURITY_STATUS_OPEN;
    g_strlcpy(ev.target, "host", sizeof(ev.target));
    g_snprintf(ev.summary, sizeof(ev.summary),
               "security group nft 테이블 복원 실패 — 바인딩된 %d개 그룹의 VM 이 부팅 시 미필터 상태",
               group_count);
    g_strlcpy(ev.recommended_action, "sg-resync", sizeof(ev.recommended_action));
    JsonObject *ej = json_object_new();
    json_object_set_string_member(ej, "error",
        (err && err->message) ? err->message : "unknown");
    JsonNode *ejroot = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(ejroot, ej);
    gchar *ejstr = json_to_string(ejroot, FALSE);
    _sg_set_evidence(ev.evidence_json, sizeof ev.evidence_json, ejstr);
    g_free(ejstr);
    json_node_free(ejroot);
    pcv_security_event_make_id(&ev, "sg");
    GError *serr = nullptr;
    if (!pcv_security_store_insert_event(&ev, &serr)) {
        PCV_LOG_WARN("SG", "restore security_event 기록 실패: %s",
                     serr ? serr->message : "unknown");
        g_clear_error(&serr);
    }
}

/**
 * pcv_security_group_restore — 데몬 시작 시 SQLite에서 보안 그룹 복원
 *
 * DB에서 모든 보안 그룹, 규칙, VM 바인딩을 로드하고
 * nftables 체인을 재생성합니다.
 */
void
pcv_security_group_restore(void)
{
    _sg_db_init();
    if (!g_sg_db) return;

    g_mutex_lock(&g_sg_mu);
    _ensure_init();

    /* 보안 그룹 로드 */
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(g_sg_db, "SELECT name, description FROM security_groups",
                       -1, &stmt, NULL) == SQLITE_OK && stmt) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const gchar *name = (const gchar *)sqlite3_column_text(stmt, 0);
            const gchar *desc = (const gchar *)sqlite3_column_text(stmt, 1);
            SecurityGroup *sg = g_new0(SecurityGroup, 1);
            sg->name = g_strdup(name);
            sg->description = g_strdup(desc ? desc : "");
            sg->rules = g_ptr_array_new_with_free_func(_sg_rule_free);
            sg->vm_bindings = g_ptr_array_new_with_free_func(g_free);
            g_hash_table_insert(g_sg_map, sg->name, sg);
        }
        sqlite3_finalize(stmt);
    }

    /* 규칙 로드 */
    stmt = nullptr;
    if (sqlite3_prepare_v2(g_sg_db,
        "SELECT id, group_name, direction, protocol, port_start, port_end, source FROM sg_rules",
        -1, &stmt, NULL) == SQLITE_OK && stmt) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            gint64 id = sqlite3_column_int64(stmt, 0);
            const gchar *grp = (const gchar *)sqlite3_column_text(stmt, 1);
            SecurityGroup *sg = g_hash_table_lookup(g_sg_map, grp);
            if (!sg) continue;
            SgRule *r = g_new0(SgRule, 1);
            r->db_id = id;
            r->direction  = g_strdup((const gchar *)sqlite3_column_text(stmt, 2));
            r->protocol   = g_strdup((const gchar *)sqlite3_column_text(stmt, 3));
            r->port_start = sqlite3_column_int(stmt, 4);
            r->port_end   = sqlite3_column_int(stmt, 5);
            r->source     = g_strdup((const gchar *)sqlite3_column_text(stmt, 6));
            g_ptr_array_add(sg->rules, r);
        }
        sqlite3_finalize(stmt);
    }

    /* VM 바인딩 로드 */
    stmt = nullptr;
    if (sqlite3_prepare_v2(g_sg_db, "SELECT group_name, vm_name FROM sg_vm_bindings",
                       -1, &stmt, NULL) == SQLITE_OK && stmt) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const gchar *grp = (const gchar *)sqlite3_column_text(stmt, 0);
            const gchar *vm  = (const gchar *)sqlite3_column_text(stmt, 1);
            SecurityGroup *sg = g_hash_table_lookup(g_sg_map, grp);
            if (sg) g_ptr_array_add(sg->vm_bindings, g_strdup(vm));
        }
        sqlite3_finalize(stmt);
    }

    gint count = g_hash_table_size(g_sg_map);
    g_mutex_unlock(&g_sg_mu);

    /* ── nft 반영 (spec §4-3 (4)): legacy 정리 → ensure → 그룹 체인 → 디스패치 ── */
    _nft_teardown_legacy();

    GError *error = nullptr;
    if (!_nft_ensure(&error)) {
        gint gc = g_hash_table_size(g_sg_map);
        PCV_LOG_ERROR("SG", "restore: pcv_sg 테이블 준비 실패 — nft 반영 건너뜀: %s",
                      error ? error->message : "unknown");
        /* [M-5] 조기 반환 경로도 로드 결과를 남긴다 — 위 ERROR 만 보면 몇 개
         * 그룹이 메모리/DB 에 적재됐는지 알 수 없으므로 운영자에게 count 를 surface. */
        PCV_LOG_INFO("SG", "Loaded %d security groups from DB (nft 미반영 — 위 ERROR 참조)", gc);
        /* [M-7] fleet-wide fail-open 을 security_event 로도 surface (error 정리 전에 emit) */
        _emit_sg_restore_failure_event(gc, error);
        g_clear_error(&error);
        return;   /* 부팅 블로킹 금지 (spec §5) — 메모리/DB 상태는 이미 적재됨 */
    }

    /* 그룹 단위로 계속 진행 — 한 그룹 실패가 다른 그룹을 막지 않는다 */
    g_mutex_lock(&g_sg_mu);
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *views = g_ptr_array_new();
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, g_sg_map);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        g_ptr_array_add(names, g_strdup((const gchar *)k));
        g_ptr_array_add(views, _snapshot_rule_view((SecurityGroup *)v));
    }
    g_mutex_unlock(&g_sg_mu);

    for (guint i = 0; i < names->len; i++) {
        gchar *script = pcv_sg_nft_build_group_script(
            g_ptr_array_index(names, i), g_ptr_array_index(views, i));
        if (!_nft_run_script(script, &error)) {
            PCV_LOG_ERROR("SG", "restore: 그룹 '%s' 체인 복원 실패: %s",
                          (gchar *)g_ptr_array_index(names, i),
                          error ? error->message : "unknown");
            g_clear_error(&error);
        }
        g_free(script);
        _rule_view_free(g_ptr_array_index(views, i));
    }
    g_ptr_array_unref(names);
    g_ptr_array_unref(views);

    if (!_rebuild_dispatch(&error)) {
        PCV_LOG_ERROR("SG", "restore: 디스패치 복원 실패: %s",
                      error ? error->message : "unknown");
        g_clear_error(&error);
    }

    if (count > 0)
        PCV_LOG_INFO("SG", "Restored %d security groups from database", count);
}

/**
 * pcv_security_group_resync_all — 바인딩된 모든 VM 의 vnet 을 fresh 재해석
 *
 * [I2-R1] _rebuild_dispatch 는 캐시 HIT 을 liveness 재확인 없이 신뢰하므로,
 * 라이프사이클 이벤트가 유실되면 stale vnet 캐시로 fail-open 할 수 있다.
 * 이 함수는 바인딩된 VM 전부를 virsh 로 다시 조회해 캐시를 교정한다.
 * virsh 가 빈 결과(off/transient)를 내면 그 VM 의 기존 캐시는 그대로 둔다
 * (keep-old-on-empty) — transient 실패로 dispatch 에서 vnet 이 사라지는
 * fail-open 을 만들지 않기 위함.
 */
void
pcv_security_group_resync_all(void)
{
    /* 바인딩된 VM 이름 스냅샷 (dedup) — g_sg_mu 하, 스폰 금지 구간 */
    g_mutex_lock(&g_sg_mu);
    _ensure_init();
    GHashTable *set = g_hash_table_new(g_str_hash, g_str_equal);  /* 값 없는 set, borrowed key */
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, g_sg_map);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        SecurityGroup *sg = v;
        for (guint i = 0; i < sg->vm_bindings->len; i++)
            g_hash_table_add(set, g_ptr_array_index(sg->vm_bindings, i));  /* 락 안에서만 유효 */
    }
    GPtrArray *vms = g_ptr_array_new_with_free_func(g_free);
    g_hash_table_iter_init(&it, set);
    while (g_hash_table_iter_next(&it, &k, &v))
        g_ptr_array_add(vms, g_strdup((const gchar *)k));   /* 락 밖 사용 위해 복사 */
    g_hash_table_destroy(set);
    g_mutex_unlock(&g_sg_mu);

    if (vms->len == 0) {   /* 바인딩 없음 → no-op */
        g_ptr_array_unref(vms);
        return;
    }

    /* 락 밖: 각 VM fresh 재해석, non-empty 만 캐시 덮어씀 (keep-old-on-empty) */
    for (guint i = 0; i < vms->len; i++) {
        const gchar *vm = g_ptr_array_index(vms, i);
        GPtrArray *ifaces = pcv_vm_iface_list(vm);   /* virsh, 항상 non-NULL(계약) */
        if (ifaces->len > 0)
            pcv_vm_vnet_cache_put(vm, ifaces);       /* 빈 결과면 캐시 그대로 (keep-old) */
        g_ptr_array_unref(ifaces);
    }
    g_ptr_array_unref(vms);

    GError *error = nullptr;
    if (!_rebuild_dispatch(&error)) {
        PCV_LOG_WARN("SG", "resync_all: dispatch 재생성 실패 (다음 주기 재시도): %s",
                     error ? error->message : "unknown");
        g_clear_error(&error);
    }
}

/* [I2-R1] 워커 스레드 — 블로킹 resync 실행 후 in-flight 플래그 리셋 */
static void
_sg_resync_worker(GTask *task, gpointer src, gpointer td, GCancellable *c)
{
    (void)src; (void)td; (void)c;
    pcv_security_group_resync_all();
    g_atomic_int_set(&g_sg_resync_inflight, 0);
    g_task_return_boolean(task, TRUE);
}

/* [I2-R1] 타이머 tick — 메인 루프, 논블로킹. 이전 resync 진행 중이면 skip. */
static gboolean
_sg_resync_tick(gpointer data)
{
    (void)data;
    if (!g_atomic_int_compare_and_exchange(&g_sg_resync_inflight, 0, 1))
        return G_SOURCE_CONTINUE;   /* 이전 resync 아직 진행 중 → 이번 tick skip */
    GTask *t = g_task_new(NULL, NULL, NULL, NULL);
    pcv_worker_pool_push(t, _sg_resync_worker);
    g_object_unref(t);   /* worker pool 이 자체 ref 를 잡음 (M-7 확인) */
    return G_SOURCE_CONTINUE;
}

void
pcv_security_group_resync_timer_init(void)
{
    gint interval = pcv_config_get_int("security_group", "resync_interval_sec", 300);
    if (interval <= 0) {
        PCV_LOG_INFO("SG", "vnet resync 타이머 비활성 (resync_interval_sec=%d)", interval);
        return;
    }
    g_sg_resync_timer_id = g_timeout_add_seconds((guint)interval, _sg_resync_tick, NULL);
    PCV_LOG_INFO("SG", "vnet resync 타이머 등록 (%d초 주기)", interval);
}

void
pcv_security_group_resync_timer_shutdown(void)
{
    if (g_sg_resync_timer_id) {
        g_source_remove(g_sg_resync_timer_id);
        g_sg_resync_timer_id = 0;
    }
}

gboolean
pcv_security_group_detach_vm(const gchar *vm, const gchar *sg_name)
{
    if (!vm || !sg_name) return FALSE;
    /* [M-9] apply_to_vm 과 대칭 — vm 은 롤백 재바인딩 경로에서 virsh argv 로
     * 흘러갈 수 있으므로 화이트리스트로 심층 방어 (정상 경로는 apply 검증 덕에
     * 무효 이름이 없으나 계약을 대칭으로 유지). */
    if (!pcv_validate_vm_name(vm)) {
        PCV_LOG_WARN("SG", "Rejected detach: invalid vm '%s'", vm);
        return FALSE;
    }

    g_mutex_lock(&g_sg_mu);
    _ensure_init();
    SecurityGroup *sg = g_hash_table_lookup(g_sg_map, sg_name);
    gboolean found = FALSE;
    if (sg) {
        for (guint i = 0; i < sg->vm_bindings->len; i++) {
            if (g_strcmp0(g_ptr_array_index(sg->vm_bindings, i), vm) == 0) {
                g_ptr_array_remove_index(sg->vm_bindings, i);
                found = TRUE;
                break;
            }
        }
    }
    g_mutex_unlock(&g_sg_mu);
    if (!found) return FALSE;

    GError *error = nullptr;
    if (!_rebuild_dispatch(&error)) {
        PCV_LOG_ERROR("SG", "detach '%s'←VM '%s' nft 반영 실패 — 롤백: %s",
                      sg_name, vm, error ? error->message : "unknown");
        g_clear_error(&error);
        g_mutex_lock(&g_sg_mu);
        SecurityGroup *sg2 = g_hash_table_lookup(g_sg_map, sg_name);
        if (sg2) {
            /* [M-2] apply_to_vm 과 대칭으로 멱등 재삽입 — nft 창 동안 동시 apply 가
             * 같은 vm 을 이미 재바인딩했다면 중복 바인딩(중복 jump)을 만들지 않는다. */
            gboolean dup = FALSE;
            for (guint i = 0; i < sg2->vm_bindings->len; i++) {
                if (g_strcmp0(g_ptr_array_index(sg2->vm_bindings, i), vm) == 0) {
                    dup = TRUE;
                    break;
                }
            }
            if (!dup) g_ptr_array_add(sg2->vm_bindings, g_strdup(vm));
        }
        g_mutex_unlock(&g_sg_mu);
        return FALSE;
    }
    _sg_db_delete_binding(sg_name, vm);
    /* [R3] 이 detach 로 vm 이 어떤 그룹에도 안 묶이면 dormant 캐시 엔트리 회수.
     * 동시 apply 가 재바인딩했으면 is_bound=TRUE → evict 안 함(정확). */
    if (!pcv_security_group_vm_is_bound(vm))
        pcv_vm_vnet_cache_evict(vm);
    PCV_LOG_INFO("SG", "Detached security group '%s' from VM '%s'", sg_name, vm);
    return TRUE;
}

/* sync 실패 = 해당 VM 이 SG 미적용 상태로 러닝 중 — 운영자에게 반드시 surface */
static void
_emit_sg_sync_failure_event(const gchar *vm, const GError *err)
{
    PcvSecurityEvent ev = {0};
    ev.timestamp   = g_get_real_time() / G_USEC_PER_SEC;
    ev.source      = PCV_SECURITY_SOURCE_PCV_AUDIT;
    ev.type        = PCV_SECURITY_EVENT_AUDIT_PATTERN;
    ev.severity    = PCV_SECURITY_SEVERITY_CRIT;
    ev.confidence  = 100;
    ev.target_kind = PCV_SECURITY_TARGET_VM;
    ev.status      = PCV_SECURITY_STATUS_OPEN;
    g_strlcpy(ev.target, vm, sizeof(ev.target));
    g_snprintf(ev.summary, sizeof(ev.summary),
               "security group dispatch sync failed for VM '%s' (SG 미적용 상태)", vm);
    g_strlcpy(ev.recommended_action, "sg-resync", sizeof(ev.recommended_action));
    JsonObject *ej = json_object_new();
    json_object_set_string_member(ej, "error",
        (err && err->message) ? err->message : "unknown");
    JsonNode *ejroot = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(ejroot, ej);
    gchar *ejstr = json_to_string(ejroot, FALSE);
    _sg_set_evidence(ev.evidence_json, sizeof ev.evidence_json, ejstr);
    g_free(ejstr);
    json_node_free(ejroot);
    pcv_security_event_make_id(&ev, "sg");
    GError *serr = nullptr;
    if (!pcv_security_store_insert_event(&ev, &serr)) {
        PCV_LOG_WARN("SG", "security_event 기록 실패: %s",
                     serr ? serr->message : "unknown");
        g_clear_error(&serr);
    }
}

/* g_sg_mu 를 이미 보유한 상태에서 vm 바인딩 여부 스캔 (내부 공용) */
static gboolean
_vm_is_bound_locked(const gchar *vm)
{
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, g_sg_map);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        SecurityGroup *sg = v;
        for (guint i = 0; i < sg->vm_bindings->len; i++)
            if (g_strcmp0(g_ptr_array_index(sg->vm_bindings, i), vm) == 0)
                return TRUE;
    }
    return FALSE;
}

gboolean
pcv_security_group_vm_is_bound(const gchar *vm)
{
    if (!vm) return FALSE;
    g_mutex_lock(&g_sg_mu);
    _ensure_init();
    gboolean bound = _vm_is_bound_locked(vm);
    g_mutex_unlock(&g_sg_mu);
    return bound;
}

void
pcv_security_group_sync_vm(const gchar *vm_name)
{
    if (!vm_name) return;

    /* 바인딩 없으면 디스패치 미등장 → no-op */
    g_mutex_lock(&g_sg_mu);
    _ensure_init();
    gboolean bound = _vm_is_bound_locked(vm_name);
    g_mutex_unlock(&g_sg_mu);
    if (!bound) return;

    /* I-2: 라이프사이클 변화(start/stop/restart)로 vnet 이 바뀌었을 수 있으므로
     * 캐시를 무효화한다. 이어지는 _rebuild_dispatch 의 lazy-miss 가 fresh 재해석·적재.
     * (evict 만: 여기서 virsh 를 부르지 않아 블로킹 최소화 — 해석은 rebuild 로 위임) */
    pcv_vm_vnet_cache_evict(vm_name);

    GError *error = nullptr;
    if (_rebuild_dispatch(&error)) return;
    g_clear_error(&error);
    if (_rebuild_dispatch(&error)) return;   /* 1회 재시도 (spec §5) */

    PCV_LOG_ERROR("SG", "sync_vm '%s' 실패 (재시도 포함) — SG 미적용 상태: %s",
                  vm_name, error ? error->message : "unknown");
    _emit_sg_sync_failure_event(vm_name, error);
    g_clear_error(&error);
}
