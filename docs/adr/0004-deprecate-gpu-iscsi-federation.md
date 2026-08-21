# ADR-0004: GPU/iSCSI/Federation을 deprecated로 표시하고 격리한다

날짜: 2026-04-10
상태: **superseded by ADR-0006** (2026-04-10, deprecated 윈도우 종료 전 흡수 결정)

## 맥락
코드 감사(2026-04-10)에서 다음 3개 카테고리, 합계 ~10 RPC가 어떤 시나리오
에도 매핑되지 않음:

- **GPU/vGPU** (gpu.list, gpu.attach, mdevctl 래핑) — lspci/mdevctl/vfio-pci
- **iSCSI** (iscsi.target.{create,list,delete}, CHAP) — tgt 데몬 래핑
- **Federation** (federation.site.{join,list,remove}) — 멀티사이트 가입

각각의 비매핑 이유:

**GPU**: §0 "사설 KVM 클러스터"는 GPU 워크로드 대상이 아니다. AI 학습/
추론 클러스터는 별도 제품 영역(k8s + GPU operator). 운영 3노드에 GPU
하드웨어 자체가 없음.

**iSCSI**: §6 "외부 SAN 미지원" 정신과 충돌. ZFS 풀 + 로컬 디스크가 S5
백업 요구사항을 충족시키며, iSCSI 타겟 노출은 PureCVisor를 스토리지
어플라이언스로 만드는 방향 — 제품 정의(§0) 일탈.

**Federation**: §6에 "사이트 자동 페일오버 제외" 명시. 클러스터 빌드 전용
이지만 어떤 운영 시나리오도 멀티사이트를 요구하지 않는다. 3노드 단일
사이트가 현재 검증된 형상.

## 결정
ADR-0002/0003과 동일 패턴 + deprecated 명시:

1. `-DPCV_GPU_ENABLED=0`, `-DPCV_ISCSI_ENABLED=0`, `-DPCV_FEDERATION_ENABLED=0`
   3개 플래그 도입, **기본 0**
2. 옵션 빌드 타깃은 만들지 않음 (DPDK와의 차이 — 사용 흔적이 더 약함)
3. 호출 시 -32601 + 응답 메시지에 "deprecated, see ADR-0004" 한 줄 포함
4. CI 검증 동일
5. **2026-07-10 (3개월) 재평가** — OVN/DPDK보다 짧게: 사용 흔적이 없음
6. 재평가에서 사용 0건이면 코드 자체 삭제, 1건이라도 있으면 해당 시나리오
   를 DESIGN.md에 추가 + 본 ADR superseded

## 결과
- 좋음: 기본 빌드 RPC 표면 ~10개 추가 축소 (~218 → ~208)
- 좋음: tgt 데몬 의존 제거, hostdev 권한 축소
- 좋음: 클러스터 데몬 바이너리 추가 감소
- 좋음: 3개월의 짧은 deprecated 윈도우로 의사결정 빠르게 종결
- 나쁨: deprecated 메시지가 응답에 노출되어 기존 클라이언트가 로그 노이즈
- 포기한 것: "있으면 좋다"식 기능 보존. DESIGN.md 시나리오로 정당화되지
  않으면 코드는 부채.

## 하지 않기로 한 것
- 3개를 단일 플래그로 묶기. 각자 독립 결정.
- iSCSI 이니시에이터(ZFS 위에 올린 LUN 마운트)까지 격리. 그것은 S5 백업의
  원격 저장소 옵션으로 *시나리오에 추가될 여지*가 있음 — 본 ADR은 iSCSI
  *타겟* 노출만 격리.
- GPU passthrough를 영원히 거부. 향후 §0이 확장되면(예: AI/ML 워크로드
  공식 지원) 본 ADR을 supersede.
- Federation을 통째로 삭제하기 전에 etcd 멀티 클러스터 구조와의 의존성을
  먼저 끊을 것 (별도 ADR 필요 시).
