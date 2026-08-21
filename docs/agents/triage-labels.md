# Triage Labels

Codex workflow가 쓰는 canonical triage role과 실제 issue tracker label/status 매핑.

| Canonical role | Tracker label/status | Meaning |
|---|---|---|
| `needs-triage` | `needs-triage` | maintainer 평가 필요 |
| `needs-info` | `needs-info` | reporter 응답 대기 |
| `ready-for-agent` | `ready-for-agent` | agent가 추가 human context 없이 수행 가능 |
| `ready-for-human` | `ready-for-human` | human 판단이나 수동 작업 필요 |
| `wontfix` | `wontfix` | 처리하지 않기로 결정 |

## Category

| Canonical category | Tracker label/status | Meaning |
|---|---|---|
| `bug` | `bug` | 기존 동작이 깨짐 |
| `enhancement` | `enhancement` | 새 기능 또는 개선 |

## Rules

- triaged issue는 category 하나와 state 하나를 가진다.
- state가 충돌하면 먼저 사용자에게 확인한다.
- tracker에 다른 naming convention이 있으면 오른쪽 열만 바꾼다.
