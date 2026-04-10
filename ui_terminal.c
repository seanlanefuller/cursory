#define _GNU_SOURCE
#include "cursory.h"
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <pty.h>
#include <fcntl.h>

void init_terminal(AppState *state) {
    buffer_init(&state->terminal.base);
    state->terminal.shell_pid = forkpty(&state->terminal.master_fd, NULL, NULL, NULL);
    if (state->terminal.shell_pid == 0) { setenv("TERM", "vt100", 1); execlp("/bin/bash", "bash", NULL); exit(1); }
    fcntl(state->terminal.master_fd, F_SETFL, fcntl(state->terminal.master_fd, F_GETFL) | O_NONBLOCK);
}

void update_terminal(AppState *state) {
    char buf[4012]; ssize_t n; static int esc = 0;
    while ((n = read(state->terminal.master_fd, buf, sizeof(buf)-1)) > 0) {
        buf[n] = '\0';
        for (int i=0; i<n; i++) {
            if (esc == 0) {
                if (buf[i] == '\x1b') esc = 1; 
                else if (buf[i] == '\n') { 
                    buffer_insert_line(&state->terminal.base, state->terminal.base.line_count, "");
                }
                else if (buf[i] == 8 || buf[i] == 127) {
                    int last = state->terminal.base.line_count-1;
                    if (last >= 0 && state->terminal.base.lines[last]) {
                        int len = strlen(state->terminal.base.lines[last]);
                        if (len > 0) state->terminal.base.lines[last][len-1] = '\0';
                    }
                }
                else if (buf[i] >= 32 || buf[i] == '\t') { 
                    int last = state->terminal.base.line_count-1;
                    if (last >= 0 && state->terminal.base.lines[last]) {
                        int len = strlen(state->terminal.base.lines[last]); 
                        state->terminal.base.lines[last] = realloc(state->terminal.base.lines[last], len+2); 
                        state->terminal.base.lines[last][len] = buf[i]; state->terminal.base.lines[last][len+1] = '\0';
                    }
                }
            } else if (esc == 1) esc = (buf[i] == '[') ? 2 : 0; else if (esc == 2) if (buf[i] >= '@' && buf[i] <= '~') esc = 0;
        }
        int vh = state->terminal.base.scroll.viewport_height > 0 ? state->terminal.base.scroll.viewport_height : (state->panels[PANEL_TERMINAL].height - 2);
        state->terminal.base.scroll.content_height = state->terminal.base.line_count;
        state->terminal.base.scroll.scroll_y = state->terminal.base.line_count - vh;
        clamp_scroll(&state->terminal.base.scroll);
    }
}

void draw_terminal(AppState *state, Panel *p) {
    if (p->height < 3) return;
    TerminalBuffer *buf = &state->terminal; int vis = p->height-2; 
    buf->base.scroll.viewport_height = vis; buf->base.scroll.content_height = buf->base.line_count;
    for (int i=0; i<vis; i++) {
        int ly = buf->base.scroll.scroll_y + i;
        if (ly>=0 && ly<buf->base.line_count && buf->base.lines[ly]) {
            char *line = buf->base.lines[ly];
            int len = strlen(line);
            move(p->y + 1 + i, p->x + 1);
            for (int x = 0; x < p->width - 2 && x < len; x++) {
                bool selected = false;
                if (buf->base.selection.active) {
                    Selection n = get_normalized_selection(&buf->base.selection);
                    if (ly > n.start_line && ly < n.end_line) selected = true;
                    else if (ly == n.start_line && ly == n.end_line) selected = (x >= n.start_col && x < n.end_col);
                    else if (ly == n.start_line) selected = (x >= n.start_col);
                    else if (ly == n.end_line) selected = (x < n.end_col);
                }
                if (selected) attron(A_REVERSE);
                addch(line[x]);
                if (selected) attroff(A_REVERSE);
            }
        }
    }
}

void handle_terminal_input(AppState *state, int ch) {
    bool shift = (ch == KEY_SR || ch == KEY_SF || ch == KEY_SLEFT || ch == KEY_SRIGHT);
    int base_ch = ch;
    if (shift) switch(ch) { case KEY_SR: base_ch=KEY_UP; break; case KEY_SF: base_ch=KEY_DOWN; break; case KEY_SLEFT: base_ch=KEY_LEFT; break; case KEY_SRIGHT: base_ch=KEY_RIGHT; break; }

    if (base_ch == KEY_PPAGE) { state->terminal.base.scroll.scroll_y -= state->terminal.base.scroll.viewport_height/2; } 
    else if (base_ch == KEY_NPAGE) { state->terminal.base.scroll.scroll_y += state->terminal.base.scroll.viewport_height/2; }
    else { 
        char c = (ch == KEY_BACKSPACE || ch == 127) ? 127 : (ch == '\n' || ch == KEY_ENTER) ? '\n' : (char)ch; 
        if (!shift && write(state->terminal.master_fd, &c, 1) < 0) {} 
        if (!shift) {
            state->terminal.base.scroll.content_height = state->terminal.base.line_count;
            state->terminal.base.scroll.scroll_y = state->terminal.base.line_count - state->terminal.base.scroll.viewport_height;
        }
    }
    
    clamp_scroll(&state->terminal.base.scroll);
}
