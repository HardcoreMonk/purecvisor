















#ifndef TUI_WIDGETS_H
#define TUI_WIDGETS_H

#include <ncurses.h>
#include <glib.h>







#define C_CPU       1
#define C_MEM       2
#define C_FLEET     3
#define C_GREEN     4
#define C_RED       5
#define C_YELLOW    6
#define C_DIM       7
#define C_HIGHLIGHT 8
#define C_LOG       9
#define C_TITLE     10
#define C_CYAN      11
#define C_TAB_ACT   12
#define C_SPARK_CPU 13
#define C_SPARK_NET 14

#define C_GRAD_BASE 15
#define C_GRAD_0  15
#define C_GRAD_1  16
#define C_GRAD_2  17
#define C_GRAD_3  18
#define C_GRAD_4  19
#define C_GRAD_5  20
#define C_GRAD_6  21

#define C_CH_CPU   22
#define C_CH_MEM   23
#define C_CH_DSK   24
#define C_CH_NETDL 25
#define C_CH_NETUL 26

#define C_CHART_FRAME 27
#define C_CHART_WARN  28
#define C_CHART_CRIT  29
#define C_CHART_AXIS  30

#define C_SWAP        31
#define C_TEMP        32
#define C_LOAD        33
#define C_CACHED      34




extern const char *SPARK_BLOCKS[];
#define SPARK_N 9




#define MAX_CHART_W  128
#define MAX_CHART_H  16
#define BRAILLE_BASE_CP 0x2800

extern const int BRAILLE_DOTS[4][2];

typedef struct {
    int grid[MAX_CHART_H][MAX_CHART_W];
    int w;
    int h;
} BrailleGrid;

void braille_to_utf8(int bits, char out[4]);




typedef struct {
    int total;
    int position;
    int viewport;
} ScrollState;

static inline void scroll_up(ScrollState *s) {
    if (s->position > 0) s->position--;
}
static inline void scroll_down(ScrollState *s) {
    if (s->position + s->viewport < s->total) s->position++;
}
static inline void scroll_select_clamp(int *sel, int count) {
    if (*sel >= count) *sel = count > 0 ? count - 1 : 0;
    if (*sel < 0) *sel = 0;
}




#define SPIN_N 10
extern const char *SPIN_FRAMES[];

typedef struct {
    gboolean active;
    int      frame;
    gint64   last_tick_us;
    int      interval_ms;
    gchar    label[80];
} Spinner;

void spinner_start(Spinner *sp, const gchar *label);
void spinner_stop(Spinner *sp);
void spinner_tick(Spinner *sp);
void spinner_draw(WINDOW *win, int y, int x, const Spinner *sp);




typedef enum {
    PC_LENGTH,
    PC_PERCENT,
    PC_MIN,
    PC_MAX,
    PC_FILL
} PcConstraintType;

typedef struct {
    PcConstraintType type;
    int val;
} PcConstraint;

void pcv_layout_split(int total, const PcConstraint *c, int n, int *sizes);






int pcv_color_for_pct(double ratio);





void draw_panel(WINDOW *win, int y, int x, int h, int w,
                const char *title, int cp);

void draw_bar(WINDOW *win, int y, int x, int w, double pct, int cp);

void draw_sparkline(WINDOW *win, int y, int x, int w,
                    const double *data, int n, int hist_pos,
                    double max_val, int cp);

void draw_scrollbar(WINDOW *win, int y, int h, int x_col, ScrollState *s);

void draw_table(WINDOW *win, int y0, int x0, int h, int w,
                const char **headers, const int *col_w, int ncols,
                const char **rows, int nrows,
                ScrollState *scroll, int selected, int cp_head);





WINDOW *create_popup(int h, int w, const char *title);
void destroy_popup(WINDOW *pop);
int prompt_input(const gchar *prompt, char *buf, int buf_len, int cp);
gboolean confirm_dialog(const char *warn, const char *target);





void bgrid_init(BrailleGrid *g, int w, int h);
void bgrid_set_pixel(BrailleGrid *g, int px, int py);
void bgrid_draw_line(BrailleGrid *g, int x0, int y0, int x1, int y1);
void bgrid_plot_series(BrailleGrid *g,
                       const double *data, int n, int hist_pos,
                       double min_val, double max_val);

typedef int (*BrailleColorFn)(double row_ratio);

void bgrid_render(WINDOW *win, const BrailleGrid *g,
                  int win_y, int win_x,
                  BrailleColorFn cp_fn,
                  int warn_row, int crit_row);

int braille_color_gradient(double row_ratio);
int braille_color_netdl(double r);

void draw_y_axis(WINDOW *win, int win_y, int chart_h,
                 double y_lo, double y_hi, const char *unit,
                 int label_x);
void draw_x_axis(WINDOW *win, int win_y, int win_x,
                 int chart_w, int hist_count, int refresh_ms);

#endif
