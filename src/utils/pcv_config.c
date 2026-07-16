/**
 * @file pcv_config.c
 * @brief GKeyFile 기반 설정 시스템 — 환경변수 > daemon.conf > 컴파일 기본값
 *
 * PureCVisor 데몬의 모든 설정을 중앙 관리하는 모듈입니다.
 * Sprint D-2에서 도입되었고, Sprint E(REST), Phase T-2(클러스터)에서 확장되었습니다.
 *
 * [아키텍처 위치]
 *   main.c → pcv_config_init() (pcv_log_init 직후, 다른 모든 초기화보다 앞서 호출)
 *          → 각 모듈이 pcv_config_get_*() 로 값 조회
 *          → pcv_config_shutdown() (프로그램 종료 시 최말미 호출)
 *
 * [설정 우선순위 (높은 순)]
 *   1. 환경변수    : PURECVISOR_SOCKET_PATH, PURECVISOR_LIBVIRT_URI, ...
 *   2. 설정 파일   : /etc/purecvisor/daemon.conf (GKeyFile INI 형식)
 *   3. 컴파일 기본값: PCV_DEFAULT_* 상수 (pcv_config.h)
 *
 *   이 3단계 우선순위 체계는 12-Factor App 원칙에 따른 것으로,
 *   개발(기본값) → 스테이징(설정 파일) → 프로덕션(환경변수) 순으로
 *   설정을 오버라이드할 수 있게 합니다.
 *
 * [설정 파일 섹션 검색 전략]
 *   _cfg_str() / _cfg_int() 내부에서:
 *     1단계: [daemon] 섹션에서 키 검색
 *     2단계: [daemon]에 없으면 전체 섹션을 순회하며 fallback 검색
 *   이 전략은 실전 배포 중 [iscsi]에 cluster 키를 잘못 넣은 사례에서 도입됨.
 *   운영자가 섹션을 혼동해도 설정이 무시되지 않도록 하는 방어적 설계입니다.
 *
 * [제네릭 getter (Phase T-2)]
 *   pcv_config_get_string(section, key, default): 임의 섹션/키 조회
 *   pcv_config_get_int(section, key, default):    임의 섹션/키 정수 조회
 *   etcd, 클러스터, 스토리지, 오버레이 등 다양한 섹션을 각 모듈에서 직접 조회.
 *   반환된 문자열은 GKeyFile 수명 동안 유효 (프로세스 전체 수명).
 *
 * [샘플 설정 자동 생성]
 *   설정 파일이 없으면 기본값으로 /etc/purecvisor/daemon.conf를 자동 생성합니다.
 *   운영자가 처음 설치 시 참고할 수 있는 템플릿 역할.
 *
 * [다른 모듈과의 관계]
 *   - pcv_log.c       : 로그 레벨 설정값 조회 (log_level)
 *   - pcv_jwt.c       : JWT 시크릿 키 조회 (jwt_secret)
 *   - pcv_tls.c       : TLS 인증서 경로 조회 ([tls] 섹션)
 *   - rest_server.c   : REST 포트, 관리자 인증정보 조회
 *   - virt_conn_pool.c: libvirt URI, 풀 최대 연결 수 조회
 *   - drain.c         : 드레인 타임아웃 조회
 *   - etcd_client.c   : etcd 엔드포인트 조회 ([cluster] 섹션)
 *   - zfs_driver.c    : ZFS 풀 경로 조회 ([storage] 섹션)
 *   - lxc_driver.c    : 컨테이너 풀 경로 조회 ([storage] 섹션)
 *
 * [주의사항]
 *   - g_cfg는 전역 싱글턴 (프로세스당 1개): 멀티스레드에서 읽기만 수행
 *   - pcv_config_get_string()의 반환 포인터: 내부 캐시 소유 (shutdown까지 유효)
 *   - pcv_config_init() 이전에 getter를 호출하면 기본값이 반환됨
 *   - 설정 파일 파싱 실패는 non-fatal: 기본값으로 fallback하여 데몬은 항상 시작
 */

#include "pcv_config.h"
#include "pcv_log.h"
#include "pcv_jwt.h"

#include <glib.h>
#include <string.h>

/**
 * CFG_LOG_DOM - 이 모듈의 로그 도메인 식별자
 *
 * PCV_LOG_INFO(CFG_LOG_DOM, "...") 형태로 사용됩니다.
 * journalctl에서 "dom":"pcv_config"로 필터링하여 설정 관련 로그만 볼 수 있습니다.
 */
#define CFG_LOG_DOM  "pcv_config"

/**
 * CFG_GROUP - GKeyFile에서 우선 검색할 기본 섹션명
 *
 * daemon.conf 파일의 [daemon] 섹션이 기본 검색 대상입니다.
 * 이 섹션에서 키를 찾지 못하면 전체 섹션을 순회하여 fallback 검색합니다.
 *
 * 예시 (daemon.conf):
 *   [daemon]           ← CFG_GROUP = "daemon" (우선 검색)
 *   socket_path = /var/run/purecvisor/daemon.sock
 *
 *   [cluster]          ← fallback 검색 대상
 *   ssh_user = pcvdev
 */
#define CFG_GROUP    "daemon"

/* ── 내부 상태 ────────────────────────────────────────── */

/**
 * PcvConfig - 설정 값 보관 구조체 (전역 싱글턴)
 *
 * pcv_config_init()에서 한 번 채워진 후 프로세스 수명 동안 불변(immutable)입니다.
 * 모든 필드는 main 스레드에서 초기화되고, 이후 워커 스레드에서 읽기만 합니다.
 * 따라서 별도의 뮤텍스 없이 스레드 안전합니다.
 *
 * [메모리 관리]
 *   gchar* 필드: g_strdup()으로 할당 → pcv_config_shutdown()에서 g_free()
 *   kf (GKeyFile*): 제네릭 getter용으로 보존 → shutdown에서 해제
 */
typedef struct {
    gchar   *socket_path;    /* UDS 소켓 경로 (기본: /var/run/purecvisor/daemon.sock) */
    gchar   *libvirt_uri;    /* libvirt 연결 URI (기본: qemu:///system) */
    gint     pool_max_conn;  /* 커넥션 풀 최대 크기 (기본: 8, 범위: 1-64) */
    gint     drain_timeout;  /* 그레이스풀 드레인 타임아웃 초 (기본: 30, 최소: 5) */
    gchar   *db_path;        /* SQLite DB 경로 (기본: /var/lib/purecvisor/vm_state.db) */
    gchar   *log_level;      /* 로그 레벨 문자열 (기본: "info") */
    /* Sprint E: REST API 관련 설정 */
    gint     rest_port;      /* REST API 수신 포트 (기본: 80, 범위: 1-65535) */
    gchar   *admin_user;     /* REST API 관리자 사용자명 (기본: "admin") */
    gchar   *admin_password; /* REST API 관리자 비밀번호 (기본값 없음, 설정 필요) */
    gchar   *jwt_secret;     /* JWT HMAC 서명 키 (빈 문자열이면 랜덤 생성) */
    GKeyFile *kf;            /* Phase T-2: 설정 파일 핸들 유지 (제네릭 getter용) */
    GHashTable *string_cache; /* section/key → gchar* 제네릭 문자열 getter 캐시 */
    GPtrArray *retired_string_caches; /* reload 전 반환 포인터 수명 보존용 캐시 묶음 */
    gboolean initialized;    /* pcv_config_init() 호출 완료 여부 플래그 */
    GRWLock  kf_lock;        /* SIGHUP reload 시 kf 교체와 동시 읽기 보호 (RWLock) */
} PcvConfig;

/**
 * g_cfg - 전역 설정 싱글턴 인스턴스
 *
 * 프로세스당 정확히 1개만 존재합니다.
 * { 0 } 초기화로 모든 포인터가 NULL, 정수가 0으로 시작합니다.
 * pcv_config_init() 이전에 getter를 호출하면 initialized=FALSE이므로
 * 각 getter의 삼항 연산자에 의해 PCV_DEFAULT_* 기본값이 반환됩니다.
 */
static PcvConfig g_cfg = { 0 };

/* ── 내부 유틸리티 ────────────────────────────────────── */

/**
 * _cfg_str - 3단계 우선순위로 문자열 설정값을 조회하는 내부 헬퍼
 * @kf:          GKeyFile 핸들 (NULL이면 설정 파일 미로드 상태)
 * @key:         설정 키 이름 (예: "socket_path", "libvirt_uri")
 * @env_var:     대응하는 환경변수 이름 (예: "PURECVISOR_SOCKET_PATH")
 * @default_val: 환경변수와 설정 파일 모두에 없을 때 사용할 기본값
 *
 * @return: g_strdup()으로 할당된 문자열 — 호출자가 반드시 g_free()로 해제
 *
 * [검색 순서]
 *   1. 환경변수 (빈 문자열이 아닌 경우만)
 *   2. 설정 파일 [daemon] 섹션
 *   3. 설정 파일 전체 섹션 순회 (fallback)
 *   4. 컴파일 기본값 (default_val)
 *
 * [설계 결정: 전체 섹션 fallback 검색]
 *   실전 배포에서 운영자가 peer_rest_ips를 [iscsi] 섹션에 넣었을 때
 *   설정이 무시되는 문제가 있었습니다. 이를 방지하기 위해
 *   [daemon] 섹션에 없는 키를 다른 모든 섹션에서 재검색합니다.
 *   단, [daemon] 섹션의 값이 항상 우선합니다.
 */
static gchar *
_cfg_str(GKeyFile   *kf,
         const gchar *key,
         const gchar *env_var,
         const gchar *default_val)
{
    /* 1. 환경변수 최우선 — 12-Factor App의 핵심 원칙.
     * 왜 환경변수가 최우선인가:
     * - Docker/K8s: 설정 파일을 마운트하지 않고 env로 주입 가능
     * - CI 테스트: PCV_CONFIG_PATH로 격리된 설정 파일 사용
     * - 보안: 비밀값(jwt_secret)을 파일에 남기지 않고 env로 전달
     * - 빈 문자열("")은 의도적 설정이 아니라 미설정으로 간주하여 무시 */
    const gchar *env = g_getenv(env_var);
    if (env && env[0] != '\0')
        return g_strdup(env);

    /* 2. 설정 파일 — [daemon] 섹션 우선, 없으면 전체 섹션 검색 */
    if (kf) {
        GError *err = NULL;

        /* 2-1. [daemon] 섹션에서 키 검색 */
        gchar *val = g_key_file_get_string(kf, CFG_GROUP, key, &err);
        if (val) {
            g_strstrip(val);  /* 앞뒤 공백 제거 (운영자 편의) */
            return val;
        }
        if (err) { g_error_free(err); err = NULL; }

        /* 2-2. fallback: 모든 섹션에서 키 검색 (운영자 실수 방어) */
        gchar **groups = g_key_file_get_groups(kf, NULL);
        if (groups) {
            for (gchar **g = groups; *g; g++) {
                /* [daemon]은 이미 검색했으므로 건너뜀 */
                if (g_strcmp0(*g, CFG_GROUP) == 0) continue;
                val = g_key_file_get_string(kf, *g, key, &err);
                if (val) {
                    g_strstrip(val);
                    g_strfreev(groups);
                    return val;
                }
                if (err) { g_error_free(err); err = NULL; }
            }
            g_strfreev(groups);
        }
    }

    /* 3. 기본값 — pcv_config.h의 PCV_DEFAULT_* 상수 */
    return g_strdup(default_val);
}

/**
 * _cfg_int - 3단계 우선순위로 정수 설정값을 조회하는 내부 헬퍼
 * @kf:          GKeyFile 핸들 (NULL이면 설정 파일 미로드 상태)
 * @key:         설정 키 이름
 * @env_var:     대응하는 환경변수 이름
 * @default_val: 기본값 (환경변수/설정 파일 모두 없을 때)
 *
 * @return: 정수 설정값
 *
 * [환경변수 파싱 규칙]
 *   - strtol()로 10진수 파싱
 *   - 파싱 실패(빈 문자열, 비숫자 포함) 시 환경변수 무시
 *   - 0 이하 값도 무시 (양수만 허용)
 *   - end != env: 최소 1자 이상 파싱 성공
 *   - *end == '\0': 문자열 끝까지 모두 숫자
 *
 * [설정 파일 검색]
 *   _cfg_str()과 동일한 [daemon] → 전체 섹션 fallback 전략을 사용합니다.
 */
static gint
_cfg_int(GKeyFile   *kf,
         const gchar *key,
         const gchar *env_var,
         gint         default_val)
{
    /* 1. 환경변수 최우선 */
    const gchar *env = g_getenv(env_var);
    if (env && env[0] != '\0') {
        gchar *end = NULL;
        glong v = strtol(env, &end, 10);
        /* 파싱 유효성: 최소 1자 파싱 + 문자열 끝 + 양수 */
        if (end != env && *end == '\0' && v > 0)
            return (gint)v;
    }

    /* 2. 설정 파일 — [daemon] 섹션 우선 + fallback */
    if (kf) {
        GError *err = NULL;

        /* 2-1. [daemon] 섹션에서 키 검색 */
        gint val = g_key_file_get_integer(kf, CFG_GROUP, key, &err);
        if (!err) return val;
        g_error_free(err); err = NULL;

        /* 2-2. fallback: 모든 섹션에서 키 검색 */
        gchar **groups = g_key_file_get_groups(kf, NULL);
        if (groups) {
            for (gchar **g = groups; *g; g++) {
                if (g_strcmp0(*g, CFG_GROUP) == 0) continue;
                val = g_key_file_get_integer(kf, *g, key, &err);
                if (!err) { g_strfreev(groups); return val; }
                g_error_free(err); err = NULL;
            }
            g_strfreev(groups);
        }
    }

    /* 3. 기본값 */
    return default_val;
}

/* ── 공개 API ─────────────────────────────────────────── */

/**
 * pcv_config_init - 설정 시스템 초기화 (프로세스당 1회 호출)
 *
 * [호출 시점]
 *   main.c에서 pcv_log_init() 직후, 다른 모든 초기화보다 앞서 호출.
 *   이 함수 이후에 pcv_jwt_init(), pcv_tls_init_from_config() 등이
 *   설정값을 조회하므로 반드시 먼저 호출되어야 합니다.
 *
 * [동작 순서]
 *   1. /etc/purecvisor/daemon.conf 파일 로드 시도 (GKeyFile INI 파서)
 *   2. 각 설정 항목을 환경변수 > 설정파일 > 기본값 순으로 해석
 *   3. 해석된 값을 전역 g_cfg 싱글턴에 저장
 *   4. 설정 값 범위 검증 (포트 범위, 타임아웃 최소값, 풀 크기 범위)
 *   5. 소켓/DB 디렉터리 자동 생성
 *   6. pcv_config_dump()로 현재 설정을 로그 출력
 *   7. 설정 파일이 없으면 샘플 파일 자동 생성 (운영자 가이드)
 *
 * [에러 처리]
 *   설정 파일 파싱 실패 시 경고 로그 후 기본값으로 동작 (non-fatal).
 *   이 함수는 실패하지 않음 — 항상 기본값으로 fallback 가능.
 *   운영자가 잘못된 설정을 넣어도 데몬이 시작되어야 하기 때문입니다.
 *
 * [설정 값 범위 검증 규칙]
 *   - rest_port: 1~65535 범위. 벗어나면 기본 포트(80)로 복구
 *   - drain_timeout: 최소 5초. 이보다 짧으면 inflight RPC가 완료되기 전에 종료될 수 있음
 *   - pool_max_conn: 1~64 범위. 너무 크면 libvirt 리소스 소모, 너무 작으면 병목
 */
void
pcv_config_init(void)
{
    if (g_cfg.initialized)
        pcv_config_shutdown();

    g_rw_lock_init(&g_cfg.kf_lock);
    g_cfg.string_cache = g_hash_table_new_full(g_str_hash, g_str_equal,
                                               g_free, g_free);
    g_cfg.retired_string_caches =
        g_ptr_array_new_with_free_func((GDestroyNotify)g_hash_table_destroy);

    /* GKeyFile: GLib의 INI 파일 파서. [섹션]\nkey=value 형식을 읽습니다.
     * 테스트 격리: PCV_CONFIG_PATH 환경변수로 경로 오버라이드 가능. */
    const gchar *cfg_env = g_getenv("PCV_CONFIG_PATH");
    const gchar *cfg_path = (cfg_env && *cfg_env) ? cfg_env : PCV_CONFIG_FILE_PATH;
    GKeyFile *kf   = g_key_file_new();
    GError   *err  = NULL;
    gboolean  loaded = g_key_file_load_from_file(
        kf, cfg_path,
        G_KEY_FILE_NONE, &err);

    /* [왜 파싱 실패가 non-fatal인가]
     * 설정 파일 문제로 데몬이 시작되지 않으면 운영자가 SSH로 접속하여
     * 수동으로 수정해야 한다. 원격 서버에서는 이것이 불가능할 수 있으므로
     * 기본값으로라도 시작하여 REST API를 통한 원격 진단을 가능하게 한다. */
    if (!loaded) {
        if (err->code != G_FILE_ERROR_NOENT) {
            /* 파일은 있지만 파싱 실패 (구문 오류 등) → 경고 후 기본값 사용 */
            PCV_LOG_WARN(CFG_LOG_DOM,
                         "Failed to parse config file '%s': %s — using defaults",
                         cfg_path, err->message);
        } else {
            /* 파일 자체가 없음 → 첫 설치 시 정상적인 상황 */
            PCV_LOG_INFO(CFG_LOG_DOM,
                         "Config file not found ('%s') — using defaults",
                         cfg_path);
        }
        g_error_free(err);
        g_key_file_free(kf);
        kf = NULL;   /* NULL이면 _cfg_str/_cfg_int에서 설정 파일 검색 건너뜀 */
    }

    /*
     * 각 항목 해석: 환경변수 > 설정파일 > 기본값 순으로 결정
     *
     * [명명 규칙]
     *   설정 키:   "socket_path"              (INI 파일 내)
     *   환경변수:  "PURECVISOR_SOCKET_PATH"    (접두사 PURECVISOR_)
     *   기본값:    PCV_DEFAULT_SOCKET_PATH     (pcv_config.h)
     *   getter:    pcv_config_get_socket_path() (공개 API)
     */
    g_cfg.socket_path   = _cfg_str(kf, "socket_path",   "PURECVISOR_SOCKET_PATH",
                                   PCV_DEFAULT_SOCKET_PATH);
    g_cfg.libvirt_uri   = _cfg_str(kf, "libvirt_uri",   "PURECVISOR_LIBVIRT_URI",
                                   PCV_DEFAULT_LIBVIRT_URI);
    g_cfg.db_path       = _cfg_str(kf, "db_path",       "PURECVISOR_DB_PATH",
                                   PCV_DEFAULT_DB_PATH);
    g_cfg.log_level     = _cfg_str(kf, "log_level",     "PURECVISOR_LOG_LEVEL",
                                   PCV_DEFAULT_LOG_LEVEL);
    g_cfg.pool_max_conn = _cfg_int(kf, "pool_max_conn", "PURECVISOR_POOL_MAX_CONN",
                                   PCV_DEFAULT_POOL_MAX_CONN);
    g_cfg.drain_timeout = _cfg_int(kf, "drain_timeout", "PURECVISOR_DRAIN_TIMEOUT",
                                   PCV_DEFAULT_DRAIN_TIMEOUT);
    /* Sprint E: REST API 설정 항목 추가 */
    g_cfg.rest_port      = _cfg_int(kf, "rest_port",       "PURECVISOR_REST_PORT",
                                    PCV_DEFAULT_REST_PORT);
    g_cfg.admin_user     = _cfg_str(kf, "admin_user",      "PURECVISOR_ADMIN_USER",
                                    PCV_DEFAULT_ADMIN_USER);
    g_cfg.admin_password = _cfg_str(kf, "admin_password",  "PURECVISOR_ADMIN_PASSWORD",
                                    PCV_DEFAULT_ADMIN_PASSWORD);
    g_cfg.jwt_secret     = _cfg_str(kf, "jwt_secret",      "PURECVISOR_JWT_SECRET",
                                    PCV_DEFAULT_JWT_SECRET);

    /*
     * Phase T-2: GKeyFile 핸들을 해제하지 않고 보존합니다.
     *
     * [이유]
     *   pcv_config_get_string("cluster", "etcd_endpoints", "http://...") 같은
     *   제네릭 getter가 임의 섹션/키를 조회할 수 있어야 합니다.
     *   이 핸들은 프로세스 수명 동안 유지되며, pcv_config_shutdown()에서 해제됩니다.
     */
    g_cfg.kf = kf;  /* Phase T-2: 제네릭 getter를 위해 보존 */

    /* ── 설정 값 범위 검증 (P4-2 fix) ──────────────────────────────
     *
     * [왜 초기화 시점에 검증하는가]
     * 검증 없이 잘못된 값을 그대로 전달하면:
     *   - rest_port=0 → bind() 실패 → 데몬 크래시 (REST 서비스 불가)
     *   - drain_timeout=1 → inflight RPC 도중 소켓 강제 종료 → 클라이언트 에러
     *   - pool_max_conn=1000 → libvirt fd 고갈 → 전체 VM 관리 불가
     *
     * "빠르게 실패하고 기본값으로 복구" 전략으로 데몬 가용성을 보장.
     * 운영자에게 WARN 로그로 알려 수정을 유도한다.
     */

    /* REST 포트: 1~65535 (TCP 포트 범위). 80/443은 CAP_NET_BIND_SERVICE 필요 */
    if (g_cfg.rest_port < 1 || g_cfg.rest_port > 65535) {
        PCV_LOG_WARN(CFG_LOG_DOM, "rest_port=%d out of range [1-65535] — using default %d",
                     g_cfg.rest_port, PCV_DEFAULT_REST_PORT);
        g_cfg.rest_port = PCV_DEFAULT_REST_PORT;
    }

    /* 드레인 타임아웃: 최소 5초. inflight RPC가 완료되기까지 필요한 최소 시간 */
    if (g_cfg.drain_timeout < 5) {
        PCV_LOG_WARN(CFG_LOG_DOM, "drain_timeout=%d too low — using minimum 5",
                     g_cfg.drain_timeout);
        g_cfg.drain_timeout = 5;
    }

    /* 커넥션 풀 크기: 1~64. libvirt 연결은 비용이 높으므로 64개 제한 */
    if (g_cfg.pool_max_conn < 1 || g_cfg.pool_max_conn > 64) {
        PCV_LOG_WARN(CFG_LOG_DOM, "pool_max_conn=%d out of range [1-64] — using default %d",
                     g_cfg.pool_max_conn, PCV_DEFAULT_POOL_MAX_CONN);
        g_cfg.pool_max_conn = PCV_DEFAULT_POOL_MAX_CONN;
    }

    /* log_level: 허용 목록 검증 (오타 방지) */
    {
        const gchar *valid_levels[] = {"error", "warn", "info", "debug", "trace", NULL};
        gboolean valid = FALSE;
        for (int i = 0; valid_levels[i]; i++) {
            if (g_strcmp0(g_cfg.log_level, valid_levels[i]) == 0) { valid = TRUE; break; }
        }
        if (!valid) {
            PCV_LOG_WARN(CFG_LOG_DOM, "log_level='%s' invalid — using default '%s'",
                         g_cfg.log_level, PCV_DEFAULT_LOG_LEVEL);
            g_free(g_cfg.log_level);
            g_cfg.log_level = g_strdup(PCV_DEFAULT_LOG_LEVEL);
        }
    }

    /*
     * admin_password는 공개판에 내장 기본값을 두지 않습니다.
     * 최초 bootstrap ADMIN 자동 생성은 daemon.conf 또는 환경변수에 명시값이 있을 때만 동작합니다.
     */
    if (!g_cfg.admin_password || !*g_cfg.admin_password) {
        PCV_LOG_WARN(CFG_LOG_DOM,
            "admin_password is not configured — bootstrap admin auto-create is disabled "
            "until /etc/purecvisor/daemon.conf or PURECVISOR_ADMIN_PASSWORD is set");
    } else if (strlen(g_cfg.admin_password) < 12) {
        PCV_LOG_WARN(CFG_LOG_DOM,
                     "admin_password is shorter than 12 characters — consider a stronger password");
    } else if (g_strcmp0(g_cfg.admin_password, "admin") == 0 ||
               g_strcmp0(g_cfg.admin_password, "password") == 0) {
        PCV_LOG_WARN(CFG_LOG_DOM,
            "SECURITY: admin_password uses a commonly known example value — "
            "change it in /etc/purecvisor/daemon.conf before production use");
    }

    /* jwt_secret: 수동 설정 시 최소 32바이트 권장 (빈 문자열은 랜덤 생성이므로 OK) */
    if (g_cfg.jwt_secret && *g_cfg.jwt_secret && strlen(g_cfg.jwt_secret) < 32) {
        PCV_LOG_WARN(CFG_LOG_DOM,
                     "jwt_secret is shorter than 32 bytes — HMAC-SHA256 security may be weakened");
    }

    /* ── 미인식 키 경고 ([daemon] 섹션) ────────────────────────── */
    if (kf) {
        gchar **keys = g_key_file_get_keys(kf, "daemon", nullptr, nullptr);
        if (keys) {
            const gchar *known[] = {
                "socket_path", "rest_port", "drain_timeout", "pool_max_conn",
                "log_level", "libvirt_uri", "jwt_secret", "db_path",
                "admin_user", "admin_password", nullptr
            };
            for (gchar **k = keys; *k; k++) {
                gboolean found = FALSE;
                for (const gchar **kn = known; *kn; kn++) {
                    if (g_strcmp0(*k, *kn) == 0) { found = TRUE; break; }
                }
                if (!found)
                    PCV_LOG_WARN(CFG_LOG_DOM,
                                 "Unknown key in [daemon]: '%s' (ignored)", *k);
            }
            g_strfreev(keys);
        }
    }

    /*
     * 소켓 디렉터리 자동 생성 (/var/run/purecvisor/)
     * UDS 소켓 파일은 디렉터리가 존재해야 bind()가 성공합니다.
     * 0700 권한(Wave C Item 1 / A01·V8): root 전용 — 비-root 는 디렉터리 진입 불가라
     * 소켓 경로에 도달조차 못 한다(SO_PEERCRED 게이트·소켓 0660 위의 심층 방어).
     */
    /* ── fail-fast: 필수 디렉터리 생성 검증 ────────────────────── */
    {
        gchar *sock_dir = g_path_get_dirname(g_cfg.socket_path);
        if (g_mkdir_with_parents(sock_dir, 0700) != 0) {
            g_critical("[%s] FATAL: Cannot create socket directory '%s' — daemon cannot start",
                       CFG_LOG_DOM, sock_dir);
            g_free(sock_dir);
            exit(1);
        }
        g_free(sock_dir);
    }

    {
        gchar *db_dir = g_path_get_dirname(g_cfg.db_path);
        if (g_mkdir_with_parents(db_dir, 0755) != 0) {
            g_critical("[%s] FATAL: Cannot create db directory '%s' — daemon cannot start",
                       CFG_LOG_DOM, db_dir);
            g_free(db_dir);
            exit(1);
        }
        g_free(db_dir);
    }

    /* ── fail-fast: etcd 엔드포인트 형식 검증 ───────────────── */
    {
        const gchar *etcd_ep = pcv_config_get_string("cluster", "etcd_endpoints", "");
        if (etcd_ep && etcd_ep[0] != '\0') {
            if (!g_str_has_prefix(etcd_ep, "http://") &&
                !g_str_has_prefix(etcd_ep, "https://")) {
                PCV_LOG_WARN(CFG_LOG_DOM,
                    "etcd_endpoints='%s' does not start with http(s):// — cluster features may fail",
                    etcd_ep);
            }
        }
    }

    /* ── fail-fast: image_dir 접근 가능 여부 확인 ──────────── */
    {
        const gchar *img_dir = pcv_config_get_string("storage", "image_dir",
                                                      "/var/lib/libvirt/images");
        if (!g_file_test(img_dir, G_FILE_TEST_IS_DIR)) {
            if (g_mkdir_with_parents(img_dir, 0755) == 0) {
                PCV_LOG_INFO(CFG_LOG_DOM, "Created image_dir: %s", img_dir);
            } else {
                PCV_LOG_WARN(CFG_LOG_DOM,
                    "image_dir '%s' does not exist and cannot be created — qcow2 fallback may fail",
                    img_dir);
            }
        }
    }

    g_cfg.initialized = TRUE;

    /* 현재 적용된 설정을 로그에 출력 (시작 시 진단용) */
    pcv_config_dump();

    /*
     * 설정 파일이 없으면 예시 파일 자동 생성 (운영자 안내)
     *
     * [목적]
     *   처음 설치한 운영자가 /etc/purecvisor/daemon.conf 파일의
     *   키 이름과 형식을 참고할 수 있도록 샘플을 생성합니다.
     *   생성된 파일에는 현재 적용된 기본값이 기록됩니다.
     */
    if (!loaded) {
        GKeyFile *sample = g_key_file_new();
        g_key_file_set_comment(sample, NULL, NULL,
            " PureCVisor daemon configuration\n"
            " Generated automatically — edit as needed", NULL);
        g_key_file_set_string (sample, CFG_GROUP, "socket_path",   g_cfg.socket_path);
        g_key_file_set_string (sample, CFG_GROUP, "libvirt_uri",   g_cfg.libvirt_uri);
        g_key_file_set_integer(sample, CFG_GROUP, "pool_max_conn", g_cfg.pool_max_conn);
        g_key_file_set_integer(sample, CFG_GROUP, "drain_timeout", g_cfg.drain_timeout);
        g_key_file_set_string (sample, CFG_GROUP, "db_path",       g_cfg.db_path);
        g_key_file_set_string (sample, CFG_GROUP, "log_level",     g_cfg.log_level);
        g_key_file_set_integer(sample, CFG_GROUP, "rest_port",     g_cfg.rest_port);
        g_key_file_set_string (sample, CFG_GROUP, "admin_user",    g_cfg.admin_user);
        /* [R12] value-first — set_comment 은 키 부재 시 조용히 실패(GLib)하므로
         * set_string 으로 키를 먼저 만든 뒤 주석을 붙인다(안 그러면 주석 유실). */
        g_key_file_set_string (sample, CFG_GROUP, "admin_password", "");
        g_key_file_set_comment(sample, CFG_GROUP, "admin_password",
            " REQUIRED: set before first bootstrap login or use PURECVISOR_ADMIN_PASSWORD", NULL);
        g_key_file_set_string (sample, CFG_GROUP, "jwt_secret",    "");
        g_key_file_set_comment(sample, CFG_GROUP, "jwt_secret",
            " Optional: leave empty to generate a random in-memory key on startup", NULL);

        /* [R6] SG vnet 캐시 주기 재동기화 knob 발견성 — 새 그룹이라 value-first
         * (키 생성 후 주석 부착; comment-first 는 그룹 선존재 전제라 부적합) */
        g_key_file_set_integer(sample, "security_group", "resync_interval_sec", 300);
        g_key_file_set_comment (sample, "security_group", "resync_interval_sec",
            " SG vnet 캐시 주기 재동기화 간격(초). 기본 300. 0 또는 음수면 타이머 비활성.", NULL);

        /* /etc/purecvisor/ 디렉터리 생성 후 샘플 저장 */
        if (g_mkdir_with_parents("/etc/purecvisor", 0755) == 0) {
            GError *save_err = NULL;
            if (!g_key_file_save_to_file(sample, PCV_CONFIG_FILE_PATH, &save_err)) {
                PCV_LOG_WARN(CFG_LOG_DOM, "Could not write sample config: %s",
                             save_err->message);
                g_error_free(save_err);
            } else {
                PCV_LOG_INFO(CFG_LOG_DOM,
                             "Sample config written to '%s'", cfg_path);
            }
        }
        g_key_file_free(sample);
    }
}

/**
 * pcv_config_shutdown - 설정 시스템 종료 및 메모리 해제
 *
 * main.c 종료 시 최말미에 호출합니다.
 * g_cfg의 모든 동적 할당 문자열과 GKeyFile 핸들을 해제하고,
 * 구조체를 0으로 초기화합니다.
 *
 * [호출 후 동작]
 *   initialized가 FALSE로 되므로, 이후 getter 호출 시
 *   NULL 체크에 걸려 기본값(PCV_DEFAULT_*)이 반환됩니다.
 *
 * [주의사항]
 *   pcv_config_get_string()으로 반환된 포인터를 shutdown 이후에
 *   사용하면 use-after-free 위험이 있습니다. 일반적으로 shutdown은
 *   프로세스 종료 직전에 호출되므로 실제 문제가 되지는 않습니다.
 */
void
pcv_config_shutdown(void)
{
    if (!g_cfg.initialized) return;
    g_free(g_cfg.socket_path);
    g_free(g_cfg.libvirt_uri);
    g_free(g_cfg.db_path);
    g_free(g_cfg.log_level);
    g_free(g_cfg.admin_user);
    g_free(g_cfg.admin_password);
    g_free(g_cfg.jwt_secret);
    if (g_cfg.string_cache) g_hash_table_destroy(g_cfg.string_cache);
    if (g_cfg.retired_string_caches) g_ptr_array_free(g_cfg.retired_string_caches, TRUE);
    if (g_cfg.kf) g_key_file_free(g_cfg.kf);
    g_rw_lock_clear(&g_cfg.kf_lock);
    /* memset 0: 모든 포인터 NULL, 정수 0, initialized FALSE */
    memset(&g_cfg, 0, sizeof(g_cfg));
}

/* ── 타입별 getter: 초기화 전이면 컴파일 기본값 반환 ──────────── */
/*
 * 각 getter는 삼항 연산자로 NULL/0 체크를 수행합니다.
 * pcv_config_init() 호출 전에 getter를 사용하면 기본값이 반환되어
 * 데몬이 최소한의 설정으로라도 동작할 수 있습니다.
 *
 * [삼항 연산자 패턴 설명 (주니어 참고)]
 *   return g_cfg.socket_path ? g_cfg.socket_path : PCV_DEFAULT_SOCKET_PATH;
 *   ↑ g_cfg.socket_path이 NULL이 아니면 (= 초기화됨) 그 값을 반환,
 *     NULL이면 (= 미초기화 또는 shutdown 이후) 기본값 상수를 반환.
 *
 * [반환값 수명]
 *   const gchar* 반환: g_cfg 내부 포인터 (프로세스 수명 동안 유효)
 *     → 호출자가 g_free()하면 안 됩니다!
 *   gint 반환: 값 복사이므로 수명 제한 없음
 *
 * [한 줄 함수를 사용하는 이유]
 *   getter는 단순한 필드 접근자(accessor)입니다.
 *   헤더 파일(pcv_config.h)에서 인라인으로 선언할 수도 있지만,
 *   .c 파일에 두어 구현 변경 시 재컴파일 범위를 최소화합니다.
 */
const gchar *pcv_config_get_socket_path(void)   { return g_cfg.socket_path   ? g_cfg.socket_path   : PCV_DEFAULT_SOCKET_PATH; }
const gchar *pcv_config_get_libvirt_uri(void)   { return g_cfg.libvirt_uri   ? g_cfg.libvirt_uri   : PCV_DEFAULT_LIBVIRT_URI; }
const gchar *pcv_config_get_db_path(void)       { return g_cfg.db_path       ? g_cfg.db_path       : PCV_DEFAULT_DB_PATH; }
const gchar *pcv_config_get_log_level(void)     { return g_cfg.log_level     ? g_cfg.log_level     : PCV_DEFAULT_LOG_LEVEL; }
gint         pcv_config_get_pool_max_conn(void) {
    gint v = g_cfg.pool_max_conn;
    return (v >= 1 && v <= 64) ? v : PCV_DEFAULT_POOL_MAX_CONN;
}
gint         pcv_config_get_drain_timeout(void) {
    gint v = g_cfg.drain_timeout;
    return (v >= 5) ? v : PCV_DEFAULT_DRAIN_TIMEOUT;
}
/* Sprint E: REST API getter */
gint         pcv_config_get_rest_port(void) {
    gint v = g_cfg.rest_port;
    return (v >= 1 && v <= 65535) ? v : PCV_DEFAULT_REST_PORT;
}
const gchar *pcv_config_get_admin_user(void)     { return g_cfg.admin_user     ? g_cfg.admin_user     : PCV_DEFAULT_ADMIN_USER; }
const gchar *pcv_config_get_admin_password(void) { return g_cfg.admin_password ? g_cfg.admin_password : PCV_DEFAULT_ADMIN_PASSWORD; }
const gchar *pcv_config_get_jwt_secret(void)     { return g_cfg.jwt_secret     ? g_cfg.jwt_secret     : PCV_DEFAULT_JWT_SECRET; }

/**
 * pcv_config_dump - 현재 설정을 INFO 레벨로 로그 출력
 *
 * pcv_config_init() 내부에서 호출되며, 시작 시 적용된 설정을
 * 한눈에 확인할 수 있도록 합니다.
 *
 * [보안 마스킹]
 *   jwt_secret은 보안상 값 대신 "(set)" 또는 "(random)"으로 마스킹합니다.
 *   admin_password도 운영 로그에 노출되므로 주의가 필요합니다.
 *   (현재 admin_password는 마스킹하지 않으나, 보안 감사 시 고려 대상)
 */
void
pcv_config_dump(void)
{
    PCV_LOG_INFO(CFG_LOG_DOM,
                 "Config: socket=%s uri=%s pool=%d drain=%ds db=%s log=%s",
                 g_cfg.socket_path, g_cfg.libvirt_uri,
                 g_cfg.pool_max_conn, g_cfg.drain_timeout,
                 g_cfg.db_path, g_cfg.log_level);
    PCV_LOG_INFO(CFG_LOG_DOM,
                 "Config(REST): port=%d admin_user=%s jwt_secret=%s",
                 g_cfg.rest_port, g_cfg.admin_user,
                 (g_cfg.jwt_secret && *g_cfg.jwt_secret) ? "(set)" : "(random)");
}

/* ── Phase T-2: 제네릭 설정 getter ────────────────────── */

/**
 * pcv_config_get_string - 임의 섹션/키에서 문자열 값 조회
 * @section: INI 섹션명 (예: "cluster", "tls", "storage", "overlay", "alert", "ai")
 * @key:     키 이름 (예: "etcd_endpoints", "enabled", "zvol_pool")
 * @def:     설정 파일에 없을 때 반환할 기본값
 *
 * @return: 설정값 문자열 포인터.
 *          프로세스 수명 동안 유효 — 호출자가 free하지 않음.
 *          설정 파일 미로드 시 def 그대로 반환.
 *
 * [사용 예시]
 *   // etcd 엔드포인트 조회 (etcd_client.c에서)
 *   const gchar *endpoints = pcv_config_get_string("cluster", "etcd_endpoints",
 *                                                   "http://127.0.0.1:2379");
 *
 *   // TLS 활성 여부 조회 (pcv_tls.c에서)
 *   const gchar *enabled = pcv_config_get_string("tls", "enabled", "false");
 *
 * [메모리 소유권 주의]
 *   반환값은 내부 캐시에 보관되며 호출자가 g_free하면 안 됩니다.
 *   캐시는 pcv_config_shutdown()에서 해제됩니다. reload 전 캐시는 기존
 *   반환 포인터 수명을 보존하기 위해 retired 목록에 보관했다가 shutdown
 *   시 함께 해제합니다.
 *
 * [_cfg_str()과의 차이]
 *   _cfg_str(): 환경변수 → [daemon] → fallback → 기본값 (전용 getter 초기화에 사용)
 *   이 함수:   지정된 section에서만 검색 (제네릭, 각 모듈이 직접 호출)
 */
const gchar *
pcv_config_get_string(const gchar *section, const gchar *key, const gchar *def)
{
    if (!section || !key)
        return def;

    if (!g_cfg.initialized)
        return def;

    g_rw_lock_writer_lock(&g_cfg.kf_lock);
    if (!g_cfg.kf) {
        g_rw_lock_writer_unlock(&g_cfg.kf_lock);
        return def;  /* 설정 파일 미로드: 기본값 직접 반환 */
    }

    if (!g_cfg.string_cache) {
        g_cfg.string_cache = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                   g_free, g_free);
    }

    gchar *cache_key = g_strdup_printf("%s\x1f%s", section, key);
    const gchar *cached = g_hash_table_lookup(g_cfg.string_cache, cache_key);
    if (cached) {
        g_free(cache_key);
        g_rw_lock_writer_unlock(&g_cfg.kf_lock);
        return cached;
    }

    GError *err = NULL;
    gchar *val = g_key_file_get_string(g_cfg.kf, section, key, &err);
    if (err) {
        g_error_free(err);
        g_free(cache_key);
        g_rw_lock_writer_unlock(&g_cfg.kf_lock);
        return def;   /* 섹션 또는 키가 없음 */
    }
    if (!val || !*val) {
        g_free(val);
        g_free(cache_key);
        g_rw_lock_writer_unlock(&g_cfg.kf_lock);
        return def;   /* 빈 문자열도 기본값 처리 */
    }

    g_hash_table_insert(g_cfg.string_cache, cache_key, val);
    g_rw_lock_writer_unlock(&g_cfg.kf_lock);
    return val;
}

/**
 * pcv_config_get_int - 임의 섹션/키에서 정수 값 조회
 * @section: INI 섹션명
 * @key:     키 이름
 * @def:     설정 파일에 없을 때 반환할 기본값
 *
 * @return: 정수 설정값. 파싱 실패(비숫자 문자 등) 시 def 반환.
 *
 * [사용 예시]
 *   // etcd 타임아웃 조회 (etcd_client.c에서)
 *   gint timeout = pcv_config_get_int("cluster", "etcd_timeout", 15);
 *
 *   // 알림 임계값 조회 (alert_engine.c에서)
 *   gint cpu_warn = pcv_config_get_int("alert", "cpu_warn", 80);
 */
gint
pcv_config_get_int(const gchar *section, const gchar *key, gint def)
{
    if (!g_cfg.kf) return def;  /* 설정 파일 미로드: 기본값 직접 반환 */

    g_rw_lock_reader_lock(&g_cfg.kf_lock);
    GError *err = NULL;
    gint val = g_key_file_get_integer(g_cfg.kf, section, key, &err);
    g_rw_lock_reader_unlock(&g_cfg.kf_lock);
    if (err) {
        g_error_free(err);
        return def;   /* 섹션/키 없음 또는 파싱 실패 */
    }
    return val;
}

/* ── 스토리지 경로 getter ─────────────────────────────── */

/**
 * pcv_config_get_zvol_pool - VM zvol 스토리지 풀 경로 반환
 *
 * daemon.conf [storage] zvol_pool 키에서 조회.
 * 미설정 시 기본값 "pcvpool/vms" 반환.
 *
 * [사용처]
 *   VM 생성 시 ZFS zvol 경로 결정:
 *     /dev/zvol/<zvol_pool>/<vm_name>
 *     예: /dev/zvol/pcvpool/vms/web-prod
 *
 *   ZFS 스냅샷 경로:
 *     <zvol_pool>/<vm_name>@<snap_name>
 *     예: pcvpool/vms/web-prod@daily-20260326
 *
 * [설정 예시]
 *   [storage]
 *   zvol_pool = pcvpool/vms
 *
 * [주의]
 *   소스 코드 내 rpool/vms 하드코딩은 이 함수로 전환 완료.
 *   반환값은 "/" 접두사 없이 ZFS 데이터셋 이름 형식입니다.
 */
const gchar *
pcv_config_get_zvol_pool(void)
{
    return pcv_config_get_string("storage", "zvol_pool", "pcvpool/vms");
}

/**
 * pcv_config_get_container_pool - LXC 컨테이너 ZFS 데이터셋 베이스 반환
 *
 * daemon.conf [storage] container_pool 키에서 조회.
 * 미설정 시 기본값 "pcvpool/containers" 반환.
 */
const gchar *
pcv_config_get_container_pool(void)
{
    return pcv_config_get_string("storage", "container_pool", "pcvpool/containers");
}

/**
 * pcv_config_get_container_path - LXC 컨테이너 루트 경로 반환
 *
 * daemon.conf [container] lxc_path 키에서 조회.
 * 미설정 시 기본값 "/var/lib/purecvisor/lxc" 반환.
 */
const gchar *
pcv_config_get_container_path(void)
{
    return pcv_config_get_string("container", "lxc_path", "/var/lib/purecvisor/lxc");
}

/**
 * pcv_config_get_image_dir - qcow2 파일 디스크 저장 경로 반환
 *
 * daemon.conf [storage] image_dir 키에서 조회.
 * 미설정 시 기본값 "/var/lib/libvirt/images" 반환.
 *
 * [사용처]
 *   ZFS 풀이 없는 환경에서 qcow2 파일 디스크 폴백:
 *     qemu-img create -f qcow2 <image_dir>/<vm_name>.qcow2 20G
 */
const gchar *
pcv_config_get_image_dir(void)
{
    return pcv_config_get_string("storage", "image_dir", "/var/lib/libvirt/images");
}

/**
 * pcv_config_get_iso_dirs - ISO 이미지 검색 디렉터리 목록 반환
 *
 * daemon.conf [storage] iso_dirs 키에서 조회.
 * 콤마 구분 다중 경로. 미설정 시 기본값 "/pcvpool/iso,/var/lib/libvirt/images,/iso" 반환.
 *
 * [사용처]
 *   iso.list RPC 핸들러에서 여러 디렉터리를 순회하며 ISO 파일 스캔.
 */
const gchar *
pcv_config_get_iso_dirs(void)
{
    return pcv_config_get_string("storage", "iso_dirs",
                                 "/pcvpool/iso,/var/lib/libvirt/images,/iso");
}

/**
 * pcv_config_get_ssh_user - 클러스터 SSH 연결 사용자명 반환
 *
 * daemon.conf [cluster] ssh_user 키에서 조회.
 * 미설정 시 기본값 "pcvdev" 반환.
 *
 * [사용처]
 *   - ZFS 원격 복제: zfs send | ssh <ssh_user>@<peer> sudo zfs recv
 *   - 라이브 마이그레이션: qemu+ssh://<ssh_user>@<peer>/system
 *   - 클러스터 RPC 프록시: 원격 노드 nc 호출
 *
 * [보안 주의]
 *   이 사용자는 원격 노드에서 sudo zfs recv, virsh 등을 실행할 수 있어야 합니다.
 *   패스워드 없는 SSH 키 인증 설정이 필수입니다.
 */
const gchar *
pcv_config_get_ssh_user(void)
{
    return pcv_config_get_string("cluster", "ssh_user", "pcvdev");
}

/* ── SIGHUP 런타임 재로드 ───────────────────────────────── */

/**
 * pcv_config_reload - SIGHUP 시그널 수신 시 설정 파일 핫 리로드
 *
 * @return: TRUE이면 리로드 성공, FALSE이면 파일 파싱 실패 (기존 설정 유지)
 *
 * [호출 시점]
 *   데몬 실행 중 `kill -HUP $(pidof purecvisorsd)` 또는
 *   `kill -HUP $(pidof purecvisormd)`,
 *   `systemctl reload purecvisorsd` / `systemctl reload purecvisormd`
 *   시 시그널 핸들러에서 호출됩니다.
 *   데몬을 재시작하지 않고 설정을 변경할 수 있어 다운타임이 없습니다.
 *
 * [리로드 가능 항목]
 *   - log_level: 런타임 로그 레벨 변경 (예: "info" → "debug" 디버깅 후 복구)
 *   - [alert] 섹션: 알림 임계값, Webhook URL 변경
 *   - rate_limit: API Rate Limiting 값 변경
 *   - etcd_timeout: etcd 연결 타임아웃 조정
 *
 * [리로드 불가 항목 (재시작 필요)]
 *   - socket_path, rest_port: 이미 바인딩된 소켓은 변경 불가
 *   - libvirt_uri: 커넥션 풀이 이미 생성됨
 *   - jwt_secret: 기존 발행된 토큰과의 호환성 문제
 *
 * [스레드 안전: GRWLock 패턴]
 *   pcv_config_get_int은 reader lock으로 g_cfg.kf를 읽고,
 *   pcv_config_get_string과 이 함수는 writer lock으로 캐시와 g_cfg.kf를
 *   교체/갱신합니다.
 *   RWLock 덕분에 여러 스레드가 동시에 설정을 읽는 중에도
 *   안전하게 설정 파일을 교체할 수 있습니다.
 *
 *   [RWLock 동작 원리]
 *     reader_lock: 여러 스레드가 동시에 읽기 가능 (공유 잠금)
 *     writer_lock: 하나의 스레드만 쓰기 가능 (배타적 잠금, 모든 reader 완료 대기)
 *     이 패턴은 "읽기 빈번, 쓰기 드문" 설정 조회에 최적입니다.
 *
 * [설정 파일 교체 순서 (메모리 안전)]
 *   1. 새 GKeyFile 파싱 (실패하면 기존 설정 유지)
 *   2. writer lock 획득 → g_cfg.kf와 문자열 캐시 교체 → writer lock 해제
 *   3. 이전 GKeyFile 해제. 이전 문자열 캐시는 반환 포인터 수명 보존을 위해
 *      shutdown까지 retired 목록에 보관
 */
gboolean
pcv_config_reload(void)
{
    GKeyFile *kf = g_key_file_new();
    GError   *err = NULL;

    if (!g_key_file_load_from_file(kf, PCV_CONFIG_FILE_PATH,
                                   G_KEY_FILE_NONE, &err)) {
        PCV_LOG_WARN(CFG_LOG_DOM,
                     "Config reload failed: %s", err->message);
        g_error_free(err);
        g_key_file_free(kf);
        return FALSE;
    }

    /*
     * GRWLock writer lock으로 GKeyFile 포인터 원자적 교체
     *
     * writer lock 획득 시 모든 reader(pcv_config_get_string 등)가
     * 현재 읽기를 완료할 때까지 대기합니다. 이후 새 kf 포인터를 설정하면
     * 다음 reader부터는 새 설정 파일을 읽게 됩니다.
     */
    GHashTable *new_cache = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                  g_free, g_free);

    g_rw_lock_writer_lock(&g_cfg.kf_lock);
    GKeyFile *old_kf = g_cfg.kf;
    GHashTable *old_cache = g_cfg.string_cache;
    g_cfg.kf = kf;
    g_cfg.string_cache = new_cache;
    if (!g_cfg.retired_string_caches) {
        g_cfg.retired_string_caches =
            g_ptr_array_new_with_free_func((GDestroyNotify)g_hash_table_destroy);
    }
    if (old_cache)
        g_ptr_array_add(g_cfg.retired_string_caches, old_cache);
    g_rw_lock_writer_unlock(&g_cfg.kf_lock);
    if (old_kf) g_key_file_free(old_kf);  /* lock 밖에서 해제 — 해제 시간이 reader를 블로킹하지 않음 */

    /*
     * log_level 핫 리로드 (non-destructive)
     *
     * log_level은 구조체 필드에 직접 저장되므로 별도로 갱신합니다.
     * 다른 설정값(socket_path 등)은 GKeyFile에서 직접 읽으므로
     * kf 교체만으로 자동 반영되지만, log_level은 g_cfg.log_level에
     * 캐시되어 있어 명시적 갱신이 필요합니다.
     *
     * [값이 변경된 경우에만 교체]
     *   불필요한 g_free/g_strdup을 방지하고, 변경 로그를 정확히 출력합니다.
     */
    {
        GError *le = NULL;
        gchar *new_level = g_key_file_get_string(kf, CFG_GROUP, "log_level", &le);
        if (new_level) {
            g_strstrip(new_level);
            if (g_strcmp0(new_level, g_cfg.log_level) != 0) {
                PCV_LOG_INFO(CFG_LOG_DOM, "Reload: log_level '%s' → '%s'",
                             g_cfg.log_level, new_level);
                g_free(g_cfg.log_level);
                g_cfg.log_level = new_level;  /* 소유권 이전: new_level은 이제 g_cfg가 소유 */
            } else {
                g_free(new_level);  /* 값이 같으면 새로 할당된 문자열 해제 */
            }
        }
        if (le) g_error_free(le);
    }

    /* ── pool_max_conn 핫 리로드 ─────────────────────────────────────
     * 커넥션 풀 최대 연결 수를 런타임에 변경합니다.
     * virt_conn_pool이 이미 생성되어 있으므로, 새 max 값은
     * 다음 acquire/release 사이클에서 반영됩니다. */
    {
        GError *pe = NULL;
        gint new_pool = g_key_file_get_integer(kf, CFG_GROUP, "pool_max_conn", &pe);
        if (!pe && new_pool >= 1 && new_pool <= 64) {
            g_cfg.pool_max_conn = new_pool;
            PCV_LOG_INFO(CFG_LOG_DOM, "Reload: pool_max_conn → %d", new_pool);
        }
        if (pe) g_error_free(pe);
    }

    /* ── drain_timeout 핫 리로드 ──────────────────────────────────
     * 그레이스풀 드레인 타임아웃(초)을 런타임에 변경합니다. */
    {
        GError *de = NULL;
        gint new_drain = g_key_file_get_integer(kf, CFG_GROUP, "drain_timeout", &de);
        if (!de && new_drain >= 5) {
            g_cfg.drain_timeout = new_drain;
            PCV_LOG_INFO(CFG_LOG_DOM, "Reload: drain_timeout → %d", new_drain);
        }
        if (de) g_error_free(de);
    }

    /* ── JWT 시크릿 SIGHUP 리로드 — 보안 사고 대응용 ────────────
     * daemon.conf의 jwt_secret을 변경한 뒤 SIGHUP을 보내면
     * 런타임에 서명 키가 교체됩니다. 기존 토큰은 자동 무효화. */
    {
        gchar *new_secret = pcv_config_get_secret("auth", "jwt_secret", NULL);
        if (new_secret && *new_secret) {
            pcv_jwt_update_secret(new_secret);
            PCV_LOG_INFO(CFG_LOG_DOM,
                         "Reload: jwt_secret updated (existing tokens invalidated)");
        }
        g_free(new_secret);
    }

    /* ── 클러스터 설정 버전 증가 (BE-6) ───────────────────────────
     * SIGHUP 리로드 후 etcd config_version을 증가시켜
     * 다른 노드가 설정 변경을 자동 감지하도록 합니다. */
#if PCV_CLUSTER_ENABLED
    {
        extern void pcv_cluster_notify_config_reload(void);
        pcv_cluster_notify_config_reload();
    }
#endif

    PCV_LOG_INFO(CFG_LOG_DOM,
                 "Config reloaded — [alert], rate_limit, etcd_timeout, "
                 "log_level, pool_max_conn, drain_timeout now reflect daemon.conf on disk");
    return TRUE;
}

/* ══════════════════════════════════════════════════════════
 *  암호화 비밀 관리 — AES-256-GCM + PBKDF2
 *
 *  daemon.conf에 "ENC:base64..." 형식으로 저장된 값을 복호화합니다.
 *  마스터 키는 /etc/machine-id + 고정 솔트로 PBKDF2 유도합니다.
 *
 *  [보안 고려]
 *    - machine-id는 호스트별 고유값으로 다른 호스트에서는 복호화 불가
 *    - PBKDF2 100,000 반복으로 brute-force 비용 증가
 *    - AES-256-GCM은 인증된 암호화 (변조 탐지)
 *    - 평문 값은 하위 호환으로 그대로 반환 (점진적 마이그레이션)
 *
 *  [사용법]
 *    암호화: pcvctl config encrypt "mypassword" → "ENC:base64..."
 *    설정:   daemon.conf에 chap_password=ENC:base64... 기록
 *    복호화: pcv_config_get_secret("iscsi", "chap_password", NULL)
 * ══════════════════════════════════════════════════════════ */

#include <openssl/evp.h>
#include <openssl/sha.h>

/** AES-256-GCM 키 길이 (32바이트 = 256비트) */
#define PCV_SECRET_KEY_LEN   32

/** AES-256-GCM IV 길이 (12바이트 = 96비트, NIST 권장) */
#define PCV_SECRET_IV_LEN    12

/** AES-256-GCM 인증 태그 길이 (16바이트 = 128비트) */
#define PCV_SECRET_TAG_LEN   16

/** PBKDF2 반복 횟수 (OWASP 2024 권장: 최소 100,000) */
#define PCV_SECRET_PBKDF2_ITER 100000

/**
 * _derive_master_key - /etc/machine-id로부터 AES-256 마스터 키 + IV 유도
 * @out_key: 32바이트 키 출력 버퍼
 * @out_iv:  12바이트 IV 출력 버퍼
 *
 * @return: TRUE=성공, FALSE=machine-id 읽기 실패
 *
 * [키 유도 전략]
 *   1. /etc/machine-id 읽기 (systemd가 설치 시 생성, 호스트별 고유)
 *   2. PBKDF2-HMAC-SHA256(machine-id, salt, 100000) → 32바이트 키
 *   3. SHA256(machine-id + "iv-derivation") → 앞 12바이트를 IV로 사용
 *
 *   machine-id가 동일하면 항상 같은 키/IV가 유도됩니다.
 *   이는 결정적(deterministic) 암호화를 의미하지만, daemon.conf 암호화는
 *   동일 값을 반복 암호화할 일이 드물어 실질적 보안 영향이 최소입니다.
 */
static gboolean
_derive_master_key(guchar *out_key, guchar *out_iv)
{
    gchar  *machine_id = NULL;
    gsize   mid_len    = 0;

    if (!g_file_get_contents("/etc/machine-id", &machine_id, &mid_len, NULL)) {
        PCV_LOG_WARN(CFG_LOG_DOM, "Cannot read /etc/machine-id — secret decryption unavailable");
        return FALSE;
    }
    /* machine-id 끝의 개행 제거 */
    g_strstrip(machine_id);
    mid_len = strlen(machine_id);

    /* PBKDF2로 32바이트 키 유도 */
    static const guchar salt[] = "purecvisor-config-v1";
    if (PKCS5_PBKDF2_HMAC(machine_id, (int)mid_len,
                           salt, sizeof(salt) - 1,
                           PCV_SECRET_PBKDF2_ITER, EVP_sha256(),
                           PCV_SECRET_KEY_LEN, out_key) != 1) {
        g_free(machine_id);
        return FALSE;
    }

    /* IV 유도: SHA256(machine-id + "iv-derivation")의 앞 12바이트 */
    {
        guchar sha_buf[SHA256_DIGEST_LENGTH];
        EVP_MD_CTX *md_ctx = EVP_MD_CTX_new();
        EVP_DigestInit_ex(md_ctx, EVP_sha256(), NULL);
        EVP_DigestUpdate(md_ctx, machine_id, mid_len);
        EVP_DigestUpdate(md_ctx, "iv-derivation", 13);
        EVP_DigestFinal_ex(md_ctx, sha_buf, NULL);
        EVP_MD_CTX_free(md_ctx);
        memcpy(out_iv, sha_buf, PCV_SECRET_IV_LEN);
    }

    g_free(machine_id);
    return TRUE;
}

/**
 * _decrypt_aes_gcm - base64 인코딩된 암호문을 AES-256-GCM으로 복호화
 * @b64_ciphertext: base64 인코딩된 문자열 (tag 16바이트 + 암호문)
 *
 * @return: 복호화된 평문 (g_free 필요). 실패 시 NULL.
 *
 * [바이너리 레이아웃]
 *   base64 디코딩 후: [16바이트 GCM TAG] [암호문 N바이트]
 *   TAG가 앞에 오는 이유: 복호화 전에 TAG를 추출하여 인증에 사용하기 위함
 */
static gchar *
_decrypt_aes_gcm(const gchar *b64_ciphertext)
{
    guchar key[PCV_SECRET_KEY_LEN], iv[PCV_SECRET_IV_LEN];
    if (!_derive_master_key(key, iv))
        return NULL;

    /* base64 디코딩 */
    gsize raw_len = 0;
    guchar *raw = g_base64_decode(b64_ciphertext, &raw_len);
    if (!raw || raw_len < PCV_SECRET_TAG_LEN + 1) {
        g_free(raw);
        return NULL;
    }

    const guchar *tag        = raw;
    const guchar *ciphertext = raw + PCV_SECRET_TAG_LEN;
    int cipher_len = (int)(raw_len - PCV_SECRET_TAG_LEN);

    /* AES-256-GCM 복호화 */
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) { g_free(raw); return NULL; }

    guchar *plaintext = g_malloc0((gsize)cipher_len + 1);
    int out_len = 0, final_len = 0;
    gboolean ok = FALSE;

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, key, iv) == 1 &&
        EVP_DecryptUpdate(ctx, plaintext, &out_len, ciphertext, cipher_len) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, PCV_SECRET_TAG_LEN,
                            (void *)(guintptr)tag) == 1 &&
        EVP_DecryptFinal_ex(ctx, plaintext + out_len, &final_len) == 1) {
        plaintext[out_len + final_len] = '\0';
        ok = TRUE;
    }

    EVP_CIPHER_CTX_free(ctx);
    g_free(raw);

    if (!ok) {
        PCV_LOG_WARN(CFG_LOG_DOM, "AES-256-GCM decryption failed — check machine-id or ciphertext");
        g_free(plaintext);
        return NULL;
    }
    return (gchar *)plaintext;
}

/**
 * pcv_config_get_secret - "ENC:" 접두사 감지 후 자동 복호화하는 비밀값 조회
 * @group:    INI 섹션명 (예: "iscsi", "cluster")
 * @key:      키 이름 (예: "chap_password", "api_key")
 * @fallback: 설정 미존재 시 반환할 기본값 (NULL 가능)
 *
 * @return: 복호화된 평문 또는 평문 그대로 (호출자 g_free 필수), 미존재 시 fallback 복사
 *
 * [동작 흐름]
 *   1. pcv_config_get_string()으로 원시 설정값 조회
 *   2. "ENC:" 접두사 있으면 → _decrypt_aes_gcm()으로 복호화
 *   3. "ENC:" 접두사 없으면 → 평문 그대로 반환 (하위 호환)
 *   4. 값 미존재 시 → fallback 복사 반환
 *
 * [하위 호환 패턴]
 *   기존에 평문으로 저장된 설정값도 정상 동작합니다.
 *   운영자가 점진적으로 암호화된 값으로 마이그레이션할 수 있습니다.
 *   예: chap_password=mypass → chap_password=ENC:base64...
 */
gchar *
pcv_config_get_secret(const gchar *group, const gchar *key,
                       const gchar *fallback)
{
    /* 1순위: 환경 변수 PCV_SECRET_<SECTION>_<KEY> (대문자, - → _) */
    gchar *env_name = g_strdup_printf("PCV_SECRET_%s_%s", group, key);
    for (gchar *p = env_name; *p; p++) {
        *p = g_ascii_toupper(*p);
        if (*p == '-') *p = '_';
    }
    const gchar *env_val = g_getenv(env_name);
    g_free(env_name);
    if (env_val && *env_val) {
        PCV_LOG_DEBUG(CFG_LOG_DOM, "Secret [%s] %s loaded from environment variable", group, key);
        return g_strdup(env_val);  /* 환경 변수 값 복사 반환 */
    }

    /* 2순위: daemon.conf에서 읽기 (ENC: 접두사 복호화 포함) */
    const gchar *raw = pcv_config_get_string(group, key, NULL);
    if (!raw || !*raw)
        return g_strdup(fallback);

    /* "ENC:" 접두사가 있으면 복호화 시도 */
    if (g_str_has_prefix(raw, "ENC:")) {
        gchar *decrypted = _decrypt_aes_gcm(raw + 4);  /* "ENC:" 4바이트 스킵 */
        return decrypted ? decrypted : g_strdup(fallback);
    }

    /* 평문 그대로 반환 (하위 호환) */
    return g_strdup(raw);
}

/**
 * pcv_config_encrypt_value - 평문을 AES-256-GCM 암호화 후 "ENC:base64..." 반환
 * @plaintext: 암호화할 평문 문자열 (예: 비밀번호, API 키)
 *
 * @return: "ENC:base64..." 형식 문자열 (호출자 g_free 필수), 실패 시 NULL
 *
 * [사용법]
 *   CLI에서 암호화: pcvctl config encrypt "mypassword"
 *   출력: ENC:ABCDeFgHiJkL...
 *   이 값을 daemon.conf에 기록: chap_password=ENC:ABCDeFgHiJkL...
 *
 * [암호화 흐름]
 *   1. _derive_master_key()로 /etc/machine-id 기반 AES 키 + IV 유도
 *   2. EVP_EncryptInit_ex(AES-256-GCM) → Update → Final → GET_TAG
 *   3. [TAG 16바이트][암호문] 결합 → base64 인코딩 → "ENC:" 접두사 추가
 *
 * [출력 바이너리 레이아웃]
 *   base64 인코딩 전: [GCM TAG 16B] [AES 암호문 N B]
 *   TAG를 앞에 배치하는 이유: _decrypt_aes_gcm()에서 TAG를 먼저 추출하여
 *   복호화 인증에 사용하기 위함 (GCM은 복호화 전 인증 필요)
 */
gchar *
pcv_config_encrypt_value(const gchar *plaintext)
{
    if (!plaintext) return NULL;

    guchar key[PCV_SECRET_KEY_LEN], iv[PCV_SECRET_IV_LEN];
    if (!_derive_master_key(key, iv))
        return NULL;

    int pt_len = (int)strlen(plaintext);

    /* AES-256-GCM 암호화 */
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return NULL;

    /* 출력 버퍼: TAG(16) + ciphertext(최대 pt_len + 블록크기) */
    guchar *out_buf = g_malloc0(PCV_SECRET_TAG_LEN + (gsize)pt_len + EVP_MAX_BLOCK_LENGTH);
    guchar *ciphertext = out_buf + PCV_SECRET_TAG_LEN;
    int out_len = 0, final_len = 0;
    guchar tag[PCV_SECRET_TAG_LEN];
    gboolean ok = FALSE;

    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, key, iv) == 1 &&
        EVP_EncryptUpdate(ctx, ciphertext, &out_len, (const guchar *)plaintext, pt_len) == 1 &&
        EVP_EncryptFinal_ex(ctx, ciphertext + out_len, &final_len) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, PCV_SECRET_TAG_LEN, tag) == 1) {
        ok = TRUE;
    }

    EVP_CIPHER_CTX_free(ctx);

    if (!ok) {
        g_free(out_buf);
        return NULL;
    }

    /* TAG를 앞에 배치: [TAG 16B][CIPHERTEXT] */
    memcpy(out_buf, tag, PCV_SECRET_TAG_LEN);
    gsize total_len = PCV_SECRET_TAG_LEN + (gsize)(out_len + final_len);

    /* base64 인코딩 후 "ENC:" 접두사 추가 */
    gchar *b64 = g_base64_encode(out_buf, total_len);
    g_free(out_buf);

    gchar *result = g_strdup_printf("ENC:%s", b64);
    g_free(b64);
    return result;
}
