# QGA 오류 수명 회귀

실제 네 worker와 RPC formatter를 연결한다. libvirt 경계만 대역이며 domain/pool 반환이 오류 문자열을 해제하도록 한다. 정상·비 UTF-8·빈 오류·성공 16개와 sanitizer, 수정 전 worker 복원 반례 4개를 확인한다. 실제 VM 연결 인수와 구분한다.

```bash
python3 tests/guest_agent_errors/run.py --source . --baseline .scratch/aws-manual-stabilization-20261004/baseline/src/modules/dispatcher/handler_vm_lifecycle.c --output .scratch/aws-manual-stabilization-20261004/qga-after
```
