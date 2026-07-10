/**
 * @file grpc_server.h
 * @brief PureCVisor gRPC 서버 — protobuf-c 기반 내부 고속 API
 *
 * == 이중 구조 ==
 *   REST API (포트 80/443) — 외부 클라이언트, 브라우저, curl
 *   gRPC    (포트 50051)   — CLI, Agent, 내부 서비스, 자동화
 *
 * == 설정 ==
 *   daemon.conf:
 *     [grpc]
 *     enabled = true
 *     port = 50051
 *
 * == 프로토콜 ==
 *   현재: protobuf-c 바이너리 프레이밍 (TCP 직접)
 *   향후: gRPC HTTP/2 정식 프레이밍 (grpc-c 래퍼)
 */
#ifndef PCV_GRPC_SERVER_H
#define PCV_GRPC_SERVER_H

/**
 * pcv_grpc_server_start — gRPC 서버 시작
 *
 * daemon.conf [grpc] enabled=true 시 별도 GThread에서 실행.
 * 기존 dispatcher.c의 156 RPC를 UDS 프록시로 재사용합니다.
 */
void pcv_grpc_server_start(void);

/**
 * pcv_grpc_server_stop — gRPC 서버 종료
 *
 * listen 소켓을 shutdown하여 accept 루프를 종료합니다.
 */
void pcv_grpc_server_stop(void);

#endif /* PCV_GRPC_SERVER_H */
