/*
 * scene_terminal.h — terminal emulator as a native scene app.
 *
 * Draws into a scene content node: a text buffer with cursor,
 * handles keyboard input, runs a shell via popen2.
 */
#ifndef SCENE_TERMINAL_H
#define SCENE_TERMINAL_H

#include "scene_app.h"
#include <stdint.h>

typedef struct scene_terminal scene_terminal;

typedef struct scene_terminal_config {
    int32_t  cols;              /* columns (default 80) */
    int32_t  rows;              /* rows (default 24) */
    uint32_t bg_color;          /* background ARGB (default 0xFF0C0C0C) */
    uint32_t fg_color;          /* text ARGB (default 0xFFCCCCCC) */
    uint32_t cursor_color;      /* cursor ARGB (default 0xFFCCCCCC) */
} scene_terminal_config;

void scene_terminal_config_defaults(scene_terminal_config *cfg);

scene_terminal *scene_terminal_new(scene_app *app, scene_node_id content_id,
                                   const scene_terminal_config *cfg);

void scene_terminal_free(scene_terminal *term);

/* Feed a key event into the terminal. */
void scene_terminal_input_key(scene_terminal *term, uint32_t key_code,
                              uint8_t state, uint8_t modifiers);

/* Read from the child process and update the text buffer.
 * Returns 0 on success, -1 if the child has exited. */
int scene_terminal_pump(scene_terminal *term);

/* Get the current line count. */
int32_t scene_terminal_line_count(const scene_terminal *term);

/* Get the absolute index of the first visible line (scroll origin). */
int32_t scene_terminal_view_top(const scene_terminal *term);

/* Get a line of text (caller must free). Returns NULL on error. */
char *scene_terminal_line(const scene_terminal *term, int32_t row);

/* Returns 1 if the child process has exited. */
int scene_terminal_exited(const scene_terminal *term);

/* Get the child exit status (only valid after exited). */
int scene_terminal_exit_status(const scene_terminal *term);

#endif /* SCENE_TERMINAL_H */
