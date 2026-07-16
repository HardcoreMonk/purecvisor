# ADR-0026: 호스트 데몬 MAC 하드닝 — seccomp/NNP 비활성 수용 + capabilities + AppArmor

날짜: 2026-07-17
상태: accepted (2026-07-17 사용자 승인)

## 맥락

보안 평가(OWASP A05 / ISMS-P 2.10, 잔여 하드닝 G1)가 지적했다: 데몬의 **seccomp-bpf**와 **NO_NEW_PRIVS(NNP)**가 둘 다 비활성이고, 실효 호스트 통제는 **capabilities 뿐**이다. 부수적으로, 권한격하 요약 로그가 `nnp=OK`로 표기해 NNP가 활성인 것으로 오인될 소지가 있었다(실제 미적용).

코드 근거(`src/utils/pcv_privdrop.c`):
- **Capabilities** — drop + bounding set으로 **활성**(유지집합: net_admin·net_bind_service·sys_admin·setuid·dac_override 등 libvirt/KVM/LXC 관리에 필요한 최소분).
- **NNP** — 의도적 비활성. `lxc-start`의 **AppArmor 프로필 전환**이 NNP 해제를 요구한다(`PR_SET_NO_NEW_PRIVS=1`이면 exec 후 setuid/AppArmor 전환 불가 → 컨테이너 기동 파손).
- **seccomp** — 의도적 비활성. BPF 필터는 **fork/exec로 전 자식에 상속되고 해제 불가**. 데몬이 `lxc-start`를 spawn하면 컨테이너 내부 systemd가 필터를 상속받아 `clone/mount/pivot_root` 등 수백 syscall이 EPERM으로 차단 → 컨테이너 부팅 파손(과거 실증 breakage: 컨테이너 exec·ZFS clone).
- **업계 관행** — libvirt/Proxmox는 호스트 데몬에 seccomp을 적용하지 않는다. capabilities + AppArmor/SELinux로 호스트를 보호하고, 게스트/컨테이너 격리는 런타임(LXC `lxc.seccomp.profile`/runc)이 자체 seccomp으로 처리한다.

즉 seccomp/NNP 비활성은 "간과"가 아니라 **문서화된 설계 결정**이나, 명시적 ADR로 박혀 있지 않아 감사에서 갭으로 재부상했다(insecure-by-design은 correctness 감사가 잡지 못한다 — ADR-0025 회고 참조).

## 결정

1. **seccomp/NNP는 호스트 데몬에서 비활성 유지**한다. 전면 재활성화는 (a) BPF 상속-파손, (b) NNP↔LXC AppArmor 전환 충돌, (c) VM/LXC/ZFS/OVS/DPDK/backup/cloud 전 경로 syscall 전수 열거의 난제(런타임-only 파손), 로 **고위험**이며 업계 관행에 반한다 → 기각.

2. **실효 호스트 통제 = capabilities(유지) + AppArmor MAC 프로필(신규 채택)**. AppArmor는 seccomp의 상속 문제가 없다 — 자식(qemu·lxc-start)은 자기 프로필(`libvirt-<uuid>`·`lxc-container-*`)로 전환(프로필 규칙상 `Ux`/`Px`)하므로 데몬 프로필이 상속돼 컨테이너/VM을 깨지 않는다. 프로필은 **complain(감사-only) 모드로 배포**하여 기본 배포 동작을 바꾸지 않고, 실서버 검증(위반 로그 수집·`aa-logprof` 보정) 후 운영자가 **opt-in으로 enforce** 전환한다.

3. **정직-로그** — 요약 로그를 `nnp=disabled(LXC-AppArmor) seccomp=disabled(LXC-inherit)`로 정정(오인 제거).

4. **장기(현 스코프 밖)** — seccomp을 실제로 적용하려면 spawn-민감 로직을 별도 헬퍼 프로세스로 분리(공격표면인 RPC/파싱 메인만 tight seccomp) + `SECCOMP_RET_LOG` LOG-모드 선행으로 allowlist를 실증 구축하는 아키텍처 변경이 전제다. 별도 프로젝트로 이연.

## 대안

- **전면 seccomp/NNP 활성**: 기각(상속 파손·NNP↔AppArmor 충돌·수주 규모 고위험·관행 위배).
- **SELinux**: AppArmor 채택(Ubuntu 24.04 기본 LSM, libvirt 관행 정합).
- **현상 유지(capabilities만)**: 부분 채택하되 AppArmor MAC 계층을 추가해 심층방어 강화.

## 결과

- **잔여 위험을 MODERATE로 명시 수용** — capabilities 제한 + AppArmor MAC(complain→enforce 경로) + 업계 관행 정합. 정보보안 결정을 명문화(ADR-0025 "insecure-by-design → challenge + 문서화" 규율 적용).
- 산출물: AppArmor 프로필 `packaging/apparmor/usr.local.bin.purecvisorsd`(complain 배포·postinst complain 로드-only), 검증·enforce 절차 문서 `docs/operations/2026-07-17-apparmor-profile.md`, 정직-로그 수정(`pcv_privdrop.c`).
- **재평가**: 실서버(complain) 위반 로그로 프로필 규칙 완전성 검증 → 무-DENIED 확인 후 enforce 전환. seccomp LOG-모드/아키텍처 분리는 향후 재검토.
