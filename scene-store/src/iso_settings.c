/*
 * iso_settings.c — settings GUI guest app + the small text-file model
 * behind it (Option=Value files, wpa_supplicant networks, volume).
 *
 * Compiled twice:
 *   -DISO_SETTINGS_MAIN   -> the iso-settings guest app (TCP connect,
 *                            window, input loop)
 *   (nothing)             -> library object the test suite drives
 *                            in-process over the loopback harness
 */
#define _CRT_SECURE_NO_WARNINGS
#include "iso_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#define ISG_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#include <sys/types.h>
#define ISG_MKDIR(p) mkdir((p), 0755)
#endif

/* ---- small helpers ----------------------------------------------------- */

static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t n;
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static long read_file(const char *path, char *buf, size_t cap)
{
    FILE *f;
    size_t n;
    if (!path || !buf || cap == 0) return -1;
    f = fopen(path, "rb");
    if (!f) return -1;
    n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = '\0';
    return (long)n;
}

static int make_dir(const char *p)
{
    return ISG_MKDIR(p);
}

static void mkpath(const char *dir)
{
    char tmp[ISO_SETTINGS_PATH_MAX];
    size_t n, i;
    if (!dir) return;
    n = strlen(dir);
    if (n == 0 || n >= sizeof(tmp)) return;
    memcpy(tmp, dir, n + 1);
    for (i = 1; i < n; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char save = tmp[i];
            tmp[i] = '\0';
            (void)make_dir(tmp);
            tmp[i] = save;
        }
    }
    (void)make_dir(tmp);
}

static void ensure_parent(const char *path)
{
    char dir[ISO_SETTINGS_PATH_MAX];
    const char *slash = strrchr(path, '/');
    const char *bslash = strrchr(path, '\\');
    const char *sep = slash;
    size_t n;
    if (bslash && (!sep || bslash > sep)) sep = bslash;
    if (!sep) return;
    n = (size_t)(sep - path);
    if (n == 0 || n >= sizeof(dir)) return;
    memcpy(dir, path, n);
    dir[n] = '\0';
    mkpath(dir);
}

static int buf_append(char *out, size_t cap, size_t *used, const char *s)
{
    size_t n = strlen(s);
    if (*used + n >= cap) return -1;
    memcpy(out + *used, s, n);
    *used += n;
    out[*used] = '\0';
    return 0;
}

/* ---- paths ------------------------------------------------------------- */

void iso_settings_paths_default(iso_settings_paths *p)
{
    if (!p) return;
    copy_str(p->base, sizeof(p->base), "/etc");
    copy_str(p->state, sizeof(p->state), "/run");
}

int iso_settings_paths_set_base(iso_settings_paths *p, const char *base)
{
    if (!p) return -1;
    if (!base || !base[0]) { iso_settings_paths_default(p); return 0; }
    if (strlen(base) >= sizeof(p->base)) return -1;
    copy_str(p->base, sizeof(p->base), base);
    copy_str(p->state, sizeof(p->state), base);
    return 0;
}

int iso_settings_paths_set_state(iso_settings_paths *p, const char *state)
{
    if (!p) return -1;
    if (!state || !state[0]) { copy_str(p->state, sizeof(p->state), "/run"); return 0; }
    if (strlen(state) >= sizeof(p->state)) return -1;
    copy_str(p->state, sizeof(p->state), state);
    return 0;
}

int iso_settings_path(const iso_settings_paths *p, const char *name,
                      char *out, size_t cap)
{
    int n;
    if (!p || !name || !out || cap == 0) return -1;
    n = snprintf(out, cap, "%s/%s", p->base, name);
    if (n < 0 || (size_t)n >= cap) return -1;
    return 0;
}

int iso_settings_state_path(const iso_settings_paths *p, const char *name,
                            char *out, size_t cap)
{
    int n;
    if (!p || !name || !out || cap == 0) return -1;
    n = snprintf(out, cap, "%s/%s", p->state, name);
    if (n < 0 || (size_t)n >= cap) return -1;
    return 0;
}

/* ---- atomic write ------------------------------------------------------ */

int iso_settings_write_atomic(const char *path, const char *data, size_t len)
{
    char tmp[ISO_SETTINGS_PATH_MAX + 8];
    FILE *f;
    int n;
    if (!path || !data) return -1;
    n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (n < 0 || (size_t)n >= sizeof(tmp)) return -1;
    ensure_parent(tmp);
    f = fopen(tmp, "wb");
    if (!f) return -1;
    if (len > 0 && fwrite(data, 1, len, f) != len) {
        fclose(f);
        remove(tmp);
        return -1;
    }
    if (fflush(f) != 0) { fclose(f); remove(tmp); return -1; }
    if (fclose(f) != 0) { remove(tmp); return -1; }
    if (rename(tmp, path) != 0) {
        remove(path);
        if (rename(tmp, path) != 0) { remove(tmp); return -1; }
    }
    return 0;
}

/* ---- Option=Value files ------------------------------------------------ */

static int kv_store(iso_settings_kvfile *k, const char *key, const char *val)
{
    int i;
    for (i = 0; i < k->count; i++) {
        if (strcmp(k->kv[i].key, key) == 0) {
            copy_str(k->kv[i].val, sizeof(k->kv[i].val), val);
            return 0;
        }
    }
    if (k->count >= ISO_SETTINGS_KV_MAX) return -1;
    copy_str(k->kv[k->count].key, sizeof(k->kv[k->count].key), key);
    copy_str(k->kv[k->count].val, sizeof(k->kv[k->count].val), val);
    k->count++;
    return 0;
}

static void kv_parse(iso_settings_kvfile *k)
{
    const char *p = k->raw;
    while (p && *p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        const char *end = p + len;
        const char *s = p;
        const char *eq;
        char key[ISO_SETTINGS_KEY_MAX];
        char val[ISO_SETTINGS_VAL_MAX];
        size_t kl, vl;

        p = nl ? nl + 1 : end;

        while (s < end && (*s == ' ' || *s == '\t' || *s == '\r')) s++;
        if (s >= end || *s == '#' || *s == ';') continue;
        eq = (const char *)memchr(s, '=', (size_t)(end - s));
        if (!eq) continue;
        kl = (size_t)(eq - s);
        while (kl > 0 && (s[kl - 1] == ' ' || s[kl - 1] == '\t' ||
                          s[kl - 1] == '\r')) kl--;
        if (kl == 0 || kl >= sizeof(key)) continue;
        memcpy(key, s, kl);
        key[kl] = '\0';

        {
            const char *v = eq + 1;
            vl = (size_t)(end - v);
            while (vl > 0 && (*v == ' ' || *v == '\t' || *v == '\r')) { v++; vl--; }
            while (vl > 0 && (v[vl - 1] == ' ' || v[vl - 1] == '\t' ||
                              v[vl - 1] == '\r')) vl--;
            if (vl >= sizeof(val)) vl = sizeof(val) - 1;
            memcpy(val, v, vl);
            val[vl] = '\0';
        }
        (void)kv_store(k, key, val);
    }
}

int iso_settings_kv_load(iso_settings_kvfile *k, const char *path)
{
    long n;
    if (!k || !path) return -1;
    memset(k, 0, sizeof(*k));
    copy_str(k->path, sizeof(k->path), path);
    n = read_file(path, k->raw, sizeof(k->raw));
    if (n < 0) return -1;
    k->raw_len = (size_t)n;
    kv_parse(k);
    return 0;
}

int iso_settings_kv_get(const iso_settings_kvfile *k, const char *key,
                        const char **out)
{
    int i;
    if (out) *out = NULL;
    if (!k || !key) return -1;
    for (i = 0; i < k->count; i++) {
        if (strcmp(k->kv[i].key, key) == 0) {
            if (out) *out = k->kv[i].val;
            return 0;
        }
    }
    return -1;
}

int iso_settings_kv_get_int(const iso_settings_kvfile *k, const char *key,
                            int deflt)
{
    const char *v = NULL;
    char *end = NULL;
    long r;
    if (iso_settings_kv_get(k, key, &v) != 0 || !v) return deflt;
    r = strtol(v, &end, 0);
    if (end == v) return deflt;
    return (int)r;
}

int iso_settings_kv_set(iso_settings_kvfile *k, const char *key,
                        const char *val)
{
    if (!k || !key || !key[0]) return -1;
    return kv_store(k, key, val ? val : "");
}

int iso_settings_kv_save(iso_settings_kvfile *k)
{
    size_t cap, used = 0;
    char *out;
    int emitted[ISO_SETTINGS_KV_MAX];
    const char *p;
    int i, rc;

    if (!k || !k->path[0]) return -1;
    cap = ISO_SETTINGS_RAW_MAX * 2u + 512u;
    out = (char *)malloc(cap);
    if (!out) return -1;
    out[0] = '\0';
    memset(emitted, 0, sizeof(emitted));

    p = k->raw;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        const char *end = p + len;
        const char *s = p;
        int idx = -1;

        while (s < end && (*s == ' ' || *s == '\t' || *s == '\r')) s++;
        if (s < end && *s != '#' && *s != ';') {
            const char *eq = (const char *)memchr(s, '=', (size_t)(end - s));
            if (eq) {
                size_t kl = (size_t)(eq - s);
                while (kl > 0 && (s[kl - 1] == ' ' || s[kl - 1] == '\t' ||
                                  s[kl - 1] == '\r')) kl--;
                if (kl > 0 && kl < ISO_SETTINGS_KEY_MAX) {
                    char key[ISO_SETTINGS_KEY_MAX];
                    memcpy(key, s, kl);
                    key[kl] = '\0';
                    for (i = 0; i < k->count; i++) {
                        if (strcmp(k->kv[i].key, key) == 0) { idx = i; break; }
                    }
                }
            }
        }

        if (idx >= 0) {
            if (!emitted[idx]) {
                char line[ISO_SETTINGS_KEY_MAX + ISO_SETTINGS_VAL_MAX + 4];
                snprintf(line, sizeof(line), "%s=%s\n",
                         k->kv[idx].key, k->kv[idx].val);
                if (buf_append(out, cap, &used, line) != 0) { free(out); return -1; }
                emitted[idx] = 1;
            }
        } else {
            size_t n = len + (nl ? 1u : 0u);
            if (used + n >= cap) { free(out); return -1; }
            memcpy(out + used, p, n);
            used += n;
            out[used] = '\0';
        }
        p = nl ? nl + 1 : end;
    }

    for (i = 0; i < k->count; i++) {
        if (emitted[i]) continue;
        {
            char line[ISO_SETTINGS_KEY_MAX + ISO_SETTINGS_VAL_MAX + 4];
            snprintf(line, sizeof(line), "%s=%s\n", k->kv[i].key, k->kv[i].val);
            if (buf_append(out, cap, &used, line) != 0) { free(out); return -1; }
        }
    }

    rc = iso_settings_write_atomic(k->path, out, used);
    free(out);
    return rc;
}

/* ---- wpa_supplicant.conf ----------------------------------------------- */

static void strip_quotes(char *s)
{
    size_t n = strlen(s);
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') {
        memmove(s, s + 1, n - 2);
        s[n - 2] = '\0';
    }
}

int iso_settings_wpa_load(iso_settings_wpa *w, const char *path)
{
    char buf[8192];
    long got;
    char *p;
    iso_settings_net *cur = NULL;

    if (!w || !path) return -1;
    memset(w, 0, sizeof(*w));
    copy_str(w->path, sizeof(w->path), path);
    got = read_file(path, buf, sizeof(buf));
    if (got < 0) return -1;

    p = buf;
    while (*p) {
        char line[256];
        char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char *s;

        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = '\0';
        p = nl ? nl + 1 : p + strlen(p);

        s = line;
        while (*s == ' ' || *s == '\t' || *s == '\r') s++;
        {
            size_t sl = strlen(s);
            while (sl > 0 && (s[sl - 1] == ' ' || s[sl - 1] == '\t' ||
                              s[sl - 1] == '\r')) s[--sl] = '\0';
        }
        if (!*s) continue;

        if (!cur) {
            if (strncmp(s, "network", 7) == 0 && strchr(s, '{')) {
                if (w->count < ISO_SETTINGS_NET_MAX) {
                    cur = &w->nets[w->count++];
                    memset(cur, 0, sizeof(*cur));
                }
                continue;
            }
            if (strlen(w->head) + strlen(s) + 2 < sizeof(w->head)) {
                size_t hl = strlen(w->head), sl = strlen(s);
                memcpy(w->head + hl, s, sl);
                w->head[hl + sl] = '\n';
                w->head[hl + sl + 1] = '\0';
            }
            continue;
        }

        if (strchr(s, '}')) { cur = NULL; continue; }
        if (strncmp(s, "ssid", 4) == 0) {
            char *eq = strchr(s, '=');
            if (eq) {
                char *v = eq + 1;
                while (*v == ' ' || *v == '\t') v++;
                copy_str(cur->ssid, sizeof(cur->ssid), v);
                strip_quotes(cur->ssid);
            }
        } else if (strncmp(s, "psk", 3) == 0) {
            char *eq = strchr(s, '=');
            if (eq) {
                char *v = eq + 1;
                while (*v == ' ' || *v == '\t') v++;
                copy_str(cur->psk, sizeof(cur->psk), v);
                strip_quotes(cur->psk);
            }
        }
    }
    return 0;
}

int iso_settings_wpa_add(iso_settings_wpa *w, const char *ssid,
                         const char *psk)
{
    int i;
    if (!w || !ssid || !ssid[0]) return -1;
    for (i = 0; i < w->count; i++) {
        if (strcmp(w->nets[i].ssid, ssid) == 0) {
            copy_str(w->nets[i].psk, sizeof(w->nets[i].psk), psk ? psk : "");
            return 0;
        }
    }
    if (w->count >= ISO_SETTINGS_NET_MAX) return -1;
    copy_str(w->nets[w->count].ssid, sizeof(w->nets[w->count].ssid), ssid);
    copy_str(w->nets[w->count].psk, sizeof(w->nets[w->count].psk),
             psk ? psk : "");
    w->count++;
    return 0;
}

int iso_settings_wpa_set(iso_settings_wpa *w, int idx, const char *ssid,
                         const char *psk)
{
    if (!w || idx < 0 || idx >= w->count) return -1;
    if (ssid)
        copy_str(w->nets[idx].ssid, sizeof(w->nets[idx].ssid), ssid);
    if (psk)
        copy_str(w->nets[idx].psk, sizeof(w->nets[idx].psk), psk);
    return 0;
}

int iso_settings_wpa_remove(iso_settings_wpa *w, int idx)
{
    int i;
    if (!w || idx < 0 || idx >= w->count) return -1;
    for (i = idx; i + 1 < w->count; i++) w->nets[i] = w->nets[i + 1];
    w->count--;
    return 0;
}

int iso_settings_wpa_save(iso_settings_wpa *w)
{
    size_t cap, used = 0;
    char *out;
    int i, rc;

    if (!w || !w->path[0]) return -1;
    cap = (size_t)ISO_SETTINGS_NET_MAX *
              (ISO_SETTINGS_SSID_MAX + ISO_SETTINGS_PSK_MAX + 64u)
          + sizeof(w->head) + 64u;
    out = (char *)malloc(cap);
    if (!out) return -1;
    out[0] = '\0';

    if (w->head[0]) {
        if (buf_append(out, cap, &used, w->head) != 0) { free(out); return -1; }
    } else {
        const char *preamble =
            "ctrl_interface=DIR=/var/run/wpa_supplicant GROUP=netdev\n"
            "update_config=1\n";
        if (buf_append(out, cap, &used, preamble) != 0) { free(out); return -1; }
    }

    for (i = 0; i < w->count; i++) {
        char block[ISO_SETTINGS_SSID_MAX + ISO_SETTINGS_PSK_MAX + 64];
        snprintf(block, sizeof(block),
                 "network={\n\tssid=\"%s\"\n\tpsk=\"%s\"\n}\n",
                 w->nets[i].ssid, w->nets[i].psk);
        if (buf_append(out, cap, &used, block) != 0) { free(out); return -1; }
    }

    rc = iso_settings_write_atomic(w->path, out, used);
    free(out);
    return rc;
}

/* ---- volume ------------------------------------------------------------ */

int iso_settings_volume_load(const char *path, int *out)
{
    char buf[32];
    long n;
    long v;
    char *end = NULL;
    if (out) *out = 100;
    if (!path || !out) return -1;
    n = read_file(path, buf, sizeof(buf));
    if (n <= 0) return -1;
    v = strtol(buf, &end, 10);
    if (end == buf) return -1;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    *out = (int)v;
    return 0;
}

int iso_settings_volume_save(const char *path, int vol)
{
    char buf[16];
    int n;
    if (!path) return -1;
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    n = snprintf(buf, sizeof(buf), "%d\n", vol);
    if (n < 0) return -1;
    return iso_settings_write_atomic(path, buf, (size_t)n);
}

/* ---- keyboard mapping (evdev KEY_* -> ASCII, shift-aware) --------------- */

static const struct { uint32_t code; char lo; } isg_letters[] = {
    {30,'a'},{48,'b'},{46,'c'},{32,'d'},{18,'e'},{33,'f'},{34,'g'},{35,'h'},
    {23,'i'},{36,'j'},{37,'k'},{38,'l'},{50,'m'},{49,'n'},{24,'o'},{25,'p'},
    {16,'q'},{19,'r'},{31,'s'},{20,'t'},{22,'u'},{47,'v'},{17,'w'},{45,'x'},
    {21,'y'},{44,'z'}
};

static int isg_key_to_char(uint32_t sc, uint8_t mods)
{
    size_t i;
    int shift = (mods & SCENE_MOD_SHIFT) != 0;

    for (i = 0; i < sizeof(isg_letters) / sizeof(isg_letters[0]); i++) {
        if (sc == isg_letters[i].code) {
            char c = isg_letters[i].lo;
            if (shift) c = (char)(c - 'a' + 'A');
            return c;
        }
    }
    if (sc >= 2 && sc <= 10) {
        static const char nu[] = "123456789";
        static const char sh[] = "!@#$%^&*(";
        return shift ? sh[sc - 2] : nu[sc - 2];
    }
    if (sc == 11) return shift ? ')' : '0';
    if (sc == 12) return shift ? '_' : '-';
    if (sc == 13) return shift ? '+' : '=';
    if (sc == 26) return shift ? '{' : '[';
    if (sc == 27) return shift ? '}' : ']';
    if (sc == 39) return shift ? ':' : ';';
    if (sc == 40) return shift ? '"' : '\'';
    if (sc == 41) return shift ? '~' : '`';
    if (sc == 43) return shift ? '|' : '\\';
    if (sc == 51) return shift ? '<' : ',';
    if (sc == 52) return shift ? '>' : '.';
    if (sc == 53) return shift ? '?' : '/';
    if (sc == 57) return ' ';
    return -1;
}

/* ---- GUI --------------------------------------------------------------- */

#define ISG_NODE_MAX 96
#define ISG_ROW_MAX 64
#define ISG_EDIT_MAX 128
#define ISG_STATUS_MAX 192
#define ISG_ROW_H 26
#define ISG_ROW_GAP 4
#define ISG_PAD 8

enum {
    ROW_NONE = 0,
    ROW_NAV,
    ROW_BACK,
    ROW_SHELL,
    ROW_CLOCK,
    ROW_VOL,
    ROW_NET_SSID,
    ROW_NET_PSK,
    ROW_NET_DEL,
    ROW_NET_ADD,
    ROW_VOL_DEC,
    ROW_VOL_INC
};

typedef struct isg_row {
    scene_node_id id;
    int16_t kind;
    int16_t idx;
} isg_row;

struct iso_settings_gui {
    scene_client *cl;
    iso_settings_paths paths;
    iso_settings_kvfile shell;
    iso_settings_kvfile clock;
    iso_settings_wpa wpa;
    int volume;

    int view;
    int open;
    scene_node_id content;
    int32_t x, y, w, h;

    scene_node_id nodes[ISG_NODE_MAX];
    int node_count;
    isg_row rows[ISG_ROW_MAX];
    int row_count;

    int editing;
    int edit_kind;
    int edit_idx;
    char edit_name[32];
    char edit_buf[ISG_EDIT_MAX];
    int edit_len;

    iso_settings_gui_cbs cbs;
    void *ud;
    char status[ISG_STATUS_MAX];
};

static const char *const isg_nav_names[ISO_SETTINGS_LIST_ITEMS] = {
    "shell", "network", "volume", "clock"
};

static const char *const isg_shell_keys[ISO_SETTINGS_SHELL_FIELDS] = {
    "panel_height", "hover_color", "button_color",
    "background_color", "clock_12h", "launcher_apps"
};

static void gui_notify(iso_settings_gui *g, const char *what)
{
    if (g->cbs.applied) g->cbs.applied(g->ud, what);
}

static void gui_status_set(iso_settings_gui *g, const char *s)
{
    copy_str(g->status, sizeof(g->status), s ? s : "");
    if (g->open)
        (void)scene_client_set_text(g->cl, ISO_SETTINGS_ID_STATUS, 0,
                                    g->status, (uint32_t)strlen(g->status));
}

static scene_node_id gui_node(iso_settings_gui *g, scene_node_id id,
                              scene_role role, int32_t x, int32_t y,
                              int32_t w, int32_t h, const char *text,
                              uint8_t flags)
{
    scene_rect r;
    if (g->node_count >= ISG_NODE_MAX) return 0;
    r.x = x; r.y = y; r.w = w; r.h = h;
    if (scene_client_create_node(g->cl, g->content, id, role, &r, flags) != 0)
        return 0;
    if (text && text[0])
        (void)scene_client_set_text(g->cl, id, 0, text,
                                    (uint32_t)strlen(text));
    g->nodes[g->node_count++] = id;
    return id;
}

static void gui_row(iso_settings_gui *g, int kind, int idx,
                    scene_node_id id, const char *text, int32_t y)
{
    if (g->row_count < ISG_ROW_MAX) {
        g->rows[g->row_count].kind = (int16_t)kind;
        g->rows[g->row_count].idx = (int16_t)idx;
        g->rows[g->row_count].id = id;
        g->row_count++;
    }
    (void)gui_node(g, id, SCENE_ROLE_BUTTON, g->x + 10, y, g->w - 20,
                   ISG_ROW_H, text,
                   SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE);
}

static int32_t gui_row_y(const iso_settings_gui *g, int n)
{
    return g->y + ISG_PAD + (int32_t)n * (ISG_ROW_H + ISG_ROW_GAP);
}

static void gui_clear(iso_settings_gui *g)
{
    int i;
    for (i = 0; i < g->node_count; i++)
        (void)scene_client_destroy_node(g->cl, g->nodes[i]);
    g->node_count = 0;
    g->row_count = 0;
}

static void gui_shell_label(iso_settings_gui *g, int i, char *buf, size_t cap)
{
    const char *v = NULL;
    if (i < 0 || i >= ISO_SETTINGS_SHELL_FIELDS) { buf[0] = '\0'; return; }
    if (iso_settings_kv_get(&g->shell, isg_shell_keys[i], &v) != 0)
        v = "(unset)";
    snprintf(buf, cap, "%s: %s", isg_shell_keys[i], v);
}

static void gui_rebuild(iso_settings_gui *g)
{
    int32_t y;
    int i;
    char buf[192];

    gui_clear(g);
    if (!g->open) return;

    (void)gui_node(g, ISO_SETTINGS_ID_STATUS, SCENE_ROLE_LABEL,
                   g->x + 10, g->y + g->h - 20, g->w - 20, 16,
                   g->status, SCENE_FLAG_VISIBLE);
    gui_row(g, ROW_BACK, -1, ISO_SETTINGS_ID_BACK, "back",
            g->y + g->h - 44);

    y = gui_row_y(g, 0);
    switch (g->view) {
    case ISO_SETTINGS_VIEW_LIST:
        for (i = 0; i < ISO_SETTINGS_LIST_ITEMS; i++) {
            gui_row(g, ROW_NAV, i, ISO_SETTINGS_ID_ITEM + (scene_node_id)i,
                    isg_nav_names[i], y);
            y += ISG_ROW_H + ISG_ROW_GAP;
        }
        break;
    case ISO_SETTINGS_VIEW_SHELL:
        for (i = 0; i < ISO_SETTINGS_SHELL_FIELDS; i++) {
            gui_shell_label(g, i, buf, sizeof(buf));
            gui_row(g, ROW_SHELL, i, ISO_SETTINGS_ID_FIELD + (scene_node_id)i,
                    buf, y);
            y += ISG_ROW_H + ISG_ROW_GAP;
        }
        break;
    case ISO_SETTINGS_VIEW_CLOCK: {
        int v = iso_settings_kv_get_int(&g->clock, "clock_12h", 0);
        snprintf(buf, sizeof(buf), "clock_12h: %d (%s)", v,
                 v ? "12-hour" : "24-hour");
        gui_row(g, ROW_CLOCK, 0, ISO_SETTINGS_ID_FIELD, buf, y);
        break;
    }
    case ISO_SETTINGS_VIEW_VOLUME:
        snprintf(buf, sizeof(buf), "volume: %d", g->volume);
        gui_row(g, ROW_VOL, 0, ISO_SETTINGS_ID_FIELD, buf, y);
        y += ISG_ROW_H + ISG_ROW_GAP;
        gui_row(g, ROW_VOL_DEC, 0, ISO_SETTINGS_ID_VOL_DEC, "volume -", y);
        gui_row(g, ROW_VOL_INC, 0, ISO_SETTINGS_ID_VOL_INC, "volume +", y);
        break;
    case ISO_SETTINGS_VIEW_NETWORK:
        gui_row(g, ROW_NET_ADD, 0, ISO_SETTINGS_ID_NET_ADD, "add network", y);
        y += ISG_ROW_H + ISG_ROW_GAP;
        for (i = 0; i < g->wpa.count; i++) {
            snprintf(buf, sizeof(buf), "ssid: %s", g->wpa.nets[i].ssid);
            gui_row(g, ROW_NET_SSID, i,
                    ISO_SETTINGS_ID_NET_SSID + (scene_node_id)i, buf, y);
            y += ISG_ROW_H + ISG_ROW_GAP;
            snprintf(buf, sizeof(buf), "psk: %s", g->wpa.nets[i].psk);
            gui_row(g, ROW_NET_PSK, i,
                    ISO_SETTINGS_ID_NET_PSK + (scene_node_id)i, buf, y);
            y += ISG_ROW_H + ISG_ROW_GAP;
            gui_row(g, ROW_NET_DEL, i,
                    ISO_SETTINGS_ID_NET_DEL + (scene_node_id)i, "remove", y);
            y += ISG_ROW_H + ISG_ROW_GAP;
        }
        break;
    default:
        break;
    }
}

static uint32_t isg_color_next(const char *cur)
{
    static const uint32_t pal[] = {
        0xFF1A1A2Eu, 0xFF16213Eu, 0xFF0F3460u, 0xFF2A2A4Eu,
        0xFF533483u, 0xFF3C3C3Cu, 0xFF0C0C0Cu, 0xFFFFFFFFu
    };
    const int n = (int)(sizeof(pal) / sizeof(pal[0]));
    unsigned long v = 0;
    int i;
    if (cur && *cur) v = strtoul(cur, NULL, 16);
    for (i = 0; i < n; i++)
        if ((uint32_t)v == pal[i]) return pal[(i + 1) % n];
    return pal[0];
}

static void gui_edit_status(iso_settings_gui *g)
{
    char buf[ISG_STATUS_MAX];
    snprintf(buf, sizeof(buf), "%.24s: %.*s_", g->edit_name,
             ISG_EDIT_MAX, g->edit_buf);
    copy_str(g->status, sizeof(g->status), buf);
    if (g->open)
        (void)scene_client_set_text(g->cl, ISO_SETTINGS_ID_STATUS, 0,
                                    g->status, (uint32_t)strlen(g->status));
}

static void gui_edit_begin(iso_settings_gui *g, int kind, int idx,
                           const char *name)
{
    const char *cur = "";
    if ((kind == 0 || kind == 1) && idx >= 0 && idx < g->wpa.count)
        cur = (kind == 0) ? g->wpa.nets[idx].ssid : g->wpa.nets[idx].psk;
    else if (kind == 2 && iso_settings_kv_get(&g->shell, "launcher_apps",
                                              &cur) != 0)
        cur = "";
    g->editing = 1;
    g->edit_kind = kind;
    g->edit_idx = idx;
    copy_str(g->edit_name, sizeof(g->edit_name), name);
    copy_str(g->edit_buf, sizeof(g->edit_buf), cur ? cur : "");
    g->edit_len = (int)strlen(g->edit_buf);
    gui_edit_status(g);
}

static void gui_edit_commit(iso_settings_gui *g)
{
    int rc = -1;
    if (g->edit_kind == 0 && g->edit_idx >= 0 && g->edit_idx < g->wpa.count) {
        rc = iso_settings_wpa_set(&g->wpa, g->edit_idx, g->edit_buf,
                                  g->wpa.nets[g->edit_idx].psk);
        if (rc == 0) rc = iso_settings_wpa_save(&g->wpa);
    } else if (g->edit_kind == 1 && g->edit_idx >= 0 &&
               g->edit_idx < g->wpa.count) {
        rc = iso_settings_wpa_set(&g->wpa, g->edit_idx,
                                  g->wpa.nets[g->edit_idx].ssid, g->edit_buf);
        if (rc == 0) rc = iso_settings_wpa_save(&g->wpa);
    } else if (g->edit_kind == 2) {
        rc = iso_settings_kv_set(&g->shell, "launcher_apps", g->edit_buf);
        if (rc == 0) rc = iso_settings_kv_save(&g->shell);
    }
    g->editing = 0;
    if (rc == 0) {
        gui_status_set(g, "saved");
        gui_notify(g, g->edit_kind == 2 ? "shell" : "wifi");
    } else {
        gui_status_set(g, "write failed");
    }
    gui_rebuild(g);
}

static void gui_shell_apply(iso_settings_gui *g, int idx)
{
    char buf[ISO_SETTINGS_VAL_MAX];
    if (idx < 0 || idx >= ISO_SETTINGS_SHELL_FIELDS) return;
    if (idx == 5) { gui_edit_begin(g, 2, 0, "launcher_apps"); return; }

    if (idx == 0) {
        int v = iso_settings_kv_get_int(&g->shell, "panel_height", 32) + 4;
        if (v > 64) v = 16;
        snprintf(buf, sizeof(buf), "%d", v);
    } else if (idx == 4) {
        int v = iso_settings_kv_get_int(&g->shell, "clock_12h", 0) ? 0 : 1;
        snprintf(buf, sizeof(buf), "%d", v);
    } else {
        const char *cur = NULL;
        if (iso_settings_kv_get(&g->shell, isg_shell_keys[idx], &cur) != 0)
            cur = "";
        snprintf(buf, sizeof(buf), "0x%08X",
                 (unsigned)isg_color_next(cur));
    }

    if (iso_settings_kv_set(&g->shell, isg_shell_keys[idx], buf) == 0 &&
        iso_settings_kv_save(&g->shell) == 0) {
        char msg[ISO_SETTINGS_VAL_MAX + 32];
        snprintf(msg, sizeof(msg), "%s = %s", isg_shell_keys[idx], buf);
        gui_status_set(g, msg);
        gui_notify(g, "shell");
    } else {
        gui_status_set(g, "write failed");
    }
    gui_rebuild(g);
}

static void gui_clock_toggle(iso_settings_gui *g)
{
    char buf[8];
    int v = iso_settings_kv_get_int(&g->clock, "clock_12h", 0) ? 0 : 1;
    snprintf(buf, sizeof(buf), "%d", v);
    if (iso_settings_kv_set(&g->clock, "clock_12h", buf) == 0 &&
        iso_settings_kv_save(&g->clock) == 0) {
        gui_status_set(g, v ? "clock_12h = 1" : "clock_12h = 0");
        gui_notify(g, "clock");
    } else {
        gui_status_set(g, "write failed");
    }
    gui_rebuild(g);
}

static void gui_volume_apply(iso_settings_gui *g, int v)
{
    char path[ISO_SETTINGS_PATH_MAX];
    char msg[32];
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    g->volume = v;
    if (iso_settings_state_path(&g->paths, "scene-volume", path,
                                sizeof(path)) == 0 &&
        iso_settings_volume_save(path, v) == 0) {
        snprintf(msg, sizeof(msg), "volume = %d", v);
        gui_status_set(g, msg);
        gui_notify(g, "volume");
    } else {
        gui_status_set(g, "write failed");
    }
    gui_rebuild(g);
}

static void gui_net_add(iso_settings_gui *g)
{
    char ssid[32];
    snprintf(ssid, sizeof(ssid), "network%d", g->wpa.count + 1);
    if (iso_settings_wpa_add(&g->wpa, ssid, "") == 0 &&
        iso_settings_wpa_save(&g->wpa) == 0) {
        gui_status_set(g, "network added");
        gui_notify(g, "wifi");
    } else {
        gui_status_set(g, "write failed");
    }
    gui_rebuild(g);
}

iso_settings_gui *iso_settings_gui_new(scene_client *cl, const char *base_dir)
{
    iso_settings_gui *g;
    char path[ISO_SETTINGS_PATH_MAX];

    if (!cl) return NULL;
    g = (iso_settings_gui *)calloc(1, sizeof(*g));
    if (!g) return NULL;
    g->cl = cl;
    iso_settings_paths_set_base(&g->paths, base_dir);

    if (iso_settings_path(&g->paths, "shell.conf", path, sizeof(path)) == 0)
        (void)iso_settings_kv_load(&g->shell, path);
    if (iso_settings_path(&g->paths, "conf.d", path, sizeof(path)) == 0)
        (void)iso_settings_kv_load(&g->clock, path);
    if (iso_settings_path(&g->paths, "wpa_supplicant/wpa_supplicant.conf",
                          path, sizeof(path)) == 0)
        (void)iso_settings_wpa_load(&g->wpa, path);

    g->volume = 100;
    if (iso_settings_state_path(&g->paths, "scene-volume", path,
                                sizeof(path)) == 0)
        (void)iso_settings_volume_load(path, &g->volume);

    g->view = ISO_SETTINGS_VIEW_LIST;
    return g;
}

void iso_settings_gui_free(iso_settings_gui *g)
{
    free(g);
}

void iso_settings_gui_set_cbs(iso_settings_gui *g,
                              const iso_settings_gui_cbs *cbs, void *ud)
{
    if (!g) return;
    if (cbs) g->cbs = *cbs;
    else memset(&g->cbs, 0, sizeof(g->cbs));
    g->ud = ud;
}

int iso_settings_gui_open(iso_settings_gui *g, scene_node_id content,
                          int32_t x, int32_t y, int32_t w, int32_t h)
{
    if (!g || content == 0 || w <= 0 || h <= 0) return -1;
    g->content = content;
    g->x = x; g->y = y; g->w = w; g->h = h;
    g->open = 1;
    g->view = ISO_SETTINGS_VIEW_LIST;
    gui_rebuild(g);
    return 0;
}

int iso_settings_gui_tick(iso_settings_gui *g)
{
    (void)g;
    return 0;
}

int iso_settings_gui_handle_activate(iso_settings_gui *g, scene_node_id id)
{
    int i;
    if (!g || g->editing) return 0;
    for (i = 0; i < g->row_count; i++)
        if (g->rows[i].id == id) break;
    if (i >= g->row_count) return 0;

    switch (g->rows[i].kind) {
    case ROW_NAV:
        g->view = g->rows[i].idx + 1;
        gui_status_set(g, "");
        gui_rebuild(g);
        return 1;
    case ROW_BACK:
        g->view = ISO_SETTINGS_VIEW_LIST;
        gui_status_set(g, "");
        gui_rebuild(g);
        return 1;
    case ROW_SHELL:
        gui_shell_apply(g, g->rows[i].idx);
        return 1;
    case ROW_CLOCK:
        gui_clock_toggle(g);
        return 1;
    case ROW_VOL:
    case ROW_VOL_INC:
        gui_volume_apply(g, g->volume + 5);
        return 1;
    case ROW_VOL_DEC:
        gui_volume_apply(g, g->volume - 5);
        return 1;
    case ROW_NET_ADD:
        gui_net_add(g);
        return 1;
    case ROW_NET_SSID:
        gui_edit_begin(g, 0, g->rows[i].idx, "ssid");
        return 1;
    case ROW_NET_PSK:
        gui_edit_begin(g, 1, g->rows[i].idx, "psk");
        return 1;
    case ROW_NET_DEL:
        if (iso_settings_wpa_remove(&g->wpa, g->rows[i].idx) == 0) {
            if (iso_settings_wpa_save(&g->wpa) == 0) {
                gui_status_set(g, "network removed");
                gui_notify(g, "wifi");
            } else {
                gui_status_set(g, "write failed");
            }
            gui_rebuild(g);
        }
        return 1;
    default:
        break;
    }
    return 0;
}

int iso_settings_gui_handle_key(iso_settings_gui *g, uint32_t key_code,
                                uint8_t state, uint8_t modifiers)
{
    int c;
    if (!g || !g->editing || !state) return 0;

    if (key_code == SCENE_KEY_ENTER) { gui_edit_commit(g); return 1; }
    if (key_code == SCENE_KEY_ESC) {
        g->editing = 0;
        gui_status_set(g, "cancelled");
        gui_rebuild(g);
        return 1;
    }
    if (key_code == SCENE_KEY_BACKSPACE) {
        if (g->edit_len > 0) g->edit_buf[--g->edit_len] = '\0';
        gui_edit_status(g);
        return 1;
    }
    c = isg_key_to_char(key_code, modifiers);
    if (c > 0 && g->edit_len < ISG_EDIT_MAX - 1) {
        g->edit_buf[g->edit_len++] = (char)c;
        g->edit_buf[g->edit_len] = '\0';
        gui_edit_status(g);
        return 1;
    }
    return 0;
}

int iso_settings_gui_view(const iso_settings_gui *g)
{
    return g ? g->view : -1;
}

const char *iso_settings_gui_status(const iso_settings_gui *g)
{
    return g ? g->status : "";
}

int iso_settings_gui_editing(const iso_settings_gui *g)
{
    return g ? g->editing : 0;
}

/* ---- guest app --------------------------------------------------------- */

#if defined(ISO_SETTINGS_MAIN)
#include "scene_app.h"

#if defined(_WIN32)
#include <windows.h>
static void isg_sleep(unsigned m) { Sleep(m); }
#else
#include <time.h>
static void isg_sleep(unsigned m)
{
    struct timespec ts;
    ts.tv_sec = (time_t)(m / 1000);
    ts.tv_nsec = (long)(m % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}
#endif

static scene_app *g_app;
static iso_settings_gui *g_gui;
static FILE *g_log;

static void alog(const char *s)
{
    if (g_log) { fputs(s, g_log); fflush(g_log); }
    else { fputs(s, stderr); fflush(stderr); }
}

static void cb_pointer(void *ud, uint64_t seq, int32_t x, int32_t y,
                       uint8_t buttons)
{
    (void)ud; (void)x; (void)y; (void)buttons;
    scene_app_ack(g_app, seq);
}

static void cb_activate(void *ud, uint64_t seq, scene_node_id id)
{
    (void)ud;
    iso_settings_gui_handle_activate(g_gui, id);
    scene_app_present(g_app);
    scene_app_ack(g_app, seq);
}

static void cb_key(void *ud, uint64_t seq, uint32_t key_code,
                   uint8_t state, uint8_t modifiers)
{
    (void)ud;
    iso_settings_gui_handle_key(g_gui, key_code, state, modifiers);
    scene_app_present(g_app);
    scene_app_ack(g_app, seq);
}

static void cb_applied(void *ud, const char *what)
{
    (void)ud;
    alog("iso-settings: applied ");
    alog(what ? what : "?");
    alog("\n");
}

int main(int argc, char **argv)
{
    const char *port = getenv("SCENE_STORE_PORT");
    const char *base = (argc > 1) ? argv[1] : NULL;
    scene_app_cbs cbs;
    scene_transport *t;
    char target[64];
    scene_node_id content;
    int i;

    if (!port) return 2;
    if (argc > 2) g_log = fopen(argv[2], "w");

    snprintf(target, sizeof(target), "127.0.0.1:%s", port);
    t = scene_tcp_client(target);
    if (!t) { alog("iso-settings: tcp client failed\n"); return 3; }

    memset(&cbs, 0, sizeof(cbs));
    cbs.pointer = cb_pointer;
    cbs.activate = cb_activate;
    cbs.key = cb_key;
    g_app = scene_app_new_on(t, target, &cbs, NULL);
    if (!g_app) { alog("iso-settings: app_new failed\n"); return 4; }

    for (i = 0; i < 500; i++) {
        scene_app_pump(g_app);
        scene_app_flush(g_app);
        if (scene_client_welcomed(scene_app_client(g_app))) break;
        isg_sleep(5);
    }
    if (i >= 500) { alog("iso-settings: no welcome\n"); return 5; }

    content = scene_app_create_window(g_app, 100, 50, 460, 340, "Settings");
    if (content == SCENE_NO_PARENT) {
        alog("iso-settings: window create failed\n");
        return 6;
    }

    g_gui = iso_settings_gui_new(scene_app_client(g_app), base);
    if (!g_gui) { alog("iso-settings: gui alloc failed\n"); return 7; }
    {
        iso_settings_gui_cbs gcbs;
        memset(&gcbs, 0, sizeof(gcbs));
        gcbs.applied = cb_applied;
        iso_settings_gui_set_cbs(g_gui, &gcbs, NULL);
    }
    iso_settings_gui_open(g_gui, content, 100, 82, 460, 308);
    scene_app_present(g_app);
    scene_app_flush(g_app);
    alog("iso-settings: opened\n");

    for (;;) {
        scene_app_pump(g_app);
        scene_app_flush(g_app);
        isg_sleep(5);
    }
}
#endif /* ISO_SETTINGS_MAIN */
