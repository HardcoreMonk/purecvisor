# ADR-0016: Supanova Taste Layer — 테마 축소 + accent 변수화

날짜: 2026-04-11
상태: accepted

## 맥락

프로젝트는 `uxjoseph/supanova-design-skill`(Leonxlnx/taste-skill 한국어
포크)을 검토하며 Web 대시보드의 디자인 방향을 재평가했다. 기존 테마 시스템은
10개(NEON_DARK, PURE_LIGHT, PURE_DARK, MIDNIGHT_BLUE, FOREST_GREEN,
SUNSET_WARM, HIGH_CONTRAST, SOLARIZED_DARK, SOLARIZED_LIGHT, CUSTOM)로
운영자가 실제로 사용하는 테마는 1~2개이고 나머지는 유지 부담만 있었다.

Supanova 스킬의 검토 결과 핵심 디자인 원칙은:
- Max 1 accent color per page, saturation < 80%
- No neon/outer glow, no AI purple gradient
- Pretendard + Outfit 한국어·영문 디스플레이
- Spring motion `cubic-bezier(0.16, 1, 0.3, 1)`
- Double-Bezel 카드 아키텍처, 한국어 `word-break: keep-all`

Phase 1~4에서 로그인·가이드·테마·대시보드에 Supanova Taste Layer를 단계별로
이식했고, 결과적으로 사용자가 실제로 고를 의미 있는 선택지는 "같은 기반에
accent 톤만 다른 변형"임이 드러났다.

기존 9개 테마를 유지하면 다음 비용이 발생한다:
- 각 테마마다 chart-cpu/mem/disk/net 4개 컬러 정의 필요
- 테마별 toolbar h1 text-shadow, login-orb 오버라이드, neon-glow
- 테마 editor UI(custom 테마) 유지
- prefers-color-scheme auto 전환 로직 pure-light/pure-dark 의존
- ui-audit Lighthouse+axe-core CI가 모든 테마 조합 검증 필요 (매트릭스 확대)

## 결정

1. **테마 3개로 축소** (Supanova 변형만 유지):
   - `supanova` (Teal-500 `#14b8a6`, 기본)
   - `supanova-cyan` (Cyan-600 `#0891b2`, 채도 92%로 규율 경계)
   - `supanova-hicontrast` (고대비 접근성 변형)

2. **선택자 확장 `[data-theme|="supanova"]`**로 3개 변형이 모든 Supanova
   규칙을 공유한다. cyan 변형은 `--accent`/`--chart-cpu`만 바꾸고,
   접근성 변형(`hicontrast`)은 대비에 필요한 token set만 좁게 override한다.

3. **파생 accent 변수**로 hover/tint/ring/radial 모두 `--accent`에 연동:
   ```css
   --accent-hover-border: color-mix(in srgb, var(--accent) 40%, transparent);
   --accent-tint-15/10/06/04
   --neon-glow:           color-mix(in srgb, var(--accent) 12%, transparent);
   ```
   accent 한 줄 변경만으로 전 UI가 자동 추종. 하드코딩 rgba 제거.

4. **레거시 테마 id 자동 마이그레이션**: index.html `<head>`의 inline
   script가 localStorage `pcv-theme`를 검사하여 삭제된 테마 id(예:
   `midnight-blue`)가 있으면 `supanova`로 재작성. NEON_DARK flash 없음.

5. **제거 대상**:
   - `[data-theme="pure-light"]` 외 8개 테마 CSS 블록
   - `@media (prefers-color-scheme: light)` 자동 감지 블록
   - `autoThemeEnabled` / `checkAutoTheme` / `prefers-color-scheme` listener
   - Theme editor 내 `--accent`/`--bg` 이외 커스텀 변수 에디터 항목은 유지
     (supanova 변형 내에서 사용자 미세 조정 용도)

6. **유지 대상**:
   - Theme editor 자체 (Custom 테마는 지원 유지, 단 THEME_PREVIEWS에서 제거)
   - 프리뷰 샘플 파일 2개 (`ui/samples/supanova-accent-{teal,cyan}.html`)
     로 육안 비교 가능

## 결과

- **좋음**: 테마 코드 ~350줄 감소. CSS 11.2 KB 절약
- **좋음**: accent 조정이 1~2줄 변경으로 완결 → 디자인 반복 속도 ↑
- **좋음**: `@supports not (color-mix)` 폴백으로 구형 브라우저 graceful
- **좋음**: Supanova 금칙(neon glow, clip-path, cyan→magenta 그라디언트)
  전량 제거 → 감사 통과
- **해소**: 색맹 배려/대비 테마 회귀는 `supanova-hicontrast`로 보완.
- **트레이드오프**: light OS 사용자용 밝은 변형은 제거하고, 운영 콘솔 기본값은
  dark Supanova 계열로 고정한다.
- **중립**: Theme editor의 custom 테마는 유지되나 기본 드롭다운에서
  제거 → 상급 사용자만 접근

## 적용 방법
```css
[data-theme|="supanova"] { /* 기본 규칙 */ }
[data-theme="supanova-cyan"]    { --accent: #0891b2; --chart-cpu: rgb(8,145,178); }
[data-theme="supanova-hicontrast"] { /* 고대비 token override */ }
```

## 참고
- 커밋 `1120ba9` refactor(ui): Supanova 테마 3개 축소
- 2026-04-26 후속: `supanova-hicontrast`, `supanova-light` 추가
- 2026-05-08 후속: 대시보드 선택지 단순화를 위해 `supanova-emerald`,
  `supanova-light` 제거
- `ui/samples/supanova-preview.html` — 전체 대시보드 스킨 프리뷰
- `uxjoseph/supanova-design-skill` (GitHub)
- ADR-0013 프론트엔드 IIFE 모듈 스코프 (관련)
