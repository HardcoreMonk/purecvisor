/**
 * @file pcv_rbac.h
 * @brief RBAC(역할 기반 접근 제어) + 멀티테넌트 인증 모듈 공개 헤더
 *
 * [파일 역할]
 *   PureCVisor 인증/인가 시스템의 공개 인터페이스.
 *   SQLite 기반 사용자 관리, 3단계 누적 역할(VIEWER/OPERATOR/ADMIN),
 *   선택적 테넌트 격리, SHA256 비밀번호 해싱, JWT 토큰 발급을 제공합니다.
 *
 * [아키텍처 위치]
 *   main.c         -> pcv_rbac_init() / pcv_rbac_shutdown()
 *   rest_server.c  -> pcv_rbac_authenticate() (POST /auth/token -> JWT 발급)
 *   dispatcher.c   -> pcv_rbac_check_permission() (모든 RPC 처리 전 권한 검사)
 *   handler_auth.c -> pcv_rbac_user_create/delete/list/set_role() (auth.* RPC)
 *
 * [주요 자료구조]
 *   PcvRole — 역할 열거형 (VIEWER=0, OPERATOR=1, ADMIN=2, 누적 모델)
 *   PcvUser — 사용자 정보 (username, role, tenant), 비밀번호 미포함
 *
 * [사용 예시 (main.c 기준)]
 *   pcv_rbac_init("/var/lib/purecvisor/rbac.db");
 *   gchar *token = pcv_rbac_authenticate("admin", "password", &err);
 *   gboolean ok  = pcv_rbac_check_permission("admin", "vm.delete");
 *   pcv_rbac_shutdown();
 *
 * [메모리 관리]
 *   - pcv_rbac_authenticate() 반환 JWT: 호출자가 g_free()
 *   - pcv_rbac_user_list() 반환 GPtrArray: 호출자가 g_ptr_array_unref()
 *   - pcv_rbac_get_tenant() 반환값: TLS 버퍼이므로 g_free() 금지
 *   - PcvUser: pcv_user_free()로 해제
 *
 * [주의사항]
 *   - pcv_rbac_init()은 pcv_config_init() + pcv_jwt_init() 이후에 호출
 *   - 모든 함수는 스레드 안전 (내부 GMutex 보호)
 *   - DB 경로 기본값: /var/lib/purecvisor/rbac.db
 */

#ifndef PCV_RBAC_H
#define PCV_RBAC_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * PcvRole:
 * 역할 열거형. 숫자가 클수록 높은 권한을 가집니다.
 * 권한 확인: user_role >= method_min_role → 허용
 *
 * 누적 모델이므로 ADMIN은 OPERATOR/VIEWER 권한을 모두 포함합니다.
 */
typedef enum {
    PCV_ROLE_VIEWER   = 0,   /* 읽기 전용: *.list, *.metrics, monitor.*, telemetry.* */
    PCV_ROLE_OPERATOR = 1,   /* VIEWER + 운영: start/stop/create, own VM actions */
    PCV_ROLE_ADMIN    = 2    /* 전체: auth.*, 전역 delete, failover, 설정 변경 등 */
} PcvRole;

/**
 * PcvUser:
 * 사용자 정보 구조체 — pcv_rbac_user_list()의 반환 원소 타입.
 * 비밀번호 해시/salt는 보안상 포함하지 않습니다.
 */
typedef struct {
    gchar  *username;        /* 로그인 ID (동적 할당, g_free 필요) */
    PcvRole role;            /* 역할 (VIEWER/OPERATOR/ADMIN) */
    gchar  *tenant;          /* 테넌트 격리 키 (NULL = 전체 접근, 동적 할당) */
} PcvUser;

/* ── 초기화 / 종료 ────────────────────────────────────────── */

/**
 * pcv_rbac_init:
 * @db_path: SQLite DB 경로 (NULL이면 /var/lib/purecvisor/rbac.db)
 *
 * RBAC DB를 열고(없으면 생성) users 테이블을 보장합니다.
 * 관리자 사용자가 없으면 daemon.conf 설정으로 자동 생성합니다.
 * main.c에서 pcv_config_init() 및 pcv_jwt_init() 이후 1회 호출.
 */
void pcv_rbac_init(const gchar *db_path);

/**
 * pcv_rbac_shutdown:
 * SQLite 연결을 닫고 내부 상태를 해제합니다.
 */
void pcv_rbac_shutdown(void);

/* ── 사용자 CRUD ──────────────────────────────────────────── */

/**
 * pcv_rbac_user_create:
 * @username: 고유 사용자 이름
 * @password: 평문 비밀번호 (내부에서 SHA256 해싱)
 * @role:     PcvRole 열거형 (VIEWER/OPERATOR/ADMIN)
 * @tenant:   테넌트 이름 (NULL이면 전체 접근)
 * @error:    GError 반환
 *
 * Returns: 성공 시 TRUE, 중복/DB 오류 시 FALSE
 */
gboolean pcv_rbac_user_create(const gchar *username,
                              const gchar *password,
                              PcvRole      role,
                              const gchar *tenant,
                              GError     **error);

/**
 * pcv_rbac_user_delete:
 * @username: 삭제할 사용자
 * @error:    GError 반환
 *
 * Returns: 성공 시 TRUE, 사용자 미존재/DB 오류 시 FALSE
 */
gboolean pcv_rbac_user_delete(const gchar *username, GError **error);

/**
 * pcv_rbac_user_list:
 * Returns: (transfer full): GPtrArray of PcvUser*.
 *   호출자가 g_ptr_array_unref()로 해제해야 합니다.
 */
GPtrArray *pcv_rbac_user_list(void);

/**
 * pcv_rbac_user_set_role:
 * @username: 대상 사용자
 * @role:     새 역할
 * @error:    GError 반환
 *
 * Returns: 성공 시 TRUE
 */
gboolean pcv_rbac_user_set_role(const gchar *username,
                                PcvRole      role,
                                GError     **error);

/**
 * pcv_rbac_change_password:
 * 본인 비밀번호 변경. old_password 검증 후 새 salt+hash로 교체.
 * 성공 시 모든 활성 세션 자동 무효화.
 * 부트스트랩 admin은 사용 불가 (daemon.conf 직접 수정).
 */
gboolean pcv_rbac_change_password(const gchar *username,
                                  const gchar *old_password,
                                  const gchar *new_password,
                                  GError     **error);

/* ── 인증 ─────────────────────────────────────────────────── */

/**
 * pcv_rbac_authenticate:
 * @username: 사용자 이름
 * @password: 평문 비밀번호
 * @error:    GError 반환
 *
 * RBAC DB에서 자격 증명을 검증합니다.
 * 성공 시 pcv_jwt_sign()으로 JWT 토큰을 발급합니다 (subject=username).
 *
 * Returns: (transfer full): JWT 토큰 문자열 (g_free 필요), 실패 시 NULL
 */
gchar *pcv_rbac_authenticate(const gchar *username,
                             const gchar *password,
                             GError     **error);

/**
 * pcv_rbac_authenticate_v2:
 * @username:          사용자 이름
 * @password:          평문 비밀번호
 * @out_refresh_token: (out)(optional): refresh token 반환 (g_free 필요), NULL이면 미생성
 * @error:             GError 반환
 *
 * pcv_rbac_authenticate()의 확장 버전.
 * 인증 성공 시 access_token (JWT, 15분) + refresh_token (랜덤 64hex, 7일) 반환.
 * refresh_token은 SHA256 해시로 sessions 테이블에 저장됩니다.
 *
 * Returns: (transfer full): JWT access_token (g_free 필요), 실패 시 NULL
 */
gchar *pcv_rbac_authenticate_v2(const gchar *username,
                                const gchar *password,
                                gchar      **out_refresh_token,
                                GError     **error);

/* ── 세션 관리 (Refresh Token) ───────────────────────────── */

/**
 * pcv_rbac_refresh_token:
 * @refresh_token: 클라이언트가 보유한 refresh token (64 hex chars)
 * @out_new_refresh: (out)(optional): 회전된 새 refresh token (g_free 필요)
 * @error:         GError 반환
 *
 * refresh token을 검증하고 새 access_token을 발급합니다.
 * 기존 refresh token은 무효화(revoke)하고 새 것을 발급합니다 (토큰 회전).
 *
 * Returns: (transfer full): 새 JWT access_token (g_free 필요), 실패 시 NULL
 */
gchar *pcv_rbac_refresh_token(const gchar *refresh_token,
                              gchar      **out_new_refresh,
                              GError     **error);

/**
 * pcv_rbac_revoke_session:
 * @username: 세션을 무효화할 사용자
 * @error:    GError 반환
 *
 * 해당 사용자의 모든 활성 세션(refresh token)을 무효화합니다.
 * 로그아웃, 비밀번호 변경, 보안 사고 시 사용됩니다.
 *
 * Returns: 성공 시 TRUE
 */
gboolean pcv_rbac_revoke_session(const gchar *username, GError **error);

/**
 * pcv_rbac_cleanup_expired_sessions:
 *
 * 만료되거나 무효화된 세션을 DB에서 삭제합니다.
 * 주기적으로 호출하여 sessions 테이블 비대화를 방지합니다.
 *
 * Returns: 삭제된 행 수
 */
gint pcv_rbac_cleanup_expired_sessions(void);

/**
 * pcv_rbac_list_sessions:
 * @username: 세션을 조회할 사용자
 *
 * 해당 사용자의 활성 세션 목록을 반환합니다.
 *
 * Returns: (transfer full): JsonArray of {id, created_at, expires_at}.
 *   호출자가 json_array_unref()로 해제.
 */
JsonArray *pcv_rbac_list_sessions(const gchar *username);

/**
 * pcv_rbac_revoke_session_by_id:
 * @username:   세션 소유자 (권한 확인용)
 * @session_id: 해지할 세션 ID
 *
 * 특정 세션을 개별적으로 무효화합니다.
 *
 * Returns: 성공 시 TRUE
 */
gboolean pcv_rbac_revoke_session_by_id(const gchar *username,
                                       gint64       session_id);

/* ── 권한 확인 ────────────────────────────────────────────── */

/**
 * pcv_rbac_check_permission:
 * @username: 인증된 사용자
 * @method:   RPC 메서드 이름 (예: "vm.delete")
 *
 * Returns: 사용자의 역할이 해당 메서드를 허용하면 TRUE
 */
[[nodiscard]] gboolean pcv_rbac_check_permission(const gchar *username,
                                   const gchar *method);

/**
 * pcv_rbac_get_role:
 * Returns: 사용자의 PcvRole, 미존재 시 PCV_ROLE_VIEWER (최소 권한)
 */
PcvRole pcv_rbac_get_role(const gchar *username);

/**
 * pcv_rbac_get_tenant:
 * Returns: (transfer none): 테넌트 문자열 (TLS 버퍼, g_free 금지), 전체 접근 시 NULL
 */
const gchar *pcv_rbac_get_tenant(const gchar *username);

/* ── API Key 인증 (CI/자동화용 프로그래밍 방식 접근) ──────── */

/**
 * pcv_rbac_create_api_key:
 * @username:    키를 소유할 사용자
 * @description: 키 설명 (용도 메모)
 * @expires_days: 만료일 수 (1~365)
 * @error:       GError 반환
 *
 * 68자 API 키("pcv_" + 64 hex)를 생성합니다. SHA256 해시만 DB에 저장되며,
 * 평문 키는 이 함수 반환값으로만 1회 확인 가능합니다.
 *
 * Returns: (transfer full): 평문 API 키 (g_free 필요), 실패 시 NULL
 */
gchar *pcv_rbac_create_api_key(const gchar *username,
                               const gchar *description,
                               gint         expires_days,
                               GError     **error);

/**
 * pcv_rbac_revoke_api_key:
 * @key_prefix: API 키의 처음 12자 이상 (해시 조회용)
 * @error:      GError 반환
 *
 * Returns: 성공 시 TRUE
 */
gboolean pcv_rbac_revoke_api_key(const gchar *key_prefix,
                                 GError     **error);

/**
 * pcv_rbac_verify_api_key:
 * @api_key: 전체 API 키 문자열 ("pcv_..." 68자)
 * @error:   GError 반환
 *
 * 키를 SHA256 해시하여 DB 조회. 유효하면 소유자 username을 반환합니다.
 *
 * Returns: (transfer full): 사용자명 (g_free 필요), 실패 시 NULL
 */
gchar *pcv_rbac_verify_api_key(const gchar *api_key,
                               GError     **error);

/**
 * pcv_rbac_list_api_keys:
 * @username: 필터할 사용자 (NULL이면 전체)
 *
 * Returns: (transfer full): JsonArray of {key_prefix, username, description,
 *          created_at, expires_at, revoked}. 호출자가 json_array_unref().
 */
JsonArray *pcv_rbac_list_api_keys(const gchar *username);

/* ── 브루트포스 방어 ─────────────────────────────────────── */

/**
 * pcv_rbac_is_locked:
 * @username: 사용자 이름
 *
 * 해당 사용자가 로그인 시도 초과로 잠겨있는지 확인합니다.
 *
 * Returns: TRUE 잠김, FALSE 잠기지 않음
 */
gboolean pcv_rbac_is_locked(const gchar *username);

/**
 * pcv_rbac_get_remaining_lockout:
 * @username: 사용자 이름
 *
 * 남은 잠금 시간(초)을 반환합니다. 잠기지 않은 경우 0.
 *
 * Returns: 남은 잠금 시간 (초), 잠기지 않으면 0
 */
gint pcv_rbac_get_remaining_lockout(const gchar *username);

/**
 * B6-M1: IP-based brute force defense — credential stuffing 방어.
 * per-IP 20회 실패 → 5분 잠금. REST 서버에서 호출.
 */
gint pcv_rbac_get_ip_remaining_lockout(const gchar *ip);
void pcv_rbac_ip_record_auth_failure(const gchar *ip);
void pcv_rbac_ip_record_auth_success(const gchar *ip);

/* ── 유틸리티 ─────────────────────────────────────────────── */

/** 역할 → 문자열 변환 ("viewer"/"operator"/"admin") */
const gchar *pcv_rbac_role_to_str(PcvRole role);

/** 문자열 → 역할 변환 (대소문자 무시, 미인식 시 VIEWER) */
PcvRole      pcv_rbac_str_to_role(const gchar *str);

/** PcvUser 구조체 해제 (GPtrArray free_func으로 사용) */
void pcv_user_free(PcvUser *u);

/* ── [백엔드 4차] API Key 관리 ─────────────────────────────── */
gboolean   pcv_rbac_apikey_create(const gchar *client_name, PcvRole role, gchar **out_key, GError **error);
gint       pcv_rbac_apikey_validate(const gchar *api_key);
JsonArray *pcv_rbac_apikey_list(void);
gboolean   pcv_rbac_apikey_revoke(const gchar *client_name, GError **error);

/* ── 세션 블랙리스트 ───────────────────────────────────────── */
void       pcv_rbac_session_revoke(const gchar *jti);
gboolean   pcv_rbac_session_is_revoked(const gchar *jti);

/* ── 권한 캐싱 ─────────────────────────────────────────────── */
void pcv_rbac_perm_cache_init(void);
void pcv_rbac_perm_cache_invalidate(const gchar *username);
gint pcv_rbac_perm_cache_check(const gchar *username, const gchar *method);
void pcv_rbac_perm_cache_set(const gchar *username, const gchar *method, gboolean allowed);

/* ── Per-user Rate Limiting ────────────────────────────────── */
gboolean pcv_rbac_check_user_rate(const gchar *username);

/* ── Per-user 스토리지 쿼터 (BE-A9) ──────────────────────── */

/**
 * pcv_rbac_check_quota:
 * @username:          사용자 이름
 * @current_vm_count:  현재 VM 수
 *
 * 쿼터 이내이면 TRUE, 초과 시 FALSE. quota=0은 무제한.
 */
gboolean pcv_rbac_check_quota(const gchar *username, gint current_vm_count);

/**
 * pcv_rbac_set_quota:
 * @username:    사용자 이름
 * @vm_count:    VM 한도 (0 = 무제한)
 * @storage_gb:  스토리지 한도 GB (0 = 무제한)
 */
gboolean pcv_rbac_set_quota(const gchar *username, gint vm_count, gint storage_gb);

/* ── API Key 만료 임박 경고 (BE-A10) ─────────────────────── */

/**
 * pcv_rbac_get_expiring_api_keys:
 * @days_threshold: 만료까지 남은 일수 이내인 키 조회
 *
 * Returns: (transfer full): JsonArray of {key_prefix, username, expires_at}
 */
JsonArray *pcv_rbac_get_expiring_api_keys(gint days_threshold);

G_END_DECLS

#endif /* PCV_RBAC_H */
