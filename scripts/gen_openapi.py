#!/usr/bin/env python3
"""
gen_openapi.py — REST_ENDPOINTS.md를 파싱해 openapi.yaml 생성

목적:
  - docs/REST_ENDPOINTS.md를 단일 진실 소스로 유지
  - OpenAPI 3.0 스펙을 자동 생성해 회귀 게이트(CI)에서 비교 검증

사용:
  python3 scripts/gen_openapi.py            # openapi.yaml 갱신
  python3 scripts/gen_openapi.py --check    # 비교만 (CI 게이트)
"""
import re
import sys
import json
import argparse
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "docs" / "REST_ENDPOINTS.md"
OUT = ROOT / "openapi.yaml"

# The generator intentionally supports only the table shape used by
# REST_ENDPOINTS.md. That keeps CI drift checks deterministic and avoids a
# partial Markdown parser that would accept undocumented endpoint formats.
# If the docs format changes, update ROW_RE and the corresponding check mode
# together so generated OpenAPI remains a faithful derived artifact.

# 파일 헤더 (정적)
HEADER = """openapi: 3.0.3
info:
  title: PureCVisor REST API
  description: |
    PureCVisor 하이퍼바이저 오케스트레이터의 REST API.
    이 파일은 docs/REST_ENDPOINTS.md에서 자동 생성됩니다 — 직접 수정 금지.
    재생성: `python3 scripts/gen_openapi.py`
  version: 1.0
  license:
    name: GPL-3.0
servers:
  - url: http://localhost/api/v1
    description: Local
  - url: https://localhost/api/v1
    description: Local TLS
security:
  - bearerAuth: []
components:
  securitySchemes:
    bearerAuth:
      type: http
      scheme: bearer
      bearerFormat: JWT
    apiKey:
      type: apiKey
      in: header
      name: X-API-Key
    csrf:
      type: apiKey
      in: header
      name: X-CSRF-Token
  parameters:
    NameParam:
      name: name
      in: path
      required: true
      schema: { type: string, minLength: 1, maxLength: 64, pattern: '^[a-zA-Z0-9_-]+$' }
    SnapParam:
      name: snap
      in: path
      required: true
      schema: { type: string, minLength: 1, maxLength: 128 }
    BridgeParam:
      name: br
      in: path
      required: true
      schema: { type: string, minLength: 1, maxLength: 16 }
    MacParam:
      name: mac
      in: path
      required: true
      schema: { type: string, pattern: '^([0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}$' }
    IdParam:
      name: id
      in: path
      required: true
      schema: { type: string }
  responses:
    Unauthorized:
      description: 인증 실패
      content:
        application/json:
          schema: { $ref: '#/components/schemas/Error' }
    Forbidden:
      description: RBAC 권한 부족
    NotFound:
      description: 리소스 없음
    RateLimited:
      description: Rate Limit 초과
  schemas:
    Error:
      type: object
      properties:
        error: { type: string }
        code: { type: integer }
        correlation_id: { type: string }
    Ok:
      type: object
      properties:
        ok: { type: boolean }
tags:
  - { name: auth, description: 인증/세션 }
  - { name: vm, description: VM 라이프사이클 }
  - { name: container, description: LXC 컨테이너 }
  - { name: network, description: 네트워크/브릿지 }
  - { name: storage, description: ZFS/iSCSI 스토리지 }
  - { name: cluster, description: 클러스터 HA }
  - { name: monitor, description: 메트릭/알림 }
  - { name: cloud, description: Cloud Migration }
  - { name: jobs, description: Job Queue }
  - { name: config, description: 설정 관리 }
  - { name: health, description: 헬스/probe }
"""

# 마크다운 라인 파서: | `METHOD` | `/path` | ... |
ROW_RE = re.compile(r"^\s*\|\s*`([A-Z/]+)`\s*\|\s*`([^`]+)`\s*\|\s*(.*?)\s*\|")

# 경로 → 태그 매핑
def tag_for(path):
    p = path.lstrip("/")
    seg = p.split("/", 1)[0]
    mapping = {
        "vms": "vm", "containers": "container", "networks": "network",
        "storage": "storage", "cluster": "cluster", "monitor": "monitor",
        "alerts": "monitor", "processes": "monitor",
        "cloud": "cloud", "jobs": "jobs", "config": "config",
        "auth": "auth", "iso": "vm", "health": "health",
        "metrics": "health", "internal": "health",
    }
    return mapping.get(seg, "default")

# 경로 변수 → OpenAPI 매개변수
def params_for(path):
    refs = []
    if "{name}" in path: refs.append("NameParam")
    if "{snap}" in path: refs.append("SnapParam")
    if "{br}" in path:   refs.append("BridgeParam")
    if "{mac}" in path:  refs.append("MacParam")
    if "{id}" in path:   refs.append("IdParam")
    return refs

def parse_md(text):
    """마크다운에서 (method, path, desc) 튜플 추출. GET/POST 같은 다중 메서드는 분리."""
    rows = []
    for line in text.splitlines():
        m = ROW_RE.match(line)
        if not m:
            continue
        methods = m.group(1).split("/")
        path = m.group(2).strip()
        desc = re.sub(r"`", "", m.group(3) or "").strip()
        if not path.startswith("/"):
            continue
        for method in methods:
            method = method.strip().lower()
            if method in ("get","post","put","delete","patch"):
                rows.append((method, path, desc))
    return rows

def emit_paths(rows):
    """OpenAPI paths 섹션 YAML 문자열 생성."""
    by_path = {}
    for method, path, desc in rows:
        by_path.setdefault(path, {})[method] = desc

    out = ["paths:"]
    for path in sorted(by_path.keys()):
        ops = by_path[path]
        out.append(f"  {path}:")
        params = params_for(path)
        if params:
            out.append("    parameters:")
            for ref in params:
                out.append(f"      - $ref: '#/components/parameters/{ref}'")
        for method in sorted(ops.keys()):
            desc = ops[method]
            tag = tag_for(path)
            # 인증 불필요 경로
            no_auth = path in ("/health", "/metrics", "/auth/token", "/auth/refresh", "/auth/logout") \
                      or path.startswith("/internal/")
            # YAML 안전한 description (특수문자 escape — quote 사용)
            safe_desc = desc.replace('"', '\\"').replace("\n", " ")[:200]
            out.append(f"    {method}:")
            out.append(f"      tags: [{tag}]")
            out.append(f'      summary: "{safe_desc}"')
            if no_auth:
                out.append("      security: []")
            out.append("      responses:")
            out.append("        '200':")
            out.append("          description: Success")
            out.append("        '401': { $ref: '#/components/responses/Unauthorized' }")
            if method != "get":
                out.append("        '403': { $ref: '#/components/responses/Forbidden' }")
            out.append("        '429': { $ref: '#/components/responses/RateLimited' }")
    return "\n".join(out) + "\n"

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="비교만 수행 (CI)")
    args = ap.parse_args()

    md = SRC.read_text(encoding="utf-8")
    rows = parse_md(md)
    if not rows:
        print(f"ERROR: {SRC}에서 경로를 찾지 못했습니다", file=sys.stderr)
        sys.exit(2)

    yaml = HEADER + "\n" + emit_paths(rows)
    print(f"Parsed {len(rows)} routes from {SRC.name}", file=sys.stderr)

    if args.check:
        if not OUT.exists():
            print(f"ERROR: {OUT} 없음 — gen_openapi.py 실행 필요", file=sys.stderr)
            sys.exit(1)
        existing = OUT.read_text(encoding="utf-8")
        if existing.strip() != yaml.strip():
            print(f"ERROR: {OUT}가 REST_ENDPOINTS.md와 동기화되지 않음", file=sys.stderr)
            print("  → python3 scripts/gen_openapi.py 실행 후 커밋하세요", file=sys.stderr)
            sys.exit(1)
        print(f"OK: {OUT.name} 동기화됨 ({len(rows)} routes)")
    else:
        OUT.write_text(yaml, encoding="utf-8")
        print(f"WROTE {OUT} ({len(rows)} routes)")

if __name__ == "__main__":
    main()
