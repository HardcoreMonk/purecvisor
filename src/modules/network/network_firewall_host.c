/**
 * @file network_firewall_host.c
 * @brief 호스트 방화벽(UFW/iptables-DROP) 자동 공존 구현 (VP-6)
 *
 * ====================================================================
 * 책임:
 *   호스트가 이미 운영 중인 방화벽 정책(UFW 또는 iptables FORWARD DROP)과
 *   purecvisor 관리형 NAT 네트워크가 공존하도록, 게스트 포워딩 및 호스트
 *   dnsmasq 로의 DHCP(67)/DNS(53) 경로를 호스트 방화벽에 자동으로 뚫는다.
 *   자기 nftables 테이블 관리는 network_firewall.c 소관이고, 이 파일은
 *   "남의 방화벽"과의 공존만 맡는다.
 *
 * 직접 만지는 외부 시스템:
 *   - ufw            : route allow / allow 룰 (영구, ufw 가 중복 자동 스킵)
 *   - iptables(-nft) : FORWARD/INPUT -I/-D (비영구 → 재부팅 시 소멸, -C 로 멱등)
 *   - /etc/ufw/ufw.conf, firewall-cmd(감지만)
 *   외부 명령은 모두 pcv_spawn 을 경유한다 (셸 해석 없음 — 인젝션 방지).
 *
 * 읽는 순서 / entry point:
 *   pcv_host_fw_detect()  → 상태 판정
 *   pcv_host_fw_plan()    → 상태별 명령 목록 생성 (순수)
 *   _host_fw_apply()      → 공통 executor (detect→plan→-C가드→spawn)
 *   pcv_host_fw_integrate()/_remove() → _host_fw_apply 얇은 래퍼
 *
 * 실패 시 사용자 영향:
 *   개별 명령 실패는 soft(WARN 후 계속)이며, 하나라도 실패하면 전체 FALSE.
 *   setup_nat 배선부는 이 반환값을 무시하므로(soft) NAT 자체는 계속 뜬다.
 *   다만 공존 룰이 안 걸리면 게스트가 DHCP 주소를 못 받거나 인터넷 불통.
 *
 * 관련 문서 / 검증:
 *   docs/superpowers/specs/2026-07-06-vp6-host-firewall-coexist-design.md
 *   유닛(plan 순수 함수): ./test_runner -p /network/host_fw_plan_ufw_add
 * ====================================================================
 */
#include <glib.h>
#include <gio/gio.h>
#include <string.h>

#include "modules/network/network_firewall_host.h"
#include "../../utils/pcv_spawn.h"
#include "../../utils/pcv_validate.h"
#include "../../utils/pcv_log.h"

#define HOST_FW_LOG_DOM "network_fw_host"

/* 상태 → 감사 로그용 문자열 */
static const gchar *_state_name(PcvHostFwState st) {
    switch (st) {
    case PCV_HOST_FW_UFW:           return "ufw";
    case PCV_HOST_FW_IPTABLES_DROP: return "iptables_drop";
    case PCV_HOST_FW_FIREWALLD:     return "firewalld";
    case PCV_HOST_FW_OPEN:          return "open";
    default:                        return "unknown";
    }
}

/* ── UFW 활성 판정: /etc/ufw/ufw.conf 의 ENABLED=yes (주석줄 제외) ──
 *
 * ufw.conf 는 `KEY=VALUE` 라인 포맷. `#` 로 시작하는(선행 공백 허용) 줄은
 * 주석이므로 제외한다. 값 앞뒤 공백은 관용적으로 허용하고 대소문자 무시.
 * 파일이 없거나 읽기 실패면 "비활성"으로 본다 (개입 불요 쪽 안전 폴백). */
static gboolean _ufw_enabled(void) {
    gchar *content = NULL;
    if (!g_file_get_contents("/etc/ufw/ufw.conf", &content, NULL, NULL))
        return FALSE;

    gboolean enabled = FALSE;
    gchar **lines = g_strsplit(content, "\n", -1);
    for (gchar **l = lines; *l; l++) {
        gchar *s = g_strstrip(g_strdup(*l));
        if (s[0] == '#' || s[0] == '\0') { g_free(s); continue; }
        if (g_str_has_prefix(s, "ENABLED")) {
            const gchar *eq = strchr(s, '=');
            if (eq) {
                gchar *val = g_strstrip(g_strdup(eq + 1));
                if (g_ascii_strcasecmp(val, "yes") == 0) enabled = TRUE;
                g_free(val);
            }
        }
        g_free(s);
    }
    g_strfreev(lines);
    g_free(content);
    return enabled;
}

/* ── iptables FORWARD 기본 정책 DROP 판정 ──
 *
 * `iptables -S FORWARD` 첫 줄이 `-P FORWARD DROP` 이면 기본 정책이 DROP.
 * 스폰 실패(iptables 미설치/권한 등)는 crash 없이 FALSE(=DROP 아님)로 폴백. */
static gboolean _iptables_forward_drop(void) {
    const gchar *argv[] = {"iptables", "-S", "FORWARD", NULL};
    gchar *out = NULL;
    if (!pcv_spawn_sync(argv, &out, NULL, NULL) || !out) {
        g_free(out);
        return FALSE;
    }
    gboolean drop = FALSE;
    gchar **lines = g_strsplit(out, "\n", -1);
    if (lines[0]) {
        gchar *first = g_strstrip(g_strdup(lines[0]));
        drop = (g_strcmp0(first, "-P FORWARD DROP") == 0);
        g_free(first);
    }
    g_strfreev(lines);
    g_free(out);
    return drop;
}

/* ── firewalld 활성 판정 (감지만; 실개입 비범위) ──
 *
 * firewall-cmd 바이너리 존재 + `firewall-cmd --state` 가 running 이면 exit 0.
 * 스폰 실패/미설치는 비활성으로 본다. */
static gboolean _firewalld_active(void) {
    gchar *path = g_find_program_in_path("firewall-cmd");
    if (!path) return FALSE;
    g_free(path);
    const gchar *argv[] = {"firewall-cmd", "--state", NULL};
    gboolean running = pcv_spawn_sync(argv, NULL, NULL, NULL);
    return running;
}

PcvHostFwState pcv_host_fw_detect(void) {
    /* 1. UFW: 바이너리 존재 + ENABLED=yes */
    gchar *ufw = g_find_program_in_path("ufw");
    if (ufw) {
        gboolean en = _ufw_enabled();
        g_free(ufw);
        if (en) return PCV_HOST_FW_UFW;
    }
    /* 2. (UFW 아님) iptables FORWARD 기본 정책 DROP */
    if (_iptables_forward_drop())
        return PCV_HOST_FW_IPTABLES_DROP;
    /* 3. (둘 다 아님) firewalld 활성 — 감지·경고만 */
    if (_firewalld_active())
        return PCV_HOST_FW_FIREWALLD;
    /* 4. 그 외: 개입 불요 */
    return PCV_HOST_FW_OPEN;
}

/* ── plan: 상태별 실행 명령 문자열 목록 생성 (순수 함수) ──
 *
 * 각 요소는 "공백으로 조인한 명령 문자열"이며, executor 가 공백 split 후
 * pcv_spawn 으로 실행한다(모든 토큰에 공백이 없음이 전제 — bridge 는 검증됨).
 * iptables 의 -C 존재 검사는 여기 넣지 않는다(executor 책임). */
GPtrArray *pcv_host_fw_plan(PcvHostFwState st, const gchar *bridge, gboolean remove) {
    GPtrArray *cmds = g_ptr_array_new_with_free_func(g_free);
    if (!bridge) return cmds;   /* 방어: NULL bridge → 빈 목록 */

    switch (st) {
    case PCV_HOST_FW_UFW:
        /* 인터페이스 전체 allow 3종 (libvirt 동급, 포트 스코프 아님).
         * remove 는 `--force delete` 로 대칭 (ufw 대화형 확인 회피). */
        if (!remove) {
            g_ptr_array_add(cmds, g_strdup_printf("ufw route allow in on %s",  bridge));
            g_ptr_array_add(cmds, g_strdup_printf("ufw route allow out on %s", bridge));
            g_ptr_array_add(cmds, g_strdup_printf("ufw allow in on %s",        bridge));
        } else {
            g_ptr_array_add(cmds, g_strdup_printf("ufw --force delete route allow in on %s",  bridge));
            g_ptr_array_add(cmds, g_strdup_printf("ufw --force delete route allow out on %s", bridge));
            g_ptr_array_add(cmds, g_strdup_printf("ufw --force delete allow in on %s",        bridge));
        }
        break;

    case PCV_HOST_FW_IPTABLES_DROP: {
        /* FORWARD 2종(정방향 accept + 역방향 conntrack) + INPUT 3종(DHCP67/DNS53). */
        const gchar *op = remove ? "-D" : "-I";
        g_ptr_array_add(cmds, g_strdup_printf(
            "iptables %s FORWARD -i %s -j ACCEPT", op, bridge));
        g_ptr_array_add(cmds, g_strdup_printf(
            "iptables %s FORWARD -o %s -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT", op, bridge));
        g_ptr_array_add(cmds, g_strdup_printf(
            "iptables %s INPUT -i %s -p udp --dport 67 -j ACCEPT", op, bridge));
        g_ptr_array_add(cmds, g_strdup_printf(
            "iptables %s INPUT -i %s -p udp --dport 53 -j ACCEPT", op, bridge));
        g_ptr_array_add(cmds, g_strdup_printf(
            "iptables %s INPUT -i %s -p tcp --dport 53 -j ACCEPT", op, bridge));
        break;
    }

    case PCV_HOST_FW_OPEN:
    case PCV_HOST_FW_FIREWALLD:
    default:
        /* 개입 불요 / 실개입 비범위 → 빈 목록 */
        break;
    }
    return cmds;
}

/* ── iptables 룰 존재 검사(-C 가드) ──
 *
 * argv 는 g_strsplit 로 분해된 iptables 명령이며 argv[1] 이 "-I"/"-D" 이다.
 * 이를 잠시 "-C" 로 바꿔 존재 검사를 수행한다(exit 0 이면 룰 존재).
 * argv 는 호출자가 소유/해제하며 이 함수는 원상복구 후 반환한다. */
static gboolean _iptables_rule_exists(gchar **argv) {
    gchar *saved = argv[1];
    argv[1] = (gchar *) "-C";               /* 스택 리터럴 — free 대상 아님 */
    gboolean exists = pcv_spawn_sync((const gchar * const *) argv, NULL, NULL, NULL);
    argv[1] = saved;                        /* 원상복구 (g_strfreev 정합) */
    return exists;
}

/* ── 공통 executor: detect → plan → (-C 가드) → spawn ──
 *
 * Developer note:
 *   개별 명령 실패는 soft — WARN 후 계속하되 하나라도 실패하면 all_ok=FALSE.
 *   iptables 는 -C 가드로 멱등: add 는 이미 있으면 skip, remove 는 없으면 skip.
 *   ufw 는 자체 중복 스킵이 있어 -C 가드 없이 그대로 실행한다.
 *   반환 후 감사 로그(op/target/state/명령수)를 남긴다 — "데몬이 호스트
 *   방화벽을 수정했다"는 추적은 규정상 필수.
 *
 * Operator note:
 *   이 경로는 "게스트가 인터넷과 DHCP를 쓸 수 있도록 호스트 방화벽에 구멍을
 *   내는" 자동 개입 지점이다. UFW 룰은 영구(재부팅 후에도 남음), iptables
 *   룰은 비영구(재부팅 시 사라지며 브릿지 재생성 경로에서 재적용)이다.
 *   firewalld 호스트는 자동 개입 대상이 아니므로 경고만 남기고 건너뛴다 —
 *   그 경우 운영자가 직접 firewalld zone 에 브릿지를 등록해야 한다.
 */
static gboolean _host_fw_apply(const gchar *bridge, gboolean remove, GError **error) {
    const gchar *op_name = remove ? "network.host_fw_remove"
                                  : "network.host_fw_integrate";

    /* [보안] bridge_name 인젝션 방어: [a-zA-Z0-9_-] 이외 문자 차단.
     * (plan 문자열을 공백 split 해 argv 로 쓰므로 공백/메타문자 유입 차단 필수) */
    if (!pcv_validate_bridge_name(bridge)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Invalid bridge name for host firewall: %s",
                    bridge ? bridge : "(null)");
        return FALSE;
    }

    PcvHostFwState st = pcv_host_fw_detect();

    /* firewalld: 실개입 비범위 — 경고 + audit(result=skipped) 후 성공 반환 */
    if (st == PCV_HOST_FW_FIREWALLD) {
        PCV_LOG_WARN(HOST_FW_LOG_DOM,
                     "firewalld 활성 호스트 — 자동 개입 비범위. 브릿지 '%s' 를 "
                     "firewalld zone 에 수동 등록 필요 (예: firewall-cmd "
                     "--zone=trusted --add-interface=%s --permanent && "
                     "firewall-cmd --reload)", bridge, bridge);
        PCV_LOG_AUDIT(HOST_FW_LOG_DOM, op_name, bridge,
                      "state=firewalld result=skipped (manual firewalld zone required)");
        return TRUE;
    }

    /* OPEN: 개입 불요 — 디버그만 (감사 소음 회피) */
    if (st == PCV_HOST_FW_OPEN) {
        PCV_LOG_DEBUG(HOST_FW_LOG_DOM,
                      "host firewall open — no coexistence rules needed (%s %s)",
                      op_name, bridge);
        return TRUE;
    }

    /* UFW / IPTABLES_DROP: 실제 개입 */
    GPtrArray *cmds = pcv_host_fw_plan(st, bridge, remove);
    guint total = cmds->len, applied = 0, skipped = 0, failed = 0;
    gboolean all_ok = TRUE;

    for (guint i = 0; i < total; i++) {
        gchar **argv = g_strsplit(g_ptr_array_index(cmds, i), " ", -1);

        /* iptables 만 -C 가드로 멱등화 (ufw 는 자체 중복 스킵) */
        if (g_strcmp0(argv[0], "iptables") == 0) {
            gboolean exists = _iptables_rule_exists(argv);
            if (!remove && exists) {          /* add: 이미 있음 → skip */
                skipped++; g_strfreev(argv); continue;
            }
            if (remove && !exists) {          /* remove: 이미 없음 → skip */
                skipped++; g_strfreev(argv); continue;
            }
        }

        GError *cmd_err = NULL;
        if (!pcv_spawn_sync((const gchar * const *) argv, NULL, NULL, &cmd_err)) {
            /* soft: 개별 실패는 경고 후 계속 (전체 결과에는 반영) */
            PCV_LOG_WARN(HOST_FW_LOG_DOM,
                         "host firewall 명령 실패 (계속): '%s' — %s",
                         (const gchar *) g_ptr_array_index(cmds, i),
                         cmd_err ? cmd_err->message : "unknown");
            g_clear_error(&cmd_err);
            failed++;
            all_ok = FALSE;
        } else {
            applied++;
        }
        g_strfreev(argv);
    }
    g_ptr_array_unref(cmds);

    PCV_LOG_AUDIT(HOST_FW_LOG_DOM, op_name, bridge,
                  "state=%s total=%u applied=%u skipped=%u failed=%u result=%s",
                  _state_name(st), total, applied, skipped, failed,
                  all_ok ? "ok" : "partial");

    if (!all_ok) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "host firewall %s partial failure (%u of %u commands failed)",
                    op_name, failed, total);
        return FALSE;
    }
    return TRUE;
}

gboolean pcv_host_fw_integrate(const gchar *bridge, GError **error) {
    return _host_fw_apply(bridge, FALSE, error);
}

gboolean pcv_host_fw_remove(const gchar *bridge, GError **error) {
    return _host_fw_apply(bridge, TRUE, error);
}
