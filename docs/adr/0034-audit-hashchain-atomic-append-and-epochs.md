# ADR-0034: 감사 해시체인은 SQLite 원자 append와 보존 가능한 epoch를 사용한다

날짜: 2026-08-09
상태: Verified — 로컬 회귀와 운영 DB migration·live 검증 완료
Single Edge 적용 상태: 활성, production·운영 적용 및 current epoch 전수 검증 완료

## 맥락

감사 워커는 프로세스 안에서 하나지만 `pcv_audit_init()`이 DB head를 메모리에 한 번만
복사한다. 같은 DB를 연 두 프로세스가 겹치면 각자 stale head로 append할 수 있고, 실제
운영 DB에서 같은 predecessor를 가진 rowid 1516과 1522가 확인됐다. 그 뒤 수만 건은
기존 검증기가 첫 break에서 멈춰 연속성을 판정할 수 없다.

또한 30일 retention은 prefix를 단순 삭제한다. 첫 잔존 행의 predecessor도 함께 사라지므로
현재 구조는 동시 writer가 없어도 보존 기간을 처음 넘는 순간 검증이 실패할 수 있다.

## 결정

1. `G.chain_head`를 해시 정본으로 사용하지 않는다.
2. 모든 append는 `BEGIN IMMEDIATE` 안에서 active epoch의 최신 `rec_hash`를 읽고 hash 계산,
   INSERT, COMMIT한다.
3. `(chain_epoch, prev_hash)` partial unique index로 한 predecessor의 두 successor를 거부한다.
4. `<db_path>.lock` sidecar advisory lock으로 두 번째 데몬을 listener 전에 거부한다.
5. 기존 행은 재해시하지 않는다. nullable `chain_epoch`와 별도 epoch metadata를 추가하고,
   legacy가 파손됐으면 break rowid를 보존한 새 epoch에서 전방 체인을 시작한다.
6. retention은 삭제 전 predecessor checkpoint 기록과 prefix DELETE를 같은 transaction에서
   수행한다.
7. health와 Prometheus는 active epoch의 현재 무결성과 알려진 과거 break를 별도 상태로
   노출한다.
8. hash/append/verify production 모듈을 테스트가 직접 링크하고, 두 connection/process
   반사실로 분기 불가를 검증한다.
9. active epoch가 기동 검증에서 파손되면 자동 baseline이나 file-only 서비스로 우회하지
   않고 listener 오픈 전에 데몬 기동을 중단한다. legacy 파손의 최초 migration만 과거
   break를 metadata와 health에 보존한 새 epoch로 격리한다.

## 결과

- 여러 connection이 있어도 DB가 head 선택과 append 순서를 직렬화한다.
- 이미 파손된 과거를 정상으로 가장하지 않고 새 기록의 전방 검증을 회복한다.
- 정상 retention이 위변조 경보로 오인되지 않는다.
- 매 append에 transaction과 head SELECT 비용이 추가된다. 임시 WAL DB에서 atomic append
  1,000건을 0.051~0.052초(약 19.3k ops/s)에 처리해 현재 초당 1,000건 한도를 상회했다.
- 평문 SHA-256과 동일 DB metadata이므로 로컬 root의 전체 재작성은 계속 비범위다.

## 관련

- ADR-0001: 단일 프로세스 + GMainLoop
- ADR-0018: fire-and-forget audit 기록 정책
- ADR-0025: production-path 반사실 검증
- `tests/`의 hashchain 원자 append·epoch 연속성 회귀 테스트
