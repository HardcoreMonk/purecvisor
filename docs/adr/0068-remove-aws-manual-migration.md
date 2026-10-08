# ADR-0068: PCV 본체의 AWS 수동 이관 제거

- **상태:** Implemented
- **일자:** 2026-10-08
- **적용:** 공개 Single Edge 소스. 구현 반영이며 전체 환경 인증을 뜻하지 않는다.

## 결정

EC2 Import/Export·Near-Live와 전용 작업 조회/취소, CLI/UI/REST/RPC·빌드 입력을 제거한다.
일반 VM·OVA·S3 backup·공통 Job/WS·QGA는 보존한다. 과거 cloud DB·VM·디스크·AWS 자원은
삭제하지 않는다. 폐기 RPC/REST는 일반 미지원 처리, UI 북마크는 기존 도움말로 이동한다.
기존 2.0.0 artifact를 덮어쓰지 않으며 HCP v3/addon을 자동 대체품으로 선언하지 않는다.


## 검증 경계

[최신 공개 인계](../operations/2026-10-08-public-source-refresh-handoff.md)를 따른다. 독립 리뷰·장비별 인수와
전체 감사 미완료 상태는 별도다.
