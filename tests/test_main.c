/* tests/test_main.c
 *
 * ============================================================================
 *  테스트 러너 진입점 -- GLib g_test 프레임워크
 * ============================================================================
 *  이 파일은 모든 테스트 파일을 등록하고 실행하는 단일 진입점이다.
 *  정확한 테스트 개수는 실행 시 TAP의 `1..N` 출력과 Makefile 결과를 기준으로 본다.
 *
 *  실행 방법:
 *    sudo ./test_runner -v             # 상세 출력
 *    sudo ./test_runner --tap          # TAP 형식 (CI용)
 *    sudo ./test_runner -p /validate   # 특정 경로만 실행
 *
 *  [M-8b] GLib 2.80 의 `-p` 는 전체 leaf 테스트 경로를 요구한다. `-p /security_group`
 *    같은 부분 prefix 는 0건 매칭 → `-p /security_group/accept_clean` 처럼 full leaf
 *    로 지정하거나, 그게 아니면 필터 없이 전체 스위트로 실행한다.
 *
 *  새 테스트 파일 추가 시:
 *    1. tests/test_xxx.c 작성 (void test_xxx_register(void) 함수 포함)
 *    2. 이 파일에 extern 선언 추가 (void test_xxx_register(void);)
 *    3. main() 안에서 test_xxx_register(); 호출 추가
 *    4. Makefile의 TEST_SRCS에 test_xxx.c 등록
 *
 *  WARNING 처리:
 *    GLib 테스트는 기본적으로 G_LOG_LEVEL_WARNING을 fatal로 처리한다.
 *    그런데 일부 테스트가 의도적으로 에러 경로를 탐색하면서 WARNING을 발생시킨다.
 *    _test_log_handler()가 WARNING을 TAP 주석(#)으로 리다이렉트하여
 *    테스트가 중단되지 않도록 한다.
 *
 *  netns 격리 (root 실행 시):
 *    2026-07-04, /security_group 스위트가 nft 에 host 전역 drop 체인을
 *    설치해 gti12 호스트 전체 네트워크(루프백 포함)가 반복 다운되는 장애가
 *    있었다. 재발 방지를 위해 root 로 실행될 때는 main() 진입 직후
 *    _isolate_netns() 가 새 network namespace 로 격리한 뒤 테스트를
 *    돌린다 (비 root 는 nft 자체가 실패하므로 기존 동작 그대로 무해).
 * ============================================================================
 */

#include <glib.h>

/* unshare(2)/CLONE_NEWNET 에 _GNU_SOURCE 필요 -- Makefile CFLAGS 에 이미 -D_GNU_SOURCE 존재 */
#include <sched.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>

void test_validate_register(void);
void test_circuit_breaker_register(void);
void test_restart_breaker_register(void);   /* AF-1 후속: VM 재시작 브레이커 */
void test_self_healing_restart_register(void);   /* self-healing-restart 결정 seam 효과테스트 */
void test_self_healing_anomaly_register(void);   /* AIO-1 anomaly 트래킹 뮤텍스 3스레드 liveness */
void test_cancellable_map_register(void);
void test_cpu_allocator_register(void);
void test_config_register(void);
void test_vm_signals_register(void);       /* GIO P6 */
void test_spawn_launcher_register(void);   /* GIO P7 */
void test_jwt_register(void);              /* Sprint E: JWT */
void test_network_register(void);          /* D/B: 네트워크 검증 */
void test_security_group_register(void);   /* V5: 보안 그룹 nft 인젝션 회귀 */
void test_sg_nft_builder_register(void);   /* SG nft 스크립트 빌더 (스코프 재설계) */
void test_container_register(void);        /* D/B: 컨테이너 검증 */
void test_privdrop_register(void);         /* D/B: 권한 격하 */
void test_ovn_register(void);              /* OVN SDN */
void test_dpdk_register(void);             /* OVS-DPDK Phase 4 */
void test_sriov_register(void);            /* SR-IOV Phase 4 */
void test_uring_register(void);            /* io_uring Phase U-1 */
void test_handler_params_register(void);   /* 핸들러 파라미터 검증 */
void test_validate_ext_register(void);    /* CIDR/PCI/네트워크 검증 */
void test_vm_config_register(void);       /* VM 설정 빌더 */
void test_vm_clone_plan_register(void);   /* VM clone disk plan */
void test_alert_basic_register(void);     /* 알림 엔진 기본 검증 */
void test_alert_silence_register(void);   /* AIO-3: alert.silence 대소문자 무시 효과테스트 */
void test_alert_dlq_register(void);        /* AIO-4: DLQ 값매칭 제거(스냅샷 후 배열 변동) */
void test_backup_basic_register(void);    /* 백업 정책 기본 검증 */
void test_lxc_basic_register(void);       /* LXC 드라이버 기본 검증 */
void test_ws_basic_register(void);        /* WebSocket 기본 검증 */
void test_hotreload_register(void);       /* 핫 리로드 기본 검증 */
void test_txn_register(void);             /* 트랜잭션/롤백 유틸 */
void test_worker_pool_register(void);     /* GThreadPool 워커 풀 */
void test_job_queue_register(void);       /* SQLite 작업 큐 */
void test_vm_state_register(void);        /* SQLite VM 락 */
void test_log_register(void);             /* 로깅 시스템 */
void test_conn_pool_register(void);       /* libvirt 커넥션 풀 */
void test_zfs_register(void);             /* ZFS 드라이버 에러 경로 */
void test_vm_manager_register(void);      /* libvirt test:/// 드라이버 */
void test_rest_middleware_register(void); /* ETag/Rate Limit */
void test_rest_auth_register(void);       /* REST bootstrap auth fallback */
void test_rpc_utils_register(void);       /* JSON-RPC 2.0 응답 빌더 */
void test_rpc_parse_guarded_register(void); /* JSON 파싱 초크포인트 래퍼 (깊이+크기 가드) */
void test_drain_register(void);           /* graceful drain inflight */
void test_ai_agent_register(void);        /* AI agent 입력 위생 + 파싱 */
void test_prometheus_register(void);      /* Prometheus 체크포인트/검증 */
void test_plugin_register(void);          /* 플러그인 경로·심볼·ABI·언로드 */
void test_snapshot_rollback_register(void); /* 스냅샷 롤백 네거티브 경로 */
void test_bootstrap_register(void);         /* edition bootstrap 분리 */
void test_bootstrap_rpc_registration_register(void); /* edition RPC registration 분리 */
void test_security_event_register(void);    /* Native Host HIDS/HIPS event model */
void test_security_store_register(void);    /* Native Host HIDS/HIPS SQLite store */
void test_security_policy_register(void);   /* Native Host HIDS/HIPS policy */
void test_security_actions_register(void);  /* Native Host HIDS/HIPS action queue */
void test_hids_file_integrity_register(void); /* Native Host HIDS file integrity */
void test_vm_iface_register(void);          /* VM 인터페이스 해석 (virsh domiflist 파서) */
void test_vm_vnet_cache_register(void);     /* I-2: vnet 캐시 */
void test_apikey_register(void);            /* apikey.create 만료 집행 + 컬럼 마이그레이션 */
void test_rbac_user_exists_register(void);  /* SEC-2: RBAC 사용자 존재 3-상태 조회 */
void test_handler_snapshot_verify_register(void);  /* ADR-0025: snapshot.verify 존재-검증 프로브 */
void test_handler_vm_batch_register(void);         /* ADR-0025: vm.batch whitelist/reject 결정 */

/*
 * 테스트 환경 전용 로그 핸들러
 *
 * g_test_* 는 기본적으로 G_LOG_LEVEL_WARNING 을 fatal 처리합니다.
 * PCV_LOG_WARN → G_LOG_LEVEL_WARNING 이 테스트 중 cb_record_failure()
 * 내부에서 호출되면 "Bail out!" 이 발생합니다.
 *
 * WARNING 을 테스트 출력(TAP 주석)으로 리다이렉트하고 계속 진행합니다.
 * CRITICAL/ERROR 는 여전히 fatal 로 처리합니다.
 */
static void
_test_log_handler(const gchar    *log_domain,
                  GLogLevelFlags  log_level,
                  const gchar    *message,
                  gpointer        user_data)
{
    (void)user_data;

    if (log_level & G_LOG_LEVEL_WARNING) {
        /* TAP 주석으로 출력 (# 으로 시작) → CI 에서 확인 가능 */
        g_test_message("WARN [%s] %s",
                       log_domain ? log_domain : "?", message);
        return;  /* fatal 처리 안 함 */
    }

    /* WARNING 외: 기본 핸들러로 위임 */
    g_log_default_handler(log_domain, log_level, message, user_data);
}

/*
 * root 실행 시 network namespace 격리
 *
 * 2026-07-04: /security_group 스위트가 pcv_security_group_create() ->
 * (당시) _nft_create_chain() [현재 제거됨 — bridge pcv_sg 스코프 설계로 대체]
 * 을 통해 라이브 호스트에 hook input/output nft 체인 + 무조건 drop 규칙을
 * 설치하여 gti12 호스트 전체 네트워크(루프백 포함)가 반복 다운되는 장애가 발생했다.
 * (재설계 후에도 root 실행 시 nft 를 다루므로 netns 격리는 계속 유효한 방어다.)
 * root 로 테스트를 돌릴 때는 새 netns 로 격리해 nft 규칙이 호스트가 아닌
 * 격리된 netns 에만 적용되도록 한다. 격리에 실패하면 격리 없이 root 로
 * 계속 도는 것보다 즉시 중단하는 편이 안전하므로 _exit(1) 한다.
 *
 * 비 root 실행은 unshare(CLONE_NEWNET) 이 애초에 불가능하고, nft 자체도
 * 권한 부족으로 실패하므로 호스트에 영향이 없다 -- 기존 동작 그대로 둔다.
 */
static void
_isolate_netns(void)
{
    if (geteuid() != 0) {
        return;
    }

    if (unshare(CLONE_NEWNET) != 0) {
        fprintf(stderr,
                "FATAL: root 로 test_runner 를 실행하려 했으나 network "
                "namespace 격리(unshare(CLONE_NEWNET))에 실패했습니다: %s\n"
                "격리 없이 root 로 테스트를 계속하면 /security_group 스위트가 "
                "호스트 네트워크에 nft drop 체인을 설치해 네트워크 전체가 "
                "다운될 수 있습니다 (2026-07-04 gti12 장애 재현). 실행을 "
                "중단합니다.\n",
                strerror(errno));
        _exit(1);
    }

    /* 새 netns 의 lo 는 기본 DOWN -- ws/rest 등 루프백 의존 테스트를 위해 UP */
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        fprintf(stderr,
                "FATAL: netns 격리 후 lo 설정용 소켓 생성 실패: %s\n",
                strerror(errno));
        _exit(1);
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, "lo", IFNAMSIZ - 1);

    if (ioctl(sock, SIOCGIFFLAGS, &ifr) != 0) {
        fprintf(stderr,
                "FATAL: lo 인터페이스 플래그 조회 실패: %s\n", strerror(errno));
        close(sock);
        _exit(1);
    }

    ifr.ifr_flags |= IFF_UP;  /* IFF_RUNNING 은 커널이 관리하므로 건드리지 않음 */

    if (ioctl(sock, SIOCSIFFLAGS, &ifr) != 0) {
        fprintf(stderr,
                "FATAL: lo 인터페이스 UP 설정 실패: %s\n", strerror(errno));
        close(sock);
        _exit(1);
    }

    close(sock);
}

int main(int argc, char *argv[]) {
    _isolate_netns();  /* 다른 어떤 초기화보다 먼저 -- root 실행 시 host 네트워크 보호 */

    /* config 격리: 실호스트 /etc/purecvisor/daemon.conf가 있으면 /config/defaults
     * 같은 기본값 단언이 깨진다 (2026-07-05 서버 root make test 실측 — Bail out).
     * pcv_config는 PCV_CONFIG_PATH 오버라이드를 지원하므로 존재하지 않는 경로로
     * 고정해 어떤 호스트에서도 내장 기본값으로 돌게 한다. */
    g_setenv("PCV_CONFIG_PATH", "/nonexistent/pcv-test-isolated.conf", TRUE);

    g_test_init(&argc, &argv, NULL);

    /*
     * WARNING 을 non-fatal 로 설정.
     * g_test_init() 이 fatal_mask 에 WARNING 을 추가하므로
     * 여기서 명시적으로 제거합니다.
     */
    g_log_set_always_fatal(G_LOG_LEVEL_ERROR | G_LOG_FLAG_RECURSION);

    /* 모든 도메인의 WARNING 을 테스트 핸들러로 교체 */
    g_log_set_handler(NULL,
                      G_LOG_LEVEL_WARNING | G_LOG_FLAG_FATAL,
                      _test_log_handler, NULL);
    /* libvirt-gobject / purecvisor 도메인도 동일하게 */
    g_log_set_handler("circuit_breaker",
                      G_LOG_LEVEL_WARNING | G_LOG_FLAG_FATAL,
                      _test_log_handler, NULL);
    g_log_set_handler("restart_breaker",
                      G_LOG_LEVEL_WARNING | G_LOG_FLAG_FATAL,
                      _test_log_handler, NULL);
    g_log_set_handler("conn_pool",
                      G_LOG_LEVEL_WARNING | G_LOG_FLAG_FATAL,
                      _test_log_handler, NULL);

    test_validate_register();
    test_circuit_breaker_register();
    test_restart_breaker_register();  /* AF-1 후속: VM 재시작 브레이커 */
    test_self_healing_restart_register();  /* self-healing-restart 결정 seam 효과테스트 */
    test_self_healing_anomaly_register();  /* AIO-1 anomaly 트래킹 뮤텍스 3스레드 liveness */
    test_cancellable_map_register();
    test_cpu_allocator_register();
    test_config_register();
    test_vm_signals_register();       /* GIO P6: GObject 신호 */
    test_spawn_launcher_register();   /* GIO P7: GSubprocessLauncher */
    test_jwt_register();              /* Sprint E: JWT */
    test_network_register();          /* D/B: 네트워크 검증 */
    test_security_group_register();   /* V5: 보안 그룹 nft 인젝션 회귀 */
    test_sg_nft_builder_register();   /* SG nft 스크립트 빌더 */
    test_container_register();        /* D/B: 컨테이너 검증 */
    test_privdrop_register();         /* D/B: 권한 격하 */
    test_ovn_register();              /* OVN SDN */
    test_sriov_register();            /* SR-IOV Phase 4 */
    test_uring_register();            /* io_uring Phase U-1 */
    test_handler_params_register();   /* 핸들러 파라미터 검증 */
    test_validate_ext_register();    /* CIDR/PCI/네트워크 검증 */
    test_vm_config_register();       /* VM 설정 빌더 */
    test_vm_clone_plan_register();   /* VM clone disk plan */
    test_alert_basic_register();      /* 알림 엔진 기본 검증 */
    test_alert_silence_register();    /* AIO-3: alert.silence casefold 효과테스트 */
    test_alert_dlq_register();        /* AIO-4: DLQ 값매칭 제거(스냅샷 후 배열 변동) */
    test_backup_basic_register();     /* 백업 정책 기본 검증 */
    test_lxc_basic_register();        /* LXC 드라이버 기본 검증 */
    test_ws_basic_register();         /* WebSocket 기본 검증 */
    test_hotreload_register();        /* 핫 리로드 기본 검증 */
    test_txn_register();              /* 트랜잭션/롤백 유틸 */
    test_worker_pool_register();      /* GThreadPool 워커 풀 */
    test_job_queue_register();        /* SQLite 작업 큐 */
    test_vm_state_register();         /* SQLite VM 락 */
    test_log_register();              /* 로깅 시스템 */
    test_conn_pool_register();        /* libvirt 커넥션 풀 */
    test_zfs_register();              /* ZFS 드라이버 에러 경로 */
    test_vm_manager_register();       /* libvirt test:/// 드라이버 */
    test_rest_middleware_register();  /* ETag/Rate Limit */
    test_rest_auth_register();        /* REST bootstrap auth fallback */
    test_rpc_utils_register();        /* JSON-RPC 2.0 응답 빌더 */
    test_rpc_parse_guarded_register(); /* JSON 파싱 초크포인트 래퍼 (깊이+크기 가드) */
    test_drain_register();            /* graceful drain inflight */
    test_ai_agent_register();         /* AI agent 입력 위생 + 파싱 */
    test_prometheus_register();       /* Prometheus 체크포인트/검증 */
    test_plugin_register();           /* 플러그인 경로·심볼·ABI·언로드 */
    test_snapshot_rollback_register(); /* 스냅샷 롤백 네거티브 경로 */
    test_bootstrap_register();         /* edition bootstrap 분리 */
    test_bootstrap_rpc_registration_register(); /* edition RPC registration 분리 */
    test_security_event_register();    /* Native Host HIDS/HIPS event model */
    test_security_store_register();    /* Native Host HIDS/HIPS SQLite store */
    test_security_policy_register();   /* Native Host HIDS/HIPS policy */
    test_security_actions_register();  /* Native Host HIDS/HIPS action queue */
    test_hids_file_integrity_register(); /* Native Host HIDS file integrity */
    test_vm_iface_register();          /* VM 인터페이스 해석 (virsh domiflist 파서) */
    test_vm_vnet_cache_register();     /* I-2: vnet 캐시 */
    test_apikey_register();            /* apikey.create 만료 집행 + 컬럼 마이그레이션 */
    test_rbac_user_exists_register();  /* SEC-2: RBAC 사용자 존재 3-상태 조회 */
    test_handler_snapshot_verify_register(); /* ADR-0025: snapshot.verify 존재-검증 프로브 */
    test_handler_vm_batch_register();        /* ADR-0025: vm.batch whitelist/reject 결정 */
    test_dpdk_register();             /* OVS-DPDK Phase 4 — 환경 의존, 마지막 실행 */

    return g_test_run();
}
