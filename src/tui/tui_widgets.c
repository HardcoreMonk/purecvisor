





















































#include "tui_widgets.h"

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>









const char *SPARK_BLOCKS[] = {
    " ", "\xe2\x96\x81", "\xe2\x96\x82", "\xe2\x96\x83",
    "\xe2\x96\x84", "\xe2\x96\x85", "\xe2\x96\x86",
    "\xe2\x96\x87", "\xe2\x96\x88"
};














const int BRAILLE_DOTS[4][2] = {
    {0x01, 0x08},
    {0x02, 0x10},
    {0x04, 0x20},
    {0x40, 0x80},
};





const char *SPIN_FRAMES[] = {
    "\xe2\xa0\x8b","\xe2\xa0\x99","\xe2\xa0\xb9","\xe2\xa0\xb8",
    "\xe2\xa0\xbc","\xe2\xa0\xb4","\xe2\xa0\xa6","\xe2\xa0\xa7",
    "\xe2\xa0\x87","\xe2\xa0\x8f"
};















void braille_to_utf8(int bits, char out[4]) {
    int cp = BRAILLE_BASE_CP | (bits & 0xFF);

    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    out[3] = '\0';
}














int pcv_color_for_pct(double ratio) {
    if (ratio < 0.20) return C_GRAD_0;
    if (ratio < 0.40) return C_GRAD_1;
    if (ratio < 0.60) return C_GRAD_2;
    if (ratio < 0.75) return C_GRAD_3;
    if (ratio < 0.85) return C_GRAD_4;
    if (ratio < 0.95) return C_GRAD_5;
    return C_GRAD_6;
}











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














void draw_bar(WINDOW *win, int y, int x, int w, double pct, int cp) {
    if (w < 4) return;

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























void draw_table(WINDOW *win, int y0, int x0, int h, int w,
                const char **headers, const int *col_w, int ncols,
                const char **rows, int nrows,
                ScrollState *scroll, int selected, int cp_head) {

    wattron(win, COLOR_PAIR(cp_head) | A_BOLD | A_REVERSE);
    int cx = x0 + 1;
    for (int c = 0; c < ncols; c++) {
        mvwprintw(win, y0, cx, "%-*.*s", col_w[c], col_w[c], headers[c]);
        cx += col_w[c] + 1;
    }
    wattroff(win, COLOR_PAIR(cp_head) | A_BOLD | A_REVERSE);


    scroll->total    = nrows;
    scroll->viewport = h - 2;


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


    draw_scrollbar(win, y0, h, x0 + w - 2, scroll);
    (void)w;
}














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






void destroy_popup(WINDOW *pop) {
    werase(pop); wrefresh(pop); delwin(pop);
    touchwin(stdscr); refresh();
}














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















void bgrid_init(BrailleGrid *g, int w, int h) {
    int cw = w < MAX_CHART_W ? w : MAX_CHART_W;
    int ch = h < MAX_CHART_H ? h : MAX_CHART_H;
    g->w = cw; g->h = ch;
    memset(g->grid, 0, sizeof(g->grid));
}







void bgrid_set_pixel(BrailleGrid *g, int px, int py) {
    if (px < 0 || py < 0) return;
    int cx_val = px / 2, cy = py / 4;
    int cr = px % 2, rr = py % 4;
    if (cx_val >= g->w || cy >= g->h) return;
    g->grid[cy][cx_val] |= BRAILLE_DOTS[rr][cr];
}








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





int braille_color_gradient(double row_ratio) {
    double val_ratio = 1.0 - row_ratio;
    return pcv_color_for_pct(val_ratio);
}


int braille_color_netdl(double r __attribute__((unused))) { return C_CH_NETDL; }
















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











void draw_x_axis(WINDOW *win, int win_y, int win_x,
                 int chart_w, int hist_count, int refresh_ms) {
    int total_sec = hist_count * refresh_ms / 1000;
    wattron(win, COLOR_PAIR(C_CHART_AXIS));
    mvwprintw(win, win_y, win_x, "-%ds", total_sec);
    mvwprintw(win, win_y, win_x + chart_w/2 - 2, "-%ds", total_sec/2);
    mvwprintw(win, win_y, win_x + chart_w - 3, "NOW");
    wattroff(win, COLOR_PAIR(C_CHART_AXIS));
}
