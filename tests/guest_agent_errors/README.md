# QGA 오류 수명 회귀

실제 네 worker와 RPC formatter를 연결한다. libvirt 경계만 대역이며 domain/pool 반환이 오류 문자열을 해제하도록 한다. 정상·비 UTF-8·빈 오류·성공 16개와 sanitizer, 수정 전 worker 복원 반례 4개를 확인한다. 실제 VM 연결 인수와 구분한다.

exec worker는 `guest-get-osinfo`를 먼저 부르고 요청 조립·예산·출력을 `src/modules/dispatcher/guest_exec_request.c`에 맡긴다. 그래서 러너는 그 파일을 함께 링크하고, 대역에 `virGetLastErrorCode`(비치명 코드 — osinfo 실패 뒤 `/bin/sh` guest-exec까지 가서 같은 원래 오류로 끝난다)를 두며, agent 상한 단언은 osinfo 5초·그 밖 1..30초다. Windows·Linux·무응답·여러 줄·CP949·호출 순서·예산 시나리오는 `make test`가 돌리는 hermetic 결선 시험 `tests/test_guest_exec_worker_fixture.py`가 맡는다(이 러너는 오류 수명 회귀만).

러너는 `--source` 트리의 `src/modules/dispatcher/handler_vm_guest.c`(2026-11-01 VM 계열 분할 [ADR-0096](../../docs/adr/0096-vm-family-split.md) 뒤 게스트 에이전트 worker 위치)를 읽는다. `--baseline`은 수정 전 worker가 든 C 파일이며 아래 예시의 기준선(분할 전 `handler_vm_lifecycle.c` 사본)은 2026-10-04 로컬 작업 사본이라 저장소에 없다.

```bash
python3 tests/guest_agent_errors/run.py --source . --baseline baseline/src/modules/dispatcher/handler_vm_lifecycle.c --output qga-after
```
