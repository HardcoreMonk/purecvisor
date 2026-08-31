# generic OVN 문서·GitHub Pages 동기화 운영 인계

> **일자:** 2026-08-31
> **대상:** PureCVisor Single Edge 공개 저장소와 `purecvisor.site`
> **상태:** LOCAL-PASS / DEPLOY-PENDING — commit, push, Pages run과 live smoke receipt 기록 전

## 1. 목적

공개 네트워크 문서를 현재 source contract와 맞추고, GitHub Pages의 Networking 장으로
직접 이동할 수 있게 한다. 공개 문서에는 운영 노드 식별자, 사설 주소, 내부 녹화 경로,
비공개 저장소 commit 식별자와 인증정보를 기록하지 않는다.

## 2. 현행 공개 계약

- 네트워크 변경 전에 Web UI `호스트 네트워크 기준선` 또는
  `GET /api/v1/networks/host-baseline`으로 관리 interface/IP, IPv4 route·connected CIDR,
  Linux bridge/port, OVS bridge/port와 tenant VPC CIDR을 확인한다. 이 REST 경로는 읽기
  전용 RPC `network.host.info`에 매핑된다.
- dispatcher에 등록된 generic OVN은 정확히 18개 RPC다.

| 영역 | 개수 | RPC |
|---|---:|---|
| 상태 | 1 | `ovn.status` |
| 스위치 | 4 | `ovn.switch.create`, `ovn.switch.delete`, `ovn.switch.list`, `ovn.switch.detail` |
| 포트 | 2 | `ovn.port.add`, `ovn.port.remove` |
| ACL | 2 | `ovn.acl.add`, `ovn.acl.list` |
| 라우터 | 5 | `ovn.router.create`, `ovn.router.delete`, `ovn.router.list`, `ovn.router.detail`, `ovn.router.add_port` |
| DHCP | 1 | `ovn.dhcp.enable` |
| NAT | 2 | `ovn.nat.add`, `ovn.nat.list` |
| 테넌트 | 1 | `ovn.tenant.create` |

- switch create는 L2 리소스만 만들고 subnet을 받지 않는다. DHCP는 별도 enable 단계에서
  switch ownership을 기록한다.
- switch 삭제는 같은 ownership marker의 DHCP option을 자동 정리하고 다른 switch와
  foreign row를 보존한다.
- 인증 REST ACL/NAT 목록은 각각 `switch`, `router` query를 canonical RPC params에
  전달한다. 필수 filter 누락은 전체 목록으로 넓히지 않고 `-32602`로 거부한다.
- ACL/NAT/DHCP/tenant의 미등록 역동작, router port 제거, 미완성 OVN/NFV Load Balancer와
  production caller가 없는 VM 자동 포트 helper는 공개 사용자 기능이 아니다.
- generic OVN `NET-OVN-01~07`과 Local VPC OVN backend의 지원 승격은 별도 gate다.
- 공개 데이터베이스 문서와 서비스 아키텍처는 이 공개 source에 실제 존재하는 로컬 SQLite
  9개를 기준으로 하며 다른 내부 배포판의 저장소 수와 혼용하지 않는다.

## 3. 문서와 Pages 변경 범위

- 운영 정본: `docs/GUIDE.md`, `docs/SERVICE_FUNCTIONAL_TEST_SCENARIOS.md`,
  `docs/DEVELOPMENT_VERIFICATION_POLICY.md`
- 공개 경계: `docs/PUBLIC_RELEASE_BOUNDARY.md`, `docs/ADR_INDEX.md`, `README.md`
- Pages 운영: `docs/PUBLIC_DOCUMENTATION_SITE.md`, landing `lastUpdated`, Header의 Networking
  6장 직접 link와 `site/scripts/check-site.mjs`. Storage는 landing 문서 맵·sidebar에서 유지
- 근거 리뷰: `docs/ui-reviews/2026-08-30-public-site-current-state-refresh.md`의 2026-08-31
  후속 기록

## 4. 검증 receipt

| 검증 | 기대 결과 | Receipt |
|---|---|---|
| `git diff --check` | Markdown·Astro·JavaScript whitespace 오류 0 | **PASS** |
| `cd site` 후 `npm run check` | Networking 계약, navigation, 전체 route와 내부 link PASS | **PASS** — HTML 26개, artifact 91개 |
| 공개 소스·경계 관련 저장소 게이트 | 정확한 18 RPC, 금지 기능 비노출, 공개 주석 정책 PASS | **PASS** — `make test` C 1,375/1,375 + audit 5/5, `make check-all` 38/38, first-party comments 0 |
| release·UI artifact | C23 release 경고 0, bundle/SW freshness, 소스맵 부재 | **PASS** — source `42a9b244`, cache `vf4b3dfc0` |
| GitHub Pages workflow | build·deploy 성공 | PENDING |
| custom domain live smoke | `/`, `/ko/`, `/en/`, `/ko/infrastructure/networking/` HTTP 200과 현행 계약 확인 | PENDING |

## 5. 배포 receipt

| 항목 | 값 |
|---|---|
| 공개 commit | PENDING |
| push | PENDING |
| GitHub Pages run | PENDING |
| live canonical 확인 | PENDING |

commit·run 식별자는 공개 저장소에서 확인 가능한 값만 여기에 기록한다. 이 문서 자체의
receipt 보강 commit이 따로 생기면 기능·콘텐츠 commit과 구분해 표시한다.

## 6. 완료 판정

다음 조건을 모두 만족할 때만 `Verified`로 갱신한다.

1. 로컬 Pages check와 공개 저장소 범위 게이트가 통과한다.
2. main push 뒤 Pages build·deploy가 성공한다.
3. custom domain Networking 장에서 host baseline, 정확한 18 RPC, 올바른 switch create,
   DHCP cleanup, REST ACL/NAT filter와 `-32602` 경계를 확인한다.
4. Header의 Networking이 6장으로 이동하고 Storage는 landing 문서 맵·sidebar에서
   접근 가능하다.
5. 과거 잘못된 subnet 포함 switch create, `vm_port` 사용자 기능, Load Balancer 호출 절차가
   공개 Networking 장에 없다.
