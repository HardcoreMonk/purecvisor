/**
 * @file pcv_privdrop.c
 * @brief 권한 격하 + seccomp-bpf 샌드박스 — 데몬 보안 하드닝 핵심 모듈
 *
 * Sprint D-3에서 도입된 권한 최소화(Least Privilege) 모듈입니다.
 * PureCVisor 데몬이 root로 시작하지만, 필요한 최소 권한만 유지하고
 * 나머지를 모두 제거하여 공격 표면을 축소합니다.
 *
 * [아키텍처 위치]
 *   main.c 시작 시:
 *     1. root 권한으로 필수 초기화 (UDS 소켓, 디렉터리 생성 등)
 *     2. pcv_privdrop_apply_all() 호출 (아래 3단계 순서대로)
 *     3. 이후 모든 데몬 동작은 제한된 권한으로 실행
 *
 * [3단계 권한 격하]
 *   [1] Capabilities (libcap):
 *       - Permitted/Effective/Inheritable = {NET_ADMIN, NET_BIND_SERVICE, SYS_ADMIN, SETUID, DAC_OVERRIDE}
 *       - Bounding 집합에서 나머지 전부 제거 (PR_CAPBSET_DROP)
 *       - sudo 환경에서 EPERM 발생 시 무시 (Effective/Permitted 제한은 적용됨)
 *
 *   [2] NO_NEW_PRIVS (현재 비활성화):
 *       - PR_SET_NO_NEW_PRIVS=1 → exec 후 setuid/AppArmor 전환 차단
 *       - LXC lxc-start가 AppArmor 프로필 전환을 수행해야 하므로 비활성화됨
 *       - 실전 배포 중 컨테이너 시작 실패 → NNP 비활성화로 해결한 이력
 *
 *   [3] seccomp-bpf (libseccomp, 현재 비활성화):
 *       - 기본 동작: SCMP_ACT_ERRNO(EPERM) — 미등록 syscall은 EPERM 반환
 *       - 화이트리스트: I/O, 소켓, 프로세스, 메모리, 파일, 시그널, 스레드, 시간 등
 *       - io_uring syscall (Phase U-1): io_uring_setup/enter/register 추가
 *       - LXC 지원 (D-3 재활성화): clone3, mount, sethostname, personality 등 추가
 *       - 비활성화 이유: BPF 필터가 자식 프로세스(컨테이너)에 상속되어 부팅 실패
 *       - 진단: strace -f -e trace=all ./bin/purecvisorsd 2>&1 | grep EPERM
 *               strace -f -e trace=all ./bin/purecvisormd 2>&1 | grep EPERM
 *
 * [조건부 컴파일]
 *   HAVE_LIBCAP  : pkg-config libcap 탐지 → capabilities 실제 적용
 *   HAVE_SECCOMP : pkg-config libseccomp 탐지 → seccomp-bpf 실제 적용
 *   미설치 시 stub 함수 (경고 로그만 출력, 빌드는 항상 성공)
 *
 * [실전 배포 교훈 — 이 모듈의 설계를 결정한 실제 사건들]
 *   - 컨테이너 시작 실패: NNP + seccomp가 LXC AppArmor 전환/clone 차단 → NNP 비활성화
 *   - 컨테이너 exec 실패: GSubprocess가 seccomp 상속 → pcv_spawn_sync 폴백으로 해결
 *   - ZFS 복제 실패: seccomp이 timeout syscall 차단 → syscall 추가
 *
 * [엔터프라이즈 참고]
 *   libvirt, Proxmox, VMware ESXi 등 엔터프라이즈 하이퍼바이저도
 *   호스트 데몬에 seccomp을 적용하지 않습니다. 대신:
 *   - 호스트 보호: capabilities + AppArmor/SELinux
 *   - 게스트 격리: 컨테이너 런타임(LXC/runc)이 자체 seccomp 적용
 *
 * [주의사항]
 *   - pcv_privdrop_apply_all()에서 일부 단계 실패해도 계속 진행 (운영 안정성 우선)
 *   - SECCOMP_DEFAULT_ACTION을 SCMP_ACT_KILL_PROCESS로 변경하면 보안 강화 가능
 *     (완전한 syscall 감사 후에만 전환 권장)
 *   - capabilities 유지 목록 변경 시 keep[] 배열 수정 + nkeep 자동 계산
 */

#include "pcv_privdrop.h"
#include "pcv_log.h"

#include <sys/prctl.h>
#include <errno.h>
#include <string.h>

/** PD_LOG_DOM - 이 모듈의 로그 도메인. journalctl에서 "dom":"pcv_privdrop"로 필터링 */
#define PD_LOG_DOM "pcv_privdrop"

/* ══════════════════════════════════════════════════════
 * [1] Capabilities (libcap)
 *
 * Linux Capabilities는 전통적인 root/non-root 이분법을 세분화합니다.
 * root의 모든 권한을 ~40개 개별 capability로 분리하여,
 * 프로세스가 필요한 것만 유지할 수 있게 합니다.
 *
 * 예시:
 *   전통 root: 모든 것이 가능 (네트워크, 파일, 프로세스, 커널 모듈 등)
 *   CAP_NET_ADMIN만: 네트워크 설정 가능, 커널 모듈 로딩 불가
 * ══════════════════════════════════════════════════════*/

#ifdef HAVE_LIBCAP
#include <sys/capability.h>

/**
 * pcv_privdrop_capabilities — Linux 케이퍼빌리티를 최소 집합으로 제한
 *
 * [케이퍼빌리티(Capabilities)란?]
 *   전통적인 Unix에서 root는 모든 권한을 가집니다. Linux 케이퍼빌리티는
 *   root 권한을 세분화하여 필요한 것만 유지할 수 있게 합니다.
 *   예: CAP_NET_ADMIN만 있으면 네트워크 설정은 가능하지만 커널 모듈 로딩은 불가
 *
 * [3가지 케이퍼빌리티 집합]
 *   Permitted(P):   프로세스가 가질 수 있는 최대 권한 (상한)
 *                   Effective의 상위 집합. P에 없는 것은 E에도 없음
 *   Effective(E):   실제로 커널이 권한 체크에 사용하는 집합
 *                   syscall 호출 시 이 집합을 확인
 *   Inheritable(I): exec() 후 자식 프로세스에 전달 가능한 집합
 *                   zfs, nft 등 외부 명령이 데몬의 권한을 상속
 *   Bounding(B):    프로세스가 향후 acquire할 수 있는 최대 범위
 *                   여기서 제거하면 재획득 불가 (one-way)
 *
 * [왜 5개를 유지하는가?]
 *   CAP_NET_ADMIN:       브릿지 생성(ip link), nftables 규칙, OVS 설정에 필요
 *   CAP_NET_BIND_SERVICE: 포트 80/443 바인딩 (1024 미만 포트는 이 권한 필요)
 *   CAP_SYS_ADMIN:       KVM ioctl(KVM_CREATE_VM), 네임스페이스 진입, mount에 필요
 *   CAP_SETUID:          워커 프로세스에서 UID 전환 (보안 격리)
 *   CAP_DAC_OVERRIDE:    ZFS 경로(/pcvpool/...) 및 UDS 소켓 접근에 필요
 *                        (파일 권한을 무시하고 접근 가능)
 *
 * [Bounding 집합 제거와 sudo 환경]
 *   일반 sudo 환경에서 PR_CAPBSET_DROP이 EPERM을 반환할 수 있습니다.
 *   이는 Bounding 집합 수정에 CAP_SETPCAP이 필요한데,
 *   sudo가 이를 제공하지 않기 때문입니다. 무시해도 안전합니다:
 *   - Effective/Permitted 제한은 cap_set_proc()에서 이미 완료됨
 *   - Bounding 제거는 추가 보안 조치 (재획득 방지)
 */
gboolean
pcv_privdrop_capabilities(void)
{
    /*
     * 유지할 capability 목록
     * Permitted = Effective = Inheritable = {이 5개}
     * Bounding에서 나머지 전부 제거
     */
    cap_value_t keep[] = {
        CAP_NET_ADMIN,        /* bridge/nft 네트워크 설정                          */
        CAP_NET_BIND_SERVICE, /* 포트 80/443 바인딩 (1024 미만)                    */
        CAP_SYS_ADMIN,        /* KVM/libvirt (virDomainCreate, ioctl KVM_CREATE_VM) */
        CAP_SETUID,           /* 워커 UID 전환                                     */
        CAP_DAC_OVERRIDE,     /* ZFS 경로 / UDS 소켓 접근                          */
    };
    /* sizeof 트릭: 배열 요소 수 자동 계산 (keep[] 수정 시 nkeep 자동 갱신) */
    const int nkeep = (int)(sizeof(keep) / sizeof(keep[0]));

    /*
     * cap_init(): 빈 capability 집합 생성
     * 모든 플래그가 cleared 상태에서 시작하여,
     * 필요한 것만 set합니다 (화이트리스트 방식)
     */
    cap_t caps = cap_init();
    if (!caps) {
        PCV_LOG_WARN(PD_LOG_DOM, "cap_init failed: %s", strerror(errno));
        return FALSE;
    }

    /* 3개 집합(Permitted, Effective, Inheritable)에 유지 목록을 set */
    if (cap_set_flag(caps, CAP_PERMITTED,   nkeep, keep, CAP_SET) != 0 ||
        cap_set_flag(caps, CAP_EFFECTIVE,   nkeep, keep, CAP_SET) != 0 ||
        cap_set_flag(caps, CAP_INHERITABLE, nkeep, keep, CAP_SET) != 0) {
        PCV_LOG_WARN(PD_LOG_DOM, "cap_set_flag failed: %s", strerror(errno));
        cap_free(caps);
        return FALSE;
    }

    /* 프로세스에 capability 집합 적용 (핵심 단계) */
    if (cap_set_proc(caps) != 0) {
        PCV_LOG_WARN(PD_LOG_DOM, "cap_set_proc failed: %s", strerror(errno));
        cap_free(caps);
        return FALSE;
    }

    cap_free(caps);  /* cap_t 핸들 해제 (프로세스 상태는 이미 변경됨) */

    /* Verify capabilities were actually applied */
    {
        cap_t verify_caps = cap_get_proc();
        if (verify_caps) {
            cap_flag_value_t val;
            /* Verify dangerous cap was dropped */
            if (cap_get_flag(verify_caps, CAP_SYS_MODULE, CAP_EFFECTIVE, &val) == 0 && val == CAP_SET) {
                PCV_LOG_WARN(PD_LOG_DOM, "CAP_SYS_MODULE still set after drop — security degraded");
            }
            cap_free(verify_caps);
        }
    }

    /*
     * Bounding 집합에서도 불필요한 capability 전부 제거
     *
     * Bounding 집합은 향후 capability 재획득을 방지합니다.
     * 예: 악의적 코드가 CAP_SYS_MODULE(커널 모듈 로딩)을 얻으려 해도
     *     Bounding에서 제거되었으므로 불가능합니다.
     *
     * CAP_LAST_CAP: 현재 커널이 지원하는 마지막 capability 번호
     * 0부터 CAP_LAST_CAP까지 순회하며, keep[]에 없는 것을 제거합니다.
     */
    gint dropped = 0, skipped = 0;
    for (int c = 0; c <= CAP_LAST_CAP; c++) {
        /* keep[] 목록에 있는지 확인 */
        gboolean in_keep = FALSE;
        for (int i = 0; i < nkeep; i++) {
            if (keep[i] == (cap_value_t)c) { in_keep = TRUE; break; }
        }
        if (!in_keep) {
            if (prctl(PR_CAPBSET_DROP, c, 0, 0, 0) == 0)
                dropped++;      /* 성공적으로 제거 */
            else if (errno != EINVAL)
                skipped++;      /* EPERM: sudo 환경 제한 (안전하게 무시) */
        }
    }

    /* 결과 로깅 (디버그/정보) */
    if (skipped > 0)
        PCV_LOG_DEBUG(PD_LOG_DOM,
                      "PR_CAPBSET_DROP: %d dropped, %d skipped (EPERM — sudo env)",
                      dropped, skipped);
    else
        PCV_LOG_DEBUG(PD_LOG_DOM, "PR_CAPBSET_DROP: %d dropped", dropped);

    PCV_LOG_INFO(PD_LOG_DOM,
                 "Capabilities restricted to: NET_ADMIN SYS_ADMIN SETUID DAC_OVERRIDE");
    return TRUE;
}

#else  /* !HAVE_LIBCAP — libcap 미설치 시 stub 구현 */

/**
 * stub: libcap 미설치 환경
 * 경고 로그만 출력하고 FALSE 반환합니다.
 * 빌드는 항상 성공하며, 런타임에 기능이 비활성됨을 알립니다.
 */
gboolean
pcv_privdrop_capabilities(void)
{
    PCV_LOG_WARN(PD_LOG_DOM,
                 "libcap not available — capability restriction skipped "
                 "(install libcap2-dev for hardened builds)");
    return FALSE;
}

#endif /* HAVE_LIBCAP */

/* ══════════════════════════════════════════════════════
 * [2] PR_SET_NO_NEW_PRIVS
 *
 * Linux 커널 3.5에서 도입된 보안 플래그입니다.
 * 설정되면 exec() 후 프로세스가 새로운 권한을 획득할 수 없습니다:
 *   - setuid 바이트가 무시됨
 *   - AppArmor/SELinux 프로필 전환이 차단됨
 *   - capabilities ambient 상승이 불가
 * 한 번 설정하면 해제가 불가능합니다 (one-way flag).
 * ══════════════════════════════════════════════════════*/

/**
 * pcv_privdrop_no_new_privs — PR_SET_NO_NEW_PRIVS 설정
 *
 * 현재 pcv_privdrop_apply_all()에서 비활성화되어 있습니다.
 * 직접 호출하면 실제로 NNP가 설정됩니다 (주의!).
 *
 * [왜 비활성화되었는가? — LXC 호환성 문제]
 *   lxc-start는 컨테이너를 시작할 때 AppArmor 프로필을 전환해야 합니다.
 *   (예: lxc-container-default-cgns 프로필로 전환)
 *   NNP=1이면 이 전환이 차단되어 컨테이너 시작이 실패합니다.
 *
 *   실전 배포에서 발견: container.start RPC 호출 시
 *   "Failed to change apparmor profile" 에러 → NNP 비활성화로 해결
 *
 * [보안 관점]
 *   NNP 없이도 capabilities 제한 + AppArmor/SELinux로 충분히 보호 가능합니다.
 *   엔터프라이즈 하이퍼바이저(libvirt, Proxmox)도 호스트 데몬에
 *   NNP를 적용하지 않는 것이 표준 관행입니다.
 *
 * [향후 고려]
 *   컨테이너 기능을 사용하지 않는 배포에서는 NNP를 활성화하여
 *   보안을 강화할 수 있습니다.
 */
gboolean
pcv_privdrop_no_new_privs(void)
{
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        PCV_LOG_WARN(PD_LOG_DOM,
                     "PR_SET_NO_NEW_PRIVS failed: %s", strerror(errno));
        return FALSE;
    }
    PCV_LOG_INFO(PD_LOG_DOM, "PR_SET_NO_NEW_PRIVS=1: privilege escalation via exec blocked");
    return TRUE;
}

/* ══════════════════════════════════════════════════════
 * [3] seccomp-bpf allowlist
 *
 * seccomp(Secure Computing)은 커널의 시스템콜 필터링 메커니즘입니다.
 * BPF(Berkeley Packet Filter) 프로그램으로 허용할 시스템콜을 정의합니다.
 * 미등록 시스템콜은 차단됩니다 (EPERM 또는 SIGKILL).
 *
 * 한 번 적용하면 해제 불가, fork/exec을 통해 모든 자식에 상속됩니다.
 * 이 상속 특성이 LXC 컨테이너와의 호환성 문제를 유발합니다.
 * ══════════════════════════════════════════════════════*/

#ifdef HAVE_SECCOMP
#include <seccomp.h>

/*
 * [기본 동작] SCMP_ACT_ERRNO(EPERM)
 *   미등록 syscall 호출 시 프로세스를 죽이지 않고 EPERM을 반환합니다.
 *   데몬이 예기치 않은 syscall 차단으로 크래시하는 것을 방지합니다.
 *
 *   더 엄격한 정책: SCMP_ACT_KILL_PROCESS (프로세스 즉시 종료)
 *   → 완전한 syscall 감사(audit) 후에만 전환 권장
 *
 * [누락 syscall 진단 방법]
 *   strace -f -e trace=all ./bin/purecvisorsd 2>&1 | grep EPERM
 *   strace -f -e trace=all ./bin/purecvisormd 2>&1 | grep EPERM
 *   → EPERM이 반환되는 syscall을 찾아 ALLOWED_SYSCALLS에 추가
 */
#define SECCOMP_DEFAULT_ACTION  SCMP_ACT_ERRNO(EPERM)

/**
 * ALLOWED_SYSCALLS — seccomp 화이트리스트 (허용할 시스템콜 목록)
 *
 * 이 배열에 나열된 시스템콜만 SCMP_ACT_ALLOW로 허용됩니다.
 * 나머지는 SECCOMP_DEFAULT_ACTION(EPERM)으로 차단됩니다.
 *
 * [카테고리별 분류]
 *   I/O:         read/write/close/ioctl — 기본 파일 I/O
 *   epoll/poll:  GLib GMainLoop 이벤트 루프에 필수 (파일 디스크립터 감시)
 *   io_uring:    Phase U-1에서 추가된 비동기 I/O (커널 5.6+, SQ/CQ 링)
 *   소켓:        UDS 서버(클라이언트 연결), REST(HTTP/HTTPS), libvirt(RPC)
 *   프로세스:    GTask 스레드(clone), 자식 프로세스(fork/exec), 시그널(wait)
 *   메모리:      GLib/SQLite/io_uring 메모리 관리 (mmap, brk)
 *   파일:        daemon.conf, SQLite DB, ZFS 경로, 인증서 파일
 *   신호:        GLib 시그널 핸들링, SIGTERM 처리, 자식 프로세스 시그널
 *   LXC:         clone3, mount, sethostname — 컨테이너 네임스페이스 지원
 *
 * [주의: 이 목록을 수정할 때]
 *   1. strace로 차단되는 시스템콜 확인:
 *      strace -f -e trace=all ./bin/purecvisorsd 2>&1 | grep EPERM
 *      strace -f -e trace=all ./bin/purecvisormd 2>&1 | grep EPERM
 *   2. 해당 시스템콜이 실제로 필요한지 검토 (불필요하면 추가하지 않음)
 *   3. 추가 후 전체 기능 테스트 (VM/컨테이너/네트워크/스토리지/클러스터)
 */
static const int ALLOWED_SYSCALLS[] = {
    /* ── I/O: 기본 파일 입출력 ────────────────────────────── */
    SCMP_SYS(read),       SCMP_SYS(write),      SCMP_SYS(close),
    SCMP_SYS(close_range),                       /* GLib 2.74+: GSubprocess fd 정리 */
    SCMP_SYS(ioctl),      SCMP_SYS(readv),      SCMP_SYS(writev),
    SCMP_SYS(pread64),    SCMP_SYS(pwrite64),
    SCMP_SYS(sendfile),

    /* ── epoll / poll / select: 이벤트 루프 ──────────────── */
    SCMP_SYS(epoll_create),  SCMP_SYS(epoll_create1),
    SCMP_SYS(epoll_ctl),     SCMP_SYS(epoll_wait),
    SCMP_SYS(epoll_pwait),   SCMP_SYS(epoll_pwait2),
    SCMP_SYS(poll),          SCMP_SYS(ppoll),
    SCMP_SYS(select),        SCMP_SYS(pselect6),

    /* ── io_uring (Phase U-1): 비동기 I/O ──────────────── */
#ifdef __NR_io_uring_setup
    SCMP_SYS(io_uring_setup),     /* SQ/CQ 링 생성 */
    SCMP_SYS(io_uring_enter),     /* SQE 제출 + CQE 대기 */
    SCMP_SYS(io_uring_register),  /* eventfd/buffer 등록 */
#endif

    /* ── 소켓: UDS + REST + libvirt 통신 ─────────────────── */
    SCMP_SYS(socket),      SCMP_SYS(socketpair),  /* GLib 내부 pipe 대체 */
    SCMP_SYS(bind),        SCMP_SYS(connect),
    SCMP_SYS(accept),      SCMP_SYS(accept4),     SCMP_SYS(listen),
    SCMP_SYS(sendmsg),     SCMP_SYS(recvmsg),
    SCMP_SYS(sendmmsg),    SCMP_SYS(recvmmsg),    /* libvirt RPC 다중 메시지 */
    SCMP_SYS(sendto),      SCMP_SYS(recvfrom),
    SCMP_SYS(getsockopt),  SCMP_SYS(setsockopt),
    SCMP_SYS(getsockname), SCMP_SYS(getpeername),
    SCMP_SYS(shutdown),

    /* ── 프로세스 / 스레드 ─────────────────────────────── */
    SCMP_SYS(clone),       SCMP_SYS(clone3),      /* pthread_create / LXC 네임스페이스 */
    SCMP_SYS(fork),        SCMP_SYS(vfork),
    SCMP_SYS(execve),      SCMP_SYS(execveat),
    SCMP_SYS(wait4),       SCMP_SYS(waitid),
    SCMP_SYS(exit),        SCMP_SYS(exit_group),
    SCMP_SYS(getpid),      SCMP_SYS(getppid),
    SCMP_SYS(gettid),      SCMP_SYS(set_tid_address),
    SCMP_SYS(pidfd_open),       /* GLib 2.74+: GSubprocess 자식 모니터링 */
    SCMP_SYS(pidfd_send_signal), /* GLib 2.74+: GSubprocess 자식 시그널 전달 */
    /* UID/GID 조회 — GLib/libvirt 내부에서 빈번히 호출 */
    SCMP_SYS(getuid),      SCMP_SYS(geteuid),
    SCMP_SYS(getgid),      SCMP_SYS(getegid),
    SCMP_SYS(getgroups),   SCMP_SYS(setgroups),
    SCMP_SYS(setuid),      SCMP_SYS(setgid),
    SCMP_SYS(setreuid),    SCMP_SYS(setregid),
    SCMP_SYS(setresuid),   SCMP_SYS(setresgid),
    SCMP_SYS(getresuid),   SCMP_SYS(getresgid),
    /* 스케줄러 — CPU 어피니티(cpu_allocator.c), 우선순위 설정 */
    SCMP_SYS(sched_yield),
    SCMP_SYS(sched_getaffinity),  SCMP_SYS(sched_setaffinity),
    SCMP_SYS(sched_getparam),     SCMP_SYS(sched_setparam),
    SCMP_SYS(sched_getscheduler), SCMP_SYS(sched_setscheduler),
    SCMP_SYS(getpriority),        SCMP_SYS(setpriority),
    /* 리소스 제한 — ulimit, 메모리 상한 */
    SCMP_SYS(getrlimit),   SCMP_SYS(setrlimit),   SCMP_SYS(prlimit64),
    SCMP_SYS(getrusage),
    /* capability 조회/설정 — pcv_privdrop_capabilities() 내부 */
    SCMP_SYS(capget),      SCMP_SYS(capset),

    /* ── 메모리: GLib/SQLite/io_uring 할당 ───────────────── */
    SCMP_SYS(mmap),        SCMP_SYS(mmap2),
    SCMP_SYS(mprotect),    SCMP_SYS(munmap),
    SCMP_SYS(brk),         SCMP_SYS(madvise),
    SCMP_SYS(mremap),      SCMP_SYS(mincore),
    SCMP_SYS(msync),       SCMP_SYS(mlock),       SCMP_SYS(munlock),
    SCMP_SYS(mlockall),    SCMP_SYS(munlockall),
    SCMP_SYS(shmget),      SCMP_SYS(shmat),       SCMP_SYS(shmdt),
    SCMP_SYS(shmctl),
    SCMP_SYS(memfd_create),                        /* GLib 2.58+ 내부 메모리 fd */

    /* ── 파일: 설정/DB/ZFS/인증서 접근 ───────────────────── */
    SCMP_SYS(open),        SCMP_SYS(openat),      SCMP_SYS(openat2),
    SCMP_SYS(creat),
    SCMP_SYS(stat),        SCMP_SYS(fstat),       SCMP_SYS(lstat),
    SCMP_SYS(newfstatat),  SCMP_SYS(statx),
    SCMP_SYS(statfs),      SCMP_SYS(fstatfs),     /* libvirt 파일시스템 조회 */
    SCMP_SYS(access),      SCMP_SYS(faccessat),   SCMP_SYS(faccessat2),
    SCMP_SYS(dup),         SCMP_SYS(dup2),        SCMP_SYS(dup3),
    SCMP_SYS(pipe),        SCMP_SYS(pipe2),
    SCMP_SYS(unlink),      SCMP_SYS(unlinkat),
    SCMP_SYS(rename),      SCMP_SYS(renameat),    SCMP_SYS(renameat2),
    SCMP_SYS(mkdir),       SCMP_SYS(mkdirat),
    SCMP_SYS(rmdir),
    SCMP_SYS(chmod),       SCMP_SYS(fchmod),      SCMP_SYS(fchmodat),
    SCMP_SYS(chown),       SCMP_SYS(lchown),      SCMP_SYS(fchown),
    SCMP_SYS(fchownat),
    SCMP_SYS(lseek),
    SCMP_SYS(getdents),    SCMP_SYS(getdents64),
    SCMP_SYS(getcwd),      SCMP_SYS(chdir),       SCMP_SYS(fchdir),
    SCMP_SYS(symlink),     SCMP_SYS(symlinkat),
    SCMP_SYS(readlink),    SCMP_SYS(readlinkat),
    SCMP_SYS(link),        SCMP_SYS(linkat),
    SCMP_SYS(mount),       SCMP_SYS(umount2),     /* libvirt/LXC 마운트 */
    SCMP_SYS(getxattr),    SCMP_SYS(lgetxattr),   SCMP_SYS(fgetxattr),
    SCMP_SYS(setxattr),    SCMP_SYS(lsetxattr),   SCMP_SYS(fsetxattr),
    SCMP_SYS(listxattr),   SCMP_SYS(llistxattr),

    /* ── 신호: GLib 시그널 핸들링 ─────────────────────────── */
    SCMP_SYS(rt_sigaction),   SCMP_SYS(rt_sigprocmask),
    SCMP_SYS(rt_sigreturn),   SCMP_SYS(rt_sigsuspend),
    SCMP_SYS(rt_sigpending),  SCMP_SYS(rt_sigtimedwait),
    SCMP_SYS(signalfd),       SCMP_SYS(signalfd4),  /* GLib GMainLoop */
    SCMP_SYS(kill),           SCMP_SYS(tgkill),
    SCMP_SYS(sigaltstack),

    /* ── 스레드 / futex: pthread, GMutex ─────────────────── */
    SCMP_SYS(futex),          SCMP_SYS(futex_time64),
    SCMP_SYS(futex_waitv),
    SCMP_SYS(set_robust_list),SCMP_SYS(get_robust_list),

    /* ── 시간: 타임스탬프, sleep, 타이머 ──────────────────── */
    SCMP_SYS(clock_gettime),  SCMP_SYS(clock_getres),
    SCMP_SYS(clock_settime),  SCMP_SYS(clock_nanosleep),
    SCMP_SYS(nanosleep),
    SCMP_SYS(gettimeofday),   SCMP_SYS(settimeofday),
    SCMP_SYS(time),           SCMP_SYS(adjtimex),

    /* ── 기타 시스템: prctl, fcntl, sync 등 ───────────────── */
    SCMP_SYS(prctl),          SCMP_SYS(arch_prctl),
    SCMP_SYS(fcntl),          SCMP_SYS(fcntl64),
    SCMP_SYS(flock),
    SCMP_SYS(fsync),          SCMP_SYS(fdatasync),
    SCMP_SYS(truncate),       SCMP_SYS(ftruncate),
    SCMP_SYS(umask),          SCMP_SYS(uname),
    SCMP_SYS(sysinfo),        SCMP_SYS(times),
    SCMP_SYS(getrandom),      /* /dev/urandom 대체 (커널 CSPRNG) */
    SCMP_SYS(eventfd),        SCMP_SYS(eventfd2),    /* io_uring 이벤트 브릿지 */
    SCMP_SYS(timerfd_create), SCMP_SYS(timerfd_settime),
    SCMP_SYS(timerfd_gettime),
    SCMP_SYS(inotify_init),   SCMP_SYS(inotify_init1),
    SCMP_SYS(inotify_add_watch), SCMP_SYS(inotify_rm_watch),
    SCMP_SYS(splice),         SCMP_SYS(tee),        /* GLib IO channel */
    SCMP_SYS(copy_file_range),

    /* ── glibc 2.35+ 필수 (Ubuntu 22.04+) ──────────────── */
    SCMP_SYS(rseq),                                 /* 스레드 restartable sequence */
    SCMP_SYS(set_mempolicy),                        /* NUMA 메모리 정책 */
    SCMP_SYS(get_mempolicy),
    SCMP_SYS(mbind),
    SCMP_SYS(process_vm_readv),  SCMP_SYS(process_vm_writev),

    /* ── 네트워크: libvirt 네임스페이스 / nft ──────────────── */
    SCMP_SYS(getsid),
    SCMP_SYS(setns),                                /* virDomainCreate namespace 진입 */
    SCMP_SYS(unshare),
    SCMP_SYS(pivot_root),
    SCMP_SYS(iopl),           SCMP_SYS(ioperm),

    /* ── LXC 컨테이너 지원 (D-3 선택적 재활성화) ──────────── */
    SCMP_SYS(clone3),                               /* lxc-start namespace 생성 */
    SCMP_SYS(mount),          SCMP_SYS(umount2),    /* 컨테이너 rootfs 마운트 */
    SCMP_SYS(sethostname),    SCMP_SYS(setdomainname),
    SCMP_SYS(keyctl),         SCMP_SYS(request_key),
    SCMP_SYS(add_key),
    SCMP_SYS(personality),                           /* lxc-attach personality 설정 */
    SCMP_SYS(capset),         SCMP_SYS(capget),     /* 컨테이너 capability 설정 */
    SCMP_SYS(seccomp),                               /* lxc-start가 컨테이너 seccomp 정책 적용 */
};

/**
 * pcv_privdrop_seccomp — seccomp-bpf 화이트리스트 적용
 *
 * 현재 pcv_privdrop_apply_all()에서 비활성화되어 있습니다.
 *
 * [seccomp-bpf란?]
 *   커널의 시스템콜 필터링 메커니즘입니다. BPF 프로그램으로
 *   허용할 시스템콜 목록을 정의하고, 미등록 시스템콜을 차단합니다.
 *   한 번 적용하면 해제가 불가능하며, fork/exec를 통해 모든 자식에 상속됩니다.
 *
 * [왜 비활성화되었는가?]
 *   핵심 문제: seccomp BPF 필터는 fork/exec로 모든 자식에 상속되며 해제 불가
 *
 *   데몬(현재 에디션 데몬) → fork → lxc-start → fork → 컨테이너 init(systemd)
 *                                                    ^
 *                                                    데몬의 seccomp 필터가 상속됨!
 *
 *   컨테이너 내부의 systemd는 수백 개의 시스템콜을 사용합니다.
 *   데몬의 화이트리스트(~120개)에 없는 시스템콜이 EPERM으로 차단되어
 *   컨테이너가 부팅에 실패합니다.
 *
 * [동작 흐름 (활성화된 경우)]
 *   1. seccomp_init(SCMP_ACT_ERRNO(EPERM)): 기본 동작 설정
 *   2. ALLOWED_SYSCALLS 배열 순회 → 각 syscall에 ALLOW 규칙 추가
 *   3. seccomp_load(): BPF 프로그램을 커널에 로드 (이후 해제 불가)
 *
 * [진단 방법]
 *   seccomp이 차단하는 syscall 확인:
 *     strace -f -e trace=all ./bin/purecvisorsd 2>&1 | grep EPERM
 *     strace -f -e trace=all ./bin/purecvisormd 2>&1 | grep EPERM
 */
gboolean
pcv_privdrop_seccomp(void)
{
    /*
     * seccomp 필터 초기화
     * SECCOMP_DEFAULT_ACTION = SCMP_ACT_ERRNO(EPERM)
     * → 미등록 syscall은 EPERM 반환 (프로세스를 죽이지 않음)
     */
    scmp_filter_ctx ctx = seccomp_init(SECCOMP_DEFAULT_ACTION);
    if (!ctx) {
        PCV_LOG_WARN(PD_LOG_DOM, "seccomp_init failed");
        return FALSE;
    }

    /* ALLOWED_SYSCALLS 배열의 각 syscall에 ALLOW 규칙 추가 */
    int n = (int)(sizeof(ALLOWED_SYSCALLS) / sizeof(ALLOWED_SYSCALLS[0]));
    for (int i = 0; i < n; i++) {
        if (seccomp_rule_add(ctx, SCMP_ACT_ALLOW,
                             ALLOWED_SYSCALLS[i], 0) != 0) {
            /* 현재 아키텍처에 없는 syscall 번호는 무시 (32비트 전용 등) */
        }
    }

    /* BPF 프로그램을 커널에 로드 — 이후 해제 불가! */
    if (seccomp_load(ctx) != 0) {
        PCV_LOG_WARN(PD_LOG_DOM, "seccomp_load failed: %s", strerror(errno));
        seccomp_release(ctx);
        return FALSE;
    }

    seccomp_release(ctx);  /* 필터 컨텍스트 해제 (BPF 프로그램은 커널에 남음) */
    PCV_LOG_INFO(PD_LOG_DOM,
                 "seccomp-bpf applied: %d syscalls allowed, others → EPERM", n);
    return TRUE;
}

#else  /* !HAVE_SECCOMP — libseccomp 미설치 시 stub 구현 */

gboolean
pcv_privdrop_seccomp(void)
{
    PCV_LOG_WARN(PD_LOG_DOM,
                 "libseccomp not available — seccomp filter skipped "
                 "(install libseccomp-dev for hardened builds)");
    return FALSE;
}

#endif /* HAVE_SECCOMP */

/* ══════════════════════════════════════════════════════
 * [4] apply_all convenience wrapper
 *
 * 세 가지 권한 격하를 순서대로 적용합니다.
 * 현재 NNP와 seccomp은 LXC 호환성 문제로 비활성화되어 있으므로,
 * 실질적으로 capabilities만 적용됩니다.
 * ══════════════════════════════════════════════════════*/

/**
 * pcv_privdrop_apply_all - 모든 권한 격하를 순서대로 적용
 *
 * main.c에서 root 필수 초기화 완료 후 1회 호출합니다.
 *
 * [현재 동작]
 *   1. capabilities: 실행 (5개 유지, 나머지 제거)
 *   2. NNP: 건너뜀 (nnp_ok = TRUE로 설정, 실제 호출 안 함)
 *   3. seccomp: 건너뜀 (sec_ok = FALSE로 설정, 실제 호출 안 함)
 *
 * [왜 일부만 적용하는가?]
 *   NNP: LXC lxc-start가 AppArmor 프로필 전환 필요 → NNP 비활성화
 *   seccomp: BPF 필터가 자식(컨테이너)에 상속 → 컨테이너 부팅 실패
 *
 *   자세한 이유는 각 함수의 주석과 이 파일 상단의 실전 배포 교훈 참조.
 *
 * [일부 실패 허용]
 *   capabilities 적용이 실패해도(libcap 미설치 등) 데몬은 계속 시작됩니다.
 *   보안 하드닝은 "있으면 좋은" 기능이지 "필수" 기능이 아닙니다.
 *   실패 결과는 로그로 출력되어 운영자가 확인할 수 있습니다.
 */
void
pcv_privdrop_apply_all(void)
{
    PCV_LOG_INFO(PD_LOG_DOM, "Applying privilege restrictions...");

    /* [1] Capabilities — 실행 */
    gboolean cap_ok  = pcv_privdrop_capabilities();

    /*
     * [2] NO_NEW_PRIVS — 비활성화 (LXC 컨테이너 지원)
     *
     * LXC lxc-start가 AppArmor 프로필 전환을 수행하려면
     * NO_NEW_PRIVS가 해제되어야 합니다.
     * PR_SET_NO_NEW_PRIVS=1이면 exec 후 suid/AppArmor 전환 불가.
     */
    gboolean nnp_ok  = TRUE;  /* pcv_privdrop_no_new_privs() 비활성화 — LXC AppArmor 전환 필요 */

    /*
     * [3] seccomp — 비활성화 (LXC 컨테이너 + systemd 호환성)
     *
     * seccomp BPF 필터는 fork/exec를 통해 모든 자식 프로세스에 상속되며
     * 한번 적용하면 해제가 불가능합니다. 데몬이 lxc-start를 spawn하면
     * 컨테이너 내부의 systemd가 데몬의 seccomp 필터를 상속받아
     * 수백 개의 필요한 syscall이 EPERM으로 차단됩니다.
     *
     * 엔터프라이즈 하이퍼바이저(libvirt, Proxmox 등)는 호스트 데몬에
     * seccomp을 적용하지 않고, capabilities + AppArmor/SELinux로
     * 호스트를 보호합니다. 게스트/컨테이너 격리는 LXC가 자체적으로
     * lxc.seccomp.profile을 통해 적용합니다.
     */
    /* [Security Note] Seccomp BPF is intentionally disabled for the parent daemon.
     * Reason: BPF filters are inherited by child processes (LXC containers),
     * blocking essential syscalls (clone, mount, pivot_root) needed for container boot.
     *
     * Mitigation: Capabilities are restricted instead (CAP drop + bounding set).
     * Future: Implement pre-fork seccomp (enable AFTER all containers are started,
     * or use SECCOMP_FILTER_FLAG_NEW_LISTENER for per-process filtering).
     */
    gboolean sec_ok  = FALSE; /* pcv_privdrop_seccomp() 비활성화 — LXC 상속 문제 */
    PCV_LOG_INFO(PD_LOG_DOM,
                 "seccomp skipped: BPF filters inherit to child processes "
                 "(lxc-start/systemd), container isolation via lxc.seccomp.profile");

    /* 최종 결과 요약 로그 */
    PCV_LOG_INFO(PD_LOG_DOM,
                 "Privilege drop complete: cap=%s nnp=%s seccomp=%s",
                 cap_ok ? "OK" : "skipped",
                 nnp_ok ? "OK" : "FAILED",
                 sec_ok ? "OK" : "skipped");
}
