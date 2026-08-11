/*
 * scene_app.c — native app client library implementation.
 */
#include "scene_app.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Node ID allocation starts here (above shell IDs at 10000..39999). */
#define APP_ID_BASE 40000u

/* Per-window bookkeeping. */
#define MAX_WINDOWS 64

typedef struct app_window {
    scene_node_id window_id;
    scene_node_id titlebar_id;
    scene_node_id title_label_id;
    scene_node_id close_btn_id;
    scene_node_id content_id;
    int32_t       x, y;
    char          title[64];
} app_window;

struct scene_app {
    scene_client   *cl;
    scene_transport *t;
    const scene_app_cbs *cbs;
    void           *ud;
    uint32_t        next_id;       /* next node ID to allocate */
    app_window      wins[MAX_WINDOWS];
    uint32_t        win_count;
};

/* ---- transport callbacks (private) ------------------------------------ */

static void on_welcome(void *ud, uint32_t scene_id, uint16_t ver,
                       const scene_limits *lim)
{
    (void)ud; (void)scene_id; (void)ver; (void)lim;
}

static void on_error(void *ud, uint16_t code, const char *msg, uint32_t len)
{
    (void)ud; (void)code; (void)msg; (void)len;
}

static void on_pong(void *ud, uint64_t nonce)
{
    (void)ud; (void)nonce;
}

static void on_input_pointer(void *ud, uint64_t seq, uint8_t dev,
                             int32_t x, int32_t y, uint8_t btns)
{
    scene_app *app = (scene_app *)ud;
    (void)dev;
    if (app->cbs && app->cbs->pointer)
        app->cbs->pointer(app->ud, seq, x, y, btns);
}

static void on_input_activate(void *ud, uint64_t seq, scene_node_id id)
{
    scene_app *app = (scene_app *)ud;
    if (app->cbs && app->cbs->activate)
        app->cbs->activate(app->ud, seq, id);
}

static void on_input_focus(void *ud, uint64_t seq, scene_node_id id,
                           uint8_t state)
{
    scene_app *app = (scene_app *)ud;
    if (app->cbs && app->cbs->focus)
        app->cbs->focus(app->ud, seq, id, state);
}

static void on_input_key(void *ud, uint64_t seq, uint32_t key_code,
                         uint8_t state, uint8_t modifiers)
{
    scene_app *app = (scene_app *)ud;
    if (app->cbs && app->cbs->key)
        app->cbs->key(app->ud, seq, key_code, state, modifiers);
}

static void on_present_done(void *ud, uint64_t seq, uint64_t token,
                            uint64_t lat)
{
    (void)ud; (void)seq; (void)token; (void)lat;
}

static void on_text_index(void *ud, const scene_text_hit *hits, uint32_t n)
{
    (void)ud; (void)hits; (void)n;
}

static void on_closed(void *ud)
{
    (void)ud;
}

static const scene_client_cbs app_cbs = {
    on_welcome, on_error, NULL, NULL, NULL,
    on_pong, on_input_pointer, on_input_activate, on_input_focus,
    on_input_key, on_present_done, on_text_index, on_closed
};

/* ---- lifecycle -------------------------------------------------------- */

scene_app *scene_app_new_on(scene_transport *t, const char *target,
                            const scene_app_cbs *cbs, void *ud)
{
    scene_app *app = calloc(1, sizeof(*app));
    if (!app) return NULL;
    app->t = t;
    app->cl = scene_client_new();
    app->cbs = cbs;
    app->ud = ud;
    app->next_id = APP_ID_BASE;
    if (scene_client_connect(app->cl, t, target, &app_cbs, app) != 0) {
        scene_client_free(app->cl);
        free(app);
        return NULL;
    }
    return app;
}

scene_app *scene_app_new(scene_transport *t,
                         const scene_app_cbs *cbs, void *ud)
{
    return scene_app_new_on(t, "local", cbs, ud);
}

void scene_app_free(scene_app *app)
{
    if (!app) return;
    scene_client_free(app->cl);
    free(app);
}

/* ---- window management ------------------------------------------------ */

scene_node_id scene_app_create_window(scene_app *app,
                                      int32_t x, int32_t y,
                                      int32_t w, int32_t h,
                                      const char *title)
{
    if (!app || app->win_count >= MAX_WINDOWS) return SCENE_NO_PARENT;

    uint32_t base = app->next_id;
    scene_node_id win_id    = base + 0;
    scene_node_id tb_id     = base + 1;
    scene_node_id label_id  = base + 2;
    scene_node_id close_id  = base + 3;
    scene_node_id content_id = base + 4;
    app->next_id = base + 5;

    uint8_t vis = SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE;
    int32_t tb_h = 32;

    /* All coords are absolute screen positions. Parent is for hierarchy only. */

    /* WINDOW: outer container */
    scene_client_create_node(app->cl, SCENE_NO_PARENT, win_id,
                             SCENE_ROLE_WINDOW,
         &(scene_rect){x, y, w, h}, vis);

    /* TITLEBAR: top strip */
    scene_client_create_node(app->cl, win_id, tb_id,
                             SCENE_ROLE_TITLEBAR,
         &(scene_rect){x, y, w, tb_h}, vis);

    /* TITLE_LABEL: text inside titlebar */
    scene_client_create_node(app->cl, tb_id, label_id,
                             SCENE_ROLE_LABEL,
         &(scene_rect){x + 4, y + 4, w - 40, tb_h - 8}, vis);
    if (title && title[0])
        scene_client_set_text(app->cl, label_id, 0, title, (uint32_t)strlen(title));

    /* CLOSE_BUTTON: "X" at right of titlebar */
    scene_client_create_node(app->cl, tb_id, close_id,
                             SCENE_ROLE_BUTTON,
         &(scene_rect){x + w - 28, y + 4, 24, 24}, vis);
    scene_client_set_text(app->cl, close_id, 0, "X", 1);

    /* CONTENT: app draws here */
    scene_client_create_node(app->cl, win_id, content_id,
                             SCENE_ROLE_GENERIC,
         &(scene_rect){x, y + tb_h, w, h - tb_h}, vis);

    /* Track the window */
    app_window *win = &app->wins[app->win_count++];
    win->window_id    = win_id;
    win->titlebar_id  = tb_id;
    win->title_label_id = label_id;
    win->close_btn_id = close_id;
    win->content_id   = content_id;
    win->x = x;
    win->y = y;
    if (title) snprintf(win->title, sizeof(win->title), "%s", title);

    return content_id;
}

int scene_app_destroy_window(scene_app *app, scene_node_id content_id)
{
    if (!app) return -1;

    /* Find the window */
    uint32_t i;
    for (i = 0; i < app->win_count; i++) {
        if (app->wins[i].content_id == content_id) {
            scene_node_id win_id = app->wins[i].window_id;
            scene_client_destroy_node(app->cl, win_id);
            /* Compact the array */
            memmove(&app->wins[i], &app->wins[i + 1],
                    (app->win_count - i - 1) * sizeof(app_window));
            app->win_count--;
            return 0;
        }
    }
    return -1;
}

scene_node_id scene_app_content_to_window(scene_app *app,
                                          scene_node_id content_id)
{
    if (!app) return SCENE_NO_PARENT;
    uint32_t i;
    for (i = 0; i < app->win_count; i++) {
        if (app->wins[i].content_id == content_id)
            return app->wins[i].window_id;
    }
    return SCENE_NO_PARENT;
}

static app_window *find_window(scene_app *app, scene_node_id content_id)
{
    uint32_t i;
    for (i = 0; i < app->win_count; i++) {
        if (app->wins[i].content_id == content_id)
            return &app->wins[i];
    }
    return NULL;
}

int scene_app_set_title(scene_app *app, scene_node_id content_id,
                        const char *title)
{
    if (!app || !title) return -1;
    app_window *w = find_window(app, content_id);
    if (!w) return -1;
    snprintf(w->title, sizeof(w->title), "%s", title);
    return scene_client_set_text(app->cl, w->title_label_id, 0,
                                 title, (uint32_t)strlen(title));
}

int scene_app_resize_window(scene_app *app, scene_node_id content_id,
                            int32_t w, int32_t h)
{
    if (!app) return -1;
    app_window *win = find_window(app, content_id);
    if (!win) return -1;
    int32_t tb_h = 32;
    int32_t wx = win->x, wy = win->y;
    /* Update WINDOW (preserves position) */
    scene_rect wr = {wx, wy, w, h};
    scene_client_set_rect(app->cl, win->window_id, &wr);
    /* Update TITLEBAR */
    scene_rect tbr = {wx, wy, w, tb_h};
    scene_client_set_rect(app->cl, win->titlebar_id, &tbr);
    /* Update TITLE_LABEL */
    scene_rect lr = {wx + 4, wy + 4, w - 40, tb_h - 8};
    scene_client_set_rect(app->cl, win->title_label_id, &lr);
    /* Update CLOSE_BUTTON */
    scene_rect cr = {wx + w - 28, wy + 4, 24, 24};
    scene_client_set_rect(app->cl, win->close_btn_id, &cr);
    /* Update CONTENT */
    scene_rect cor = {wx, wy + tb_h, w, h - tb_h};
    scene_client_set_rect(app->cl, win->content_id, &cor);
    return 0;
}

int scene_app_minimize(scene_app *app, scene_node_id content_id)
{
    if (!app) return -1;
    app_window *win = find_window(app, content_id);
    if (!win) return -1;
    return scene_client_set_flags(app->cl, win->window_id, 0);
}

int scene_app_maximize(scene_app *app, scene_node_id content_id,
                       int32_t screen_w, int32_t screen_h, int32_t panel_h)
{
    if (!app) return -1;
    app_window *win = find_window(app, content_id);
    if (!win) return -1;
    int32_t tb_h = 32;
    int32_t win_w = screen_w;
    int32_t win_h = screen_h - panel_h;
    /* Move + resize WINDOW */
    scene_rect wr = {0, 0, win_w, win_h};
    scene_client_set_rect(app->cl, win->window_id, &wr);
    win->x = 0;
    win->y = 0;
    /* Update TITLEBAR */
    scene_rect tbr = {0, 0, win_w, tb_h};
    scene_client_set_rect(app->cl, win->titlebar_id, &tbr);
    /* Update TITLE_LABEL */
    scene_rect lr = {4, 4, win_w - 40, tb_h - 8};
    scene_client_set_rect(app->cl, win->title_label_id, &lr);
    /* Update CLOSE_BUTTON */
    scene_rect cr = {win_w - 28, 4, 24, 24};
    scene_client_set_rect(app->cl, win->close_btn_id, &cr);
    /* Update CONTENT */
    scene_rect cor = {0, tb_h, win_w, win_h - tb_h};
    scene_client_set_rect(app->cl, win->content_id, &cor);
    /* Make visible */
    return scene_client_set_flags(app->cl, win->window_id,
                                  SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE);
}

/* ---- node operations -------------------------------------------------- */

int scene_app_set_text(scene_app *app, scene_node_id id,
                       scene_text_id slot, const char *text)
{
    if (!app || !text) return -1;
    return scene_client_set_text(app->cl, id, slot, text, (uint32_t)strlen(text));
}

int scene_app_set_rect(scene_app *app, scene_node_id id,
                       int32_t x, int32_t y, int32_t w, int32_t h)
{
    if (!app) return -1;
    scene_rect r = {x, y, w, h};
    return scene_client_set_rect(app->cl, id, &r);
}

int scene_app_set_flags(scene_app *app, scene_node_id id, uint8_t flags)
{
    if (!app) return -1;
    return scene_client_set_flags(app->cl, id, flags);
}

/* ---- frame flow ------------------------------------------------------- */

int scene_app_present(scene_app *app)
{
    if (!app) return -1;
    return scene_client_present(app->cl, 0);
}

int scene_app_ack(scene_app *app, uint64_t seq)
{
    if (!app) return -1;
    return scene_client_ack(app->cl, seq);
}

int scene_app_pump(scene_app *app)
{
    if (!app) return -1;
    return scene_client_pump(app->cl);
}

int scene_app_flush(scene_app *app)
{
    if (!app) return -1;
    return scene_client_flush(app->cl);
}

/* ---- accessors -------------------------------------------------------- */

scene_client *scene_app_client(scene_app *app)
{
    return app ? app->cl : NULL;
}

uint32_t scene_app_id(const scene_app *app)
{
    return app ? app->next_id : 0;
}
