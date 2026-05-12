# Issue Tracker

이 repo의 issue, PRD, triage 기록 위치와 조작 규칙.

## Backend

- Type: `github`
- Location: `git@github.com:HardcoreMonk/purecvisor.git`
- CLI: `gh`

## Rules

- Codex는 사용자가 명시하지 않은 issue 생성, close, label 변경을 하지 않는다.
- multi-line issue body는 temp file 또는 heredoc을 사용해 shell quoting 위험을 줄인다.
- issue를 읽을 때 body, comments, labels/status를 함께 확인한다.
- local markdown 방식이면 `.scratch/<feature>/issues/<NN>-<slug>.md`를 기본으로 한다.

## Publish

When a workflow says "publish to the issue tracker":

1. 대상 backend가 설정되어 있는지 확인한다.
2. 생성/수정/close 행동을 사용자에게 명시한다.
3. 실행 후 URL 또는 파일 경로를 보고한다.

## Fetch

When a workflow says "fetch the relevant ticket":

1. issue number, URL, or local path를 확인한다.
2. body, comments/history, labels/status를 읽는다.
3. 이전 triage note가 있으면 중복 질문하지 않는다.
