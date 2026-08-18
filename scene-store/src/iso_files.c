/*
 * iso_files.c — the first-party file browser guest app.
 *
 * Started by the host with SCENE_STORE_PORT set; connects back over TCP
 * on 127.0.0.1, builds a window through the full scene_app stack
 * (titlebar + close + content, scene_app_create_window_role with
 * SCENE_ROLE_GENERIC content — mirror iso_play) and hosts a directory
 * browser:
 *
 *   argv[1] = start directory (default "."); argv[2] = event log path
 *   (tests) or nothing (stderr).
 *
 * The content area (100,82,420,218) carries three control layers:
 * a ".." BUTTON at (104,86,60,22), a status LABEL at (168,86,348,22),
 * and 12 file/dir row LABELs at (104,112+16i,412,16). A listing is a
 * snapshot taken on demand: opendir/readdir (".", ".." skipped),
 * sorted alphabetically (strcmp) with directories FIRST, first 12
 * shown as "D name" / "F name" in the row text slots. Clicking a row
 * navigates into a directory (a path stack, MAX 32 levels, makes ".."
 * a pop — from the start dir it stays) or, for a file, spawns its
 * associated app — open-with, the file browser driving real guest
 * processes: the child inherits SCENE_STORE_PORT and connects back to
 * the same launcher listener as a fresh session (iso-edit for text
 * extensions, iso-photo for images, iso-play for video). The spawned
 * child is detached (setsid + /dev/null stdio on POSIX, DETACHED on
 * Windows) and never waitpid'd here — the host reaps it on socket
 * close. Unknown extensions fall back to the "file: name" status.
 * opendir failure reports "bad dir" and the window stays
 * alive. Close button destroys the window and exits 0 (the exact
 * iso_play on_activate pattern — NO scene_app_pump inside the input
 * callback; that re-enters scene_client_pump and recurses forever,
 * seen 0xC00000FD).
 *
 * Deterministic: no timers, no wall clock; purely event-driven. The
 * input callbacks only ack and record a pending action; the re-list
 * (SetText on rows/status/title + present + flush) runs in the main
 * loop, so no op is emitted while the INPUT_ACTIVATE record is still
 * in flight.
 *
 * Node ids (base = window content id - 4, i.e. 40000 for the first
 * window): base+6 = ".." button, base+7 = status label, base+10..21 =
 * the 12 row labels (base+8/9 intentionally unused per layout).
 */
#define _POSIX_C_SOURCE 200809L
#include "scene_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <dirent.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <windows.h>
#include <direct.h>
static void msleep(unsigned m) { Sleep(m); }
#else
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
static void msleep(unsigned m)
{
    struct timespec ts = { (time_t)(m / 1000), (long)(m % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}
#endif

/* ---- window layout --------------------------------------------------- */
/* window (100,50,420,300): titlebar 32px, content (100,82,420,218). */
#define FL_X 100
#define FL_Y 50
#define FL_W 420
#define FL_H 300
#define FL_ROWS        12u
#define FL_MAX_ENTRIES 64u
#define FL_MAX_DEPTH   32u

/* Node ids relative to the window base (content id = base+4). */
#define N_UP     32u
#define N_STATUS 33u
#define N_DEL    34u   /* toolbar: delete the selected row  */
#define N_ROW0   40u

/* Pending actions (input callbacks only set these; main loop acts). */
enum { ACT_NONE = 0, ACT_UP, ACT_ROW, ACT_DEL, ACT_CLOSE };

static scene_app     *g_app;
static FILE          *g_log;
static scene_node_id  g_content;        /* window content id (base+4)  */
static uint32_t       g_base;

static char g_stack[FL_MAX_DEPTH][512]; /* path stack, current = top   */
static int  g_sp;                       /* depth (>= 1 once started)   */

struct fl_entry { char name[256]; int is_dir; };
static struct fl_entry g_entries[FL_MAX_ENTRIES];
static int g_count;

static int  g_act = ACT_NONE;
static int  g_act_row;
static int  g_sel_row = -1;   /* selected entry (-1 = none) */
static char g_status[160];

static void dlog(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (g_log) vfprintf(g_log, fmt, ap);
    else vfprintf(stderr, fmt, ap);
    va_end(ap);
    if (g_log) fflush(g_log);
    else fflush(stderr);
}

/* ---- path helpers ------------------------------------------------------ */

static const char *base_name(const char *path)
{
    const char *p, *b = path;
    for (p = path; *p; p++)
        if (*p == '/' || *p == '\\') b = p + 1;
    return b;
}

static void path_join(const char *dir, const char *name, char *out, size_t cap)
{
    size_t n = strlen(dir);
    int has_sep = n > 0 && (dir[n - 1] == '/' || dir[n - 1] == '\\');
    snprintf(out, cap, "%s%s%s", dir, has_sep ? "" : "/", name);
}

static int is_dir_path(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (st.st_mode & S_IFDIR) != 0;
}

/* Remove an empty directory (rmdir; _rmdir on Windows). */
static int fs_remove_dir(const char *path)
{
#if defined(_WIN32)
    return _rmdir(path);
#else
    return rmdir(path);
#endif
}

/* ---- open-with (spawn the associated guest app for a file) -------------- *
 * The browser itself spawns the handler child. The child inherits
 * SCENE_STORE_PORT from its environment and connects back to the SAME
 * launcher listener, so the host accepts it as a fresh session/layer
 * (the multi-session launcher path, proven in pass 15).                 */

static const char *open_app_for(const char *name)
{
    const char *dot = strrchr(name, '.');
    char ext[16];
    size_t i, n;
    static const char *text_ext[] = {
        "txt", "log", "conf", "cfg", "c", "h", "sh", "md", "ini", "rc"
    };
    static const char *img_ext[] = {
        "jpg", "jpeg", "png", "bmp", "gif"
    };
    static const char *vid_ext[] = {
        "mp4", "mkv", "webm", "avi"
    };
    int k;

    if (!dot || !dot[1]) return NULL;
    n = 0;
    for (i = dot + 1 - name; name[i] && n < sizeof(ext) - 1; i++)
        ext[n++] = (char)((name[i] >= 'A' && name[i] <= 'Z')
                          ? name[i] - 'A' + 'a' : name[i]);
    ext[n] = '\0';
    if (n == 0) return NULL;
    for (k = 0; k < (int)(sizeof(text_ext) / sizeof(text_ext[0])); k++)
        if (strcmp(ext, text_ext[k]) == 0) return "iso-edit";
    for (k = 0; k < (int)(sizeof(img_ext) / sizeof(img_ext[0])); k++)
        if (strcmp(ext, img_ext[k]) == 0) return "iso-photo";
    for (k = 0; k < (int)(sizeof(vid_ext) / sizeof(vid_ext[0])); k++)
        if (strcmp(ext, vid_ext[k]) == 0) return "iso-play";
    return NULL;
}

/* Inject `path` (relative) into the directory `dir` of the running
 * executable. Returns -1 when the sibling does not exist. Windows-only:
 * POSIX spawns resolve through PATH.                                  */
#if defined(_WIN32)
static int sibling_cmdline(const char *exe, const char *arg,
                           char *out, size_t cap)
{
    char self[1024];
    char dir[1024];
    const char *sep;
    size_t n;

    if (GetModuleFileNameA(NULL, self, sizeof(self)) == 0) return -1;
    sep = strrchr(self, '\\');
    if (!sep) sep = strrchr(self, '/');
    if (!sep) return -1;
    n = (size_t)(sep - self) + 1;
    if (n >= sizeof(dir)) return -1;
    memcpy(dir, self, n);
    dir[n] = '\0';
    if (strlen(dir) + strlen(exe) + strlen(arg) + 8 >= cap) return -1;
    snprintf(out, cap, "\"%s%s.exe\" \"%s\"", dir, exe, arg);
    return 0;
}
#endif /* _WIN32 */

static int spawn_opener(const char *exe, const char *arg)
{
#if defined(_WIN32)
    char cmd[2048];
    char exe_win[64];
    const char *p;
    size_t i, n;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;

    /* Makefile target names are underscored (build/iso_edit.exe); the
     * ISO install renames to /usr/bin/iso-edit. Follow the Makefile
     * convention on Windows: '-' -> '_'. */
    n = strlen(exe);
    if (n >= sizeof(exe_win)) return -1;
    for (i = 0; i < n; i++)
        exe_win[i] = (exe[i] == '-') ? '_' : exe[i];
    exe_win[n] = '\0';
    p = exe_win;
    if (sibling_cmdline(p, arg, cmd, sizeof(cmd)) != 0) return -1;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
                        DETACHED_PROCESS | CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) {
        dlog("iso-files: CreateProcessA '%s' failed: %lu\n", cmd,
             (unsigned long)GetLastError());
        return -1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
#else
    /* Honest failure: only fork when the handler is actually on PATH,
     * else the child would silently _exit(127) and "open:" would lie. */
    {
        const char *path = getenv("PATH");
        char buf[2048];
        int found = 0;
        if (path) {
            const char *q = path;
            while (q && *q && !found) {
                const char *e = strchr(q, ':');
                size_t n = e ? (size_t)(e - q) : strlen(q);
                if (n > 0 && n < sizeof(buf) - 1 - strlen(exe)) {
                    memcpy(buf, q, n);
                    if (buf[n - 1] != '/') buf[n++] = '/';
                    memcpy(buf + n, exe, strlen(exe) + 1);
                    if (access(buf, X_OK) == 0) found = 1;
                }
                q = e ? e + 1 : NULL;
            }
        }
        if (!found) {
            dlog("iso-files: '%s' not on PATH\n", exe);
            return -1;
        }
    }
    {
        pid_t p = fork();
        if (p < 0) { dlog("iso-files: fork failed\n"); return -1; }
        if (p == 0) {
            int devnull;
            setsid();
            devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, 1);
                dup2(devnull, 2);
                if (devnull > 2) close(devnull);
            }
            execlp(exe, exe, arg, (char *)NULL);
            _exit(127);
        }
    }
    return 0;
#endif
}

/* ---- listing ------------------------------------------------------------ */

/* Snapshot of `path` into g_entries, dirs first then alphabetical.
 * Returns 0 on success, -1 when opendir fails (g_entries clobbered). */
static int list_dir(const char *path)
{
    DIR *d = opendir(path);
    struct dirent *e;
    int n = 0;

    if (!d) return -1;
    while ((e = readdir(d)) != NULL) {
        char full[600];
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (n >= (int)FL_MAX_ENTRIES) break;
        path_join(path, e->d_name, full, sizeof(full));
        snprintf(g_entries[n].name, sizeof(g_entries[n].name), "%.255s",
                 e->d_name);
        g_entries[n].is_dir = is_dir_path(full);
        n++;
    }
    closedir(d);
    {
        /* insertion sort: dirs first, then strcmp (deterministic) */
        int i, j;
        for (i = 1; i < n; i++) {
            struct fl_entry key = g_entries[i];
            int key_dir = key.is_dir;
            j = i - 1;
            while (j >= 0) {
                struct fl_entry *a = &g_entries[j];
                int adir = a->is_dir;
                int lt = adir != key_dir ? adir > key_dir
                                         : strcmp(a->name, key.name) < 0;
                if (lt) break;
                g_entries[j + 1] = *a;
                j--;
            }
            g_entries[j + 1] = key;
        }
    }
    g_count = n;
    dlog("iso-files: DBG list %s: %d entries\n", path, n);
    {
        int di;
        for (di = 0; di < n; di++)
            dlog("iso-files: DBG   [%d] %s %s\n", di,
                 g_entries[di].is_dir ? "D" : "F", g_entries[di].name);
    }
    return 0;
}

/* ---- rendering (main-loop only, never inside input callbacks) ---------- */

static void render_rows(void)
{
    uint32_t i;
    for (i = 0; i < FL_ROWS; i++) {
        char buf[300];
        if ((int)i < g_count) {
            if (g_entries[i].is_dir)
                snprintf(buf, sizeof(buf), "D %.44s", g_entries[i].name);
            else
                snprintf(buf, sizeof(buf), "F %.44s", g_entries[i].name);
        } else {
            buf[0] = '\0';
        }
        dlog("iso-files: DBG row %u id=%u text='%s'\n", i,
             (unsigned)(g_base + N_ROW0 + i), buf);
        {
            int rc = scene_app_set_text(g_app, g_base + N_ROW0 + i, 0, buf);
            dlog("iso-files: DBG set_text rc=%d\n", rc);
        }
    }
}

static void set_status(const char *text)
{
    snprintf(g_status, sizeof(g_status), "%.60s", text);
}

static void push_status(void)
{
    scene_app_set_text(g_app, g_base + N_STATUS, 0, g_status);
}

static void finish_relist(void)
{
    set_status(base_name(g_stack[g_sp - 1]));
    push_status();
    render_rows();
    dlog("iso-files: DBG set_title rc=%d\n",
         scene_app_set_title(g_app, g_content, base_name(g_stack[g_sp - 1])));
    scene_app_present(g_app);
    scene_app_flush(g_app);
}

/* opendir failed: restore the current dir's listing and report it. */
static void fail_relist(void)
{
    if (list_dir(g_stack[g_sp - 1]) != 0) g_count = 0;
    set_status("bad dir");
    push_status();
    render_rows();
    scene_app_present(g_app);
    scene_app_flush(g_app);
}

/* Navigate into `dir`: list it; only on success push it onto the stack. */
static void nav_to(const char *dir)
{
    if (g_sp >= (int)FL_MAX_DEPTH) return;
    if (list_dir(dir) != 0) { fail_relist(); return; }
    snprintf(g_stack[g_sp], sizeof(g_stack[g_sp]), "%.511s", dir);
    g_sp++;
    finish_relist();
}

/* "..": pop; from the start dir (depth 1) it stays. */
static void act_up(void)
{
    if (g_sp <= 1) return;
    g_sp--;
    if (list_dir(g_stack[g_sp - 1]) != 0) {
        g_sp++;                       /* rollback: stay put */
        fail_relist();
        return;
    }
    finish_relist();
}

/* ---- input --------------------------------------------------------------- */

static void on_pointer(void *ud, uint64_t seq, int32_t x, int32_t y,
                       uint8_t buttons)
{
    (void)ud;
    dlog("iso-files: pointer %d,%d buttons=%u\n", (int)x, (int)y, buttons);
    scene_app_ack(g_app, seq);
}

static void on_activate(void *ud, uint64_t seq, scene_node_id id)
{
    (void)ud;
    dlog("iso-files: activate id=%u\n", (unsigned)id);
    if (id == g_content - 1) {       /* close button = base+3 */
        dlog("iso-files: close clicked, exiting\n");
        /* No scene_app_pump here (the pass-17 lesson): flush delivers
         * the DESTROY op, then exit(0) closes the socket and the host
         * reaps the session. */
        scene_app_destroy_window(g_app, g_content);
        scene_app_flush(g_app);
        exit(0);
    }
    if (id == g_base + N_UP) {
        g_act = ACT_UP;
    } else if (id == g_base + N_DEL) {
        g_act = ACT_DEL;
        dlog("iso-files: del click (sel=%d)\n", g_sel_row);
    } else if (id >= g_base + N_ROW0 && id < g_base + N_ROW0 + FL_ROWS) {
        g_act = ACT_ROW;
        g_act_row = (int)(id - (g_base + N_ROW0));
    } else {
        g_act = ACT_NONE;            /* clicking nothing: ack only */
    }
    scene_app_ack(g_app, seq);
}

static void on_key(void *ud, uint64_t seq, uint32_t key_code,
                   uint8_t state, uint8_t modifiers)
{
    (void)ud;
    dlog("iso-files: key %u state=%u mods=%u\n", key_code, state, modifiers);
    scene_app_ack(g_app, seq);
}

/* ---- main ---------------------------------------------------------------- */

int main(int argc, char **argv)
{
    const char *port = getenv("SCENE_STORE_PORT");
    const char *start = argc > 1 ? argv[1] : ".";
    char target[64];
    scene_app_cbs cbs;
    scene_client *cl;
    int i;

    if (!port) return 2;
    if (argc > 2) g_log = fopen(argv[2], "w");
    dlog("iso-files: start dir=%s port=%s\n", start, port);

    snprintf(target, sizeof(target), "127.0.0.1:%s", port);
    scene_transport *t = scene_tcp_client(target);
    if (!t) { dlog("iso-files: tcp client failed\n"); return 3; }

    memset(&cbs, 0, sizeof(cbs));
    cbs.pointer = on_pointer;
    cbs.activate = on_activate;
    cbs.key = on_key;
    g_app = scene_app_new_on(t, target, &cbs, NULL);
    if (!g_app) { dlog("iso-files: app_new failed\n"); return 4; }
    scene_tcp_set_nonblock(t, 1);    /* pass-17 lesson */
    dlog("iso-files: connected, waiting for welcome\n");

    for (i = 0; i < 500; i++) {
        scene_app_pump(g_app);
        scene_app_flush(g_app);
        if (scene_client_welcomed(scene_app_client(g_app))) break;
        msleep(5);
    }
    if (i >= 500) { dlog("iso-files: no welcome\n"); return 5; }
    dlog("iso-files: welcomed\n");

    g_content = scene_app_create_window_role(g_app, FL_X, FL_Y, FL_W, FL_H,
                                             base_name(start),
                                             SCENE_ROLE_GENERIC);
    if (g_content == SCENE_NO_PARENT) {
        dlog("iso-files: window create failed\n");
        return 6;
    }
    g_base = g_content - 4;
    cl = scene_app_client(g_app);

    /* ".." button (base+6), Del button (base+8), status label (base+7),
     * 12 row labels (base+10..base+21); all children of the content
     * node, absolute screen coords (the scene_app convention). */
    scene_client_create_node(cl, g_content, g_base + N_UP, SCENE_ROLE_BUTTON,
                             &(scene_rect){104, 86, 60, 22},
                             SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE);
    scene_client_set_text(cl, g_base + N_UP, 0, "..", 2);
    scene_client_create_node(cl, g_content, g_base + N_DEL, SCENE_ROLE_BUTTON,
                             &(scene_rect){172, 86, 48, 22},
                             SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE);
    scene_client_set_text(cl, g_base + N_DEL, 0, "Del", 3);
    scene_client_create_node(cl, g_content, g_base + N_STATUS,
                             SCENE_ROLE_LABEL,
                             &(scene_rect){228, 86, 288, 22},
                             SCENE_FLAG_VISIBLE);
    for (i = 0; i < (int)FL_ROWS; i++) {
        scene_client_create_node(cl, g_content, g_base + N_ROW0 + (uint32_t)i,
                                 SCENE_ROLE_LABEL,
                                 &(scene_rect){104, 112 + 16 * i, 412, 16},
                                 SCENE_FLAG_VISIBLE);
    }
    scene_app_present(g_app);
    scene_app_flush(g_app);
    dlog("iso-files: window built content=%u\n", (unsigned)g_content);

    snprintf(g_stack[0], sizeof(g_stack[0]), "%.511s", start);
    g_sp = 1;
    if (list_dir(start) != 0) {
        set_status("bad dir");
        push_status();
        render_rows();
    } else {
        finish_relist();
        dlog("iso-files: list %s: %d entries\n", start, g_count);
    }
    scene_app_present(g_app);
    scene_app_flush(g_app);

    for (;;) {
        scene_app_pump(g_app);
        scene_app_flush(g_app);
        if (g_act == ACT_CLOSE) {
            g_act = ACT_NONE;
            scene_app_destroy_window(g_app, g_content);
            scene_app_flush(g_app);
            exit(0);
        } else if (g_act == ACT_UP) {
            g_act = ACT_NONE;
            act_up();
        } else if (g_act == ACT_ROW) {
            int idx = g_act_row;
            g_act = ACT_NONE;
            if (idx >= 0 && idx < g_count) {
                if (idx != g_sel_row) {
                    /* First click: select the row (second click opens) */
                    char msg[300];
                    g_sel_row = idx;
                    snprintf(msg, sizeof(msg), "sel: %.48s",
                             g_entries[idx].name);
                    dlog("iso-files: select %d = %s\n", idx,
                         g_entries[idx].name);
                    set_status(msg);
                    push_status();
                    scene_app_present(g_app);
                    scene_app_flush(g_app);
                } else if (g_entries[idx].is_dir) {
                    char full[600];
                    path_join(g_stack[g_sp - 1], g_entries[idx].name,
                              full, sizeof(full));
                    dlog("iso-files: cd %s\n", full);
                    g_sel_row = -1;
                    nav_to(full);
                } else {
                    char full[600];
                    const char *opener;
                    char msg[300];
                    path_join(g_stack[g_sp - 1], g_entries[idx].name,
                              full, sizeof(full));
                    g_sel_row = -1;
                    opener = open_app_for(g_entries[idx].name);
                    if (opener) {
                        if (spawn_opener(opener, full) == 0) {
                            dlog("iso-files: open %s via %s\n", full, opener);
                            snprintf(msg, sizeof(msg), "open: %.44s",
                                     g_entries[idx].name);
                        } else {
                            dlog("iso-files: spawn '%s' failed\n", opener);
                            snprintf(msg, sizeof(msg), "no opener: %.44s",
                                     g_entries[idx].name);
                        }
                    } else {
                        snprintf(msg, sizeof(msg), "file: %.44s",
                                 g_entries[idx].name);
                    }
                    dlog("iso-files: status=%s\n", msg);
                    set_status(msg);
                    push_status();
                    scene_app_present(g_app);
                    scene_app_flush(g_app);
                }
            }
        } else if (g_act == ACT_DEL) {
            int idx = g_sel_row;
            char msg[300];
            g_act = ACT_NONE;
            g_sel_row = -1;
            if (idx >= 0 && idx < g_count && g_sp > 0) {
                char full[600];
                path_join(g_stack[g_sp - 1], g_entries[idx].name,
                          full, sizeof(full));
                int rc = g_entries[idx].is_dir ?
                         fs_remove_dir(full) : remove(full);
                int gone = (access(full, F_OK) != 0);
                dlog("iso-files: del %s rc=%d gone=%d\n", full, rc, gone);
                if (rc == 0 || gone) {
                    snprintf(msg, sizeof(msg), "removed: %.44s",
                             g_entries[idx].name);
                } else {
                    snprintf(msg, sizeof(msg), "failed: %.44s",
                             g_entries[idx].name);
                }
                set_status(msg);
                push_status();
                if (list_dir(g_stack[g_sp - 1]) != 0) g_count = 0;
                render_rows();
                scene_app_present(g_app);
                scene_app_flush(g_app);
            }
        }
        msleep(5);
    }
}
