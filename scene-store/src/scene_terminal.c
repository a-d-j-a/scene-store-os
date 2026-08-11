/*
 * scene_terminal.c — terminal emulator implementation.
 *
 * Runs a child shell via bidirectional pipe, maintains a text buffer,
 * renders into a scene content node.
 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "scene_terminal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <pty.h>
#endif

#define MAX_LINES 1024
#define MAX_COLS  512
#define TAB_STOP  8

struct scene_terminal {
    scene_app    *app;
    scene_node_id content_id;
    scene_terminal_config cfg;

    char         *lines[MAX_LINES];
    int32_t       line_count;
    int32_t       view_top;
    int32_t       cursor_row;
    int32_t       cursor_col;

#ifdef _WIN32
    HANDLE        child_in;
    HANDLE        child_out;
    HANDLE        child_proc;
    char          input_buf[4096];
    uint32_t      input_len;
#else
    int           child_write_fd;
    int           child_read_fd;
    pid_t         child_pid;
#endif
    int           exited;
    int           exit_status;
};

void scene_terminal_config_defaults(scene_terminal_config *cfg)
{
    if (!cfg) return;
    cfg->cols         = 80;
    cfg->rows         = 24;
    cfg->bg_color     = 0xFF0C0C0C;
    cfg->fg_color     = 0xFFCCCCCC;
    cfg->cursor_color = 0xFFCCCCCC;
}

/* ---- output processing (shared logic) --------------------------------- */

static void process_output_byte(scene_terminal *term, char c)
{
    if (c == '\n') {
        term->cursor_row++;
        term->cursor_col = 0;
        if (term->cursor_row >= term->cfg.rows) {
            term->cursor_row = term->cfg.rows - 1;
            term->view_top++;
        }
    } else if (c == '\r') {
        term->cursor_col = 0;
    } else if (c == '\t') {
        term->cursor_col = (term->cursor_col + TAB_STOP) & ~(TAB_STOP - 1);
        if (term->cursor_col >= term->cfg.cols)
            term->cursor_col = term->cfg.cols - 1;
    } else if (c == '\b') {
        if (term->cursor_col > 0) term->cursor_col--;
    } else if (c >= 32) {
        int32_t abs_row = term->view_top + term->cursor_row;
        while (abs_row >= term->line_count) {
            if (term->line_count < MAX_LINES) {
                term->lines[term->line_count] = calloc(term->cfg.cols + 1, 1);
                term->line_count++;
            } else {
                term->view_top++;
                abs_row = term->view_top + term->cursor_row;
            }
        }
        char *line = term->lines[abs_row];
        if (term->cursor_col < term->cfg.cols) {
            line[term->cursor_col] = c;
            term->cursor_col++;
        }
        if (term->cursor_col >= term->cfg.cols) {
            term->cursor_col = 0;
            term->cursor_row++;
            if (term->cursor_row >= term->cfg.rows) {
                term->cursor_row = term->cfg.rows - 1;
                term->view_top++;
            }
        }
    }
}

/* ---- Windows ---------------------------------------------------------- */

#ifdef _WIN32

static int spawn_shell_win32(scene_terminal *term)
{
    HANDLE h_in_r, h_in_w, h_out_r, h_out_w;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    if (!CreatePipe(&h_in_r, &h_in_w, &sa, 0)) return -1;
    if (!CreatePipe(&h_out_r, &h_out_w, &sa, 0)) {
        CloseHandle(h_in_r); CloseHandle(h_in_w);
        return -1;
    }
    term->child_in  = h_in_w;
    term->child_out = h_out_r;

    STARTUPINFOA si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = h_in_r;
    si.hStdOutput = h_out_w;
    si.hStdError  = h_out_w;

    PROCESS_INFORMATION pi;
    /* Try sh.exe first (from w64devkit/MSYS2), fall back to cmd.exe */
    const char *shells[] = {"cmd.exe", NULL};
    int spawned = 0;
    int s;
    for (s = 0; shells[s]; s++) {
        if (CreateProcessA(NULL, (char*)shells[s], NULL, NULL, TRUE, 0,
                           NULL, NULL, &si, &pi)) {
            spawned = 1;
            break;
        }
    }
    if (!spawned) {
        CloseHandle(h_in_r); CloseHandle(h_in_w);
        CloseHandle(h_out_r); CloseHandle(h_out_w);
        return -1;
    }
    CloseHandle(h_in_r);
    CloseHandle(h_out_w);
    term->child_proc = pi.hProcess;
    CloseHandle(pi.hThread);
    return 0;
}

static void drain_win32(scene_terminal *term)
{
    DWORD avail = 0;
    if (!PeekNamedPipe(term->child_out, NULL, 0, NULL, &avail, NULL) || avail == 0)
        return;
    char buf[4096];
    DWORD n = 0;
    DWORD to_read = avail < sizeof(buf) ? avail : sizeof(buf);
    if (ReadFile(term->child_out, buf, to_read, &n, NULL) && n > 0) {
        DWORD i;
        for (i = 0; i < n; i++)
            process_output_byte(term, buf[i]);
    }
}

#else /* POSIX */

static int spawn_shell_posix(scene_terminal *term)
{
    int master = -1, slave = -1;
    /* A real PTY: the shell gets a tty, so it prints a prompt, echoes
     * input, and does line editing — a pipe-based stdin is not a tty
     * and busybox ash refuses all three. */
    if (openpty(&master, &slave, NULL, NULL, NULL) == 0) {
        pid_t pid = fork();
        if (pid < 0) {
            close(master); close(slave);
            return -1;
        }
        if (pid == 0) {
            close(master);
            dup2(slave, STDIN_FILENO);
            dup2(slave, STDOUT_FILENO);
            dup2(slave, STDERR_FILENO);
            close(slave);
            setenv("TERM", "scene", 1);
            execl("/bin/sh", "sh", NULL);
            _exit(127);
        }
        close(slave);
        term->child_write_fd = master;
        term->child_read_fd  = master;
        term->child_pid = pid;
        int flags = fcntl(master, F_GETFL, 0);
        fcntl(master, F_SETFL, flags | O_NONBLOCK);
        return 0;
    }

    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        return -1;
    }
    if (pid == 0) {
        close(in_pipe[1]);
        close(out_pipe[0]);
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(out_pipe[1], STDERR_FILENO);
        close(in_pipe[0]);
        close(out_pipe[1]);
        setenv("TERM", "scene", 1);
        execl("/bin/sh", "sh", NULL);
        _exit(127);
    }
    close(in_pipe[0]);
    close(out_pipe[1]);
    term->child_write_fd = in_pipe[1];
    term->child_read_fd  = out_pipe[0];
    term->child_pid = pid;
    int flags = fcntl(term->child_read_fd, F_GETFL, 0);
    fcntl(term->child_read_fd, F_SETFL, flags | O_NONBLOCK);
    return 0;
}

static void drain_posix(scene_terminal *term)
{
    char buf[4096];
    ssize_t n;
    while ((n = read(term->child_read_fd, buf, sizeof(buf))) > 0) {
        ssize_t i;
        for (i = 0; i < n; i++)
            process_output_byte(term, buf[i]);
    }
}

#endif

/* ---- lifecycle -------------------------------------------------------- */

scene_terminal *scene_terminal_new(scene_app *app, scene_node_id content_id,
                                   const scene_terminal_config *cfg)
{
    if (!app || content_id == 0) return NULL;
    scene_terminal *term = calloc(1, sizeof(*term));
    if (!term) return NULL;
    term->app = app;
    term->content_id = content_id;
    if (cfg) term->cfg = *cfg;
    else scene_terminal_config_defaults(&term->cfg);

#ifdef _WIN32
    if (spawn_shell_win32(term) != 0) { free(term); return NULL; }
#else
    if (spawn_shell_posix(term) != 0) { free(term); return NULL; }
#endif

    term->lines[0] = calloc(term->cfg.cols + 1, 1);
    term->line_count = 1;
    return term;
}

void scene_terminal_free(scene_terminal *term)
{
    if (!term) return;
#ifdef _WIN32
    if (term->child_proc) {
        TerminateProcess(term->child_proc, 0);
        CloseHandle(term->child_proc);
    }
    if (term->child_in) CloseHandle(term->child_in);
    if (term->child_out) CloseHandle(term->child_out);
#else
    if (term->child_pid > 0) {
        kill(term->child_pid, SIGTERM);
        waitpid(term->child_pid, NULL, 0);
    }
    close(term->child_write_fd);
    close(term->child_read_fd);
#endif
    int32_t i;
    for (i = 0; i < term->line_count; i++)
        free(term->lines[i]);
    free(term);
}

/* ---- input ------------------------------------------------------------ */

void scene_terminal_input_key(scene_terminal *term, uint32_t key_code,
                              uint8_t state, uint8_t modifiers)
{
    if (!term || term->exited || state == 0) return;

    char seq[8] = {0};
    uint32_t len = 0;

    if (key_code == 28) {
        seq[0] = '\r'; len = 1;
    }
    else if (key_code == 14) { seq[0] = '\b'; len = 1; }
    else if (key_code == 15) { seq[0] = '\t'; len = 1; }
    else if (key_code == 103) { seq[0]=27; seq[1]='['; seq[2]='A'; len=3; }
    else if (key_code == 108) { seq[0]=27; seq[1]='['; seq[2]='B'; len=3; }
    else if (key_code == 106) { seq[0]=27; seq[1]='['; seq[2]='C'; len=3; }
    else if (key_code == 105) { seq[0]=27; seq[1]='['; seq[2]='D'; len=3; }
    else if (key_code == 107) { seq[0]=27; seq[1]='['; seq[2]='3'; seq[3]='~'; len=4; }
    else if (key_code == 199) { seq[0]=27; seq[1]='['; seq[2]='H'; len=3; }
    else if (key_code == 207) { seq[0]=27; seq[1]='['; seq[2]='F'; len=3; }
    else {
        /* Evdev key code → ASCII (QWERTY layout tables) */
        static const char qrow[] = "qwertyuiop";
        static const char arow[] = "asdfghjkl";
        static const char zrow[] = "zxcvbnm";
        static const char numrow[] = "1234567890";
        char ch = 0;
        if (key_code >= 2 && key_code <= 11)
            ch = numrow[key_code - 2];
        else if (key_code >= 16 && key_code <= 25)
            ch = (modifiers & 0x01) ? (qrow[key_code - 16] - 32) : qrow[key_code - 16];
        else if (key_code >= 30 && key_code <= 38)
            ch = (modifiers & 0x01) ? (arow[key_code - 30] - 32) : arow[key_code - 30];
        else if (key_code >= 44 && key_code <= 50)
            ch = (modifiers & 0x01) ? (zrow[key_code - 44] - 32) : zrow[key_code - 44];
        else if (key_code == 12) ch = '-';
        else if (key_code == 13) ch = '=';
        else if (key_code == 26) ch = '[';
        else if (key_code == 27) ch = ']';
        else if (key_code == 43) ch = '\\';
        else if (key_code == 39) ch = ';';
        else if (key_code == 40) ch = '\'';
        else if (key_code == 41) ch = '`';
        else if (key_code == 51) ch = ',';
        else if (key_code == 52) ch = '.';
        else if (key_code == 53) ch = '/';
        else if (key_code == 57) ch = ' ';

        if (ch) {
            if (modifiers & 0x02) {
                /* Ctrl+letter → control char */
                if (ch >= 'a' && ch <= 'z') ch = ch - 'a' + 1;
                else if (ch >= 'A' && ch <= 'Z') ch = ch - 'A' + 1;
            }
            seq[0] = ch;
            len = 1;
        }
    }

    if (len > 0) {
#ifdef _WIN32
        if (key_code == 28) {
            /* Enter: flush buffered input + \r\n */
            if (term->input_len > 0) {
                DWORD written = 0;
                WriteFile(term->child_in, term->input_buf,
                          term->input_len, &written, NULL);
                term->input_len = 0;
            }
            DWORD written = 0;
            WriteFile(term->child_in, "\r\n", 2, &written, NULL);
        } else if (len == 1) {
            /* Buffer single characters, flush if buffer is full */
            if (term->input_len < sizeof(term->input_buf) - 1) {
                term->input_buf[term->input_len++] = seq[0];
            }
            if (term->input_len >= sizeof(term->input_buf) - 1) {
                DWORD written = 0;
                WriteFile(term->child_in, term->input_buf,
                          term->input_len, &written, NULL);
                term->input_len = 0;
            }
        } else {
            /* Multi-byte sequences (escape): flush buffer, then write */
            if (term->input_len > 0) {
                DWORD written = 0;
                WriteFile(term->child_in, term->input_buf,
                          term->input_len, &written, NULL);
                term->input_len = 0;
            }
            DWORD written = 0;
            WriteFile(term->child_in, seq, len, &written, NULL);
        }
#else
        (void)write(term->child_write_fd, seq, len);
#endif
    }
}

/* ---- pump ------------------------------------------------------------- */

int scene_terminal_pump(scene_terminal *term)
{
    if (!term || term->exited) return -1;

#ifdef _WIN32
    DWORD exit_code = 0;
    if (WaitForSingleObject(term->child_proc, 0) == WAIT_OBJECT_0) {
        GetExitCodeProcess(term->child_proc, &exit_code);
        term->exited = 1;
        term->exit_status = (int)exit_code;
        return -1;
    }
    drain_win32(term);
#else
    int status;
    pid_t reaped = waitpid(term->child_pid, &status, WNOHANG);
    if (reaped == term->child_pid) {
        term->exited = 1;
        term->exit_status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        return -1;
    }
    drain_posix(term);
#endif
    return 0;
}

/* ---- accessors -------------------------------------------------------- */

int32_t scene_terminal_line_count(const scene_terminal *term)
{
    return term ? term->line_count : 0;
}

int32_t scene_terminal_view_top(const scene_terminal *term)
{
    if (!term) return 0;
    int32_t top = term->view_top;
    if (top + term->cfg.rows > term->line_count)
        top = term->line_count - term->cfg.rows;
    return top < 0 ? 0 : top;
}

char *scene_terminal_line(const scene_terminal *term, int32_t row)
{
    if (!term || row < 0 || row >= term->line_count) return NULL;
    return strdup(term->lines[row]);
}

int scene_terminal_exited(const scene_terminal *term)
{
    return term ? term->exited : 1;
}

int scene_terminal_exit_status(const scene_terminal *term)
{
    return term ? term->exit_status : -1;
}
