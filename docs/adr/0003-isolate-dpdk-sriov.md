# ADR-0003: DPDK/SR-IOV를 컴파일 플래그 뒤로 격리한다

날짜: 2026-04-10
상태: accepted

## 맥락
코드 감사(2026-04-10)에서 dpdk.* 7개 + sriov.* 7개 = 14 RPC가 등록되어
있으나 DESIGN.md S1~S13 어디에도 매핑되지 않는다. DPDK/SR-IOV는 NFV·통신
사업자급 워크로드를 위한 것으로, §0의 "중소 규모 사설 KVM 클러스터"와
어울리지 않는다.

운영 비용:
- 빌드 의존: ovs-dpdk, ip link 확장, hugepage 설정
- 테스트 부채: tests/test_dpdk.c (7케이스), tests/test_sriov.c (5케이스)
  — OVS-DPDK 미설치 환경에서는 사실상 스킵
- 하드웨어 의존: SR-IOV 지원 NIC + IOMMU + VT-d 활성화 BIOS
- 보안 표면: VFIO 디바이스 직접 노출

3노드 운영 클러스터에서 DPDK/SR-IOV는 한 번도 활성화된 적이 없음 (감사
로그 기준).

## 결정
ADR-0002와 동일 패턴:

1. `-DPCV_DPDK_ENABLED=0/1` + `-DPCV_SRIOV_ENABLED=0/1` 도입, **기본 0**
2. `make multi-dpdk`, `make multi-sriov` 옵션 빌드 타깃 (deprecated 표시)
3. dispatcher.c에서 `#if PCV_DPDK_ENABLED` / `#if PCV_SRIOV_ENABLED` 가드
4. off 빌드에서 호출 시 -32601
5. CI에 "off 빌드 심볼 부재" 검증
6. **2026-10-10 재평가**, 사용 보고 0이면 삭제

OVN(ADR-0002)과 별도 ADR로 분리하는 이유: 각 기능의 유지/삭제 결정은
독립적으로 내릴 수 있어야 한다. 묶으면 한 건이 살아남을 때 전체가 살아남게
된다.

## 결과
- 좋음: 기본 빌드 RPC 표면 14개 추가 축소 (~232 → ~218)
- 좋음: 빌드 의존 ovs-dpdk 제거, hugepage 사전요건 불필요
- 좋음: VFIO 보안 표면 축소
- 나쁨: 컴파일 플래그 추가 2개
- 나쁨: 6개월간 코드/테스트 유지

## 하지 않기로 한 것
- DPDK/SR-IOV를 기본값으로 재활성화. 통신사업자 워크로드는 PureCVisor의
  대상이 아니다(§0).
- DPDK와 SR-IOV를 단일 플래그로 묶는 것. 일부 운영자가 SR-IOV만 필요할
  가능성이 있어 독립 결정 가능하게 둔다.
- libvirt PCI passthrough 일반 기능까지 격리. 그것은 VM hostdev로 S1에서
  암묵적으로 정당화되며 별개 표면이다.
