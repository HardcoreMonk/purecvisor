# ADR-0010: WebSocket 인증을 query string에서 프로토콜 레벨로 전환

날짜: 2026-04-10
상태: accepted

## 맥락
현재 WebSocket 연결 시 `ws://host/api/v1/ws/events?token=JWT` 형태로 JWT를
URL query string에 포함한다. 이 방식은:
- 브라우저 히스토리, DevTools Network 탭, 프록시 로그에 토큰이 기록됨
- HTTPS 사용 시에도 URL은 TLS 암호화 범위이나, 서버 접근 로그에 남음
- WebSocket Upgrade 요청의 URL이 Referer 헤더로 유출될 수 있음

코드 리뷰(2026-04-10)에서 CRITICAL #2로 식별. ui/modules/api.js:145.

## 결정
WebSocket 연결을 2단계로 전환한다:
1. 연결 수립: 토큰 없이 `ws://host/api/v1/ws/events` 접속
2. 인증 메시지: 연결 후 첫 메시지로 `{"type":"auth","token":"JWT"}` 전송
3. 서버는 5초 내 인증 메시지 미수신 시 연결 종료
4. 인증 성공 시 `{"type":"auth_ok"}` 응답 후 이벤트 스트림 시작

하위 호환: 기존 query string 방식도 30일간 병행 지원 후 제거.

## 결과
- 좋음: 토큰이 URL에서 제거되어 로그/히스토리 노출 차단
- 좋음: 향후 토큰 갱신(refresh)도 WS 메시지로 처리 가능
- 나쁨: 연결 핸드셰이크에 1 RTT 추가 (인증 메시지 왕복)
- 나쁨: UI + CLI monitor + 외부 통합 클라이언트 모두 업데이트 필요
- 포기한 것: Subprotocol 헤더 방식 — 브라우저 JS에서 커스텀 헤더 설정 불가

## 하지 않기로 한 것
- Sec-WebSocket-Protocol에 토큰을 넣지 않는다. 표준 용도와 다르고 프록시가 로깅할 수 있다.
- Authorization 헤더를 WS handshake에 포함하지 않는다. 브라우저 WebSocket API가 지원하지 않는다.
