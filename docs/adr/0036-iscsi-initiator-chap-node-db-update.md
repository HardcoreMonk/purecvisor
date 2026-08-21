# ADR-0036: iSCSI initiator CHAP는 open-iscsi node DB를 호환 갱신한다

날짜: 2026-08-09
상태: Verified
Single Edge 적용 상태: production 구현·로컬 회귀와 정상·거부·복구 login/logout 완료

## 맥락

`iscsiadm -m node --op=update -n node.session.auth.password -v <password>`는 비밀번호를
자식 argv와 `/proc/<pid>/cmdline`에 노출한다. open-iscsi는 비밀을 stdin으로 받는 node
update API를 제공하지 않고 공개 library의 node 표면도 조회 전용이다. 한 target/portal에는
여러 TPGT·iface record가 합법적으로 존재할 수 있으므로 `default` 파일 하나만 바꾸는 것도
기존 `iscsiadm` match 의미와 다르다.

## 결정

1. discovery와 login/logout은 비밀 없는 `iscsiadm` argv를 유지한다.
2. CHAP authmethod, username, password는 별도 in-process node DB 모듈이 함께 갱신한다.
3. open-iscsi의 `lock` → `lock.write` hard-link 잠금과 같은 경계를 사용한다.
4. exact target과 address/port에 맞는 모든 TPGT·iface record를 갱신한다.
5. 모든 변경본을 먼저 stage하고 파일별 0600 atomic rename을 수행한다. 실행 중 오류면 이미
   교체한 파일을 역순 rollback한다.
6. 경로는 `openat`과 `O_NOFOLLOW`로 열거하며 symlink, 모호한 duplicate key, 무매치,
   bounded limit 초과는 login 전에 fail-closed한다.
7. 비밀번호 포함 메모리는 강제 wipe하고 최종 node record 외 argv, 로그, audit, GError,
   temp 파일에 남기지 않는다.
8. login 실패 시 node DB 자격을 되돌리지는 않는다. 기존 영속 자격 의미를 유지한다.
9. discovery는 `iscsiadm -m discoverydb -t sendtargets -p <portal> --discover`로 node record를
   영속한다.
10. production node root는 target IQN이 존재하는 `/var/lib/iscsi/nodes` 또는 legacy
    `/etc/iscsi/nodes`를 선택한다. 둘 다 존재하거나 안전하게 판별할 수 없으면 login 전에
    fail-closed한다.

## 결과

- 비특권 `/proc` 열람뿐 아니라 root 관측에서도 비밀번호 argv가 구조적으로 사라진다.
- 정상적인 다중 NIC·multipath·offload record를 단일 `default` 가정으로 차단하지 않는다.
- open-iscsi 내부 비공개 ABI에 링크하지 않는 대신 node DB 텍스트·lock 호환 계약을 유지해야
  한다. 지원 open-iscsi 업그레이드 시 fixture와 upstream format을 재대조한다.
- 프로세스 crash가 여러 파일 rename 사이에 발생하면 record 집합 일부가 새 값일 수 있다.
  각 파일은 완성본이며 다음 요청이 전체를 수렴시킨다. 실행 중 감지된 실패는 즉시 원복한다.
- open-iscsi 2.1.11에서 정상 session 생성·logout, 오답 인증 거부와 커널
  `authenticate_fails` 증가, 정상 자격 복구·재로그인을 확인했다. 실행 중 argv와 종료 후
  journal·영속 저장소에 합성 비밀이 남지 않았고 임시 자원을 모두 제거했다.

## 관련

- ADR-0025: production-path 반사실 검증
- ADR-0028: hidepid는 완화이며 secret argv 제거를 대체하지 않음
- `tests/`의 iSCSI CHAP 비밀 무잔류·rollback 회귀 테스트
