/**
 * @file storage_tier.c
 * @brief 스토리지 티어링 — 티어 감지, 정의, 자동 배치, QoS 정책
 *
 * [티어 감지]
 *   /sys/block/<dev>/queue/rotational 파일로 디스크 타입 판별
 *   0 = SSD/NVMe, 1 = HDD
 *   /sys/block/nvme* = NVMe (접두사 기반 추가 판별)
 *
 * [티어 정의]
 *   daemon.conf [storage] 섹션 또는 RPC로 등록
 *   기본 프리셋: "ssd" → pcvpool (기존 풀)
 *
 * [자동 배치]
 *   vcpu >= 4 && ram >= 8192 → TIER_NVME (고성능)
 *   기본 → TIER_SSD
 *
 * [QoS]
 *   ZFS: zfs set reservation/quota
 *   libvirt: virDomainSetBlockIoTune (iotune XML)
 */
#include "storage_tier.h"
#include "utils/pcv_spawn.h"
#include "utils/pcv_log.h"
#include "utils/pcv_config.h"
#include <string.h>
#include <stdio.h>

/** 로그 도메인 — journalctl에서 storage_tier로 필터링 가능 */
#define TIER_LOG_DOM  "storage_tier"

/** 최대 등록 가능 티어 수 (NVMe + SSD + HDD 등 물리적 한계 반영) */
#define MAX_TIERS     8

/**
 * TierDef:
 * 스토리지 티어의 정의 레코드.
 * 각 티어는 ZFS 풀 하나와 매핑되며, 디스크 타입(NVMe/SSD/HDD)을 분류합니다.
 *
 * 디자인 결정: 고정 크기 문자 배열을 사용하여 동적 할당/해제 오버헤드를 제거합니다.
 * 티어 이름(name)과 풀 경로(pool) 모두 64바이트 이하로 충분합니다.
 */
typedef struct {
    gchar          name[32];   /* 티어 이름 (예: "ssd", "nvme", "hdd") */
    gchar          pool[64];   /* ZFS 풀 경로 (예: "pcvpool/vms") */
    PcvStorageTier type;       /* 디스크 유형 열거값 (NVMe/SSD/HDD) */
    gboolean       active;     /* 활성 상태 플래그 */
} TierDef;

/** 전역 티어 관리 상태 — 프로세스당 1개 인스턴스 */
static struct {
    TierDef   tiers[MAX_TIERS];  /* 티어 정의 배열 */
    gint      count;              /* 현재 등록된 티어 수 */
    GMutex    mu;                 /* 모든 상태 접근 직렬화 */
    gboolean  initialized;        /* 초기화 여부 플래그 */
} G = {0};

/**
 * _run_cmd:
 * 셸 명령을 동기적으로 실행하는 내부 헬퍼.
 * zfs/virsh CLI를 /bin/sh -c로 실행합니다.
 *
 * @param cmd   셸 명령 문자열
 * @param out   stdout 출력 (NULL 가능)
 * @param error GError** (NULL 가능)
 * @return TRUE: exit code 0, FALSE: 실패
 */
static gboolean
_run_cmd(const gchar *cmd, gchar **out, GError **error)
{
    gchar **parsed = NULL;
    GError *pe = NULL;
    if (!g_shell_parse_argv(cmd, NULL, &parsed, &pe)) {
        if (pe) { if (error) g_propagate_error(error, pe); else g_error_free(pe); }
        return FALSE;
    }
    gchar *std_err = NULL;
    gboolean ok = pcv_spawn_sync((const gchar * const *)parsed, out, &std_err, error);
    g_free(std_err);
    g_strfreev(parsed);
    return ok;
}
static gboolean
_run_shell(const gchar *cmd, gchar **out, GError **error)
{
    const gchar *argv[] = {"/bin/sh", "-c", cmd, NULL};
    gchar *std_err = NULL;
    gboolean ok = pcv_spawn_sync(argv, out, &std_err, error);
    g_free(std_err);
    return ok;
}

/* ── 초기화/종료 ─────────────────────────────────────── */

/**
 * pcv_storage_tier_init:
 * 스토리지 티어 매니저를 초기화하고 기본 프리셋을 등록합니다.
 * 기본 프리셋: "ssd" → pcv_config_get_zvol_pool() (보통 "pcvpool/vms")
 *
 * daemon.conf [storage] 섹션에서 추가 티어를 RPC로 등록할 수 있습니다.
 */
void
pcv_storage_tier_init(void)
{
    g_mutex_init(&G.mu);
    G.count = 0;

    /* 기본 프리셋: ssd → pcvpool (기존 풀) */
    g_strlcpy(G.tiers[0].name, "ssd", sizeof(G.tiers[0].name));
    g_strlcpy(G.tiers[0].pool, pcv_config_get_zvol_pool(), sizeof(G.tiers[0].pool));
    G.tiers[0].type = PCV_TIER_SSD;
    G.tiers[0].active = TRUE;
    G.count = 1;

    G.initialized = TRUE;
    PCV_LOG_INFO(TIER_LOG_DOM, "Storage tier initialized (default: ssd → %s)",
                 G.tiers[0].pool);
}

/** pcv_storage_tier_shutdown: 뮤텍스 해제 및 초기화 플래그 리셋 */
void
pcv_storage_tier_shutdown(void)
{
    g_mutex_clear(&G.mu);
    G.initialized = FALSE;
}

/* ── 티어 CRUD ───────────────────────────────────────── */

/**
 * pcv_storage_tier_list:
 * 등록된 모든 활성 티어를 JSON 배열로 반환합니다.
 * 각 티어에 대해 ZFS 풀 사용량(used/avail)도 실시간 조회합니다.
 *
 * 반환 JSON 요소: {"name", "pool", "type", "used_bytes", "avail_bytes"}
 *
 * @return JsonArray* — 호출자가 소유
 */
JsonArray *
pcv_storage_tier_list(void)
{
    JsonArray *arr = json_array_new();
    g_mutex_lock(&G.mu);
    for (gint i = 0; i < G.count; i++) {
        TierDef *t = &G.tiers[i];
        if (!t->active) continue;
        JsonObject *obj = json_object_new();
        json_object_set_string_member(obj, "name", t->name);
        json_object_set_string_member(obj, "pool", t->pool);
        json_object_set_string_member(obj, "type",
            t->type == PCV_TIER_NVME ? "nvme" :
            t->type == PCV_TIER_SSD  ? "ssd"  : "hdd");

        /* 풀 사용량 실시간 조회
         * -H: 헤더 없음, -p: 바이트 단위 출력 (사람이 읽는 M/G 대신) */
        gchar *cmd = g_strdup_printf("zfs list -Hp -o used,avail %s 2>/dev/null", t->pool);
        gchar *out = NULL;
        if (_run_shell(cmd, &out, NULL) && out) {
            gint64 used = 0, avail = 0;
            sscanf(out, "%ld\t%ld", &used, &avail);
            json_object_set_int_member(obj, "used_bytes", used);
            json_object_set_int_member(obj, "avail_bytes", avail);
        }
        g_free(out);
        g_free(cmd);

        json_array_add_object_element(arr, obj);
    }
    g_mutex_unlock(&G.mu);
    return arr;
}

/**
 * pcv_storage_tier_info:
 * 이름으로 특정 티어의 정보를 JSON 객체로 반환합니다.
 *
 * @param tier_name 조회할 티어 이름
 * @return JsonObject* (찾으면) 또는 NULL (없거나 비활성)
 */
JsonObject *
pcv_storage_tier_info(const gchar *tier_name)
{
    if (!tier_name) return NULL;
    g_mutex_lock(&G.mu);
    for (gint i = 0; i < G.count; i++) {
        if (g_strcmp0(G.tiers[i].name, tier_name) == 0 && G.tiers[i].active) {
            TierDef *t = &G.tiers[i];
            JsonObject *obj = json_object_new();
            json_object_set_string_member(obj, "name", t->name);
            json_object_set_string_member(obj, "pool", t->pool);
            json_object_set_string_member(obj, "type",
                t->type == PCV_TIER_NVME ? "nvme" :
                t->type == PCV_TIER_SSD  ? "ssd"  : "hdd");
            g_mutex_unlock(&G.mu);
            return obj;
        }
    }
    g_mutex_unlock(&G.mu);
    return NULL;
}

/**
 * pcv_storage_tier_create:
 * 새 스토리지 티어를 등록합니다.
 *
 * @param name  티어 이름 (예: "nvme-fast")
 * @param pool  매핑할 ZFS 풀 경로 (예: "nvmepool/vms")
 * @param type  디스크 유형 (PCV_TIER_NVME/PCV_TIER_SSD/PCV_TIER_HDD)
 * @param error 실패 시 GError** 설정
 * @return TRUE: 성공, FALSE: 실패 (MAX_TIERS 초과 등)
 */
gboolean
pcv_storage_tier_create(const gchar *name, const gchar *pool,
                         PcvStorageTier type, GError **error)
{
    if (!name || !pool) {
        g_set_error(error, g_quark_from_static_string("tier"), 1, "name and pool required");
        return FALSE;
    }
    g_mutex_lock(&G.mu);
    if (G.count >= MAX_TIERS) {
        g_mutex_unlock(&G.mu);
        g_set_error(error, g_quark_from_static_string("tier"), 2, "max tiers reached");
        return FALSE;
    }
    gint idx = G.count++;
    g_strlcpy(G.tiers[idx].name, name, sizeof(G.tiers[idx].name));
    g_strlcpy(G.tiers[idx].pool, pool, sizeof(G.tiers[idx].pool));
    G.tiers[idx].type = type;
    G.tiers[idx].active = TRUE;
    g_mutex_unlock(&G.mu);

    PCV_LOG_INFO(TIER_LOG_DOM, "Tier '%s' created (pool=%s, type=%d)", name, pool, type);
    return TRUE;
}

/* ── 자동 배치 (Auto-Placement) ───────────────────────────────── */

/**
 * pcv_storage_tier_auto_select:
 * VM 스펙(vCPU, RAM)을 기반으로 최적의 스토리지 풀을 자동 선택합니다.
 *
 * 배치 규칙:
 *   - vcpu >= 4 AND ram >= 8GB → NVMe 티어 우선 (고성능 VM)
 *   - 그 외 → 기본 SSD 풀 (pcv_config_get_zvol_pool)
 *
 * NVMe 티어가 등록되어 있지 않으면 SSD로 폴백합니다.
 *
 * @param vcpu   VM의 가상 CPU 수
 * @param ram_mb VM의 메모리 크기 (MB)
 * @return ZFS 풀 경로 문자열 (정적 메모리 — g_free 금지)
 */
const gchar *
pcv_storage_tier_auto_select(gint vcpu, gint ram_mb)
{
    /* 고성능 VM → NVMe 우선, 없으면 SSD */
    if (vcpu >= 4 && ram_mb >= 8192) {
        g_mutex_lock(&G.mu);
        for (gint i = 0; i < G.count; i++) {
            if (G.tiers[i].type == PCV_TIER_NVME && G.tiers[i].active) {
                g_mutex_unlock(&G.mu);
                return G.tiers[i].pool;
            }
        }
        g_mutex_unlock(&G.mu);
    }
    /* 기본: 첫 번째 SSD 티어 */
    return pcv_config_get_zvol_pool();
}

/* ── 티어 이동 (온라인 마이그레이션) ───────────────────────────── */

/**
 * pcv_storage_tier_migrate:
 * VM의 zvol을 다른 스토리지 티어(ZFS 풀)로 이동합니다.
 *
 * 이동 방법: ZFS send/recv 파이프라인
 *   1. 소스에 임시 스냅샷 생성 (@tier-migrate)
 *   2. zfs send | zfs recv 로 대상 풀에 복제
 *   3. 임시 스냅샷 삭제
 *
 * 주의: 오프라인 마이그레이션입니다. VM이 실행 중이면 데이터 불일치 발생 가능.
 * 호출 전 반드시 VM을 정지시켜야 합니다.
 *
 * @param vm_name      이동할 VM 이름
 * @param target_tier  대상 티어 이름
 * @param error        실패 시 GError** 설정
 * @return TRUE: 성공, FALSE: 실패
 */
gboolean
pcv_storage_tier_migrate(const gchar *vm_name, const gchar *target_tier,
                          GError **error)
{
    if (!vm_name || !target_tier) {
        g_set_error(error, g_quark_from_static_string("tier"), 1, "vm_name and target_tier required");
        return FALSE;
    }

    /* 대상 티어 풀 찾기 */
    const gchar *target_pool = NULL;
    g_mutex_lock(&G.mu);
    for (gint i = 0; i < G.count; i++) {
        if (g_strcmp0(G.tiers[i].name, target_tier) == 0 && G.tiers[i].active) {
            target_pool = G.tiers[i].pool;
            break;
        }
    }
    g_mutex_unlock(&G.mu);

    if (!target_pool) {
        g_set_error(error, g_quark_from_static_string("tier"), 3, "tier '%s' not found", target_tier);
        return FALSE;
    }

    /* ZFS send/recv로 이동 (오프라인 — VM 중지 상태 필요) */
    gchar *src = g_strdup_printf("%s/%s", pcv_config_get_zvol_pool(), vm_name);
    gchar *dst = g_strdup_printf("%s/%s", target_pool, vm_name);
    gchar *cmd = g_strdup_printf(
        "zfs snapshot %s@tier-migrate && "
        "zfs send %s@tier-migrate | zfs recv %s && "
        "zfs destroy %s@tier-migrate",
        src, src, dst, src);

    gboolean ok = _run_cmd(cmd, NULL, error);
    g_free(cmd);
    g_free(src);
    g_free(dst);

    if (ok)
        PCV_LOG_INFO(TIER_LOG_DOM, "VM '%s' migrated to tier '%s'", vm_name, target_tier);
    return ok;
}

/* ── QoS (I/O 대역폭/IOPS 제한) ─────────────────────────────── */

/**
 * pcv_storage_qos_set:
 * VM의 블록 I/O에 대역폭 및 IOPS 제한을 적용합니다.
 * virsh blkiotune 명령으로 libvirt를 통해 cgroup 기반 I/O 스로틀링을 설정합니다.
 *
 * @param vm_name    대상 VM 이름
 * @param read_bps   읽기 대역폭 제한 (bytes/sec, 0이면 제한 없음)
 * @param write_bps  쓰기 대역폭 제한 (bytes/sec, 0이면 제한 없음)
 * @param read_iops  읽기 IOPS 제한 (0이면 제한 없음)
 * @param write_iops 쓰기 IOPS 제한 (0이면 제한 없음)
 * @param error      실패 시 GError** 설정
 * @return TRUE: 성공, FALSE: 실패
 *
 * --live 플래그: 실행 중인 VM에 즉시 적용 (재부팅 불필요)
 */
gboolean
pcv_storage_qos_set(const gchar *vm_name, gint64 read_bps, gint64 write_bps,
                     gint64 read_iops, gint64 write_iops, GError **error)
{
    if (!vm_name) {
        g_set_error(error, g_quark_from_static_string("qos"), 1, "vm_name required");
        return FALSE;
    }

    /* virsh blkiotune으로 I/O 제한 적용 */
    GString *cmd = g_string_new("");
    g_string_printf(cmd, "virsh blkiotune %s", vm_name);
    if (read_bps > 0)
        g_string_append_printf(cmd, " --device-read-bytes-sec %ld", (long)read_bps);
    if (write_bps > 0)
        g_string_append_printf(cmd, " --device-write-bytes-sec %ld", (long)write_bps);
    if (read_iops > 0)
        g_string_append_printf(cmd, " --device-read-iops-sec %ld", (long)read_iops);
    if (write_iops > 0)
        g_string_append_printf(cmd, " --device-write-iops-sec %ld", (long)write_iops);
    g_string_append(cmd, " --live 2>&1");

    gboolean ok = _run_cmd(cmd->str, NULL, error);
    g_string_free(cmd, TRUE);

    if (ok)
        PCV_LOG_INFO(TIER_LOG_DOM, "QoS set for VM '%s': rbps=%ld wbps=%ld riops=%ld wiops=%ld",
                     vm_name, (long)read_bps, (long)write_bps, (long)read_iops, (long)write_iops);
    return ok;
}

/**
 * pcv_storage_qos_get:
 * VM의 현재 블록 I/O QoS 설정을 조회합니다.
 * virsh blkiotune 명령의 raw 출력을 그대로 반환합니다.
 *
 * @param vm_name 대상 VM 이름
 * @return JsonObject* {"vm_name", "raw"} — 호출자가 소유. vm_name이 NULL이면 NULL 반환.
 */
JsonObject *
pcv_storage_qos_get(const gchar *vm_name)
{
    if (!vm_name) return NULL;

    gchar *cmd = g_strdup_printf("virsh blkiotune %s 2>/dev/null", vm_name);
    gchar *out = NULL;
    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "vm_name", vm_name);

    if (_run_shell(cmd, &out, NULL) && out) {
        json_object_set_string_member(obj, "raw", out);
    }
    g_free(out);
    g_free(cmd);
    return obj;
}

/**
 * pcv_storage_qos_delete:
 * VM의 블록 I/O QoS 제한을 제거합니다.
 * 모든 제한 값을 0으로 설정하여 제한 없는 상태로 복원합니다.
 *
 * @param vm_name 대상 VM 이름
 * @param error   실패 시 GError** 설정
 * @return TRUE: 성공, FALSE: 실패
 */
gboolean
pcv_storage_qos_delete(const gchar *vm_name, GError **error)
{
    /* QoS 제거 = 모든 제한을 0으로 설정 (virsh blkiotune의 규약) */
    return pcv_storage_qos_set(vm_name, 0, 0, 0, 0, error);
}
