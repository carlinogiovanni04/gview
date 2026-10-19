#define GVIEW_IMPLEMENTATION
#include "gview.h"

#include "gcui.h"

#include <SDL3/SDL_dialog.h>  // NOTE(Giovanni): I should move this to gcui
#include <sys/stat.h>

#define GV_LABEL_W 170.0f
#define GV_DETAIL_W 340.0f
#define GV_RULER_H 30.0f
#define GV_LANE_H 24.0f
#define GV_BOX_H 20.0f
#define GV_ROW_PAD 6.0f
#define GV_LINE_H 22.0f
#define GV_MAX_LANES 64u
#define GV_SPLIT_GRAB 5.0f

static const GCCOLOR C_BG = {24, 24, 27, 255};
static const GCCOLOR C_PANEL = {36, 36, 42, 255};
static const GCCOLOR C_BODY = {28, 28, 33, 255};
static const GCCOLOR C_ROW = {32, 32, 38, 255};
static const GCCOLOR C_ROW_ALT = {30, 30, 35, 255};
static const GCCOLOR C_TEXT = {245, 245, 245, 255};
static const GCCOLOR C_DIM = {180, 180, 190, 255};
static const GCCOLOR C_FAINT = {105, 105, 115, 255};
static const GCCOLOR C_LINE = {58, 58, 68, 255};
static const GCCOLOR C_GRID = {44, 44, 52, 255};
static const GCCOLOR C_WARN = {235, 165, 60, 255};
static const GCCOLOR C_ERR = {235, 90, 90, 255};
static const GCCOLOR C_SELECT = {255, 220, 120, 255};
static const GCCOLOR C_DIM_SPAN = {58, 58, 66, 255};

static const GCCOLOR GV_PALETTE[] = {
    {66, 133, 244, 255}, {52, 168, 83, 255},  {251, 188, 5, 255},
    {234, 67, 53, 255},  {155, 89, 182, 255}, {0, 172, 193, 255},
    {255, 112, 67, 255}, {124, 179, 66, 255}, {126, 87, 194, 255},
    {38, 166, 154, 255}, {216, 67, 21, 255},  {255, 167, 38, 255},
};

typedef struct {
    GCRECT toolbar;
    GCRECT body;
    GCRECT main;
    GCRECT details;
    GCRECT footer;
} GVLayout;

typedef struct {
    GCRECT view;
    GCRECT timeline;
    GCRECT ruler;
    GCRECT label;
    GCRECT scrollbar;
    float  content_h;
} GVTimeline;

typedef struct {
    GCARENA  *arena;
    GCWINDOW *window;
    GCUI     *ui;

    GViewTrace trace;
    GViewFlame flame;

    char path[1024];
    char pending_path[1024];
    char status[512];
    int  has_pending;
    int  loaded;

    int view_mode; // 0 timeline, 1 flame
    int selected;
    int hovered;
    int hovered_flame;

    double view_start;
    double view_span;
    float  vscroll;
    float  flame_scroll;

    float details_w;
    float label_w;
    int   split_drag;
    float split_start_x;
    float split_start_w;

    float    reload_timer;
    uint64_t file_mtime;
    uint64_t file_size;
    int      failed_valid;
    uint64_t failed_mtime;
    uint64_t failed_size;

    char   *filter;
    GCUSIZE filter_cap;

    int   drag_active;
    int   drag_moved;
    float drag_x;
    double drag_view_start;
} GViewApp;

static GCCOLOR gv_shade(GCCOLOR c, float f)
{
    return gc_color((GCU8)((float)c.r * f), (GCU8)((float)c.g * f),
                    (GCU8)((float)c.b * f), c.a);
}

static GCCOLOR gv_color_for(uint32_t name_id, int dim)
{
    if (dim) {
        return C_DIM_SPAN;
    }
    return GV_PALETTE[name_id % (uint32_t)(sizeof(GV_PALETTE) / sizeof(GV_PALETTE[0]))];
}

static int gv_matches(const GViewApp *app, const GViewSpan *span)
{
    if (!app->filter || !app->filter[0]) {
        return 1;
    }
    return gv__contains_ci(gview_name(&app->trace, span->name_id), app->filter);
}

static float gv_thread_row_h(const GViewThread *thread)
{
    uint32_t depth = thread->max_depth;
    if (depth > GV_MAX_LANES) {
        depth = GV_MAX_LANES;
    }
    return GV_ROW_PAD * 2.0f + (float)depth * GV_LANE_H + GV_BOX_H;
}

static const char *gv_thread_label(const GViewApp *app, uint32_t pid, uint32_t tid)
{
    uint32_t i;
    for (i = 0; i < app->trace.thread_count; i++) {
        if (app->trace.threads[i].pid == pid && app->trace.threads[i].tid == tid) {
            return app->trace.threads[i].label;
        }
    }
    return "-";
}

static void gv_fit(GViewApp *app)
{
    if (!app->loaded) {
        return;
    }

    app->view_start = (double)app->trace.t0;
    app->view_span  = (double)(app->trace.t1 - app->trace.t0);
    if (app->view_span < 1.0) {
        app->view_span = 1.0;
    }
    app->vscroll      = 0.0f;
    app->flame_scroll = 0.0f;
}

static int gv_file_sig(const char *path, uint64_t *mtime, uint64_t *size)
{
#if defined(_WIN32)
    struct _stat64 st;
    if (_stat64(path, &st) != 0) {
        return 0;
    }
    *mtime = (uint64_t)st.st_mtime;
    *size  = (uint64_t)st.st_size;
#else
    struct stat st;
    if (stat(path, &st) != 0) {
        return 0;
    }
    *mtime = (uint64_t)st.st_mtime;
    *size  = (uint64_t)st.st_size;
#endif
    return 1;
}

static int gv_load(GViewApp *app, const char *path)
{
    GViewTrace tmp;
    char       err[256];

    gview_trace_init(&tmp);
    err[0] = '\0';

    if (!gview_trace_load_file(&tmp, path, err, sizeof err)) {
        snprintf(app->status, sizeof app->status, "error: %s", err);
        return 0;
    }

    gview_trace_free(&app->trace);
    app->trace = tmp;

    snprintf(app->path, sizeof app->path, "%s", path);
    app->loaded        = 1;
    app->selected      = -1;
    app->hovered       = -1;
    app->hovered_flame = -1;
    app->vscroll       = 0.0f;
    app->flame_scroll  = 0.0f;

    gv_fit(app);

    if (!gview_flame_build(&app->flame, &app->trace)) {
        snprintf(app->status, sizeof app->status, "loaded %s (flame graph unavailable)", path);
    } else {
        snprintf(app->status, sizeof app->status, "loaded %s", path);
    }

    gv_file_sig(path, &app->file_mtime, &app->file_size);
    app->failed_valid = 0;
    return 1;
}

static void gv_reload(GViewApp *app)
{
    if (app->path[0]) {
        gv_load(app, app->path);
    }
}

static void gv_poll_reload(GViewApp *app)
{
    uint64_t mtime;
    uint64_t size;

    if (!app->path[0]) {
        return;
    }

    app->reload_timer += gc_window_dt(app->window);
    if (app->reload_timer < 0.5f) {
        return;
    }
    app->reload_timer = 0.0f;

    if (!gv_file_sig(app->path, &mtime, &size)) {
        return;
    }
    if (mtime == app->file_mtime && size == app->file_size) {
        return;
    }
    if (app->failed_valid && mtime == app->failed_mtime && size == app->failed_size) {
        return;
    }

    if (!gv_load(app, app->path)) {
        app->failed_valid = 1;
        app->failed_mtime = mtime;
        app->failed_size  = size;
    }
}

static void SDLCALL gv_dialog_cb(void *userdata, const char *const *filelist, int filter)
{
    GViewApp *app = (GViewApp *)userdata;
    (void)filter;

    if (!app || !filelist || !filelist[0]) {
        return;
    }

    snprintf(app->pending_path, sizeof app->pending_path, "%s", filelist[0]);
    app->has_pending = 1;
}

static void gv_open_dialog(GViewApp *app)
{
    static const SDL_DialogFileFilter filters[] = {
        {"Trace JSON", "json"},
        {"All files", "*"},
    };

    SDL_ShowOpenFileDialog(gv_dialog_cb, app, (SDL_Window *)app->window->handle, filters,
                           2, NULL, false);
}

static void gv_layout(GViewApp *app, GVLayout *l)
{
    float w         = gc_window_width(app->window);
    float h         = gc_window_height(app->window);
    float margin    = 12.0f;
    float gap       = 10.0f;
    float toolbar_h = 46.0f;
    float footer_h  = 24.0f;
    float max_main  = 420.0f;
    float details_w = app->details_w;
    float body_y;
    float body_h;

    if (details_w < 200.0f) details_w = 200.0f;
    if (details_w > GV_DETAIL_W * 2.0f) details_w = GV_DETAIL_W * 2.0f;

    if (w < 820.0f || w - details_w - gap < max_main) {
        details_w = 0.0f;
    }
    if (w < 820.0f) details_w = 0.0f;

    app->details_w = details_w;

    l->toolbar = (GCRECT){margin, margin, w - margin * 2.0f, toolbar_h};
    l->footer  = (GCRECT){margin, h - margin - footer_h, w - margin * 2.0f, footer_h};

    body_y = margin + toolbar_h + gap;
    body_h = h - body_y - margin - footer_h - gap;
    if (body_h < 80.0f) body_h = 80.0f;

    l->body = (GCRECT){margin, body_y, w - margin * 2.0f, body_h};

    if (details_w > 0.0f) {
        l->details = (GCRECT){w - margin - details_w, body_y, details_w, body_h};
        l->main    = (GCRECT){margin, body_y, w - margin * 2.0f - details_w - gap, body_h};
    } else {
        l->details = (GCRECT){0, 0, 0, 0};
        l->main    = l->body;
    }

    {
        float max_label = l->main.w - 120.0f;
        if (max_label > 420.0f) max_label = 420.0f;
        if (max_label < 80.0f) max_label = 80.0f;
        if (app->label_w < 80.0f) app->label_w = 80.0f;
        if (app->label_w > max_label) app->label_w = max_label;
    }
}

static void gv_splitter_input(GViewApp *app, const GVLayout *l)
{
    GCUI *ui = app->ui;

    if (app->split_drag) {
        float dx = ui->mouseX - app->split_start_x;

        if (app->split_drag == 1) {
            float w = app->split_start_w - dx;
            if (w < 200.0f) w = 200.0f;
            if (w > 900.0f) w = 900.0f;
            app->details_w = w;
        } else if (app->split_drag == 2) {
            float w = app->split_start_w + dx;
            if (w < 80.0f) w = 80.0f;
            if (w > 500.0f) w = 500.0f;
            app->label_w = w;
        }

        if (ui->mouseReleased || !ui->mouseDown) {
            app->split_drag = 0;
        }
        return;
    }

    if (l->details.w > 0.0f) {
        GCRECT hit = {l->details.x - GV_SPLIT_GRAB, l->body.y, GV_SPLIT_GRAB * 2.0f,
                      l->body.h};
        if (gc_rect_contains(hit, ui->mouseX, ui->mouseY) && ui->mousePressed) {
            app->split_drag    = 1;
            app->split_start_x = ui->mouseX;
            app->split_start_w = app->details_w;
            return;
        }
    }

    if (app->view_mode == 0) {
        GCRECT hit = {l->main.x + app->label_w - GV_SPLIT_GRAB, l->main.y,
                      GV_SPLIT_GRAB * 2.0f, l->main.h};
        if (gc_rect_contains(hit, ui->mouseX, ui->mouseY) && ui->mousePressed) {
            app->split_drag    = 2;
            app->split_start_x = ui->mouseX;
            app->split_start_w = app->label_w;
        }
    }
}

static void gv_draw_splitters(GViewApp *app, const GVLayout *l)
{
    GCUI *ui = app->ui;

    if (l->details.w > 0.0f) {
        float   x   = l->details.x - GV_SPLIT_GRAB;
        GCRECT  hit = {x, l->body.y, GV_SPLIT_GRAB * 2.0f, l->body.h};
        int     hot = app->split_drag == 1 ||
                      gc_rect_contains(hit, ui->mouseX, ui->mouseY);
        gc_draw_fill(ui->window, (GCRECT){l->details.x - 1.0f, l->body.y, 2.0f, l->body.h},
                     hot ? C_DIM : C_LINE);
    }

    if (app->view_mode == 0) {
        float   x   = l->main.x + app->label_w - GV_SPLIT_GRAB;
        GCRECT  hit = {x, l->main.y, GV_SPLIT_GRAB * 2.0f, l->main.h};
        int     hot = app->split_drag == 2 ||
                      gc_rect_contains(hit, ui->mouseX, ui->mouseY);
        gc_draw_fill(ui->window,
                     (GCRECT){l->main.x + app->label_w - 1.0f, l->main.y, 2.0f, l->main.h},
                     hot ? C_DIM : C_LINE);
    }
}

static void gv_clamp_view(GViewApp *app)
{
    double total;
    double max_start;

    if (!app->loaded) {
        return;
    }

    total = (double)(app->trace.t1 - app->trace.t0);
    if (total < 1.0) {
        total = 1.0;
    }

    if (app->view_span < 1.0) {
        app->view_span = 1.0;
    }
    if (app->view_span > total) {
        app->view_span = total;
    }

    max_start = (double)app->trace.t0 + total - app->view_span;
    if (app->view_start < (double)app->trace.t0) {
        app->view_start = (double)app->trace.t0;
    }
    if (app->view_start > max_start) {
        app->view_start = max_start;
    }
}

static void gv_zoom_at(GViewApp *app, GCRECT timeline, float mouse_x, int direction)
{
    double old_span;
    double new_span;
    double total;
    double t;
    double anchor;

    if (!app->loaded) {
        return;
    }

    old_span = app->view_span;
    new_span = old_span * (direction > 0 ? 0.8 : 1.25);
    total    = (double)(app->trace.t1 - app->trace.t0);
    if (total < 1.0) {
        total = 1.0;
    }

    if (new_span < 1.0) new_span = 1.0;
    if (new_span > total) new_span = total;

    t = ((double)mouse_x - (double)timeline.x) / (double)timeline.w;
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;

    anchor         = app->view_start + t * old_span;
    app->view_start = anchor - t * new_span;
    app->view_span  = new_span;
    gv_clamp_view(app);
}

static void gv_zoom_center(GViewApp *app, int direction)
{
    double old_span;
    double new_span;
    double total;
    double mid;

    if (!app->loaded) {
        return;
    }

    old_span = app->view_span;
    new_span = old_span * (direction > 0 ? 0.8 : 1.25);
    total    = (double)(app->trace.t1 - app->trace.t0);
    if (total < 1.0) {
        total = 1.0;
    }

    if (new_span < 1.0) new_span = 1.0;
    if (new_span > total) new_span = total;

    mid             = app->view_start + old_span * 0.5;
    app->view_start = mid - new_span * 0.5;
    app->view_span  = new_span;
    gv_clamp_view(app);
}

static void gv_pan(GViewApp *app, double delta)
{
    if (!app->loaded) {
        return;
    }
    app->view_start += delta;
    gv_clamp_view(app);
}

static void gv_timeline_geom(GViewApp *app, GCRECT view, GVTimeline *g)
{
    uint32_t i;

    memset(g, 0, sizeof(*g));
    g->view = view;
    g->label = (GCRECT){view.x, view.y + GV_RULER_H, app->label_w, view.h - GV_RULER_H};

    g->timeline = (GCRECT){view.x + app->label_w, view.y + GV_RULER_H,
                           view.w - app->label_w - 12.0f, view.h - GV_RULER_H};
    if (g->timeline.w < 1.0f) g->timeline.w = 1.0f;
    if (g->timeline.h < 1.0f) g->timeline.h = 1.0f;

    g->ruler     = (GCRECT){g->timeline.x, view.y, g->timeline.w, GV_RULER_H};
    g->scrollbar = (GCRECT){view.x + view.w - 10.0f, view.y + GV_RULER_H, 10.0f,
                            view.h - GV_RULER_H};

    for (i = 0; i < app->trace.thread_count; i++) {
        g->content_h += gv_thread_row_h(&app->trace.threads[i]);
    }
}

static int gv_hit_span(GViewApp *app, const GVTimeline *g)
{
    float    mx;
    float    my;
    float    y;
    double   px_per_us;
    uint32_t ti;
    int      best       = -1;
    uint32_t best_depth = 0;

    if (!app->loaded || !gc_rect_contains(g->timeline, app->ui->mouseX, app->ui->mouseY)) {
        return -1;
    }

    mx        = app->ui->mouseX;
    my        = app->ui->mouseY;
    y         = g->timeline.y - app->vscroll;
    px_per_us = (double)g->timeline.w / app->view_span;

    for (ti = 0; ti < app->trace.thread_count; ti++) {
        const GViewThread *thread = &app->trace.threads[ti];
        float              row_h  = gv_thread_row_h(thread);

        if (my >= y && my < y + row_h) {
            uint32_t si;
            for (si = 0; si < thread->span_count; si++) {
                const GViewSpan *span = &app->trace.spans[thread->span_idx[si]];
                double           sx;
                double           ex;
                float            lane_y;
                uint32_t         depth;

                depth = span->depth > GV_MAX_LANES ? GV_MAX_LANES : span->depth;
                sx = (double)g->timeline.x +
                     ((double)span->start_us - app->view_start) * px_per_us;
                if (sx > (double)g->timeline.x + (double)g->timeline.w) {
                    break;
                }
                ex = (double)g->timeline.x +
                     (((double)span->start_us + (double)span->dur_us) - app->view_start) *
                         px_per_us;
                if (ex < sx) {
                    ex = sx;
                }
                lane_y = y + GV_ROW_PAD + (float)depth * GV_LANE_H;

                if (mx >= (float)sx && mx <= (float)ex && my >= lane_y &&
                    my < lane_y + GV_BOX_H) {
                    if (best < 0 || depth >= best_depth) {
                        best       = (int)thread->span_idx[si];
                        best_depth = depth;
                    }
                }
            }
        }

        y += row_h;
    }

    return best;
}

static void gv_timeline_input(GViewApp *app, GVTimeline *g)
{
    GCUI *ui = app->ui;

    if (!app->loaded) {
        return;
    }

    if (ui->mouseScrollY != 0.0f && gc_rect_contains(g->view, ui->mouseX, ui->mouseY)) {
        int shift = gc_window_key_currently_pressed(app->window, GC_KEY_LSHIFT) ||
                    gc_window_key_currently_pressed(app->window, GC_KEY_RSHIFT);
        if (shift) {
            GCF32 offset = app->vscroll;
            gc_ui_handle_scrollwheel(ui, g->timeline, g->timeline.h, g->content_h, &offset,
                                     40.0f);
            app->vscroll = offset;
        } else {
            gv_zoom_at(app, g->timeline, ui->mouseX, ui->mouseScrollY > 0.0f ? 1 : -1);
        }
    }

    if (ui->mousePressed && gc_rect_contains(g->timeline, ui->mouseX, ui->mouseY)) {
        app->drag_active      = 1;
        app->drag_moved       = 0;
        app->drag_x           = ui->mouseX;
        app->drag_view_start  = app->view_start;
    }

    if (app->drag_active && ui->mouseDown) {
        float dx = ui->mouseX - app->drag_x;
        if (dx > 3.0f || dx < -3.0f) {
            app->drag_moved = 1;
        }
        if (app->drag_moved && g->timeline.w > 1.0f) {
            app->view_start = app->drag_view_start -
                              ((double)dx / (double)g->timeline.w) * app->view_span;
            gv_clamp_view(app);
        }
    }

    if (app->drag_active && ui->mouseReleased) {
        if (!app->drag_moved && gc_rect_contains(g->timeline, ui->mouseX, ui->mouseY)) {
            app->selected = gv_hit_span(app, g);
        }
        app->drag_active = 0;
    }

    if (gc_rect_contains(g->timeline, ui->mouseX, ui->mouseY)) {
        app->hovered = app->drag_active ? -1 : gv_hit_span(app, g);
    } else {
        app->hovered = -1;
    }
}

static double gv_nice_step(double raw)
{
    double base = 1.0;

    if (raw < 1.0) raw = 1.0;
    while (base * 10.0 < raw) {
        base *= 10.0;
    }

    if (base >= raw) return base;
    if (base * 2.0 >= raw) return base * 2.0;
    if (base * 5.0 >= raw) return base * 5.0;
    return base * 10.0;
}

static void gv_draw_ruler(GViewApp *app, GVTimeline *g)
{
    GCUI   *ui = app->ui;
    double  target;
    double  step;
    double  first;
    double  end;
    int     guard = 0;
    char    label[64];

    gc_draw_fill(ui->window, (GCRECT){g->view.x, g->view.y, g->view.w, GV_RULER_H}, C_PANEL);
    gc_ui_label(ui, (GCRECT){g->view.x + 8.0f, g->view.y + 5.0f, g->label.w - 16.0f,
                             GV_LINE_H},
                "Thread", C_DIM);
    gc_ui_label(ui, (GCRECT){g->ruler.x, g->ruler.y + 4.0f, g->ruler.w - 8.0f, GV_LINE_H},
                "ms", C_FAINT);

    target = app->view_span / ((double)g->timeline.w / 140.0);
    step   = gv_nice_step(target);
    first  = ((double)(uint64_t)(app->view_start / step)) * step;
    end    = app->view_start + app->view_span;

    while (first <= end && guard++ < 512) {
        double x = (double)g->ruler.x + ((first - app->view_start) / app->view_span) *
                                            (double)g->ruler.w;
        if (x >= (double)g->ruler.x - 1.0 && x <= (double)g->ruler.x + (double)g->ruler.w) {
            gc_draw_fill(ui->window, (GCRECT){(float)x, g->ruler.y + GV_RULER_H - 6.0f, 1.0f, 6.0f},
                         C_LINE);
            if (step >= 1000.0) {
                snprintf(label, sizeof label, "%.1f", first / 1000.0);
            } else if (step >= 10.0) {
                snprintf(label, sizeof label, "%.2f", first / 1000.0);
            } else {
                snprintf(label, sizeof label, "%.3f", first / 1000.0);
            }
            gc_ui_label(ui,
                        (GCRECT){(float)x + 3.0f, g->ruler.y + 4.0f, 96.0f, GV_LINE_H},
                        label, C_FAINT);
        }
        first += step;
    }

    gc_draw_fill(ui->window, (GCRECT){g->view.x, g->view.y + GV_RULER_H - 1.0f, g->view.w, 1.0f},
                 C_LINE);
    gc_draw_fill(ui->window,
                 (GCRECT){g->timeline.x - 1.0f, g->view.y, 1.0f, g->view.h}, C_LINE);
}

static void gv_draw_timeline(GViewApp *app, GCRECT view)
{
    GCUI       *ui = app->ui;
    GVTimeline  g;
    uint32_t    ti;
    double      px_per_us;
    float       y;
    GCCOLOR     text_dim = C_FAINT;

    gc_ui_panel(ui, view, C_BODY);

    if (!app->loaded) {
        //gc_text_centered(ui, "No trace loaded - press Ctrl+O to open one", view, C_DIM);
        return;
    }

    gv_timeline_geom(app, view, &g);
    px_per_us = (double)g.timeline.w / app->view_span;

    gc_text_clip_rect(ui, (GCRECT){view.x, view.y + GV_RULER_H, view.w,
                                   view.h - GV_RULER_H});

    y = g.timeline.y - app->vscroll;
    for (ti = 0; ti < app->trace.thread_count; ti++) {
        const GViewThread *thread = &app->trace.threads[ti];
        float              row_h  = gv_thread_row_h(thread);

        if (y + row_h >= g.timeline.y && y <= view.y + view.h) {
            GCCOLOR row_color = (ti & 1u) ? C_ROW_ALT : C_ROW;
            gc_draw_fill(ui->window, (GCRECT){view.x, y, view.w - 12.0f, row_h}, row_color);
            gc_ui_label(ui, (GCRECT){view.x + 8.0f, y + 6.0f, app->label_w - 16.0f,
                                     GV_LINE_H},
                        thread->label, C_TEXT);
        }

        y += row_h;
    }

    // Vertical grid lines over the row backgrounds but under the zones.
    {
        double target = app->view_span / ((double)g.timeline.w / 140.0);
        double step   = gv_nice_step(target);
        double first  = ((double)(uint64_t)(app->view_start / step)) * step;
        double end    = app->view_start + app->view_span;
        int    guard  = 0;

        while (first <= end && guard++ < 512) {
            double x = (double)g.timeline.x +
                       ((first - app->view_start) / app->view_span) * (double)g.timeline.w;
            if (x >= (double)g.timeline.x && x <= (double)g.timeline.x + (double)g.timeline.w) {
                gc_draw_fill(ui->window,
                             (GCRECT){(float)x, g.timeline.y, 1.0f, g.timeline.h}, C_GRID);
            }
            first += step;
        }
    }

    y = g.timeline.y - app->vscroll;
    for (ti = 0; ti < app->trace.thread_count; ti++) {
        const GViewThread *thread = &app->trace.threads[ti];
        float              row_h  = gv_thread_row_h(thread);
        uint32_t           si;

        if (y + row_h >= g.timeline.y && y <= view.y + view.h) {
            for (si = 0; si < thread->span_count; si++) {
                const GViewSpan *span = &app->trace.spans[thread->span_idx[si]];
                uint32_t         depth = span->depth > GV_MAX_LANES ? GV_MAX_LANES : span->depth;
                float            lane_y = y + GV_ROW_PAD + (float)depth * GV_LANE_H;
                double           sx;
                double           ex;
                float            x0;
                float            x1;
                float            width;
                int              dim = !gv_matches(app, span);
                GCCOLOR          fill;

                if (lane_y + GV_LANE_H < g.timeline.y || lane_y > view.y + view.h) {
                    continue;
                }

                sx = (double)g.timeline.x +
                     ((double)span->start_us - app->view_start) * px_per_us;
                if (sx > (double)g.timeline.x + (double)g.timeline.w) {
                    break;
                }
                ex = (double)g.timeline.x +
                     (((double)span->start_us + (double)span->dur_us) - app->view_start) *
                         px_per_us;
                if (ex < sx) ex = sx;

                if (ex < (double)g.timeline.x) {
                    continue;
                }

                x0 = (float)sx;
                x1 = (float)ex;
                if (x0 < g.timeline.x) x0 = g.timeline.x;
                if (x1 > g.timeline.x + g.timeline.w) x1 = g.timeline.x + g.timeline.w;
                width = x1 - x0;
                if (width < 1.0f) width = 1.0f;

                fill = gv_color_for(span->name_id, dim);
                gc_draw_fill(ui->window, (GCRECT){x0, lane_y, width, GV_BOX_H}, fill);
                gc_draw_outline(ui->window, (GCRECT){x0, lane_y, width, GV_BOX_H},
                                gv_shade(fill, 0.72f));

                if (app->selected == (int)thread->span_idx[si]) {
                    gc_draw_outline(ui->window,
                                    (GCRECT){x0, lane_y, width, GV_BOX_H}, C_SELECT);
                } else if (app->hovered == (int)thread->span_idx[si]) {
                    gc_draw_outline(ui->window,
                                    (GCRECT){x0, lane_y, width, GV_BOX_H}, C_TEXT);
                }

                if (width > 28.0f) {
                    gc_ui_label(ui,
                                (GCRECT){x0 + 3.0f, lane_y, width - 6.0f, GV_LINE_H},
                                gview_name(&app->trace, span->name_id),
                                dim ? text_dim : C_TEXT);
                }
            }
        }

        y += row_h;
    }

    gc_text_clip_clear(ui);
    gv_draw_ruler(app, &g);

    {
        GCF32 offset = app->vscroll;
        gc_ui_scrollbar_vertical(ui, g.scrollbar, g.timeline.h, g.content_h, &offset);
        app->vscroll = offset;
    }
}

static uint32_t gv_flame_max_depth(const GViewFlame *flame)
{
    uint32_t i;
    uint32_t max = 0;
    for (i = 0; i < flame->count; i++) {
        if (flame->nodes[i].depth > max) {
            max = flame->nodes[i].depth;
        }
    }
    return max;
}

static void gv_flame_draw_node(GViewApp *app, uint32_t id, float x, float y, float scale,
                               GCRECT view, uint32_t depth)
{
    GViewFlame     *flame = &app->flame;
    GViewFlameNode *node  = &flame->nodes[id];
    float           width = (float)((double)node->total_us * (double)scale);
    GCCOLOR         fill;
    int             dim;
    int             selected;

    if (depth > 128u || width < 0.5f) {
        return;
    }
    if (y > view.y + view.h) {
        return;
    }

    dim      = app->filter && app->filter[0] &&
               !gv__contains_ci(gview_name(&app->trace, node->name_id), app->filter);
    selected = app->selected >= 0 &&
               app->selected < (int)app->trace.span_count &&
               (int)app->trace.spans[app->selected].flame_id == (int)id;

    if (y + GV_BOX_H >= view.y && width >= 1.0f) {
        fill = gv_color_for(node->name_id, dim);
        gc_draw_fill(app->ui->window, (GCRECT){x, y, width, GV_BOX_H}, fill);
        gc_draw_outline(app->ui->window, (GCRECT){x, y, width, GV_BOX_H}, gv_shade(fill, 0.72f));

        if (selected) {
            gc_draw_outline(app->ui->window, (GCRECT){x, y, width, GV_BOX_H}, C_SELECT);
        } else if (app->hovered_flame == (int)id) {
            gc_draw_outline(app->ui->window, (GCRECT){x, y, width, GV_BOX_H}, C_TEXT);
        }

        if (width > 28.0f) {
            gc_ui_label(app->ui,
                        (GCRECT){x + 3.0f, y, width - 6.0f, GV_LINE_H},
                        gview_name(&app->trace, node->name_id), dim ? C_FAINT : C_TEXT);
        }
    }

    {
        uint32_t child = node->first_child;
        float    cx    = x;
        while (child != GV_SPAN_NONE) {
            GViewFlameNode *cn = &flame->nodes[child];
            float           cw = (float)((double)cn->total_us * (double)scale);
            gv_flame_draw_node(app, child, cx, y + GV_LANE_H, scale, view, depth + 1u);
            cx += cw;
            child = cn->next_sibling;
        }
    }
}

static int gv_flame_hit_node(GViewApp *app, uint32_t id, float x, float y, float scale,
                             float mx, float my, uint32_t depth)
{
    GViewFlame     *flame = &app->flame;
    GViewFlameNode *node  = &flame->nodes[id];
    float           width = (float)((double)node->total_us * (double)scale);
    int             best  = -1;

    if (depth > 128u || width < 0.5f) {
        return -1;
    }

    if (mx >= x && mx < x + width && my >= y && my < y + GV_BOX_H) {
        best = (int)id;
    }

    {
        uint32_t child = node->first_child;
        float    cx    = x;
        while (child != GV_SPAN_NONE) {
            GViewFlameNode *cn = &flame->nodes[child];
            float           cw = (float)((double)cn->total_us * (double)scale);
            int hit = gv_flame_hit_node(app, child, cx, y + GV_LANE_H, scale, mx, my, depth + 1u);
            if (hit >= 0) {
                best = hit;
            }
            cx += cw;
            child = cn->next_sibling;
        }
    }

    return best;
}

static int gv_flame_hit(GViewApp *app, GCRECT view)
{
    float    scale;
    float    x;
    uint32_t child;
    int      best = -1;

    if (!app->loaded || app->flame.count <= 1u || app->flame.root_total == 0u) {
        return -1;
    }

    scale = (view.w - 16.0f) / (float)app->flame.root_total;
    x     = view.x + 8.0f;
    child = app->flame.nodes[0].first_child;

    while (child != GV_SPAN_NONE) {
        GViewFlameNode *cn = &app->flame.nodes[child];
        float           cw = (float)((double)cn->total_us * (double)scale);
        int hit = gv_flame_hit_node(app, child, x, view.y + 8.0f - app->flame_scroll, scale,
                                    app->ui->mouseX, app->ui->mouseY, 1u);
        if (hit >= 0) {
            best = hit;
        }
        x += cw;
        child = cn->next_sibling;
    }

    return best;
}

static void gv_flame_input(GViewApp *app, GCRECT view)
{
    GCUI *ui = app->ui;
    float content_h;

    if (!app->loaded) {
        return;
    }

    content_h = 16.0f + (float)(gv_flame_max_depth(&app->flame) + 1u) * GV_LANE_H;

    if (ui->mouseScrollY != 0.0f && gc_rect_contains(view, ui->mouseX, ui->mouseY)) {
        GCF32 offset = app->flame_scroll;
        gc_ui_handle_scrollwheel(ui, view, view.h, content_h, &offset, 60.0f);
        app->flame_scroll = offset;
    }

    if (ui->mouseReleased && gc_rect_contains(view, ui->mouseX, ui->mouseY)) {
        int node = gv_flame_hit(app, view);
        if (node >= 0) {
            app->selected = (int)app->flame.nodes[node].span_id;
        } else {
            app->selected = -1;
        }
    }

    if (gc_rect_contains(view, ui->mouseX, ui->mouseY)) {
        app->hovered_flame = gv_flame_hit(app, view);
    } else {
        app->hovered_flame = -1;
    }
}

static void gv_draw_flame(GViewApp *app, GCRECT view)
{
    GCUI    *ui = app->ui;
    float    content_h;
    float    scale;
    float    x;
    uint32_t child;

    gc_ui_panel(ui, view, C_BODY);

    if (!app->loaded) {
        gc_text_centered(ui, "No trace loaded - press Ctrl+O to open one", view, C_DIM);
        return;
    }
    if (app->flame.count <= 1u || app->flame.root_total == 0u) {
        gc_text_centered(ui, "No zones to show", view, C_DIM);
        return;
    }

    content_h = 16.0f + (float)(gv_flame_max_depth(&app->flame) + 1u) * GV_LANE_H;

    gc_text_clip_rect(ui, view);

    scale = (view.w - 16.0f) / (float)app->flame.root_total;
    x     = view.x + 8.0f;
    child = app->flame.nodes[0].first_child;

    while (child != GV_SPAN_NONE) {
        GViewFlameNode *cn = &app->flame.nodes[child];
        float           cw = (float)((double)cn->total_us * (double)scale);
        gv_flame_draw_node(app, child, x, view.y + 8.0f - app->flame_scroll, scale, view, 1u);
        x += cw;
        child = cn->next_sibling;
    }

    gc_text_clip_clear(ui);

    {
        GCRECT bar = {view.x + view.w - 10.0f, view.y, 10.0f, view.h};
        GCF32  offset = app->flame_scroll;
        gc_ui_scrollbar_vertical(ui, bar, view.h, content_h, &offset);
        app->flame_scroll = offset;
    }
}

static void gv_detail_line(GViewApp *app, GCRECT panel, float *y, const char *label,
                           const char *value, GCCOLOR color)
{
    gc_ui_label(app->ui, (GCRECT){panel.x, *y, 104.0f, GV_LINE_H}, label, C_DIM);
    gc_ui_label(app->ui, (GCRECT){panel.x + 108.0f, *y, panel.w - 120.0f, GV_LINE_H}, value,
                color);
    *y += GV_LINE_H + 2.0f;
}

static void gv_draw_details(GViewApp *app, GCRECT panel)
{
    GCUI  *ui = app->ui;
    GCRECT inner;
    char   buf[512];
    float  y;

    if (panel.w <= 1.0f || panel.h <= 1.0f) {
        return;
    }

    gc_ui_panel(ui, panel, C_PANEL);
    inner = (GCRECT){panel.x + 14.0f, panel.y + 12.0f, panel.w - 28.0f, panel.h - 24.0f};

    gc_ui_label(ui, (GCRECT){inner.x, inner.y, inner.w, GV_LINE_H},
                app->selected >= 0 ? "Selected zone" : "Trace", C_TEXT);
    y = inner.y + GV_LINE_H + 8.0f;

    gc_text_clip_rect(ui, inner);

    if (!app->loaded) {
        gc_ui_label(ui, (GCRECT){inner.x, y, inner.w, GV_LINE_H}, "No trace loaded.", C_DIM);
        gc_text_clip_clear(ui);
        return;
    }

    if (app->selected >= 0 && app->selected < (int)app->trace.span_count) {
        const GViewSpan *s = &app->trace.spans[app->selected];
        uint32_t         i;
        uint32_t         shown = 0;

        gv_detail_line(app, inner, &y, "name", gview_name(&app->trace, s->name_id), C_TEXT);

        snprintf(buf, sizeof buf, "%s", gv_thread_label(app, s->pid, s->tid));
        gv_detail_line(app, inner, &y, "thread", buf, C_TEXT);

        snprintf(buf, sizeof buf, "%.3f ms",
                 ((double)s->start_us - (double)app->trace.t0) / 1000.0);
        gv_detail_line(app, inner, &y, "start", buf, C_TEXT);

        snprintf(buf, sizeof buf, "%.3f ms", (double)s->dur_us / 1000.0);
        gv_detail_line(app, inner, &y, "duration", buf, C_TEXT);

        snprintf(buf, sizeof buf, "%.3f ms", (double)s->self_us / 1000.0);
        gv_detail_line(app, inner, &y, "self", buf, C_TEXT);

        snprintf(buf, sizeof buf, "%u", s->depth);
        gv_detail_line(app, inner, &y, "depth", buf, C_TEXT);

        snprintf(buf, sizeof buf, "%u / %u", s->pid, s->tid);
        gv_detail_line(app, inner, &y, "pid / tid", buf, C_TEXT);

        snprintf(buf, sizeof buf, "%u", s->child_count);
        gv_detail_line(app, inner, &y, "children", buf, C_TEXT);

        gv_detail_line(app, inner, &y, "complete", s->complete ? "yes" : "no",
                       s->complete ? C_TEXT : C_WARN);

        if (s->parent != GV_SPAN_NONE && s->parent < app->trace.span_count) {
            gv_detail_line(app, inner, &y, "parent",
                           gview_name(&app->trace, app->trace.spans[s->parent].name_id),
                           C_DIM);
        } else {
            gv_detail_line(app, inner, &y, "parent", "-", C_DIM);
        }

        if (s->child_count > 0 && y + 40.0f < inner.y + inner.h) {
            y += 6.0f;
            gc_ui_label(ui, (GCRECT){inner.x, y, inner.w, GV_LINE_H}, "child zones", C_DIM);
            y += GV_LINE_H + 2.0f;

            for (i = 0; i < app->trace.span_count && shown < 8u; i++) {
                const GViewSpan *c = &app->trace.spans[i];
                if (c->parent != (uint32_t)app->selected) {
                    continue;
                }
                snprintf(buf, sizeof buf, "%s  %.3f ms",
                         gview_name(&app->trace, c->name_id), (double)c->dur_us / 1000.0);
                gc_ui_label(ui, (GCRECT){inner.x + 8.0f, y, inner.w - 8.0f, GV_LINE_H}, buf,
                            C_DIM);
                y += GV_LINE_H + 2.0f;
                shown++;
                if (y > inner.y + inner.h - GV_LINE_H) {
                    break;
                }
            }
            if (shown == 8u) {
                gc_ui_label(ui, (GCRECT){inner.x + 8.0f, y, inner.w - 8.0f, GV_LINE_H}, "...",
                            C_FAINT);
            }
        }
    } else {
        gc_ui_label(ui, (GCRECT){inner.x, y, inner.w, GV_LINE_H}, "Click a zone to inspect it.",
                    C_DIM);
        y += 28.0f;

        snprintf(buf, sizeof buf, "%u", app->trace.span_count);
        gv_detail_line(app, inner, &y, "spans", buf, C_TEXT);

        snprintf(buf, sizeof buf, "%u", app->trace.thread_count);
        gv_detail_line(app, inner, &y, "threads", buf, C_TEXT);

        snprintf(buf, sizeof buf, "%u", app->trace.name_count);
        gv_detail_line(app, inner, &y, "names", buf, C_TEXT);

        snprintf(buf, sizeof buf, "%.3f ms",
                 (double)(app->trace.t1 - app->trace.t0) / 1000.0);
        gv_detail_line(app, inner, &y, "time span", buf, C_TEXT);

        if (app->trace.bad_events || app->trace.unmatched_ends || app->trace.open_at_end) {
            snprintf(buf, sizeof buf, "%u / %u / %u", app->trace.bad_events,
                     app->trace.unmatched_ends, app->trace.open_at_end);
            gv_detail_line(app, inner, &y, "warnings", buf, C_WARN);
        }

        y += 8.0f;
        gc_ui_label(ui, (GCRECT){inner.x, y, inner.w, GV_LINE_H}, "Controls", C_DIM);
        y += GV_LINE_H + 2.0f;
        gc_ui_label(ui, (GCRECT){inner.x, y, inner.w, GV_LINE_H}, "Wheel: zoom", C_FAINT);
        y += GV_LINE_H + 2.0f;
        gc_ui_label(ui, (GCRECT){inner.x, y, inner.w, GV_LINE_H}, "Shift+wheel: scroll", C_FAINT);
        y += GV_LINE_H + 2.0f;
        gc_ui_label(ui, (GCRECT){inner.x, y, inner.w, GV_LINE_H}, "Drag: pan", C_FAINT);
        y += GV_LINE_H + 2.0f;
        gc_ui_label(ui, (GCRECT){inner.x, y, inner.w, GV_LINE_H}, "F: fit  Tab: view", C_FAINT);
    }

    gc_text_clip_clear(ui);
}

static void gv_draw_toolbar(GViewApp *app, GCRECT panel)
{
    GCUI  *ui = app->ui;
    float  x  = panel.x;
    float  y  = panel.y;
    float  h  = panel.h;
    float  fw = 220.0f;
    float  fx = panel.x + panel.w - fw;

    gc_ui_panel(ui, panel, C_PANEL);

    if (gc_ui_button(ui, (GCRECT){x, y, 76.0f, h}, "Open")) {
        gv_open_dialog(app);
    }
    x += 84.0f;

    if (gc_ui_button(ui, (GCRECT){x, y, 82.0f, h}, "Reload")) {
        gv_reload(app);
    }
    x += 90.0f;

    if (gc_ui_button(ui, (GCRECT){x, y, 54.0f, h}, "Fit")) {
        gv_fit(app);
    }
    x += 62.0f;

    if (gc_ui_button(ui, (GCRECT){x, y, 122.0f, h},
                     app->view_mode == 0 ? "Flame graph" : "Timeline")) {
        app->view_mode = !app->view_mode;
    }
    x += 130.0f;

    gc_ui_label(ui, (GCRECT){fx - 54.0f, y, 50.0f, h}, "Filter", C_DIM);
    gc_ui_textbox(ui, (GCRECT){fx, y, fw, h}, &app->filter, &app->filter_cap, app->arena);
}

static void gv_draw_footer(GViewApp *app, GCRECT panel)
{
    GCCOLOR color = C_DIM;
    char    right[256];

    if (app->status[0] && (strncmp(app->status, "error", 5) == 0)) {
        color = C_ERR;
    }

    gc_ui_label(app->ui, (GCRECT){panel.x, panel.y, panel.w * 0.56f, panel.h},
                app->status, color);

    if (app->loaded) {
        snprintf(right, sizeof right,
                 "spans %u   threads %u   wheel zoom   shift+wheel scroll   drag pan   F fit   Tab view",
                 app->trace.span_count, app->trace.thread_count);
    } else {
        snprintf(right, sizeof right, "Ctrl+O open   F fit   Tab timeline/flame");
    }

    if (app->trace.bad_events || app->trace.unmatched_ends || app->trace.open_at_end) {
        snprintf(right, sizeof right, "warn: %u bad, %u unmatched end, %u open", 
                 app->trace.bad_events, app->trace.unmatched_ends, app->trace.open_at_end);
        color = C_WARN;
    }

    gc_ui_label(app->ui, (GCRECT){panel.x + panel.w * 0.56f, panel.y, panel.w * 0.44f,
                                  panel.h},
                right, color);
}

static void gv_draw_tooltip(GViewApp *app)
{
    GCUI  *ui = app->ui;
    char   text[512];
    float  tw;
    float  x;
    float  y;

    if (app->view_mode == 0 && app->hovered >= 0 &&
        app->hovered < (int)app->trace.span_count) {
        const GViewSpan *s = &app->trace.spans[app->hovered];
        snprintf(text, sizeof text, "%s   %.3f ms   thread %s",
                 gview_name(&app->trace, s->name_id), (double)s->dur_us / 1000.0,
                 gv_thread_label(app, s->pid, s->tid));
    } else if (app->view_mode == 1 && app->hovered_flame >= 0 &&
               app->hovered_flame < (int)app->flame.count) {
        const GViewFlameNode *n = &app->flame.nodes[app->hovered_flame];
        snprintf(text, sizeof text, "%s   %.3f ms aggregated",
                 gview_name(&app->trace, n->name_id), (double)n->total_us / 1000.0);
    } else {
        return;
    }

    tw = (float)gc_text_width(ui, text, strlen(text)) + 18.0f;
    x  = ui->mouseX + 14.0f;
    y  = ui->mouseY + 18.0f;

    if (x + tw > gc_window_width(app->window) - 8.0f) {
        x = gc_window_width(app->window) - tw - 8.0f;
    }
    if (y + 30.0f > gc_window_height(app->window) - 8.0f) {
        y = ui->mouseY - 36.0f;
    }
    if (x < 8.0f) x = 8.0f;
    if (y < 8.0f) y = 8.0f;

    gc_draw_fill(ui->window, (GCRECT){x, y, tw, 30.0f}, C_PANEL);
    gc_draw_outline(ui->window, (GCRECT){x, y, tw, 30.0f}, C_LINE);
    gc_ui_label(ui, (GCRECT){x + 8.0f, y + 4.0f, tw - 16.0f, GV_LINE_H}, text, C_TEXT);
}

static void gv_shortcuts(GViewApp *app)
{
    GCWINDOW *w    = app->window;
    GCUI     *ui   = app->ui;
    int       ctrl = gc_window_key_currently_pressed(w, GC_KEY_LCTRL) ||
                     gc_window_key_currently_pressed(w, GC_KEY_RCTRL);

    if (ctrl && gc_window_key_was_pressed(w, GC_KEY_O)) {
        gv_open_dialog(app);
        return;
    }

    if (ui->focus != 0) {
        return;
    }

    if (gc_window_key_was_pressed(w, GC_KEY_R)) {
        gv_reload(app);
    }
    if (gc_window_key_was_pressed(w, GC_KEY_F) ||
        gc_window_key_was_pressed(w, GC_KEY_HOME)) {
        gv_fit(app);
    }
    if (gc_window_key_was_pressed(w, GC_KEY_TAB)) {
        app->view_mode = !app->view_mode;
    }
    if (gc_window_key_was_pressed(w, GC_KEY_1)) {
        app->view_mode = 0;
    }
    if (gc_window_key_was_pressed(w, GC_KEY_2)) {
        app->view_mode = 1;
    }
    if (gc_window_key_was_pressed(w, GC_KEY_ESCAPE)) {
        app->selected = -1;
    }
    if (gc_window_key_was_pressed(w, GC_KEY_PAGEUP)) {
        gv_zoom_center(app, 1);
    }
    if (gc_window_key_was_pressed(w, GC_KEY_PAGEDOWN)) {
        gv_zoom_center(app, -1);
    }
    if (app->view_mode == 0) {
        if (gc_window_key_was_pressed(w, GC_KEY_LEFT)) {
            gv_pan(app, -app->view_span * 0.1);
        }
        if (gc_window_key_was_pressed(w, GC_KEY_RIGHT)) {
            gv_pan(app, app->view_span * 0.1);
        }
    }
}

static void gv_frame(GViewApp *app)
{
    GVLayout layout;

    if (app->has_pending) {
        app->has_pending = 0;
        gv_load(app, app->pending_path);
    }

    gv_poll_reload(app);
    gv_layout(app, &layout);
    gv_splitter_input(app, &layout);
    gv_shortcuts(app);

    gv_draw_toolbar(app, layout.toolbar);

    if (app->view_mode == 0) {
        GVTimeline g;
        gv_timeline_geom(app, layout.main, &g);
        if (app->split_drag == 0) {
            gv_timeline_input(app, &g);
        }
        gv_draw_timeline(app, layout.main);
    } else {
        if (app->split_drag == 0) {
            gv_flame_input(app, layout.main);
        }
        gv_draw_flame(app, layout.main);
    }

    gv_draw_details(app, layout.details);
    gv_draw_footer(app, layout.footer);
    gv_draw_splitters(app, &layout);
    gv_draw_tooltip(app);
}


int app_entry(int argc, char **argv)
{
    GViewApp app;

    if (argc >= 2 && strcmp(argv[1], "--summary") == 0) {
        GViewTrace trace;
        char       err[256];
        const char *path = argc >= 3 ? argv[2] : NULL;

        if (!path) {
            printf("usage: gview --summary <trace.json>\n");
            return 2;
        }

        gview_trace_init(&trace);
        if (!gview_trace_load_file(&trace, path, err, sizeof err)) {
            printf("gview: %s\n", err);
            gview_trace_free(&trace);
            return 1;
        }

        gview_trace_print_summary(&trace, path);
        gview_trace_free(&trace);
        return 0;
    }

    if (argc >= 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        printf("usage: gview [trace.json]\n");
        printf("       gview --summary <trace.json>\n");
        return 0;
    }

    memset(&app, 0, sizeof(app));
    gview_trace_init(&app.trace);
    gview_flame_init(&app.flame);
    app.selected      = -1;
    app.hovered       = -1;
    app.hovered_flame = -1;
    app.view_mode     = 0;
    app.details_w     = GV_DETAIL_W;
    app.label_w       = GV_LABEL_W;
    app.split_drag    = 0;
    app.filter_cap    = 256;

    app.arena = gc_arena_create(GC_MB(8));
    if (!app.arena) {
        printf("gview: out of memory\n");
        return 1;
    }

    app.window = gc_window_create(app.arena, "gview", 1440, 900, GC_FALSE);
    if (!app.window) {
        printf("gview: could not create window\n");
        gc_arena_destroy(app.arena);
        return 1;
    }
    gc_window_set_clear(app.window, C_BG);

    app.ui = gc_ui_create(app.arena, app.window);
    gc_ui_load_default_font(app.ui, 15.0f);

    app.filter = (char *)gc_arena_push(app.arena, app.filter_cap);
    memset(app.filter, 0, app.filter_cap);

    if (argc >= 2 && argv[1][0] != '-') {
        gv_load(&app, argv[1]);
    }

    while (!gc_window_should_quit(app.window)) {
        gc_window_begin(app.window);
        gc_ui_begin(app.ui);

        gv_frame(&app);

        gc_ui_end(app.ui);
        gc_window_end(app.window);
    }

    gview_flame_free(&app.flame);
    gview_trace_free(&app.trace);

    gc_ui_destroy(app.ui);
    gc_window_destroy(app.window);
    gc_arena_destroy(app.arena);

    return 0;
}


#ifdef _WIN32
#include <windows.h>

int WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd)
{
    (void)hInstance;
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nShowCmd;

    int ret = app_entry(__argc, __argv);
    return ret;
}
#else
int main(int argc, char **argv)
{
    return app_entry(argc, argv);
}
#endif
