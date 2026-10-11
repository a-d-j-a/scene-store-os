/*
 * test_settings_app.c — tests for the first-party settings app.
 *
 * Two halves:
 *   1. the text-file model (Option=Value parse + comment-preserving
 *      atomic save, volume, wpa_supplicant networks) driven directly
 *      against a scratch directory;
 *   2. the GUI driven over the loopback harness (compositor + server +
 *      scene_app), including one click routed through the real
 *      compositor input path.
 *
 * The scratch dir is under %TEMP% on Windows (TMPDIR-style on POSIX),
 * so no live /etc or /run is touched.
 */
#include "iso_settings.h"
#include "scene_app.h"
#include "scene_client.h"
#include "scene_compositor.h"
#include "scene_server.h"
#include "scene_store.h"
#include "scene_transport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <process.h>
#define T_MKDIR(p) _mkdir(p)
#define T_GETPID() ((long)_getpid())
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define T_MKDIR(p) mkdir((p), 0755)
#define T_GETPID() ((long)getpid())
#endif

static int checks, failures;

#define CHECK(expr) do { \
    checks++; \
    if (!(expr)) { failures++; printf("FAIL %s:%d: %s\n", \
        __FILE__, __LINE__, #expr); } \
} while(0)
#define CHECK_EQ(a, b) do { \
    checks++; \
    if ((a) != (b)) { failures++; printf("FAIL %s:%d: %s (%ld) != %s (%ld)\n", \
        __FILE__, __LINE__, #a, (long)(a), #b, (long)(b)); } \
} while(0)
#define CHECK_STR(a, b) do { \
    checks++; \
    if (!(a) || strcmp((a), (b)) != 0) { failures++; \
        printf("FAIL %s:%d: %s (\"%s\") != \"%s\"\n", __FILE__, __LINE__, \
               #a, (a) ? (a) : "(null)", (b)); } \
} while(0)

/* ---- scratch dir ------------------------------------------------------- */

static char g_root[256];

static void test_dir_init(void)
{
#if defined(_WIN32)
    const char *t = getenv("TEMP");
    if (!t || !t[0]) t = ".";
    snprintf(g_root, sizeof(g_root), "%s\\iso_settings_app_test_%ld", t,
             T_GETPID());
    T_MKDIR(g_root);
#else
    snprintf(g_root, sizeof(g_root), "/tmp/iso_settings_app_test_%ld",
             T_GETPID());
    T_MKDIR(g_root);
#endif
}

static void new_case_dir(char *out, size_t cap)
{
    static int n;
    snprintf(out, cap, "%s/case%d", g_root, n++);
    T_MKDIR(out);
}

static int path_in(const char *dir, const char *name, char *out, size_t cap)
{
    int n = snprintf(out, cap, "%s/%s", dir, name);
    return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

static int file_write(const char *dir, const char *name, const char *text)
{
    char path[600];
    FILE *f;
    if (path_in(dir, name, path, sizeof(path)) != 0) return -1;
    f = fopen(path, "wb");
    if (!f) return -1;
    if (text) fwrite(text, 1, strlen(text), f);
    fclose(f);
    return 0;
}

static long file_read(const char *dir, const char *name, char *out, size_t cap)
{
    char path[600];
    FILE *f;
    size_t n;
    if (path_in(dir, name, path, sizeof(path)) != 0) return -1;
    f = fopen(path, "rb");
    if (!f) return -1;
    n = fread(out, 1, cap - 1, f);
    fclose(f);
    out[n] = '\0';
    return (long)n;
}

static int file_exists(const char *dir, const char *name)
{
    char path[600];
    FILE *f;
    if (path_in(dir, name, path, sizeof(path)) != 0) return 0;
    f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

/* ---- fixtures ---------------------------------------------------------- */

static const char SHELL_SAMPLE[] =
    "# ISO shell configuration\n"
    "panel_height=32\n"
    "button_color=0xFF0F3460\n"
    "hover_color=0xFF2A2A4E\n"
    "background_color=0xFF1A1A2E\n"
    "clock_12h=0\n"
    "launcher_apps=terminal,editor,browser\n"
    "panel_color=0xFF16213E\n";

static const char WPA_SAMPLE[] =
    "ctrl_interface=DIR=/var/run/wpa_supplicant GROUP=netdev\n"
    "update_config=1\n"
    "country=US\n"
    "\n"
    "network={\n"
    "\tssid=\"HomeNet\"\n"
    "\tpsk=\"secret123\"\n"
    "}\n"
    "\n"
    "network={\n"
    "\tssid=\"Office\"\n"
    "\tpsk=\"workpass\"\n"
    "}\n";

/* ---- file-model tests -------------------------------------------------- */

static void test_shell_conf_parse(void)
{
    char dir[300], path[600];
    iso_settings_kvfile kv;
    const char *v = NULL;

    printf("test_shell_conf_parse:\n");
    new_case_dir(dir, sizeof(dir));
    file_write(dir, "shell.conf", SHELL_SAMPLE);

    snprintf(path, sizeof(path), "%s/shell.conf", dir);
    CHECK_EQ(iso_settings_kv_load(&kv, path), 0);

    CHECK_EQ(iso_settings_kv_get(&kv, "panel_height", &v), 0);
    CHECK_STR(v, "32");
    CHECK_EQ(iso_settings_kv_get(&kv, "background_color", &v), 0);
    CHECK_STR(v, "0xFF1A1A2E");
    CHECK_EQ(iso_settings_kv_get(&kv, "hover_color", &v), 0);
    CHECK_STR(v, "0xFF2A2A4E");
    CHECK_EQ(iso_settings_kv_get(&kv, "button_color", &v), 0);
    CHECK_STR(v, "0xFF0F3460");
    CHECK_EQ(iso_settings_kv_get(&kv, "launcher_apps", &v), 0);
    CHECK_STR(v, "terminal,editor,browser");

    CHECK_EQ(iso_settings_kv_get_int(&kv, "panel_height", 0), 32);
    CHECK_EQ(iso_settings_kv_get_int(&kv, "clock_12h", 7), 0);
    CHECK_EQ(iso_settings_kv_get_int(&kv, "missing", 7), 7);
    CHECK_EQ(iso_settings_kv_get(&kv, "missing", &v), -1);
}

static void test_kv_atomic_roundtrip(void)
{
    char dir[300], path[600];
    iso_settings_kvfile kv;
    const char *v = NULL;

    printf("test_kv_atomic_roundtrip:\n");
    new_case_dir(dir, sizeof(dir));
    file_write(dir, "shell.conf", SHELL_SAMPLE);

    snprintf(path, sizeof(path), "%s/shell.conf", dir);
    CHECK_EQ(iso_settings_kv_load(&kv, path), 0);
    CHECK_EQ(iso_settings_kv_set(&kv, "panel_height", "40"), 0);
    CHECK_EQ(iso_settings_kv_set(&kv, "new_key", "hello"), 0);
    CHECK_EQ(iso_settings_kv_save(&kv), 0);
    CHECK(!file_exists(dir, "shell.conf.tmp"));

    CHECK_EQ(iso_settings_kv_load(&kv, path), 0);
    CHECK_EQ(iso_settings_kv_get(&kv, "panel_height", &v), 0);
    CHECK_STR(v, "40");
    CHECK_EQ(iso_settings_kv_get(&kv, "new_key", &v), 0);
    CHECK_STR(v, "hello");
    CHECK_EQ(iso_settings_kv_get(&kv, "panel_color", &v), 0);
    CHECK_STR(v, "0xFF16213E");
    CHECK(strstr(kv.raw, "# ISO shell configuration") != NULL);

    CHECK_EQ(iso_settings_kv_set(&kv, "panel_height", "44"), 0);
    CHECK_EQ(iso_settings_kv_save(&kv), 0);
    CHECK_EQ(iso_settings_kv_load(&kv, path), 0);
    CHECK_EQ(iso_settings_kv_get(&kv, "panel_height", &v), 0);
    CHECK_STR(v, "44");
    CHECK(strstr(kv.raw, "# ISO shell configuration") != NULL);
}

static void test_volume_io(void)
{
    char dir[300], path[600];
    int out = -1;

    printf("test_volume_io:\n");
    new_case_dir(dir, sizeof(dir));
    snprintf(path, sizeof(path), "%s/scene-volume", dir);

    CHECK_EQ(iso_settings_volume_load(path, &out), -1);
    CHECK_EQ(out, 100);

    CHECK_EQ(iso_settings_volume_save(path, 75), 0);
    out = -1;
    CHECK_EQ(iso_settings_volume_load(path, &out), 0);
    CHECK_EQ(out, 75);

    CHECK_EQ(iso_settings_volume_save(path, 150), 0);
    out = -1;
    CHECK_EQ(iso_settings_volume_load(path, &out), 0);
    CHECK_EQ(out, 100);

    CHECK_EQ(iso_settings_volume_save(path, -5), 0);
    out = -1;
    CHECK_EQ(iso_settings_volume_load(path, &out), 0);
    CHECK_EQ(out, 0);
}

static void test_wpa_parse_edit(void)
{
    char dir[300], sub[400], path[600];
    iso_settings_wpa w;
    iso_settings_wpa r;
    char text[4096];
    int i;

    printf("test_wpa_parse_edit:\n");
    new_case_dir(dir, sizeof(dir));
    snprintf(sub, sizeof(sub), "%s/wpa_supplicant", dir);
    T_MKDIR(sub);
    file_write(dir, "wpa_supplicant/wpa_supplicant.conf", WPA_SAMPLE);

    snprintf(path, sizeof(path), "%s/wpa_supplicant/wpa_supplicant.conf", dir);
    CHECK_EQ(iso_settings_wpa_load(&w, path), 0);
    CHECK_EQ(w.count, 2);
    CHECK_STR(w.nets[0].ssid, "HomeNet");
    CHECK_STR(w.nets[0].psk, "secret123");
    CHECK_STR(w.nets[1].ssid, "Office");
    CHECK_STR(w.nets[1].psk, "workpass");
    CHECK(strstr(w.head, "ctrl_interface") != NULL);
    CHECK(strstr(w.head, "country=US") != NULL);

    CHECK_EQ(iso_settings_wpa_add(&w, "Cafe", "coffee"), 0);
    CHECK_EQ(w.count, 3);
    CHECK_EQ(iso_settings_wpa_add(&w, "Cafe", "coffee2"), 0);
    CHECK_EQ(w.count, 3);
    CHECK_STR(w.nets[2].psk, "coffee2");

    for (i = 0; i < 40; i++) {
        char ssid[32];
        snprintf(ssid, sizeof(ssid), "extra%d", i);
        (void)iso_settings_wpa_add(&w, ssid, "x");
    }
    CHECK_EQ(w.count, ISO_SETTINGS_NET_MAX);
    CHECK_EQ(iso_settings_wpa_add(&w, "overflow", "x"), -1);

    CHECK_EQ(iso_settings_wpa_load(&w, path), 0);
    CHECK_EQ(iso_settings_wpa_set(&w, 1, "Office2", "newpass"), 0);
    CHECK_EQ(iso_settings_wpa_remove(&w, 0), 0);
    CHECK_EQ(w.count, 1);
    CHECK_STR(w.nets[0].ssid, "Office2");
    CHECK_STR(w.nets[0].psk, "newpass");

    CHECK_EQ(iso_settings_wpa_add(&w, "Cafe", "coffee"), 0);
    CHECK_EQ(iso_settings_wpa_save(&w), 0);
    CHECK(!file_exists(dir, "wpa_supplicant/wpa_supplicant.conf.tmp"));

    CHECK_EQ(iso_settings_wpa_load(&r, path), 0);
    CHECK_EQ(r.count, 2);
    CHECK_STR(r.nets[0].ssid, "Office2");
    CHECK_STR(r.nets[0].psk, "newpass");
    CHECK_STR(r.nets[1].ssid, "Cafe");
    CHECK_STR(r.nets[1].psk, "coffee");
    CHECK(strstr(r.head, "ctrl_interface") != NULL);
    CHECK(strstr(r.head, "country=US") != NULL);
    CHECK(file_read(dir, "wpa_supplicant/wpa_supplicant.conf", text,
                    sizeof(text)) > 0);
    CHECK(strstr(text, "ssid=\"Office2\"") != NULL);
    CHECK(strstr(text, "psk=\"coffee\"") != NULL);
}

/* ---- GUI harness ------------------------------------------------------- */

struct harness {
    scene_loopback *lb;
    scene_transport *server_ts;
    scene_compositor *cp;
    scene_app *app;
    iso_settings_gui *gui;
    scene_app_cbs cbs;
    char dir[300];
    int applied;
    char last[64];
};

static void h_tickf(struct harness *h, int rounds)
{
    int r;
    for (r = 0; r < rounds; r++) {
        uint8_t buf[8192];
        uint32_t got;
        const uint8_t *f;
        uint32_t flen;

        scene_app_flush(h->app);
        while (scene_transport_recv(h->server_ts, buf, sizeof(buf), &got) == 0
               && got)
            (void)scene_server_feed(scene_compositor_server(h->cp), buf, got);
        while (scene_server_out_next_frame(scene_compositor_server(h->cp),
                                           &f, &flen) == 1)
            (void)scene_transport_send(h->server_ts, f, flen);
        scene_app_pump(h->app);
        scene_compositor_frame(h->cp);
    }
}

static void h_cb_pointer(void *ud, uint64_t seq, int32_t x, int32_t y,
                         uint8_t buttons)
{
    struct harness *h = (struct harness *)ud;
    (void)x; (void)y; (void)buttons;
    scene_app_ack(h->app, seq);
}

static void h_cb_activate(void *ud, uint64_t seq, scene_node_id id)
{
    struct harness *h = (struct harness *)ud;
    iso_settings_gui_handle_activate(h->gui, id);
    scene_app_present(h->app);
    scene_app_ack(h->app, seq);
}

static void h_cb_key(void *ud, uint64_t seq, uint32_t key_code,
                     uint8_t state, uint8_t modifiers)
{
    struct harness *h = (struct harness *)ud;
    iso_settings_gui_handle_key(h->gui, key_code, state, modifiers);
    scene_app_ack(h->app, seq);
}

static void h_cb_applied(void *ud, const char *what)
{
    struct harness *h = (struct harness *)ud;
    h->applied++;
    snprintf(h->last, sizeof(h->last), "%s", what ? what : "");
}

static int harness_init(struct harness *h, const char *dir)
{
    iso_settings_gui_cbs gcbs;
    scene_node_id content;
    int i;

    memset(h, 0, sizeof(*h));
    snprintf(h->dir, sizeof(h->dir), "%s", dir);

    h->lb = scene_loopback_new();
    h->server_ts = scene_loopback_server_end(h->lb);
    h->cp = scene_compositor_new(NULL, 800, 600);
    scene_compositor_set_clear(h->cp, 0xFF101010);
    scene_server_attach(scene_compositor_server(h->cp));

    h->cbs.pointer = h_cb_pointer;
    h->cbs.activate = h_cb_activate;
    h->cbs.key = h_cb_key;
    h->app = scene_app_new(scene_loopback_client_end(h->lb), &h->cbs, h);
    if (!h->app) return -1;

    for (i = 0; i < 200; i++) {
        if (scene_client_welcomed(scene_app_client(h->app))) break;
        h_tickf(h, 1);
    }
    if (!scene_client_welcomed(scene_app_client(h->app))) return -1;

    content = scene_app_create_window(h->app, 100, 50, 460, 340, "Settings");
    if (content == SCENE_NO_PARENT) return -1;

    h->gui = iso_settings_gui_new(scene_app_client(h->app), h->dir);
    if (!h->gui) return -1;
    memset(&gcbs, 0, sizeof(gcbs));
    gcbs.applied = h_cb_applied;
    iso_settings_gui_set_cbs(h->gui, &gcbs, h);

    CHECK_EQ(iso_settings_gui_open(h->gui, content, 100, 82, 460, 308), 0);
    scene_app_present(h->app);
    h_tickf(h, 4);
    return 0;
}

static void harness_destroy(struct harness *h)
{
    if (h->gui) iso_settings_gui_free(h->gui);
    if (h->app) scene_app_free(h->app);
    if (h->cp) scene_compositor_free(h->cp);
    if (h->lb) scene_loopback_free(h->lb);
}

static int store_has(scene_store *st, const char *term)
{
    scene_node_id nodes[64];
    scene_text_id texts[64];
    size_t tcap = 64;
    size_t n = scene_store_search(st, term, (uint32_t)strlen(term),
                                  nodes, 64, texts, &tcap);
    return n > 0;
}

static int read_int_file(const char *dir, const char *name, int deflt)
{
    char buf[64];
    if (file_read(dir, name, buf, sizeof(buf)) <= 0) return deflt;
    return atoi(buf);
}

static int read_kv(const char *dir, const char *name, const char *key,
                   int deflt)
{
    char path[600];
    iso_settings_kvfile kv;
    if (path_in(dir, name, path, sizeof(path)) != 0) return deflt;
    if (iso_settings_kv_load(&kv, path) != 0) return deflt;
    return iso_settings_kv_get_int(&kv, key, deflt);
}

/* ---- GUI tests --------------------------------------------------------- */

static void test_gui_list_view(void)
{
    char dir[300];
    struct harness h;
    scene_store *st;

    printf("test_gui_list_view:\n");
    new_case_dir(dir, sizeof(dir));
    CHECK_EQ(harness_init(&h, dir), 0);
    st = scene_compositor_store(h.cp);

    CHECK_EQ(iso_settings_gui_view(h.gui), ISO_SETTINGS_VIEW_LIST);
    CHECK_EQ(iso_settings_gui_editing(h.gui), 0);
    CHECK(scene_store_node_count(st) >= 6);
    CHECK(store_has(st, "shell"));
    CHECK(store_has(st, "network"));
    CHECK(store_has(st, "volume"));
    CHECK(store_has(st, "clock"));
    CHECK(store_has(st, "back"));

    CHECK_EQ(iso_settings_gui_handle_activate(h.gui, ISO_SETTINGS_ID_ITEM + 0),
             1);
    CHECK_EQ(iso_settings_gui_view(h.gui), ISO_SETTINGS_VIEW_SHELL);
    h_tickf(&h, 2);
    CHECK(store_has(st, "panel_height"));

    CHECK_EQ(iso_settings_gui_handle_activate(h.gui, ISO_SETTINGS_ID_BACK), 1);
    CHECK_EQ(iso_settings_gui_view(h.gui), ISO_SETTINGS_VIEW_LIST);
    CHECK_EQ(iso_settings_gui_handle_activate(h.gui, 999999), 0);
    CHECK_EQ(iso_settings_gui_handle_activate(h.gui, ISO_SETTINGS_ID_BACK), 1);
    CHECK_EQ(iso_settings_gui_view(h.gui), ISO_SETTINGS_VIEW_LIST);

    harness_destroy(&h);
}

static void test_gui_shell_edit(void)
{
    char dir[300];
    struct harness h;
    char buf[512];

    printf("test_gui_shell_edit:\n");
    new_case_dir(dir, sizeof(dir));
    file_write(dir, "shell.conf", SHELL_SAMPLE);
    CHECK_EQ(harness_init(&h, dir), 0);

    CHECK_EQ(iso_settings_gui_handle_activate(h.gui, ISO_SETTINGS_ID_ITEM + 0),
             1);
    h_tickf(&h, 2);
    CHECK_EQ(iso_settings_gui_view(h.gui), ISO_SETTINGS_VIEW_SHELL);
    CHECK(store_has(scene_compositor_store(h.cp), "panel_height: 32"));

    CHECK_EQ(iso_settings_gui_handle_activate(h.gui, ISO_SETTINGS_ID_FIELD + 0),
             1);
    h_tickf(&h, 2);
    CHECK_EQ(read_kv(dir, "shell.conf", "panel_height", 0), 36);
    CHECK_EQ(h.applied, 1);
    CHECK_STR(h.last, "shell");
    CHECK(store_has(scene_compositor_store(h.cp), "panel_height: 36"));
    CHECK(file_read(dir, "shell.conf", buf, sizeof(buf)) > 0);
    CHECK(strstr(buf, "# ISO shell configuration") != NULL);
    CHECK(strstr(buf, "panel_color=0xFF16213E") != NULL);

    CHECK_EQ(iso_settings_gui_handle_activate(h.gui, ISO_SETTINGS_ID_FIELD + 4),
             1);
    h_tickf(&h, 2);
    CHECK(file_read(dir, "shell.conf", buf, sizeof(buf)) > 0);
    CHECK(strstr(buf, "clock_12h=1") != NULL);

    CHECK_EQ(iso_settings_gui_handle_activate(h.gui, ISO_SETTINGS_ID_FIELD + 1),
             1);
    CHECK_EQ(iso_settings_gui_handle_activate(h.gui, ISO_SETTINGS_ID_FIELD + 1),
             1);
    h_tickf(&h, 2);
    CHECK_EQ(h.applied, 4);

    CHECK_EQ(iso_settings_gui_handle_activate(h.gui, ISO_SETTINGS_ID_BACK), 1);
    CHECK_EQ(iso_settings_gui_view(h.gui), ISO_SETTINGS_VIEW_LIST);

    harness_destroy(&h);
}

static void test_gui_clock_edit(void)
{
    char dir[300];
    struct harness h;

    printf("test_gui_clock_edit:\n");
    new_case_dir(dir, sizeof(dir));
    file_write(dir, "conf.d", "clock_12h=0\n");
    CHECK_EQ(harness_init(&h, dir), 0);

    CHECK_EQ(iso_settings_gui_handle_activate(h.gui, ISO_SETTINGS_ID_ITEM + 3),
             1);
    h_tickf(&h, 2);
    CHECK_EQ(iso_settings_gui_view(h.gui), ISO_SETTINGS_VIEW_CLOCK);
    CHECK(store_has(scene_compositor_store(h.cp), "clock_12h: 0"));

    CHECK_EQ(iso_settings_gui_handle_activate(h.gui, ISO_SETTINGS_ID_FIELD), 1);
    h_tickf(&h, 2);
    CHECK_EQ(read_kv(dir, "conf.d", "clock_12h", -1), 1);
    CHECK_EQ(h.applied, 1);
    CHECK_STR(h.last, "clock");
    CHECK(store_has(scene_compositor_store(h.cp), "clock_12h: 1"));

    harness_destroy(&h);
}

static void test_gui_volume_edit(void)
{
    char dir[300];
    struct harness h;
    int v;

    printf("test_gui_volume_edit:\n");
    new_case_dir(dir, sizeof(dir));
    CHECK_EQ(harness_init(&h, dir), 0);

    CHECK_EQ(iso_settings_gui_handle_activate(h.gui, ISO_SETTINGS_ID_ITEM + 2),
             1);
    h_tickf(&h, 2);
    CHECK_EQ(iso_settings_gui_view(h.gui), ISO_SETTINGS_VIEW_VOLUME);
    CHECK(store_has(scene_compositor_store(h.cp), "volume: 100"));

    CHECK_EQ(iso_settings_gui_handle_activate(h.gui, ISO_SETTINGS_ID_VOL_DEC),
             1);
    h_tickf(&h, 2);
    v = read_int_file(dir, "scene-volume", -1);
    CHECK_EQ(v, 95);
    CHECK_EQ(h.applied, 1);
    CHECK_STR(h.last, "volume");
    CHECK(store_has(scene_compositor_store(h.cp), "volume: 95"));

    CHECK_EQ(iso_settings_gui_handle_activate(h.gui, ISO_SETTINGS_ID_VOL_INC),
             1);
    CHECK_EQ(iso_settings_gui_handle_activate(h.gui, ISO_SETTINGS_ID_VOL_INC),
             1);
    h_tickf(&h, 2);
    CHECK_EQ(read_int_file(dir, "scene-volume", -1), 100);

    CHECK_EQ(iso_settings_gui_handle_activate(h.gui, ISO_SETTINGS_ID_BACK), 1);
    CHECK_EQ(iso_settings_gui_view(h.gui), ISO_SETTINGS_VIEW_LIST);

    harness_destroy(&h);
}

static void test_gui_network_edit(void)
{
    char dir[300], sub[400], path[600];
    struct harness h;
    iso_settings_wpa w;
    scene_store *st;
    int i;

    printf("test_gui_network_edit:\n");
    new_case_dir(dir, sizeof(dir));
    snprintf(sub, sizeof(sub), "%s/wpa_supplicant", dir);
    T_MKDIR(sub);
    file_write(dir, "wpa_supplicant/wpa_supplicant.conf", WPA_SAMPLE);
    CHECK_EQ(harness_init(&h, dir), 0);
    st = scene_compositor_store(h.cp);

    CHECK_EQ(iso_settings_gui_handle_activate(h.gui, ISO_SETTINGS_ID_ITEM + 1),
             1);
    h_tickf(&h, 2);
    CHECK_EQ(iso_settings_gui_view(h.gui), ISO_SETTINGS_VIEW_NETWORK);
    CHECK(store_has(st, "add network"));
    CHECK(store_has(st, "ssid: HomeNet"));
    CHECK(store_has(st, "ssid: Office"));

    CHECK_EQ(iso_settings_gui_handle_activate(h.gui, ISO_SETTINGS_ID_NET_ADD),
             1);
    h_tickf(&h, 2);
    snprintf(path, sizeof(path), "%s/wpa_supplicant/wpa_supplicant.conf", dir);
    CHECK_EQ(iso_settings_wpa_load(&w, path), 0);
    CHECK_EQ(w.count, 3);
    CHECK_STR(w.nets[2].ssid, "network3");
    CHECK_EQ(h.applied, 1);
    CHECK_STR(h.last, "wifi");

    CHECK_EQ(iso_settings_gui_handle_activate(h.gui,
                                              ISO_SETTINGS_ID_NET_SSID + 0), 1);
    CHECK_EQ(iso_settings_gui_editing(h.gui), 1);
    CHECK(strstr(iso_settings_gui_status(h.gui), "ssid") != NULL);
    for (i = 0; i < 8; i++)
        iso_settings_gui_handle_key(h.gui, SCENE_KEY_BACKSPACE, 1, 0);
    CHECK_EQ(iso_settings_gui_handle_key(h.gui, 46, 1, SCENE_MOD_SHIFT), 1);
    CHECK_EQ(iso_settings_gui_handle_key(h.gui, 30, 1, 0), 1);
    CHECK_EQ(iso_settings_gui_handle_key(h.gui, 33, 1, 0), 1);
    CHECK_EQ(iso_settings_gui_handle_key(h.gui, 18, 1, 0), 1);
    CHECK(strstr(iso_settings_gui_status(h.gui), "Cafe") != NULL);
    CHECK_EQ(iso_settings_gui_handle_key(h.gui, SCENE_KEY_ENTER, 1, 0), 1);
    CHECK_EQ(iso_settings_gui_editing(h.gui), 0);
    h_tickf(&h, 2);
    CHECK_EQ(iso_settings_wpa_load(&w, path), 0);
    CHECK_STR(w.nets[0].ssid, "Cafe");
    CHECK_STR(w.nets[0].psk, "secret123");
    CHECK(store_has(st, "ssid: Cafe"));

    {
        char before[ISO_SETTINGS_PSK_MAX];
        CHECK_EQ(iso_settings_wpa_load(&w, path), 0);
        snprintf(before, sizeof(before), "%s", w.nets[1].psk);
        CHECK_EQ(iso_settings_gui_handle_activate(h.gui,
                                                  ISO_SETTINGS_ID_NET_PSK + 1),
                 1);
        CHECK_EQ(iso_settings_gui_editing(h.gui), 1);
        for (i = 0; i < (int)strlen(before) + 2; i++)
            iso_settings_gui_handle_key(h.gui, SCENE_KEY_BACKSPACE, 1, 0);
        CHECK_EQ(iso_settings_gui_handle_key(h.gui, 30, 1, 0), 1);
        CHECK_EQ(iso_settings_gui_handle_key(h.gui, 48, 1, 0), 1);
        CHECK_EQ(iso_settings_gui_handle_key(h.gui, 46, 1, 0), 1);
        CHECK_EQ(iso_settings_gui_handle_key(h.gui, SCENE_KEY_ENTER, 1, 0), 1);
        h_tickf(&h, 2);
        CHECK_EQ(iso_settings_wpa_load(&w, path), 0);
        CHECK_STR(w.nets[1].psk, "abc");
    }

    CHECK_EQ(iso_settings_gui_handle_activate(h.gui,
                                              ISO_SETTINGS_ID_NET_DEL + 0), 1);
    h_tickf(&h, 2);
    CHECK_EQ(iso_settings_wpa_load(&w, path), 0);
    CHECK_EQ(w.count, 2);
    CHECK(strcmp(w.nets[0].ssid, "Cafe") != 0);

    CHECK_EQ(iso_settings_gui_handle_activate(h.gui, ISO_SETTINGS_ID_BACK), 1);
    CHECK_EQ(iso_settings_gui_view(h.gui), ISO_SETTINGS_VIEW_LIST);

    harness_destroy(&h);
}

static void test_gui_click_end_to_end(void)
{
    char dir[300];
    struct harness h;

    printf("test_gui_click_end_to_end:\n");
    new_case_dir(dir, sizeof(dir));
    CHECK_EQ(harness_init(&h, dir), 0);

    CHECK_EQ(iso_settings_gui_view(h.gui), ISO_SETTINGS_VIEW_LIST);

    /* row 0 ("shell"): content x+10 .. x+w-10, first row at content y+8 */
    CHECK_EQ(scene_compositor_input_pointer(h.cp, 0, 330, 103,
                                            SCENE_BTN_LEFT), 0);
    h_tickf(&h, 2);
    CHECK_EQ(scene_compositor_input_pointer(h.cp, 0, 330, 103, 0), 0);
    h_tickf(&h, 4);

    CHECK_EQ(iso_settings_gui_view(h.gui), ISO_SETTINGS_VIEW_SHELL);
    CHECK(store_has(scene_compositor_store(h.cp), "panel_height"));

    /* the "back" row sits at content y+h-44 .. +26 */
    CHECK_EQ(scene_compositor_input_pointer(h.cp, 0, 330, 362,
                                            SCENE_BTN_LEFT), 0);
    h_tickf(&h, 2);
    CHECK_EQ(scene_compositor_input_pointer(h.cp, 0, 330, 362, 0), 0);
    h_tickf(&h, 4);
    CHECK_EQ(iso_settings_gui_view(h.gui), ISO_SETTINGS_VIEW_LIST);
    CHECK(store_has(scene_compositor_store(h.cp), "network"));

    harness_destroy(&h);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    test_dir_init();

    test_shell_conf_parse();
    test_kv_atomic_roundtrip();
    test_volume_io();
    test_wpa_parse_edit();
    test_gui_list_view();
    test_gui_shell_edit();
    test_gui_clock_edit();
    test_gui_volume_edit();
    test_gui_network_edit();
    test_gui_click_end_to_end();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
