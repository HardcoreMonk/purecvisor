/**
 * @file pcv_privdrop.h
 * @brief 권한 격하 + seccomp-bpf 샌드박스 — 공개 헤더
 *
 * Sprint D-3에서 도입된 데몬 보안 하드닝 모듈의 공개 인터페이스입니다.
 * main.c에서 root 권한 필수 초기화 완료 후 호출하여 권한을 최소화합니다.
 *
 * [최소 권한 원칙 (Principle of Least Privilege)]
 *   데몬이 root로 시작하지만, 필수 초기화 후에는 필요한 최소 권한만 유지합니다.
 *   - capabilities: 5개만 유지 (NET_ADMIN, NET_BIND_SERVICE, SYS_ADMIN, SETUID, DAC_OVERRIDE)
 *   - NNP: 현재 비활성화 (LXC AppArmor 호환성)
 *   - seccomp: 현재 비활성화 (LXC 컨테이너 seccomp 상속 문제)
 *
 * [실행 순서 — main.c에서 호출]
 *   방법 1 (개별 호출):
 *     pcv_privdrop_capabilities()  → capability 제한
 *     pcv_privdrop_no_new_privs()  → exec 권한 상승 차단 (현재 비활성화)
 *     pcv_privdrop_seccomp()       → syscall 화이트리스트 (현재 비활성화)
 *
 *   방법 2 (일괄 호출, 권장):
 *     pcv_privdrop_apply_all()     → 위 세 함수를 순서대로 호출 (일부 실패 허용)
 *
 * [현재 활성 상태]
 *   capabilities: 활성 (libcap 설치 시)
 *   NNP:          비활성 (LXC AppArmor 프로필 전환 필요)
 *   seccomp:      비활성 (BPF 필터가 자식 프로세스에 상속되어 컨테이너 부팅 실패)
 *
 * [요구 패키지]
 *   capabilities: libcap2-dev  (-lcap)
 *   seccomp:      libseccomp-dev (-lseccomp)
 *   미설치 시 stub 구현 (빌드 성공, 경고 로그만 출력)
 *
 * [조건부 컴파일]
 *   HAVE_LIBCAP:  Makefile에서 pkg-config libcap 탐지 시 정의
 *   HAVE_SECCOMP: Makefile에서 pkg-config libseccomp 탐지 시 정의
 *
 * [주의사항]
 *   - 각 함수는 독립 실행 가능 (하나 실패해도 나머지 계속)
 *   - pcv_privdrop_no_new_privs()는 LXC AppArmor 전환과 충돌하여 현재 비활성화
 *   - seccomp 화이트리스트는 pcv_privdrop.c의 ALLOWED_SYSCALLS 배열에서 관리
 *   - pcv_privdrop_apply_all()은 내부에서 NNP와 seccomp를 비활성화하므로
 *     실제로는 capabilities만 적용됩니다
 */

#ifndef PURECVISOR_PRIVDROP_H
#define PURECVISOR_PRIVDROP_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * pcv_privdrop_capabilities:
 * 데몬에 필요한 최소 capability 집합만 유지하고 나머지를 전부 제거합니다.
 *
 * [유지하는 capability]
 *   CAP_NET_ADMIN       — bridge/nft 네트워크 설정, OVS 관리
 *   CAP_NET_BIND_SERVICE— 포트 80/443 바인딩 (1024 미만 포트)
 *   CAP_SYS_ADMIN       — KVM/libvirt 필수 (virDomainCreate, ioctl KVM_*)
 *   CAP_SETUID          — 워커 프로세스 UID 변경
 *   CAP_DAC_OVERRIDE    — ZFS/socket 파일 접근 (권한 무시)
 *
 * [capability 집합별 역할]
 *   Permitted:   프로세스가 가질 수 있는 최대 권한 (상한)
 *   Effective:   실제 커널이 권한 체크에 사용하는 집합
 *   Inheritable: exec() 후 자식에 전달 가능한 집합
 *   Bounding:    새로 acquire할 수 있는 최대 범위 (불필요한 것 제거)
 *
 * @return TRUE 성공, FALSE 실패 (libcap 미설치 포함)
 */
gboolean pcv_privdrop_capabilities(void);

/**
 * pcv_privdrop_no_new_privs:
 * PR_SET_NO_NEW_PRIVS = 1 설정합니다 (현재 pcv_privdrop_apply_all에서 비활성화됨).
 *
 * [설명]
 *   이후 exec()로 실행되는 자식 프로세스(nft, zfs 등)가
 *   setuid/setgid 바이너리로 권한 상승하는 것을 차단합니다.
 *   한 번 설정하면 해제가 불가능합니다 (one-way flag).
 *
 * [비활성화 이유]
 *   LXC lxc-start가 AppArmor 프로필을 전환해야 하는데,
 *   NNP=1이면 프로필 전환이 차단되어 컨테이너 시작이 실패합니다.
 *
 * @return TRUE 성공
 */
gboolean pcv_privdrop_no_new_privs(void);

/**
 * pcv_privdrop_seccomp:
 * seccomp-bpf ALLOWLIST 필터를 적용합니다 (현재 pcv_privdrop_apply_all에서 비활성화됨).
 * 화이트리스트 외의 syscall은 EPERM을 반환합니다.
 *
 * [비활성화 이유]
 *   seccomp BPF 필터는 fork/exec을 통해 모든 자식 프로세스에 상속되며
 *   해제가 불가능합니다. 데몬 → lxc-start → 컨테이너 init(systemd)로
 *   상속되어 수백 개의 필요한 syscall이 차단됩니다.
 *
 * [허용 syscall 집합] (pcv_privdrop.c ALLOWED_SYSCALLS 배열)
 *   I/O:       read, write, close, ioctl, epoll_*, poll, select
 *   io_uring:  io_uring_setup, io_uring_enter, io_uring_register
 *   소켓:      socket, bind, connect, accept4, sendmsg, recvmsg
 *   프로세스:  clone, fork, wait4, execve, exit, exit_group
 *   메모리:    mmap, mprotect, munmap, brk, madvise
 *   파일:      open, openat, stat, fstat, lstat, access, dup
 *   신호:      rt_sigaction, rt_sigprocmask, rt_sigreturn, kill
 *   기타:      futex, clock_gettime, nanosleep, prctl, getcwd
 *
 * @return TRUE 성공, FALSE 실패 (libseccomp 미설치 포함)
 */
gboolean pcv_privdrop_seccomp(void);

/**
 * pcv_privdrop_apply_all:
 * 위 세 함수를 순서대로 호출하는 convenience 래퍼입니다.
 *
 * [현재 동작]
 *   1. pcv_privdrop_capabilities() — 실행 (libcap 설치 시)
 *   2. pcv_privdrop_no_new_privs() — 건너뜀 (LXC 호환성)
 *   3. pcv_privdrop_seccomp()      — 건너뜀 (LXC seccomp 상속)
 *
 * 일부 실패해도 계속 진행하며, 결과를 로그로 출력합니다.
 * 데몬이 권한 설정 실패로 종료되면 안 되기 때문입니다.
 */
void pcv_privdrop_apply_all(void);

G_END_DECLS

#endif /* PURECVISOR_PRIVDROP_H */
