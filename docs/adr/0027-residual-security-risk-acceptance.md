# ADR-0027: 잔여 보안 항목 수용 (OWASP/ISMS-P 평가 후속)

날짜: 2026-07-19
상태: accepted (2026-07-19 사용자 승인)

## 맥락

OWASP Top 10 / ISMS-P 전수 평가(`docs/operations/2026-07-16-security-assessment-owasp-ismsp.md`)와
시정 트랙(v1.2.8~v1.3.9)으로 **HIGH급 6 + 우선순위 잔여 하드닝(B1·C1/C2·G1)을 전부 시정**했다
(CI 계약 게이트 8→21, AppArmor complain→enforce 양 노드 롤아웃). 남은 하위 잔여 22항은 전량
**활성 HIGH 아님**(MED/LOW/하드닝/결정대기/재감사)이다. 그중 **다음 7항은 시정하지 않고 수용**하되,
"insecure-by-design은 명문화한다"(ADR-0025/0026 규율)에 따라 근거를 박는다. 전체 처분은
`docs/operations/2026-07-19-residual-security-disposition.md` tracker가 권위.

## 결정 — 수용(Accept) 7항

배포 프로파일 **P2(다중사용자/공유호스트)** 기준.

1. **MFA 미지원** (A07). 근거: 다중요소는 제품 스코프 결정 사항이며, 필요 시 nginx 종단·외부 IdP(OIDC/SAML)로 보완 가능하다. 코어는 JWT + PBKDF2 + 계정/IP 잠금 + jti revoke로 단일요소를 견고화했다. 코드 결함 아님.
2. **gRPC per-token / mTLS-CN 세분 role** (A01). 근거: gRPC는 이미 토큰 필수 + 무토큰 기동 거부 + bounded operator role 주입(Wave B). 토큰별/인증서별 세분 role은 nice-to-have이고, 현 bounded 모델이 최소권한을 만족한다.
3. **hot-reload 바이너리 무결성 미검증** (A08). 근거: hot-reload는 root 쓰기 권한을 전제하므로(이미 호스트 장악) 추가 위협면이 미미하고, 호스트 데몬 AppArmor MAC(ADR-0026)가 실행파일 접근을 심층방어한다.
4. **`/metrics` 무인증** (2.6). 근거: Prometheus 스크레이핑 관례. 기본 배포는 loopback 바인드 + nginx 종단 뒤이므로 외부 노출되지 않는다. 노출 시엔 nginx/방화벽으로 통제.
5. **버전 배너 노출** (A05). 근거: 정보 노출이 미미하고(버전은 CHANGELOG로 이미 공개), fingerprinting 완화 이득 대비 운영 편의(진단) 손실이 크다.
6. **REST 리스닝 로그** (cosmetic). 근거: 기능·보안 무관한 표기 이슈.
7. **부하 시 감사 드롭** (A09). 근거: 정상 부하에서 발생하지 않고, 기록된 감사의 무결성은 SHA-256 해시체인(Wave B)이 보장한다. backpressure/대체 저장은 향후 강화 후보이나 현 리스크 아님.

## 대안

- **전부 시정**: MFA·mTLS 세분 role·audit backpressure 등은 상당한 구현·제품 결정을 수반하나, 현 배포 리스크를 낮추는 효과가 한계적이라 기각(현 시점).
- **부분 시정**: `/metrics`·버전배너는 설정으로 잠글 수 있으나, 기본값 유지가 운영 관례에 부합.

## 결과

- 위 7항을 **잔여 위험으로 명시 수용**. 나머지 잔여(Quick 5·Medium 7·결정 3·재감사 2)는 tracker에서 시정/결정/process로 추적.
- **재평가 시**: 배포 프로파일이 P2를 넘어 인터넷 노출 멀티테넌트로 확대되면 1(MFA)·2(mTLS role)·4(/metrics)는 수용을 재검토한다.
- 관련: `docs/operations/2026-07-19-residual-security-disposition.md`(tracker), ADR-0025(반사실 규율), ADR-0026(MAC 수용).
