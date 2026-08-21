# ADR-0029: REST/WS TLS 기본 활성 + 제한된 외부 TLS 종료

날짜: 2026-07-07
상태: Verified

## 개정 이력

- 2026-07-07: 원결정은 daemon의 REST/WS TLS 기본 활성, 외부 평문 fallback
  제거, 자가서명 자동 프로비저닝이었다. 아래 맥락과 결정 1~5는 당시 판단을
  역사적 문맥 그대로 보존한다.
- 2026-07-27: 결정 6으로 nginx 외부 TLS 종료의 제한된 opt-in을 추가했다.
  구현과 정적·fixture 검증에 더해 실제 노드 배포·재부팅 복구·원격 헤더 spoof
  방어를 확인했으므로 lifecycle을 `Verified`로 올렸다. 공개 재현 절차는
  `docs/GUIDE.md`의 nginx TLS 종료 절과 통합 테스트를 따른다.

## 맥락
REST(:80)는 무조건 0.0.0.0 평문 리스닝이었고 로그인 패스워드·JWT·API key가
평문으로 전송됐다. WS(이벤트/VNC)도 동일 SoupServer 상속. [tls] enabled
opt-in 시에도 :80 평문이 병행 유지, 모든 TLS 실패는 평문 fallback(fail-open).
gRPC는 ADR-0015로 non-loopback+no-TLS 기동 거부를 강제하면서 더 큰 표면인
REST/WS가 평문 상시인 비일관을 해소하기 위해 2026-07-07 결정으로 확정했다.

## 결정
1. TLS 상시: [tls] enabled 키 폐기(무시+WARN). 인증서 부재 시 부팅에서
   자가서명 자동 생성(EC P-256/3650일/CN+SAN, openssl CLI via pcv_spawn,
   .autogen 마커, 만료<30일 또는 cert/key 쌍 불일치(openssl pubkey 비교)
   시 자동 재생성 — 부분 커밋/파손 복구). 사용자 제공 인증서 우선·불가침.
   생성 자체는 원자적: cert.tmp/key.tmp/marker.tmp 순으로 기록→chmod→
   rename 3연속으로 커밋, 어느 단계든 실패하면 tmp 파일만 정리하고
   기존 cert/key/marker 는 건드리지 않는다(rename 3연속의 파일 간 원자성
   한계는 cert/key 쌍 검증 재생성 게이트로 보정).
2. 외부 표면 = HTTPS([tls] https_port, 기본 443)만. HTTP(rest_port, 기본
   80)는 127.0.0.1 전용 격하(keepalived·로컬 curl·복구 경로 보존).
3. fail-closed degraded: TLS 셋업(인증서 로드) 또는 HTTPS 리스닝 실패 시
   평문 확장 대신 루프백 HTTP+UDS만으로 기동(degraded). CRITICAL 로그 +
   /health checks.tls.degraded 표기. 데몬(VM 수명주기)은 계속 —
   ADR-0015의 "서버만 거부" 정신과 동형. 루프백 HTTP 자체의 리스닝 실패는
   TLS degraded 범주가 아니라 기존과 동일한 데몬 시작 실패로 처리한다
   (환경 이상 — 포트 충돌 등 — 이지 외부 평문 노출과 무관하므로 fail-open
   시킬 이유가 없다).
4. cert XOR key 만 존재하면 덮어쓰지 않고 셋업 실패(사용자 자산 보호).
5. ca 는 선택으로 완화(mTLS 미배선 — 자가서명 경로에 CA 없음).
6. nginx 같은 로컬 reverse proxy가 외부 TLS를 전담하는 배포에는 제한된 예외를
   둔다. `[tls] https_enabled=false`는 `[server] bind_plaintext=loopback`을
   명시한 경우에만 허용하며, daemon은 인증서 생성·TLS context·HTTPS listener를
   만들지 않고 루프백 HTTP만 제공한다. health는
   `mode=external_termination`, `enabled=false`, `degraded=false`,
   `status=disabled_by_config`로 이 정상 선택을 degraded와 구분한다. 프록시
   신원·scheme 헤더는 루프백 peer만 신뢰한다. 배포 자동화는
   `PCV_NGINX_BIND_IP` opt-in에서만 이 모드를 설정하고, nginx 설정·systemd
   drop-in·daemon 설정을 하나의 rollback 단위로 취급한다.

### 외부 TLS 종료의 호스트 신뢰 경계

`PCV-NGINX-TRUST-BOUNDARY: host-loopback`

이 모드는 호스트 자체를 privileged/trusted boundary로 보고, 루프백에 접속할 수
있는 모든 로컬 프로세스를 nginx와 같은 신뢰 주체로 가정한다. 따라서 신뢰하지
않는 로컬 사용자나 프로세스가 daemon의 루프백 HTTP에 접속할 수 있는 호스트에서는
전달 헤더를 위조할 수 있으므로 이 모드를 활성화하면 안 된다. 전용 호스트,
최소 사용자, 서비스 sandbox/MAC, 로컬 방화벽 또는 네트워크 namespace로 직접
루프백 접근을 제한하는 구성을 권장한다.

`PCV-NGINX-COUNTERFACTUAL: untrusted-local-process`

반사실은 신뢰하지 않는 로컬 프로세스가 루프백 listener에 직접 요청하는 경우다.
이 경우 소켓 peer 자체가 loopback이므로 전달 헤더를 nginx가 보낸 것과 구분할 수
없다. 반대로 원격 클라이언트의 소켓 peer는 loopback이 아니며, 정상 nginx 경로는
전달 헤더를 덮어쓰므로 원격 클라이언트가 이 신뢰를 직접 위조할 수 없다.

## 결과
- 좋음: 자격증명 평문 노출 원천 차단. 기존 배포 무설정 마이그레이션
  (자동생성). 외부 :80 은 connection refused 로 명확 실패. 외부 종료 배포도
  daemon 평문 리스너를 루프백으로 제한해 이 경계를 유지한다.
- 나쁨: 자가서명 기본이라 브라우저 경고 — 공인 인증서는 사용자 배치로 해소.
  클라이언트(E2E·모니터링의 비-localhost 접근)는 https 전환 필요.
- 포기: :80 → 301 리다이렉트 — 이미 전송된 평문 요청(Authorization/body)을
  구제하지 못하는 절반짜리라 기각.

## 하지 않기로 한 것
- 비루프백 평문을 여는 TLS opt-out 금지. 외부 종료 예외도
  `bind_plaintext=loopback` 없는 설정은 시작 단계에서 거부한다.
- HSTS 기본 활성화 안 함(자가서명 기본 환경 부적합 — opt-in 유지).
- ACME/Let's Encrypt·인증서 rotate RPC·mTLS 는 수요 트리거 후속.
