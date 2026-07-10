/**
 * @file security_group_nft.h
 * @brief 보안 그룹 nft 룰셋 스크립트 순수 빌더 (spawn/sqlite 무의존)
 *
 * [설계 배경 — 2026-07-04 gti12 장애]
 *   구설계는 inet purecvisor 에 hook input/output base chain + 무조건 drop 을
 *   설치해 호스트 전체 네트워크를 다운시켰다. 신설계는 table bridge pcv_sg 의
 *   디스패치 체인(policy accept)에서 바인딩된 vnet 에만 jump/drop 을 건다.
 *   spec: docs/superpowers/specs/2026-07-04-security-group-scoped-nft-design.md
 *
 * [순수성 계약]
 *   이 모듈은 문자열 생성만 한다. 외부 프로세스 실행/파일 IO/전역 상태 금지.
 *   따라서 root 없이 단위 테스트 가능하다 (tests/test_sg_nft_builder.c).
 */
#pragma once
#include <glib.h>

typedef struct {
    const gchar *direction;   /* "ingress" | "egress" (검증 완료 전제) */
    const gchar *protocol;    /* "tcp" | "udp" | "icmp" */
    gint         port_start;  /* 0 = 포트 매칭 없음 */
    gint         port_end;    /* 0 = port_start 단일 포트 */
    const gchar *source;      /* CIDR. "0.0.0.0/0" = any (주소 매칭 생략) */
} SgNftRule;

typedef struct {
    const gchar *vnet;            /* 예: "vnet3" */
    GPtrArray   *groups;          /* const gchar* 그룹명 (바인딩 순서) */
    gboolean     egress_enforced; /* 바인딩 그룹 중 egress 규칙 보유 여부 */
} SgNftBinding;

gchar *pcv_sg_nft_build_ensure_script(void);
gchar *pcv_sg_nft_build_group_script(const gchar *group, GPtrArray *rules);
gchar *pcv_sg_nft_build_group_delete_script(const gchar *group);
gchar *pcv_sg_nft_build_dispatch_script(GPtrArray *bindings);
