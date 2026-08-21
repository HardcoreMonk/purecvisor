# Domain Docs

Codex가 이 repo의 domain language와 ADR을 읽는 규칙.

## Read Before Work

- `CONTEXT.md` — 단일 context의 domain language.
- `CONTEXT-MAP.md` — multi-context repo에서 각 context의 `CONTEXT.md` 위치.
- `docs/adr/` — 되돌리기 어렵고 놀라운 architecture decision.
- context-specific ADR이 있으면 해당 context의 `docs/adr/`도 확인한다.

없는 파일은 오류로 보지 않는다. 필요한 term이나 decision이 실제로 생겼을 때만
사용자 확인 후 생성한다.

## Vocabulary

- issue title, test name, architecture proposal은 `CONTEXT.md` 용어를 우선한다.
- 모호한 term은 사용자에게 canonical term을 확인한다.
- implementation detail은 domain term으로 올리지 않는다.

## ADR

ADR은 아래 세 조건이 모두 맞을 때만 제안한다.

- hard to reverse
- surprising without context
- real trade-off

기존 ADR과 충돌하는 제안은 충돌 사실과 재검토 이유를 함께 표시한다.

ADR 상태는 project pipeline과 별도다.

```text
Draft -> Review -> Approved -> Implemented -> Verified -> Archived
```

`domain-architecture`는 ADR 후보나 `Draft`를 만들고, `grill-me`는 반례/대안/risk로
`Review`를 진행하며, `plan-eng-review`가 `Approved` 여부를 결정한다. 기존 ADR의
`Accepted`는 legacy alias로 `Approved`와 같이 해석한다.
