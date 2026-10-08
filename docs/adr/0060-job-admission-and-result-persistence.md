# ADR-0060: Job 접수와 결과 저장 여부를 분리한다

- **상태:** Implemented
- **일자:** 2026-10-08
- **적용:** 공개 Single Edge 소스. 구현 반영이며 전체 환경 인증을 뜻하지 않는다.

## 결정

1. 데몬은 DB 비활성 상태에서도 생존하지만 추적 Job의 신규 접수는 거부한다.
   정확히 한 row INSERT 성공을 확인한 후에만 ID·accepted 응답을 반환한다.
2. 기존 job-XXXXXXXX 형식·SQLite 스키마를 유지하고 PK 충돌만 최대 32회 재발급한다.
   기존 row 교체와 SQL 오류의 무한/장기 재시도는 금지한다.
3. 상태·결과·취소 쓰기는 SQL 완료와 해당 row 변경을 확인해 저장 성공 여부를 반환한다.
4. 실제 작업 결과·audit는 저장 실패와 구분한다. 추적 Job 완료 WS는 result_persisted를
   전달하며 UI는 false인 경우 Job ID와 조회/로그 확인 안내를 기존 알림에 남긴다.
   legacy 비추적 완료 이벤트는 이 필드 없이 유지한다.


## 검증 경계

[최신 공개 인계](../operations/2026-10-08-public-source-refresh-handoff.md)를 따른다. 독립 리뷰·장비별 인수와
전체 감사 미완료 상태는 별도다.
