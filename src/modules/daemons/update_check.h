#ifndef PURECVISOR_UPDATE_CHECK_H
#define PURECVISOR_UPDATE_CHECK_H
/**
 * @file update_check.h
 * @brief 버전 알림 — GitHub 최신 릴리스 조회+캐시+semver 비교 (읽기 전용, 정보 표시).
 *        아웃바운드는 pcv_ssrf 가드 + NO_REDIRECT. hot_reload 미연결.
 */
#include <glib.h>

typedef struct {
    gboolean enabled;          /* config [update] check_enabled */
    char     current[32];      /* PCV_PRODUCT_VERSION */
    char     latest[32];       /* 최신 릴리스 버전(정규화), unknown이면 "" */
    char     url[256];         /* 검증된 html_url, 없으면 "" */
    gboolean update_available; /* latest > current */
    gint64   checked_at;       /* 마지막 성공 조회 epoch(초), 0=미조회 */
    char     state[16];        /* "ok" | "unknown" | "disabled" */
} PcvUpdateStatus;

/** config 로드 + 캐시 초기화. main.c 부팅 시 1회. */
void pcv_update_check_init(void);

/** 캐시된 상태 복사 반환. enabled && stale(>=interval) && !in-flight면 백그라운드 갱신을
 *  single-flight로 트리거(블록 안 함). */
PcvUpdateStatus pcv_update_check_get(void);

/** 순수 semver 비교. 둘 다 major.minor.patch 파싱되면 TRUE + *update_available.
 *  하나라도 파싱 실패면 FALSE(unknown). 선행 'v' 허용. pre-release/빌드메타는 코어 3-튜플만. */
gboolean pcv_update_check_compare(const char *current, const char *latest,
                                  gboolean *update_available);

/** GitHub releases/latest JSON 파싱. tag_name(semver)·html_url(github.com) 추출·검증.
 *  성공 TRUE + *tag_out(정규화, 호출자 free)·*url_out(유효 URL 또는 nullptr, 호출자 free). */
gboolean pcv_update_check_parse_release(const char *json, gssize len,
                                        char **tag_out, char **url_out);

#endif /* PURECVISOR_UPDATE_CHECK_H */
