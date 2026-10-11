/* scene_browser.c — host module bridging a legacy Wayland client into
 * the compositor (see scene_browser.h for the design).
 *
 * One frame of work here:
 *   1. service the wlshim (POSIX: the wayland socket; everywhere: drain
 *      committed frames through the frame callback into a host staging
 *      buffer owned by this module),
 *   2. import the newest committed frame into the compositor's texture
 *      registry for our layer (registering the ref the wire SET_TEXTURE
 *      references, sizing both window nodes to the frame),
 *   3. drive the loopback link between the scene_client and the
 *      scene_server that hosts the browser's session,
 *   4. dispose of dead sessions (server protocol death, client death,
 *      or wlshim protocol death) by removing the layer — the desktop
 *      repaints in its place.
 */
#include "scene_browser.h"

#include "scene_compositor.h"
#include "scene_client.h"
#include "scene_server.h"
#include "scene_transport.h"

#include <stdlib.h>
#include <string.h>

/* Session-private identifiers (see the header). */
#define BR_WIN      60000u
#define BR_CONTENT  60001u
#define BR_TEX_REF  1u

struct scene_browser {
    scene_compositor *cp;
    scene_client     *cl;
    scene_server     *sv;
    wlshim           *w;
    scene_loopback   *lb;      /* loopback the client/server share      */
    scene_transport  *ts;      /* server end of the loopback            */
    scene_browser_config cfg;

    uint32_t         *stage;   /* host copy of the newest frame         */
    size_t            stage_cap;
    uint32_t          fw, fh;  /* last committed frame size             */
    int               have_frame;
    uint32_t          cw, ch;  /* node rect dims last set               */
    uint32_t          last_btn;
    int               layer;   /* compositor layer index (>= 1)         */
    int               welcomed;
    int               dead;
};

/* ---- lifecycle ------------------------------------------------------- */

static void browser_kill(scene_browser *b)
{
    if (b->dead) return;
    b->dead = 1;
    if (b->sv) {
        scene_compositor_remove_session(b->cp, b->sv);
        b->sv = NULL;          /* remove_session frees the server       */
    }
}

/* ---- wlshim callbacks ------------------------------------------------ */

static void frame_cb(void *ud, const wlshim_frame *f)
{
    scene_browser *b = (scene_browser *)ud;
    size_t n = (size_t)f->w * f->h;
    uint32_t *st;

    if (n > b->stage_cap) {
        size_t cap = n > 64 ? n : 64;
        st = (uint32_t *)realloc(b->stage, cap * sizeof(uint32_t));
        if (!st) return;              /* keep the previous frame */
        b->stage = st;
        b->stage_cap = cap;
    }
    memcpy(b->stage, f->px, n * sizeof(uint32_t));
    b->fw = f->w;
    b->fh = f->h;
    b->have_frame = 1;
}

static void died_cb(void *ud)
{
    scene_browser *b = (scene_browser *)ud;
    if (!b->dead) browser_kill(b);
}

/* ---- input translation ----------------------------------------------- */

/* Map an evdev key code (+ modifiers) to the NSFB keysym the patched
 * wld.c keyboard handler forwards verbatim. QWERTY rows, digits and
 * punctuation follow the US layout; specials translate to the NSFB
 * special keysyms (see NSFB_KEY_* in libnsfb's keysym enum). */
static uint32_t browser_keysym(uint32_t code, uint8_t mods)
{
    static const char *dig   = "1234567890";
    static const char *dig_s = "!@#$%^&*()";
    static const char *r1    = "qwertyuiop";
    static const char *r1_s  = "QWERTYUIOP";
    static const char *r2    = "asdfghjkl";
    static const char *r2_s  = "ASDFGHJKL";
    static const char *r3    = "zxcvbnm";
    static const char *r3_s  = "ZXCVBNM";
    int shift = (mods & SCENE_MOD_SHIFT) != 0;

    if (code >= 2 && code <= 11)
        return (uint32_t)(shift ? dig_s : dig)[code - 2];
    if (code >= 16 && code <= 25)
        return (uint32_t)(shift ? r1_s : r1)[code - 16];
    if (code >= 30 && code <= 38)
        return (uint32_t)(shift ? r2_s : r2)[code - 30];
    if (code >= 44 && code <= 50)
        return (uint32_t)(shift ? r3_s : r3)[code - 44];

    switch (code) {
    case  1:  return 27;                 /* Escape  (ignore shift)  */
    case  12: return shift ? '_' : '-';
    case  13: return shift ? '+' : '=';
    case  14: return 8;                  /* Backspace               */
    case  15: return 9;                  /* Tab                     */
    case  26: return shift ? '{' : '[';
    case  27: return shift ? '}' : ']';
    case  28: return 13;                 /* Enter                   */
    case  39: return shift ? ':' : ';';
    case  40: return shift ? '"' : '\'';
    case  41: return shift ? '~' : '`';
    case  43: return shift ? '|' : '\\';
    case  51: return shift ? '<' : ',';
    case  52: return shift ? '>' : '.';
    case  53: return shift ? '?' : '/';
    case  57: return ' ';
    case 103: return 273;                /* Up                      */
    case 108: return 274;                /* Down                    */
    case 105: return 275;                /* Left                    */
    case 106: return 276;                /* Right                   */
    default:
        if (code >= 59 && code <= 70)
            return 282u + (code - 59);   /* F1..F12                 */
        return 0;
    }
}

/* ---- scene_client callbacks ------------------------------------------ */

static void welcome_cb(void *ud, uint32_t scene_id, uint16_t version,
                       const scene_limits *lim)
{
    scene_browser *b = (scene_browser *)ud;
    scene_rect r;
    (void)scene_id; (void)version; (void)lim;

    b->welcomed = 1;
    r.x = b->cfg.x;
    r.y = b->cfg.y;
    r.w = (int32_t)b->cfg.init_w;
    r.h = (int32_t)b->cfg.init_h;
    /* parent first; the content node hangs off the window            */
    scene_client_create_node(b->cl, SCENE_NO_PARENT, BR_WIN,
                             SCENE_ROLE_WINDOW, &r,
                             SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE);
    scene_client_set_text(b->cl, BR_WIN, 1, b->cfg.title,
                          (uint32_t)strlen(b->cfg.title));
    scene_client_create_node(b->cl, BR_WIN, BR_CONTENT, SCENE_ROLE_GENERIC,
                             &r, SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE);
}

static void input_pointer_cb(void *ud, uint64_t seq, uint8_t device,
                             int32_t x, int32_t y, uint8_t buttons)
{
    scene_browser *b = (scene_browser *)ud;
    (void)device;

    /* surface-relative coordinates for the wl client                 */
    wlshim_input_pointer_move(b->w, x - b->cfg.x, y - b->cfg.y);
    if ((buttons & SCENE_BTN_LEFT) && !(b->last_btn & SCENE_BTN_LEFT))
        wlshim_input_pointer_button(b->w, 0x110u, 1);   /* BTN_LEFT */
    else if (!(buttons & SCENE_BTN_LEFT) && (b->last_btn & SCENE_BTN_LEFT))
        wlshim_input_pointer_button(b->w, 0x110u, 0);
    b->last_btn = buttons;
    scene_client_ack(b->cl, seq);
}

static void input_activate_cb(void *ud, uint64_t seq, scene_node_id id)
{
    scene_browser *b = (scene_browser *)ud;
    (void)id;
    scene_client_ack(b->cl, seq);
}

static void input_key_cb(void *ud, uint64_t seq, uint32_t key_code,
                         uint8_t state, uint8_t modifiers)
{
    scene_browser *b = (scene_browser *)ud;
    uint32_t ksym = browser_keysym(key_code, modifiers);
    if (ksym)
        wlshim_input_keyboard_key(b->w, ksym, state);
    scene_client_ack(b->cl, seq);
}

/* scene_client_connect keeps a pointer (not a copy) to this table, so it
 * must be static storage (here: file scope), not stack memory.          */
static const scene_client_cbs g_browser_cbs = {
    .welcome        = welcome_cb,
    .input_pointer  = input_pointer_cb,
    .input_activate = input_activate_cb,
    .input_key      = input_key_cb,
};

/* ---- host-side ops --------------------------------------------------- */

static void import_frame(scene_browser *b)
{
    if (!b->welcomed || !b->have_frame) return;

    if (b->fw != b->cw || b->fh != b->ch) {
        scene_rect r, src;
        r.x = b->cfg.x;
        r.y = b->cfg.y;
        r.w = (int32_t)b->fw;
        r.h = (int32_t)b->fh;
        scene_client_set_rect(b->cl, BR_WIN, &r);
        scene_client_set_rect(b->cl, BR_CONTENT, &r);
        src.x = 0; src.y = 0;
        src.w = (int32_t)b->fw;
        src.h = (int32_t)b->fh;
        scene_client_set_texture(b->cl, BR_CONTENT, BR_TEX_REF, &src,
                                 0, 0xFFu);
        b->cw = b->fw;
        b->ch = b->fh;
    }

    /* refresh the OS-side texture: re-registering the same ref is
     * idempotent and dirties the window area on the next frame    */
    scene_compositor_register_texture_layer(b->cp, b->layer, BR_TEX_REF,
                                            b->fw, b->fh,
                                            SCENE_TEX_FMT_XRGB, 0xFFu,
                                            b->stage);
    scene_client_present(b->cl, 0);
}

static void browser_run_io(scene_browser *b)
{
    uint8_t in[4096];
    uint32_t got;

    if (b->sv && scene_server_dead(b->sv)) { browser_kill(b); return; }
#ifndef _WIN32
    if (wlshim_pump(b->w) != 0) browser_kill(b);
#endif
    if (!b->dead) import_frame(b);
    if (!b->dead && scene_client_flush(b->cl) != 0) browser_kill(b);
    if (!b->dead) {
        while (scene_transport_recv(b->ts, in, sizeof in, &got) == 0 &&
               got > 0) {
            if (scene_server_feed(b->sv, in, got) != 0) {
                browser_kill(b);
                break;
            }
        }
    }
    if (!b->dead) {
        const uint8_t *f;
        uint32_t flen;
        while (scene_server_out_next_frame(b->sv, &f, &flen) == 1) {
            if (scene_transport_send(b->ts, f, flen) != 0) {
                browser_kill(b);
                break;
            }
        }
    }
    if (!b->dead && scene_client_pump(b->cl) != 0) browser_kill(b);
#ifndef _WIN32
    if (!b->dead && wlshim_pump(b->w) != 0) browser_kill(b);
#endif
}

/* ---- public API ------------------------------------------------------ */

scene_browser *scene_browser_new(scene_compositor *cp,
                                 const scene_browser_config *cfg)
{
    scene_browser *b;

    b = (scene_browser *)calloc(1, sizeof *b);
    if (!b) return NULL;
    b->cp = cp;
    b->cfg = *cfg;
    if (!b->cfg.title) b->cfg.title = "";
    if (!b->cfg.display) b->cfg.display = "wayland-0";
    if (!b->cfg.xdg_runtime) b->cfg.xdg_runtime = "";

    b->w = wlshim_new(cfg->xdg_runtime, cfg->display, frame_cb, died_cb, b);
    b->sv = scene_server_new(NULL);
    if (!b->sv) { free(b); return NULL; }
    b->layer = scene_compositor_add_session(cp, b->sv);
    if (b->layer <= 0) {
        scene_server_free(b->sv);
        free(b);
        return NULL;
    }

    b->lb = scene_loopback_new();
    b->ts = scene_loopback_server_end(b->lb);

    b->cl = scene_client_new();
    scene_client_connect(b->cl, scene_loopback_client_end(b->lb),
                         "browser", &g_browser_cbs, b);
    scene_server_attach(b->sv);
    return b;
}

int scene_browser_pump(scene_browser *b)
{
    if (b->dead) return -1;
    browser_run_io(b);
    if (b->dead || wlshim_dead(b->w)) browser_kill(b);
    return b->dead ? -1 : 0;
}

int scene_browser_dead(const scene_browser *b)
{
    return b->dead;
}

int scene_browser_layer(const scene_browser *b)
{
    return b->layer;
}

scene_server *scene_browser_server(const scene_browser *b)
{
    return b->sv;
}

wlshim *scene_browser_shim(const scene_browser *b)
{
    return b->w;
}

void scene_browser_window_rect(const scene_browser *b, scene_rect *r)
{
    r->x = b->cfg.x;
    r->y = b->cfg.y;
    if (b->have_frame) {
        r->w = (int32_t)b->fw;
        r->h = (int32_t)b->fh;
    } else {
        r->w = (int32_t)b->cfg.init_w;
        r->h = (int32_t)b->cfg.init_h;
    }
}

void scene_browser_free(scene_browser *b)
{
    if (!b) return;
    scene_client_free(b->cl);
    b->cl = NULL;
    if (b->ts) { scene_transport_close(b->ts); b->ts = NULL; }
    if (b->lb) { scene_loopback_free(b->lb); b->lb = NULL; }
    if (b->sv) {                    /* skipped after a kill removed it   */
        scene_compositor_remove_session(b->cp, b->sv);
        b->sv = NULL;
    }
    free(b->stage);
    free(b);
}