/**
 * @file pcv_config.h
 * @brief GKeyFile 기반 설정 시스템 — 공개 헤더 (기본값 상수, API 선언)
 *
 * PureCVisor 데몬의 중앙 설정 관리 모듈 헤더입니다.
 * Sprint D-2에서 도입되었고, Sprint E(REST/JWT), Phase T-2(클러스터)에서 확장되었습니다.
 *
 * [설정 우선순위 (높은 순)]
 *   1. 환경변수      : PURECVISOR_SOCKET_PATH, PURECVISOR_REST_PORT, ...
 *   2. 설정 파일     : /etc/purecvisor/daemon.conf (GKeyFile INI 형식)
 *   3. 컴파일 기본값 : 아래 PCV_DEFAULT_* 상수
 *
 * [설정 파일 형식 예시]  /etc/purecvisor/daemon.conf
 *
 *   [daemon]
 *   socket_path     = /var/run/purecvisor/daemon.sock
 *   libvirt_uri     = qemu:///system
 *   pool_max_conn   = 8
 *   drain_timeout   = 30
 *   db_path         = /var/lib/purecvisor/vm_state.db
 *   log_level       = info
 *   rest_port       = 8080
 *   jwt_secret      = my-secret-key
 *
 *   [storage]
 *   zvol_pool       = pcvpool/vms
 *   container_pool  = pcvpool/containers
 *
 *   [cluster]
 *   ssh_user        = pcvdev
 *   peer_rest_ips   = 192.0.2.20,192.0.2.21
 *
 *   [tls]
 *   enabled         = true
 *   cert            = /etc/purecvisor/pki/node.crt
 *   key             = /etc/purecvisor/pki/node.key
 *   ca              = /etc/purecvisor/pki/ca.crt
 *
 *   [alert]
 *   enabled         = true
 *   cpu_warn        = 80
 *   cpu_crit        = 95
 *   webhook_url     = https://hooks.slack.com/...
 *
 *   [iscsi] — iSCSI Target/Initiator Configuration
 *   chap_user        = (없음)   CHAP 인증 사용자명
 *   chap_password    = (없음)   CHAP 인증 패스워드 (ENC: 접두사 암호화 지원)
 *
 * [API 분류]
 *   Lifecycle   : pcv_config_init() / pcv_config_shutdown() / pcv_config_dump()
 *   전용 getter : pcv_config_get_socket_path(), _libvirt_uri(), _rest_port() 등
 *   제네릭 getter: pcv_config_get_string(section, key, def) / _get_int()
 *   스토리지    : pcv_config_get_zvol_pool() / _container_pool()
 *   클러스터    : pcv_config_get_ssh_user()
 *
 * [사용법]
 *   pcv_config_init();                                // main.c에서 1회
 *   const gchar *sock = pcv_config_get_socket_path(); // 전용 getter
 *   const gchar *etcd = pcv_config_get_string("cluster", "etcd_endpoints",
 *                                              "http://127.0.0.1:2379"); // 제네릭
 *   pcv_config_shutdown();                            // 종료 시
 *
 * [주의사항]
 *   - REST API 설정(PCV_DEFAULT_REST_PORT 등)은 include guard 밖에 선언되어 있음
 *     (Sprint E에서 추가 시 guard 밖에 배치된 레거시 구조)
 *   - jwt_secret이 빈 문자열이면 pcv_jwt_init()에서 랜덤 키 생성 (재시작 시 기존 토큰 무효)
 *   - 모든 getter의 반환 포인터는 프로세스 수명 동안 유효 (호출자가 free하지 않음)
 *   - pcv_config_init() 이전에 getter를 호출하면 PCV_DEFAULT_* 기본값이 반환됨
 */

#ifndef PURECVISOR_CONFIG_H
#define PURECVISOR_CONFIG_H

#include <glib.h>

G_BEGIN_DECLS

/* ── 기본값 상수 ─────────────────────────────────────── */
/*
 * 아래 상수들은 설정 파일/환경변수가 모두 없을 때 사용되는 최종 fallback 값입니다.
 * 수정 시 pcv_config.c의 _cfg_str/_cfg_int 호출부와 일관성을 유지해야 합니다.
 */

/** UDS(Unix Domain Socket) 소켓 파일 경로. uds_server.c에서 bind()에 사용 */
#define PCV_DEFAULT_SOCKET_PATH   "/var/run/purecvisor/daemon.sock"

/** libvirt 연결 URI. "qemu:///system"은 로컬 QEMU/KVM 시스템 모드 */
#define PCV_DEFAULT_LIBVIRT_URI   "qemu:///system"

/** virt_conn_pool.c의 최대 libvirt 연결 수 (동시 VM 작업 병렬도) */
#define PCV_DEFAULT_POOL_MAX_CONN 8

/** 그레이스풀 드레인 타임아웃 (초). SIGTERM 수신 후 inflight RPC 완료 대기 시간 */
#define PCV_DEFAULT_DRAIN_TIMEOUT 30          /* 초 */

/** SQLite WAL DB 파일 경로. vm_state.c에서 VM 상태 영속화에 사용 */
#define PCV_DEFAULT_DB_PATH       "/var/lib/purecvisor/vm_state.db"

/** 로그 레벨 문자열. "debug", "info", "warn", "error" 중 하나 */
#define PCV_DEFAULT_LOG_LEVEL     "info"

/** 설정 파일의 절대 경로. GKeyFile INI 형식 */
#define PCV_CONFIG_FILE_PATH      "/etc/purecvisor/daemon.conf"

/* ── 초기화 / 해제 ───────────────────────────────────── */

/**
 * pcv_config_init:
 * 설정 시스템 초기화. pcv_log_init() 이후, 다른 모든 초기화보다 앞서 호출.
 * 설정 파일이 없으면 기본값으로 동작 (에러 없음).
 *
 * [내부 동작]
 *   1. daemon.conf 로드 시도
 *   2. 환경변수 > 설정파일 > 기본값 순으로 각 항목 해석
 *   3. 값 범위 검증 + 디렉터리 자동 생성
 *   4. 현재 설정 로그 출력
 *   5. 설정 파일 없으면 샘플 자동 생성
 */
void pcv_config_init(void);

/**
 * pcv_config_shutdown:
 * 설정 메모리 해제. main.c cleanup 최말미에 호출.
 * 이후 getter 호출 시 기본값이 반환됩니다.
 */
void pcv_config_shutdown(void);

/* ── 값 조회 API ─────────────────────────────────────── */

/** UDS 소켓 경로. uds_server.c에서 클라이언트 연결 수락에 사용 */
const gchar *pcv_config_get_socket_path(void);

/** libvirt 연결 URI. virt_conn_pool.c에서 virConnectOpen()에 전달 */
const gchar *pcv_config_get_libvirt_uri(void);

/** 커넥션 풀 최대 크기. virt_conn_pool.c에서 동시 연결 상한 설정 */
gint         pcv_config_get_pool_max_conn(void);

/** Graceful Drain 대기 최대 시간 (초). drain.c에서 SIGTERM 후 대기 */
gint         pcv_config_get_drain_timeout(void);

/** SQLite DB 파일 경로. vm_state.c에서 상태 영속화 DB 위치 */
const gchar *pcv_config_get_db_path(void);

/** 로그 레벨 문자열 ("debug"/"info"/"warn"/"error") */
const gchar *pcv_config_get_log_level(void);

/**
 * pcv_config_dump:
 * 현재 적용된 설정 전체를 INFO 레벨로 출력 (시작 시 진단용).
 * jwt_secret은 보안상 값 대신 "(set)/(random)"으로 마스킹합니다.
 */
void pcv_config_dump(void);

G_END_DECLS

#endif /* PURECVISOR_CONFIG_H */

/* ── Sprint E 추가: REST API 설정 ─────────────────────────── */
/*
 * [주의: include guard 밖에 위치]
 *   Sprint E에서 REST API 설정을 추가할 때 #endif 아래에 배치된 레거시 구조입니다.
 *   include guard 밖이므로 중복 include 시에도 재정의됩니다.
 *   #define 매크로는 재정의해도 동일 값이면 경고가 발생하지 않으므로 실질적 문제는 없습니다.
 *   이상적으로는 guard 안에 넣어야 하지만, 기존 동작 호환성을 위해 유지합니다.
 */

/** REST API 기본 수신 포트. 80은 CAP_NET_BIND_SERVICE 필요 (포트 8080 → 80 전환 완료) */
#define PCV_DEFAULT_REST_PORT      80

/** REST API 기본 관리자 사용자명. Web UI 로그인에 사용 */
#define PCV_DEFAULT_ADMIN_USER     "admin"

/** REST API 관리자 비밀번호 기본값. 공개 배포 안전을 위해 내장 비밀번호를 제공하지 않음 */
#define PCV_DEFAULT_ADMIN_PASSWORD ""

/** JWT 서명 키 기본값. 빈 문자열 = pcv_jwt_init()에서 /dev/urandom 랜덤 키 생성 */
#define PCV_DEFAULT_JWT_SECRET     ""   /* 빈 문자열 = 랜덤 생성 */

/** REST API 수신 포트 조회. rest_server.c에서 SoupServer 바인딩에 사용 */
gint         pcv_config_get_rest_port(void);

/** REST API 관리자 사용자명 조회. /auth/token 엔드포인트에서 인증에 사용 */
const gchar *pcv_config_get_admin_user(void);

/** REST API 관리자 비밀번호 조회. /auth/token 엔드포인트에서 비밀번호 검증 */
const gchar *pcv_config_get_admin_password(void);

/** JWT 서명 키 조회. pcv_jwt_init()에 전달하여 HMAC-SHA256 키로 사용 */
const gchar *pcv_config_get_jwt_secret(void);

/**
 * 제네릭 설정 getter — 임의 섹션/키 기반 조회
 *
 * 전용 getter가 없는 설정 항목을 각 모듈에서 직접 조회할 때 사용합니다.
 *
 * 사용 예시:
 *   // [cluster] 섹션의 etcd_endpoints 키 조회
 *   pcv_config_get_string("cluster", "etcd_endpoints", "http://127.0.0.1:2379");
 *
 *   // [alert] 섹션의 cpu_warn 키 조회 (정수)
 *   pcv_config_get_int("alert", "cpu_warn", 80);
 */
const gchar *pcv_config_get_string(const gchar *section, const gchar *key, const gchar *def);
gint         pcv_config_get_int(const gchar *section, const gchar *key, gint def);

/** 스토리지 경로 getter — [storage] 섹션 (daemon.conf) */
const gchar *pcv_config_get_zvol_pool(void);       /* default: "pcvpool/vms" */
const gchar *pcv_config_get_container_pool(void);   /* default: "pcvpool/containers" */
const gchar *pcv_config_get_container_path(void);   /* default: "/var/lib/purecvisor/lxc" */
const gchar *pcv_config_get_image_dir(void);        /* default: "/var/lib/libvirt/images" — qcow2 폴백 경로 */
const gchar *pcv_config_get_iso_dirs(void);          /* default: "/pcvpool/iso,/var/lib/libvirt/images,/iso" */

/** 클러스터 SSH 사용자 — [cluster] 섹션 (daemon.conf) */
const gchar *pcv_config_get_ssh_user(void);         /* default: "pcvdev" */

/**
 * pcv_config_reload:
 * daemon.conf를 재파싱하여 비파괴적 설정만 런타임에 반영한다.
 * SIGHUP 핸들러에서 호출. port/socket_path/TLS 등 구조적 설정은 재로드하지 않는다.
 *
 * 재로드 대상: [alert] 섹션 전체, [server] rate_limit, [cluster] etcd_timeout, log_level
 *
 * @return TRUE=재파싱 성공, FALSE=파일 로드 실패
 */
gboolean pcv_config_reload(void);

/* ── 암호화 비밀 관리 (AES-256-GCM) ───────────────────── */

/**
 * pcv_config_get_secret:
 * 시크릿 값 조회 (환경 변수 우선, ENC: 복호화 지원).
 *
 * [우선순위]
 *   1순위: 환경 변수 PCV_SECRET_<SECTION>_<KEY> (대문자, - → _)
 *          예: jwt_secret → PCV_SECRET_AUTH_JWT_SECRET
 *   2순위: daemon.conf [section] key ("ENC:" 접두사 시 AES-256-GCM 복호화)
 *   3순위: fallback 기본값
 *
 * @param group    INI 섹션명 (예: "iscsi", "ai", "auth")
 * @param key      키 이름 (예: "chap_password", "claude_api_key", "jwt_secret")
 * @param fallback 설정 파일에 없을 때 반환할 기본값
 * @return 복호화된 문자열 (호출자가 g_free() 해야 함). 실패 시 fallback 복사본 반환.
 */
[[nodiscard]] gchar *pcv_config_get_secret(const gchar *group, const gchar *key,
                              const gchar *fallback);

/**
 * pcv_config_encrypt_value:
 * 평문을 AES-256-GCM으로 암호화하고 "ENC:" 접두사가 붙은 base64 문자열을 반환합니다.
 * CLI 도구(pcvctl config encrypt)에서 사용합니다.
 *
 * @param plaintext 암호화할 평문
 * @return "ENC:base64..." 형식 문자열 (호출자가 g_free() 해야 함). 실패 시 NULL.
 */
gchar *pcv_config_encrypt_value(const gchar *plaintext);
