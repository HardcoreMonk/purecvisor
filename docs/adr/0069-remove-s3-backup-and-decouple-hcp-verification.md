# ADR-0069: S3 백업 제거와 HCP 연동 검증 분리

- **상태:** Implemented
- **일자:** 2026-10-08
- **적용:** 공개 Single Edge 소스. 구현 반영이며 전체 환경 인증을 뜻하지 않는다.

## 결정

AWS 수동 이관 제거 뒤 남겨 두었던 S3 백업도 본체에서 제거한다. S3 호환 endpoint를 통한
동일 구현도 함께 제거하며 대체 provider나 복구 기능을 새로 만들지 않는다. 실제 구현은
S3 export이며 대응하는 S3 restore 경로는 없다.

`backup.export_s3` RPC, `POST /api/v1/backup/export-s3`, `pcvctl backup export-s3`,
worker·백업 엔진·자격증명 읽기·AWS CLI 실행·AppArmor 실행 허용·도움말을 제거한다.
폐기 요청은 기존 미지원 처리로 거절한다. 로컬 백업 정책·증분·검증·restore·리텐션,
OVA·일반 VM·Job·WS·공통 보안 유틸리티는 보존한다.

기존 S3 객체·스냅샷·설정·자격증명·설치된 AWS CLI를 삭제하지 않는다. 개발 소스의 기능과
실행 허용만 제거한다. 운영 설치·공개 2.0.0 artifact 교체는 이 결정에 포함하지 않는다.

외부 관리 시스템은 별도의 범용 연동 기준으로 재설계 중이므로 이번 본체 변경의 검증·출시 gate에서
정확 버전 의존성·companion·plugin 연동을 제외한다. 외부 관리 시스템 제품 코드는 수정하지 않으며,
과거 검증 근거는 역사로 보존한다. 범용 연동 설계의 완료나 호환성을 선언하지 않는다.


## 검증 경계

[최신 공개 인계](../operations/2026-10-08-public-source-refresh-handoff.md)를 따른다. 독립 리뷰·장비별 인수와
전체 감사 미완료 상태는 별도다.
