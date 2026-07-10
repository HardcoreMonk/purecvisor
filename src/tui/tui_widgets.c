/**
 * @file tui_widgets.c
 * @brief TUI 공통 위젯 — ncurses 렌더링 헬퍼 (패널, 차트, 게이지, 스크롤, 팝업)
 *
 * ============================================================================
 *  아키텍처 위치
 * ============================================================================
 *  purecvisortui.c(TUI 메인 루프)에서 분리된 순수 위젯 렌더링 계층입니다.
 *  모든 함수는 WINDOW* + 데이터 파라미터만 받으며, 글로벌 상태에 접근하지 않습니다.
 *  이로써 위젯 로직을 단위 테스트/재사용하기 쉬운 구조가 됩니다.
 *
 *    purecvisortui.c  ──(렌더링 호출)──→  tui_widgets.c (이 파일)
 *         │                                    │
 *         │                                    ├── draw_panel()     : 테두리 패널
 *         │                                    ├── draw_bar()       : 퍼센트 게이지
 *         │                                    ├── draw_sparkline() : 미니 막대 그래프
 *         │                                    ├── draw_table()     : 스크롤 테이블
 *         │                                    ├── bgrid_*()        : 브레일 차트 엔진
 *         │                                    ├── create_popup()   : 모달 팝업
 *         │                                    └── pcv_layout_split() : 레이아웃 분할
 *         │
 *    tui_rpc.c ──(RPC 통신)──→ UDS 소켓
 *
 * ============================================================================
 *  주니어 개발자 필독: 핵심 개념
 * ============================================================================
 *
 *  1. 브레일(Braille) 차트 엔진
 *     터미널은 한 셀에 하나의 문자만 표시할 수 있지만, 유니코드 Braille 문자
 *     (U+2800~U+28FF)를 사용하면 한 셀에 2x4=8개의 점을 표현할 수 있습니다.
 *     즉, 터미널 40열 x 10행 = 80x40 픽셀 해상도의 차트를 그릴 수 있습니다.
 *     bgrid_set_pixel() → bgrid_draw_line() → bgrid_plot_series() → bgrid_render()
 *     순서로 호출하여 시계열 데이터를 고해상도 라인 차트로 렌더링합니다.
 *
 *  2. Sparkline (draw_sparkline)
 *     유니코드 블록 문자(\u2581~\u2588, 8단계)를 사용한 미니 막대 그래프입니다.
 *     CPU/메모리 사용률의 최근 히스토리를 한 줄에 표시합니다.
 *     SPARK_BLOCKS 배열이 8단계 높이 문자를 저장합니다.
 *
 *  3. 컬러 그래디언트 (pcv_color_for_pct)
 *     사용률 비율(0.0~1.0)을 7단계 색상(C_GRAD_0~C_GRAD_6)으로 매핑합니다.
 *     초록(0%) → 노랑(60%) → 빨강(95%) 그래디언트로 직관적인 상태 표시.
 *
 *  4. 레이아웃 엔진 (pcv_layout_split)
 *     CSS flexbox와 유사한 제약 기반 레이아웃입니다.
 *     PC_LENGTH(고정 크기), PC_PERCENT(비율), PC_FILL(나머지 균등 분배),
 *     PC_MIN/PC_MAX(최소/최대 제약)를 조합하여 터미널 크기에 적응합니다.
 *
 *  5. ncurses 컬러 주의사항
 *     COLOR_PAIR(n)으로 색상을 적용하고, wattron/wattroff로 토글합니다.
 *     C_WHITE는 미선언 — 절대 사용 금지 (컴파일 에러).
 *     색상 상수는 tui_widgets.h에 정의됩니다 (C_CPU, C_MEM, C_GRAD_0~6 등).
 * ============================================================================
 */
#include "tui_widgets.h"

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* ════════════════════════════════════════════════════════════════════
 *  글로벌 상수 정의
 * ════════════════════════════════════════════════════════════════════ */

/**
 * Sparkline용 유니코드 블록 문자 배열 (9단계: 공백 + ▁▂▃▄▅▆▇█)
 * 인덱스 0=공백(0%), 8=풀블록(100%). draw_sparkline()에서 데이터 값에 따라 선택.
 */
const char *SPARK_BLOCKS[] = {
    " ", "\xe2\x96\x81", "\xe2\x96\x82", "\xe2\x96\x83",
    "\xe2\x96\x84", "\xe2\x96\x85", "\xe2\x96\x86",
    "\xe2\x96\x87", "\xe2\x96\x88"
};

/**
 * 브레일 도트 비트 매핑 테이블
 *
 * 유니코드 Braille 문자(U+2800)는 8개 점을 비트 플래그로 인코딩합니다.
 * 한 셀의 점 배치:  [col0] [col1]
 *             row0:   bit0   bit3
 *             row1:   bit1   bit4
 *             row2:   bit2   bit5
 *             row3:   bit6   bit7
 *
 * BRAILLE_DOTS[row][col] = 해당 위치의 비트 값
 * 예: 좌상단 점(row=0, col=0) = 0x01, 우하단 점(row=3, col=1) = 0x80
 */
const int BRAILLE_DOTS[4][2] = {
    {0x01, 0x08},   /* row 0: 좌=bit0, 우=bit3 */
    {0x02, 0x10},   /* row 1: 좌=bit1, 우=bit4 */
    {0x04, 0x20},   /* row 2: 좌=bit2, 우=bit5 */
    {0x40, 0x80},   /* row 3: 좌=bit6, 우=bit7 */
};

/**
 * 스피너 애니메이션 프레임 (10단계 Braille 회전 패턴: ⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏)
 * 80ms 간격으로 순환하여 비동기 작업 진행중 표시에 사용
 */
const char *SPIN_FRAMES[] = {
    "\xe2\xa0\x8b","\xe2\xa0\x99","\xe2\xa0\xb9","\xe2\xa0\xb8",
    "\xe2\xa0\xbc","\xe2\xa0\xb4","\xe2\xa0\xa6","\xe2\xa0\xa7",
    "\xe2\xa0\x87","\xe2\xa0\x8f"
};

/* ════════════════════════════════════════════════════════════════════
 *  브레일 변환
 * ════════════════════════════════════════════════════════════════════ */

/**
 * braille_to_utf8 — 브레일 비트 패턴을 UTF-8 바이트열로 변환
 *
 * [호출 시점] bgrid_render()에서 각 셀의 비트를 화면 출력용 문자열로 변환할 때
 * [동작] BRAILLE_BASE_CP(U+2800)에 비트 OR 연산하여 코드포인트 생성 → UTF-8 3바이트 인코딩
 *        U+2800~U+28FF는 BMP 영역이므로 항상 3바이트 (0xE0 xx xx)
 *
 * @param bits 8비트 도트 패턴 (BRAILLE_DOTS 테이블로 조합)
 * @param out  최소 4바이트 버퍼 (3바이트 UTF-8 + NUL 종료)
 */
void braille_to_utf8(int bits, char out[4]) {
    int cp = BRAILLE_BASE_CP | (bits & 0xFF);
    /* UTF-8 인코딩: U+2800~U+28FF → 3바이트 (E2 Ax xx) */
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    out[3] = '\0';
}

/* ════════════════════════════════════════════════════════════════════
 *  컬러 유틸
 * ════════════════════════════════════════════════════════════════════ */

/**
 * pcv_color_for_pct — 사용률 비율(0.0~1.0)을 7단계 컬러 그래디언트로 변환
 *
 * [호출 시점] draw_bar, bgrid_render 등 사용률 시각화 시
 * [동작] 0~20% → C_GRAD_0(초록) ... 95~100% → C_GRAD_6(빨강)
 *
 * @param ratio 0.0~1.0 범위의 사용률
 * @return ncurses COLOR_PAIR 인덱스 (C_GRAD_0 ~ C_GRAD_6)
 */
int pcv_color_for_pct(double ratio) {
    if (ratio < 0.20) return C_GRAD_0;
    if (ratio < 0.40) return C_GRAD_1;
    if (ratio < 0.60) return C_GRAD_2;
    if (ratio < 0.75) return C_GRAD_3;
    if (ratio < 0.85) return C_GRAD_4;
    if (ratio < 0.95) return C_GRAD_5;
    return C_GRAD_6;
}

/* ════════════════════════════════════════════════════════════════════
 *  스피너 (TachyonFX EffectManager 패턴)
 * ════════════════════════════════════════════════════════════════════ */

/**
 * spinner_start — 스피너 애니메이션 시작
 *
 * [호출 시점] VM 생성 등 비동기 RPC 요청 직후, 결과 대기 중 표시
 * [동작] active 플래그 설정 + 라벨 복사 + 80ms 간격 프레임 전환 준비
 */
void spinner_start(Spinner *sp, const gchar *label) {
    sp->active       = TRUE;
    sp->frame        = 0;
    sp->last_tick_us = g_get_monotonic_time();
    sp->interval_ms  = 80;
    strncpy(sp->label, label, sizeof(sp->label)-1);
}

void spinner_stop(Spinner *sp) { sp->active = FALSE; }

void spinner_tick(Spinner *sp) {
    if (!sp->active) return;
    gint64 now = g_get_monotonic_time();
    if (now - sp->last_tick_us >= sp->interval_ms * 1000LL) {
        sp->frame = (sp->frame + 1) % SPIN_N;
        sp->last_tick_us = now;
    }
}

void spinner_draw(WINDOW *win, int y, int x, const Spinner *sp) {
    if (!sp->active) return;
    wattron(win, COLOR_PAIR(C_CYAN) | A_BOLD);
    mvwprintw(win, y, x, "%s %s", SPIN_FRAMES[sp->frame], sp->label);
    wattroff(win, COLOR_PAIR(C_CYAN) | A_BOLD);
}

/* ════════════════════════════════════════════════════════════════════
 *  레이아웃 엔진
 * ════════════════════════════════════════════════════════════════════ */

/**
 * pcv_layout_split — 제약 기반 레이아웃 분할 (CSS flexbox 유사)
 *
 * [호출 시점] TUI 화면 구성 시 패널 크기를 동적으로 계산할 때
 * [동작] total 픽셀(행 또는 열)을 n개 영역으로 분할:
 *   - PC_LENGTH: 고정 크기 (예: 사이드바 30열)
 *   - PC_PERCENT: 비율 할당 (예: 50% → total * 50 / 100)
 *   - PC_FILL: 남은 공간을 균등 분배 (flex: 1 유사)
 *   - PC_MIN: 최소 크기 보장
 *   - PC_MAX: 최대 크기 제한
 *
 *   고정/비율 먼저 할당 → 남은 공간을 FILL에 균등 분배 → MAX 제약 적용
 *
 * @param total 분할할 총 크기 (행 또는 열 수)
 * @param c     제약 조건 배열 (PcConstraint)
 * @param n     영역 개수
 * @param sizes 결과 크기 배열 (호출자가 할당, n개)
 */
void pcv_layout_split(int total, const PcConstraint *c, int n, int *sizes) {
    int used = 0; int fill_count = 0;
    for (int i = 0; i < n; i++) {
        switch (c[i].type) {
        case PC_LENGTH:  sizes[i] = c[i].val; used += sizes[i]; break;
        case PC_PERCENT: sizes[i] = total * c[i].val / 100; used += sizes[i]; break;
        case PC_MIN:     sizes[i] = c[i].val; used += sizes[i]; break;
        case PC_MAX:     sizes[i] = 0; break;
        case PC_FILL:    sizes[i] = 0; fill_count++; break;
        }
    }
    int remaining = total - used;
    if (fill_count > 0 && remaining > 0) {
        int per_fill = remaining / fill_count;
        int leftover = remaining % fill_count;
        for (int i = 0; i < n; i++) {
            if (c[i].type == PC_FILL) {
                sizes[i] = per_fill + (leftover > 0 ? 1 : 0);
                if (leftover > 0) leftover--;
            }
        }
    }
    for (int i = 0; i < n; i++) {
        if (c[i].type == PC_MAX && sizes[i] > c[i].val)
            sizes[i] = c[i].val;
    }
}

/* ════════════════════════════════════════════════════════════════════
 *  기본 위젯
 * ════════════════════════════════════════════════════════════════════ */

/**
 * draw_panel — ACS 문자로 테두리 패널을 그림 (박스형 컨테이너)
 *
 * [호출 시점] 각 탭 화면의 섹션 패널 렌더링 시
 * [동작] 네 모서리(UL/UR/LL/LR) + 가로/세로 선 + 선택적 타이틀 출력
 *
 * @param win   대상 ncurses 윈도우
 * @param y,x   좌상단 좌표
 * @param h,w   높이/너비 (최소 h=2, w=4 필요)
 * @param title 패널 상단 제목 (NULL이면 생략)
 * @param cp    COLOR_PAIR 인덱스 (테두리 색상)
 */
void draw_panel(WINDOW *win, int y, int x, int h, int w,
                const char *title, int cp) {
    if (h < 2 || w < 4) return;
    wattron(win, COLOR_PAIR(cp));
    mvwaddch(win, y,     x,     ACS_ULCORNER);
    mvwaddch(win, y,     x+w-1, ACS_URCORNER);
    mvwaddch(win, y+h-1, x,     ACS_LLCORNER);
    mvwaddch(win, y+h-1, x+w-1, ACS_LRCORNER);
    mvwhline(win, y,     x+1,   ACS_HLINE, w-2);
    mvwhline(win, y+h-1, x+1,   ACS_HLINE, w-2);
    mvwvline(win, y+1,   x,     ACS_VLINE, h-2);
    mvwvline(win, y+1,   x+w-1, ACS_VLINE, h-2);
    if (title) {
        wattron(win, A_BOLD);
        mvwprintw(win, y, x+2, " %s ", title);
        wattroff(win, A_BOLD);
    }
    wattroff(win, COLOR_PAIR(cp));
}

/**
 * draw_bar — 수평 퍼센트 게이지 바를 그림 [██████────] 형태
 *
 * [호출 시점] CPU/메모리/디스크 사용률 표시 시
 * [동작] [filled█████empty───] 형태로 렌더링. 60% 이상 노랑, 85% 이상 빨강으로 자동 변경.
 *        filled 부분은 A_REVERSE로 반전 표시, empty 부분은 ACS_HLINE으로 표시.
 *
 * @param win 대상 ncurses 윈도우
 * @param y,x 좌상단 좌표
 * @param w   바 전체 너비 (괄호 포함, 최소 4)
 * @param pct 퍼센트 값 (0.0~100.0)
 * @param cp  기본 COLOR_PAIR (60/85% 이상 시 자동으로 노랑/빨강으로 변경)
 */
void draw_bar(WINDOW *win, int y, int x, int w, double pct, int cp) {
    if (w < 4) return;
    /* 임계값에 따라 색상 자동 변경: 85%+ 빨강, 60%+ 노랑 */
    int active_cp = cp;
    if      (pct >= 85.0) active_cp = C_RED;
    else if (pct >= 60.0) active_cp = C_YELLOW;

    wattron(win, COLOR_PAIR(C_DIM)); mvwaddch(win, y, x, '['); wattroff(win, COLOR_PAIR(C_DIM));
    int filled = (int)(pct / 100.0 * (w-2));
    for (int i = 0; i < w-2; i++) {
        if (i < filled) {
            wattron(win, COLOR_PAIR(active_cp) | A_REVERSE);
            mvwaddch(win, y, x+1+i, ' ');
            wattroff(win, COLOR_PAIR(active_cp) | A_REVERSE);
        } else {
            wattron(win, COLOR_PAIR(C_DIM));
            mvwaddch(win, y, x+1+i, ACS_HLINE);
            wattroff(win, COLOR_PAIR(C_DIM));
        }
    }
    wattron(win, COLOR_PAIR(C_DIM)); mvwaddch(win, y, x+w-1, ']'); wattroff(win, COLOR_PAIR(C_DIM));
}

/**
 * draw_sparkline — 한 줄짜리 미니 막대 그래프 (유니코드 블록 문자 ▁▂▃▄▅▆▇█)
 *
 * [호출 시점] 호스트 탭의 CPU/메모리 히스토리 요약 표시 시
 * [동작] 환형 버퍼(data, hist_pos)에서 최근 w개 값을 읽어 0~8단계 블록 문자로 표시
 *        data[di]는 환형 인덱스 ((hist_pos - w + i) % n + n) % n으로 접근
 *
 * @param win      대상 ncurses 윈도우
 * @param y,x      출력 좌표
 * @param w        표시할 열 수 (sparkline 길이)
 * @param data     시계열 데이터 배열 (환형 버퍼)
 * @param n        data 배열의 전체 크기
 * @param hist_pos 환형 버퍼의 현재 기록 위치 (head)
 * @param max_val  최대값 (정규화용, 이 값이 █에 매핑)
 * @param cp       COLOR_PAIR 인덱스
 */
void draw_sparkline(WINDOW *win, int y, int x, int w,
                    const double *data, int n, int hist_pos,
                    double max_val, int cp) {
    if (w < 1 || max_val <= 0) return;
    wattron(win, COLOR_PAIR(cp));
    for (int i = 0; i < w; i++) {
        int di = ((hist_pos - w + i) % n + n) % n;
        double ratio = (data[di] / max_val);
        if (ratio < 0) ratio = 0;
        if (ratio > 1) ratio = 1;
        int idx = (int)(ratio * (SPARK_N - 1));
        if (idx < 0) idx = 0;
        if (idx >= SPARK_N) idx = SPARK_N - 1;
        mvwaddstr(win, y, x + i, SPARK_BLOCKS[idx]);
    }
    wattroff(win, COLOR_PAIR(cp));
}

/**
 * draw_scrollbar — 세로 스크롤바 렌더링 (▲ [thumb] ▼)
 *
 * [호출 시점] draw_table()의 끝에서 자동 호출
 * [동작] total/viewport 비율로 thumb 크기와 위치를 계산하여 유니코드 블록으로 표시
 *        ▲(상단) + ░(트랙) + █(thumb) + ▼(하단) 구조
 */
void draw_scrollbar(WINDOW *win, int y, int h, int x_col, ScrollState *s) {
    if (!s || s->total <= s->viewport || s->viewport <= 0) return;
    int track_h = h - 2;
    if (track_h < 1) return;

    int thumb_h = (int)((double)s->viewport / s->total * track_h);
    if (thumb_h < 1) thumb_h = 1;
    int thumb_pos = (s->total > s->viewport)
        ? (int)((double)s->position / (s->total - s->viewport) * (track_h - thumb_h))
        : 0;

    wattron(win, COLOR_PAIR(C_DIM));
    mvwaddstr(win, y,     x_col, "\xe2\x96\xb2");
    mvwaddstr(win, y+h-1, x_col, "\xe2\x96\xbc");
    for (int i = 0; i < track_h; i++) {
        bool in_thumb = (i >= thumb_pos && i < thumb_pos + thumb_h);
        mvwaddstr(win, y+1+i, x_col, in_thumb ? "\xe2\x96\x88" : "\xe2\x96\x91");
    }
    wattroff(win, COLOR_PAIR(C_DIM));
}

/**
 * draw_table — 스크롤 가능한 테이블 위젯 (헤더 + 데이터 행 + 스크롤바)
 *
 * [호출 시점] VM 목록, 컨테이너 목록, 네트워크 목록 등 탭 화면 렌더링 시
 * [동작] 1) 헤더 행 렌더링 (A_REVERSE 반전, A_BOLD)
 *        2) ScrollState 업데이트 (total/viewport)
 *        3) scroll.position부터 viewport만큼 데이터 행 렌더링
 *        4) selected 행은 C_HIGHLIGHT 반전으로 강조
 *        5) 우측에 스크롤바 자동 표시
 *
 * @param win     대상 ncurses 윈도우
 * @param y0,x0   테이블 좌상단 좌표
 * @param h,w     테이블 높이/너비
 * @param headers 컬럼 헤더 문자열 배열 (ncols개)
 * @param col_w   컬럼별 너비 배열 (ncols개)
 * @param ncols   컬럼 수
 * @param rows    2D 데이터 배열 (rows[행*ncols + 열], nrows * ncols개)
 * @param nrows   데이터 행 수
 * @param scroll  스크롤 상태 (position/total/viewport, 호출 간 유지)
 * @param selected 현재 선택된 행 인덱스 (-1이면 선택 없음)
 * @param cp_head 헤더 COLOR_PAIR 인덱스
 */
void draw_table(WINDOW *win, int y0, int x0, int h, int w,
                const char **headers, const int *col_w, int ncols,
                const char **rows, int nrows,
                ScrollState *scroll, int selected, int cp_head) {
    /* 헤더 */
    wattron(win, COLOR_PAIR(cp_head) | A_BOLD | A_REVERSE);
    int cx = x0 + 1;
    for (int c = 0; c < ncols; c++) {
        mvwprintw(win, y0, cx, "%-*.*s", col_w[c], col_w[c], headers[c]);
        cx += col_w[c] + 1;
    }
    wattroff(win, COLOR_PAIR(cp_head) | A_BOLD | A_REVERSE);

    /* 스크롤 상태 갱신 */
    scroll->total    = nrows;
    scroll->viewport = h - 2;

    /* 데이터 행 */
    int vis = scroll->viewport;
    for (int r = 0; r < vis; r++) {
        int ri = r + scroll->position;
        if (ri >= nrows) break;
        bool is_sel = (ri == selected);
        if (is_sel)
            wattron(win, COLOR_PAIR(C_HIGHLIGHT) | A_REVERSE | A_BOLD);
        cx = x0 + 1;
        for (int c = 0; c < ncols; c++) {
            const char *cell = rows[ri * ncols + c];
            mvwprintw(win, y0+1+r, cx, "%-*.*s",
                      col_w[c], col_w[c], cell ? cell : "");
            cx += col_w[c] + 1;
        }
        if (is_sel)
            wattroff(win, COLOR_PAIR(C_HIGHLIGHT) | A_REVERSE | A_BOLD);
    }

    /* 스크롤바 */
    draw_scrollbar(win, y0, h, x0 + w - 2, scroll);
    (void)w;
}

/* ════════════════════════════════════════════════════════════════════
 *  팝업/다이얼로그
 * ════════════════════════════════════════════════════════════════════ */

/**
 * create_popup — 화면 중앙에 모달 팝업 윈도우를 생성
 *
 * [호출 시점] VM 생성, 삭제 확인 등 사용자 입력이 필요한 대화상자
 * [동작] 화면 중앙 좌표 계산 → newwin 생성 → 배경색 + 테두리(box) + 타이틀 출력
 * [주의] 반환된 WINDOW*는 사용 후 반드시 destroy_popup()으로 해제해야 합니다.
 *
 * @return 새로 생성된 ncurses WINDOW 포인터
 */
WINDOW *create_popup(int h, int w, const char *title) {
    int sy = (LINES - h) / 2;
    int sx = (COLS  - w) / 2;
    if (sy < 0) sy = 0;
    if (sx < 0) sx = 0;
    WINDOW *pop = newwin(h, w, sy, sx);
    wbkgd(pop, COLOR_PAIR(C_DIM));
    werase(pop);
    box(pop, 0, 0);
    if (title) {
        wattron(pop, A_BOLD | COLOR_PAIR(C_TITLE));
        mvwprintw(pop, 0, 2, " %s ", title);
        wattroff(pop, A_BOLD | COLOR_PAIR(C_TITLE));
    }
    return pop;
}

/**
 * destroy_popup — 팝업 윈도우를 안전하게 제거하고 배경을 복원
 *
 * [동작] werase(지우기) → wrefresh(화면 갱신) → delwin(메모리 해제) → stdscr 복원
 */
void destroy_popup(WINDOW *pop) {
    werase(pop); wrefresh(pop); delwin(pop);
    touchwin(stdscr); refresh();
}

/**
 * prompt_input — 화면 하단에 입력 프롬프트를 표시하고 사용자 입력을 받음
 *
 * [호출 시점] VM 이름 입력, 검색 등 텍스트 입력이 필요할 때
 * [동작] echo 활성화 → 커서 표시 → 하단 줄에 프롬프트 표시 → getnstr로 입력 수신
 *        완료 후 noecho/curs_set(0)으로 원래 상태 복원
 *
 * @param prompt 프롬프트 문자열 (예: "VM Name: ")
 * @param buf    입력 버퍼
 * @param buf_len 버퍼 크기
 * @param cp     프롬프트 COLOR_PAIR
 * @return 입력된 문자열 길이
 */
int prompt_input(const gchar *prompt, char *buf, int buf_len, int cp) {
    echo(); curs_set(1); flushinp(); timeout(-1);
    wattron(stdscr, COLOR_PAIR(cp) | A_BOLD | A_REVERSE);
    mvprintw(LINES-1, 0, " %s%-50s", prompt, " ");
    wattroff(stdscr, COLOR_PAIR(cp) | A_BOLD | A_REVERSE);
    move(LINES-1, (int)strlen(prompt) + 1);
    getnstr(buf, buf_len - 1);
    timeout(50); noecho(); curs_set(0);
    return (int)strlen(buf);
}

/**
 * confirm_dialog — 파괴적 작업(삭제, 롤백 등)의 안전 확인 대화상자
 *
 * [호출 시점] VM 삭제, 스냅샷 롤백 등 되돌릴 수 없는 작업 전
 * [동작] 경고 메시지 표시(빨강+깜빡임) → 사용자가 target 문자열을 정확히 타이핑해야 확인
 *        잘못 입력하면 FALSE 반환 → 작업 취소
 *
 * @param warn   경고 메시지 (예: "This will DESTROY all data!")
 * @param target 확인용 타이핑 문자열 (예: VM 이름 "web-prod")
 * @return TRUE: 확인됨, FALSE: 취소됨
 */
gboolean confirm_dialog(const char *warn, const char *target) {
    wattron(stdscr, COLOR_PAIR(C_RED) | A_BOLD | A_REVERSE | A_BLINK);
    mvprintw(LINES-2, 0, " [ \xe2\x9a\xa0 ] %s ", warn);
    wattroff(stdscr, COLOR_PAIR(C_RED) | A_BOLD | A_REVERSE | A_BLINK);

    char buf[128] = {0};
    char pmsg[160];
    snprintf(pmsg, sizeof(pmsg), "Type '%s' to confirm: ", target);
    prompt_input(pmsg, buf, sizeof(buf), C_RED);
    return (strcmp(buf, target) == 0);
}

/* ════════════════════════════════════════════════════════════════════
 *  브레일 차트 엔진
 * ════════════════════════════════════════════════════════════════════ */

/**
 * bgrid_init — 브레일 그리드를 초기화 (모든 도트를 0으로 클리어)
 *
 * [호출 시점] 차트를 새로 그리기 전에 매 프레임마다 호출
 * [동작] 그리드 크기를 MAX_CHART_W/H로 클램핑하고, 전체 비트맵을 0으로 초기화
 *
 * @param g 초기화할 BrailleGrid 구조체
 * @param w 차트 너비 (터미널 셀 수, 실제 픽셀은 w*2)
 * @param h 차트 높이 (터미널 셀 수, 실제 픽셀은 h*4)
 */
void bgrid_init(BrailleGrid *g, int w, int h) {
    int cw = w < MAX_CHART_W ? w : MAX_CHART_W;
    int ch = h < MAX_CHART_H ? h : MAX_CHART_H;
    g->w = cw; g->h = ch;
    memset(g->grid, 0, sizeof(g->grid));
}

/**
 * bgrid_set_pixel — 브레일 그리드에 단일 픽셀을 설정 (점등)
 *
 * [동작] 픽셀 좌표(px, py)를 셀 좌표(cx, cy)로 변환 후 해당 비트를 OR 설정
 *        한 셀은 2x4 픽셀이므로: cx = px/2, cy = py/4, 나머지가 셀 내 도트 위치
 */
void bgrid_set_pixel(BrailleGrid *g, int px, int py) {
    if (px < 0 || py < 0) return;
    int cx_val = px / 2, cy = py / 4;
    int cr = px % 2, rr = py % 4;
    if (cx_val >= g->w || cy >= g->h) return;
    g->grid[cy][cx_val] |= BRAILLE_DOTS[rr][cr];
}

/**
 * bgrid_draw_line — 브레일 그리드에 Bresenham 직선 알고리즘으로 선분을 그림
 *
 * [동작] (x0,y0)에서 (x1,y1)까지 정수 좌표 직선을 그리는 Bresenham 알고리즘 사용.
 *        부동소수점 연산 없이 정수 덧셈/비교만으로 픽셀 정확 직선 렌더링.
 *        bgrid_plot_series()에서 인접 데이터 포인트를 연결할 때 사용.
 */
void bgrid_draw_line(BrailleGrid *g, int x0, int y0, int x1, int y1) {
    int dx = abs(x1-x0), dy = abs(y1-y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    while (1) {
        bgrid_set_pixel(g, x0, y0);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

/**
 * bgrid_plot_series — 환형 버퍼의 시계열 데이터를 브레일 그리드에 라인 차트로 플롯
 *
 * [호출 시점] CPU, 메모리, 네트워크 실시간 차트 렌더링 시
 * [동작] 1) 그리드 너비*2 = 픽셀 해상도로 데이터 매핑
 *        2) 각 픽셀 X에 대해 환형 버퍼에서 해당 값 조회 → Y 좌표로 변환
 *        3) 이전 점과 현재 점을 Bresenham 직선으로 연결
 *        4) 결과: 고해상도 시계열 라인 차트
 *
 * @param g        대상 BrailleGrid (bgrid_init 호출 후)
 * @param data     시계열 데이터 환형 버퍼
 * @param n        data 배열 크기
 * @param hist_pos 환형 버퍼의 현재 head 위치
 * @param min_val  Y축 최소값
 * @param max_val  Y축 최대값 (min_val과 같으면 자동으로 +1)
 */
void bgrid_plot_series(BrailleGrid *g,
                       const double *data, int n, int hist_pos,
                       double min_val, double max_val) {
    if (max_val <= min_val) max_val = min_val + 1.0;
    int px_w = g->w * 2;
    int px_h = g->h * 4;
    int prev_px = -1, prev_py = -1;

    for (int i = 0; i < px_w; i++) {
        int di = ((hist_pos - px_w + i) % n + n) % n;
        double v = data[di];
        double ratio = (v - min_val) / (max_val - min_val);
        if (ratio < 0.0) ratio = 0.0;
        if (ratio > 1.0) ratio = 1.0;

        int px_y = px_h - 1 - (int)(ratio * (px_h - 1));
        int px_x = i;

        if (prev_px >= 0)
            bgrid_draw_line(g, prev_px, prev_py, px_x, px_y);
        else
            bgrid_set_pixel(g, px_x, px_y);

        prev_px = px_x;
        prev_py = px_y;
    }
}

/**
 * bgrid_render — 브레일 그리드를 ncurses 윈도우에 출력
 *
 * [호출 시점] bgrid_plot_series() 이후 실제 화면에 그릴 때
 * [동작] 각 셀의 비트 패턴을 UTF-8 브레일 문자로 변환하여 출력.
 *        행별로 cp_fn 콜백으로 색상을 결정 (그래디언트 효과).
 *        warn_row/crit_row에 ▶/▷ 마커를 표시하여 임계선 시각화.
 *
 * @param win       대상 ncurses 윈도우
 * @param g         렌더링할 BrailleGrid
 * @param win_y,win_x 윈도우 내 출력 시작 좌표
 * @param cp_fn     행 비율(0.0~1.0)을 COLOR_PAIR로 변환하는 콜백 (NULL이면 C_GREEN)
 * @param warn_row  경고 임계선 행 (-1이면 표시 안 함)
 * @param crit_row  위험 임계선 행 (-1이면 표시 안 함)
 */
void bgrid_render(WINDOW *win, const BrailleGrid *g,
                  int win_y, int win_x,
                  BrailleColorFn cp_fn,
                  int warn_row, int crit_row) {
    char utf8[4];
    for (int cy = 0; cy < g->h; cy++) {
        double row_ratio = (g->h > 1) ? (double)cy / (g->h - 1) : 0.0;
        int cp = cp_fn ? cp_fn(row_ratio) : C_GREEN;

        if (cy == crit_row) {
            wattron(win, COLOR_PAIR(C_CHART_CRIT) | A_BOLD);
            mvwprintw(win, win_y + cy, win_x - 2, "\xe2\x96\xb6");
            wattroff(win, COLOR_PAIR(C_CHART_CRIT) | A_BOLD);
        } else if (cy == warn_row) {
            wattron(win, COLOR_PAIR(C_CHART_WARN));
            mvwprintw(win, win_y + cy, win_x - 2, "\xe2\x96\xb7");
            wattroff(win, COLOR_PAIR(C_CHART_WARN));
        }

        wattron(win, COLOR_PAIR(cp));
        for (int cx_val = 0; cx_val < g->w; cx_val++) {
            braille_to_utf8(g->grid[cy][cx_val], utf8);
            mvwaddstr(win, win_y + cy, win_x + cx_val, utf8);
        }
        wattroff(win, COLOR_PAIR(cp));
    }
}

/**
 * braille_color_gradient — 브레일 차트용 행 비율 → 컬러 그래디언트 변환
 * row_ratio는 0.0(상단)~1.0(하단)이므로, 값이 높은 상단=빨강, 하단=초록이 됨.
 */
int braille_color_gradient(double row_ratio) {
    double val_ratio = 1.0 - row_ratio;
    return pcv_color_for_pct(val_ratio);
}

/** 네트워크 다운로드 차트용 단색 컬러 함수 (항상 C_CH_NETDL 반환) */
int braille_color_netdl(double r __attribute__((unused))) { return C_CH_NETDL; }

/* ════════════════════════════════════════════════════════════════════
 *  Y축 / X축 레이블
 * ════════════════════════════════════════════════════════════════════ */

/**
 * draw_y_axis — 차트 좌측에 Y축 눈금 레이블을 출력 (0%, 25%, 50%, 75%, 100%)
 *
 * @param win     대상 윈도우
 * @param win_y   차트 시작 행
 * @param chart_h 차트 높이 (셀 행 수)
 * @param y_lo    Y축 최소값
 * @param y_hi    Y축 최대값
 * @param unit    단위 문자열 (현재 미사용, 향후 확장용)
 * @param label_x 레이블 출력 X 좌표
 */
void draw_y_axis(WINDOW *win, int win_y, int chart_h,
                 double y_lo, double y_hi, const char *unit,
                 int label_x) {
    static const double marks[] = {1.0, 0.75, 0.50, 0.25, 0.0};
    int n_marks = 5;
    (void)unit;
    for (int i = 0; i < n_marks; i++) {
        int row = (int)(marks[i] * (chart_h - 1));
        double val = y_hi - marks[i] * (y_hi - y_lo);
        int screen_row = (int)((1.0 - marks[i]) * (chart_h - 1));
        wattron(win, COLOR_PAIR(C_CHART_AXIS));
        mvwprintw(win, win_y + screen_row, label_x, "%4.0f", val);
        wattroff(win, COLOR_PAIR(C_CHART_AXIS));
        (void)row;
    }
}

/**
 * draw_x_axis — 차트 하단에 X축 시간 레이블을 출력 (-Ns ... -N/2s ... NOW)
 *
 * @param win        대상 윈도우
 * @param win_y      X축 출력 행
 * @param win_x      X축 시작 열
 * @param chart_w    차트 너비 (셀 수)
 * @param hist_count 히스토리 데이터 포인트 수
 * @param refresh_ms 갱신 주기 (밀리초, 예: 1000 = 1초)
 */
void draw_x_axis(WINDOW *win, int win_y, int win_x,
                 int chart_w, int hist_count, int refresh_ms) {
    int total_sec = hist_count * refresh_ms / 1000;
    wattron(win, COLOR_PAIR(C_CHART_AXIS));
    mvwprintw(win, win_y, win_x, "-%ds", total_sec);
    mvwprintw(win, win_y, win_x + chart_w/2 - 2, "-%ds", total_sec/2);
    mvwprintw(win, win_y, win_x + chart_w - 3, "NOW");
    wattroff(win, COLOR_PAIR(C_CHART_AXIS));
}
