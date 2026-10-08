# DPDK·hugepage 수용량 사전 검사

이 도구는 지정한 호스트 NUMA 노드의 2MiB 페이지가 계획한 추가 OVS·VM 메모리를
수용하는지 진단한다. 호스트·plan을 읽기만 하며 sudo·패키지 설치 없이 실행한다.
실제 VM 시작을 제어하거나 hugepage를 할당하지 않는다.

```bash
python3 scripts/dpdk_capacity_preflight.py --plan scripts/examples/dpdk-capacity-plan.json
make check-dpdk-owned-lifecycle
```

첫 명령은 예제 계획으로 현재 호스트를 조회한다. 예제는 OVS 1,024MiB·VM 2,048MiB·
추가 여유 256MiB를 node0에 요구한다. 실제 배치에 맞춘 별도 plan을 입력한다.
`--sysfs-root`는 임시 sysfs fixture 또는 별도로 연결된 sysfs root를 읽을 때 사용한다.

## Plan 계약

[예제 JSON](examples/dpdk-capacity-plan.json)의 필드만 허용하며 중복 key·오타·음수·
문자열형 메모리·1GiB page size·NUMA 미지정은 확인 불가다. MB 필드는 1MiB 단위의
0..2,147,483,647 정수이고 guest memory는 1 이상이다.

- `page_size_kib`: 2048만 지원한다. 다른 크기의 pool을 합산하거나 자동 변환하지 않는다.
- `ovs.socket_mem_mb`: 명시적 comma-separated 노드별 MiB. 이 도구는 인덱스가 Linux host node ID와 일치하는 배치를 대상으로 한다. 실제 DPDK
  socket/NUMA ID와 Linux node ID의 대응도 운영자가 확인한다.
  생략한 뒤의 노드는 0이며 비어 있는 설정으로 OVS 기본값을 추정하지 않는다.
- `ovs.allocation`: `pending`은 아직 반영되지 않은 OVS 초기 할당으로 추가 요구에 넣는다.
  `accounted`는 이미 현재 free/reserved에 반영된 OVS 몫을 재차감하지 않는다.
  도구가 OVS 상태를 조회하는 것은 아니므로 운영자가 현재 설정·프로세스로 확인해야 한다.
  OVS 재시작 뒤 기존 몫이 해제될 것으로 추정해 더하지 않는다.
- `ovs.headroom_mb`: 노드 ID 문자열과 추가 MiB의 객체. OVS의 이후 동적 소비 등을 위한
  명시적 여유량이며 socket memory 자체가 최대 소비량인 것으로 해석하지 않는다.
- `guests`: 아직 할당/예약되지 않은 신규 요청만 넣는다. 각 name은 중복 없는 ASCII
  식별자이고 `host_node`는 실제 호스트 NUMA 배치 정책으로 확인한 노드다.
  libvirt의 guest NUMA nodeset을 host node ID로 복사하거나 미정 배치를 추정하지 않는다.
  이미 free/reserved에 반영된 guest를 다시 신규 요구로 넣으면 중복 계산이다.

## 판정과 계산

| JSON status | 종료 코드 | 의미 |
|---|---:|---|
| CAPACITY-SUFFICIENT | 0 | 입력 요구가 현재 스냅샷의 보수적 노드별 가용량 이내 |
| CAPACITY-INSUFFICIENT | 1 | 관측은 가능하지만 하나 이상의 노드에서 페이지 부족 |
| INDETERMINATE | 2 | 입력 오류·읽기 실패·미확인 노드·counter 변화/불일치. 성공으로 취급하지 않음 |

각 OVS/guest/headroom 소비자를 `(MiB + 1) // 2`로 올림한 뒤 노드별로 합산한다.
`available_pages = max(0, node.free_hugepages - global.resv_hugepages)`다.
예약의 NUMA 위치는 확정할 수 없어 각 노드에 전역 예약 최대치를 보수적으로 반영한다.
부족 수는 `shortage_pages`이며 다른 노드의 여유로 지정 노드 부족을 메우지 않는다.
글로벌·노드 counter와 topology를 앞뒤로 읽어 변화·합계 불일치는 확인 불가로 반환한다.

항상 `advisory_only=true`다. 순간 관측은 예약을 만들지 않으며 앞뒤 값이 같아도 중간의
소비·해제가 숨을 수 있다. mount quota, cgroup 제한, 실제 libvirt 정책, 다른 할당자와
OVS 동적 소비는 별도다. 도구가 충분이라고 해도 실제 시작 인수와 전체 자원 예약 보장은 남는다.
이 도구는 `vm.create`/`vm.start`를 호출하지 않는다. 별도 ADR-0063 시작 연동이
실제 XML 기준 cold-start 사전 검사를 구현했다. 공개 2.0.0 소스
반영을 완료했으며 실제 OVS/hugepage 동시 인수는 남는다.

설계 근거는 [ADR-0062](../docs/adr/0062-dpdk-hugepage-capacity-snapshot.md),
검증·운영 경계는 로컬 인계를 따른다.
