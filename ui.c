#define _GNU_SOURCE
#include "cursory.h"


#include <ncurses.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <pty.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>
#include <sys/time.h>

// (File Tree code moved to ui_tree.c)


// --- Editor ---
void free_buffer(EditorBuffer *buf) {
    buffer_free(buf);
}

void load_file(AppState *state, const char *path) {
    buffer_load_file(&state->editor, path);
}

void save_file(AppState *state) {
    EditorBuffer *eb = &state->editor;
    if (eb->filepath[0] == '\0' || strcmp(eb->filepath, "[No File]") == 0) return;
    
    buffer_save_file(eb, eb->filepath);
    
    pthread_mutex_lock(&state->ai.mutex);
    snprintf(state->last_action, sizeof(state->last_action), "Saved: %s", eb->filepath);
    pthread_mutex_unlock(&state->ai.mutex);

    if (state->root) {
        free_file_tree(state->root);
        state->root = create_file_node(".", ".", true, 0);
        state->root->is_expanded = true;
        load_directory(state->root);
        struct stat dst;
        if (stat(".", &dst) == 0) state->last_dir_modified = dst.st_mtime;
    }
}

void insert_char(AppState *state, int ch) {
    buffer_insert_char(&state->editor, ch);
}

void delete_char(AppState *state) {
    buffer_delete_char(&state->editor);
}

void insert_newline(AppState *state);
void execute_command(AppState *state, CommandType cmd);
void calculate_layout(AppState *state);

void insert_newline(AppState *state) {
    buffer_insert_newline(&state->editor);
}

void editor_save(AppState *state) { save_file(state); }

// (Editor rendering moved to ui_editor.c)


// --- Terminal ---
// (Terminal lifecycle moved to ui_terminal.c)


// (Terminal rendering and input moved to ui_terminal.c)


// (Terminal drawing moved to ui_terminal.c)


// (AI input handled in ui_chat.c)



void draw_scrollbar(AppState *state, Panel *p, ScrollState *s) {
    (void)state;

    if (s->content_height <= s->viewport_height) return;
    int track_h = p->height - 2;
    if (track_h < 1) return;
    int thumb_h = (s->viewport_height * track_h) / s->content_height;
    if (thumb_h < 1) thumb_h = 1;
    
    int den = s->content_height - s->viewport_height;
    int thumb_y = (den > 0) ? (s->scroll_y * (track_h - thumb_h)) / den : 0;
    
    for (int i = 0; i < track_h; i++) {
        mvaddch(p->y + 1 + i, p->x + p->width - 1, '|');
    }
    attron(A_REVERSE | COLOR_PAIR(1));
    for (int i = 0; i < thumb_h; i++) {
        mvaddch(p->y + 1 + thumb_y + i, p->x + p->width - 1, '#');
    }
    attroff(A_REVERSE | COLOR_PAIR(1));
}

// (AI Chat logic moved to ui_chat.c)

// (Chat input rendering moved to ui_chat.c)


// --- Layout & Global Panels ---
void draw_panels(AppState *state) {
    for (int i=0; i<PANEL_COUNT; i++) {
        Panel *p = &state->panels[i]; if (!p->visible) continue;
        if ((int)state->active_panel == i) attron(A_BOLD | COLOR_PAIR(1));
        for (int y=p->y; y<p->y+p->height; y++) { mvaddch(y, p->x, ACS_VLINE); mvaddch(y, p->x+p->width-1, ACS_VLINE); }
        for (int x=p->x; x<p->x+p->width; x++) { mvaddch(p->y, x, ACS_HLINE); mvaddch(p->y+p->height-1, x, ACS_HLINE); }
        mvaddch(p->y, p->x, ACS_ULCORNER); mvaddch(p->y, p->x+p->width-1, ACS_URCORNER);
        mvaddch(p->y+p->height-1, p->x, ACS_LLCORNER); mvaddch(p->y+p->height-1, p->x+p->width-1, ACS_LRCORNER);
        char title[600];
        if (i == PANEL_EDITOR) snprintf(title, sizeof(title), " Editor: %s ", state->editor.filepath);
        else snprintf(title, sizeof(title), " %s ", p->name);
        mvprintw(p->y, p->x+2, "%.*s", p->width-4, title);
        if ((int)state->active_panel == i) attroff(A_BOLD | COLOR_PAIR(1));

        if (i == PANEL_FILE_TREE) draw_file_tree(state, p);
        else if (i == PANEL_EDITOR) draw_editor(state, p);
        else if (i == PANEL_AI) draw_ai_chat(state, p);
        else if (i == PANEL_TERMINAL) draw_terminal(state, p);
        else if (i == PANEL_CHAT_INPUT) draw_chat_input(state, p);

        ScrollState *ss = get_panel_scroll(state, (PanelType)i);
        if (ss) {
            if (i == PANEL_AI) pthread_mutex_lock(&state->ai.mutex);
            draw_scrollbar(state, p, ss);
            if (i == PANEL_AI) pthread_mutex_unlock(&state->ai.mutex);
        }
    }


    Panel *ap = &state->panels[(int)state->active_panel];
    if (state->active_panel == PANEL_EDITOR) {
        curs_set(1); move(ap->y+1+state->editor.cursor_y-state->editor.scroll.scroll_y, ap->x+1+state->editor.cursor_x-state->editor.scroll_x);
    } else if (state->active_panel == PANEL_TERMINAL) {
        curs_set(1); int last = state->terminal.base.line_count - 1;
        int sy = ap->y+1+(last-state->terminal.base.scroll.scroll_y);
        if (sy>ap->y && sy<ap->y+ap->height-1 && last >= 0 && state->terminal.base.lines[last]) 
            move(sy, ap->x+1+(int)strlen(state->terminal.base.lines[last]));
        else curs_set(0);
    } else if (state->active_panel == PANEL_CHAT_INPUT) {
        curs_set(1); ChatInputBuffer *cb = &state->ai.input;
        int vw = ap->width - 2; if (vw < 1) vw = 1;
        int vy = 0;
        for (int i=0; i < cb->cursor_y; i++) {
            int r = (strlen(cb->lines[i]) + vw - 1) / vw;
            vy += (r > 0 ? r : 1);
        }
        vy += cb->cursor_x / vw;
        int vx = cb->cursor_x % vw;
        move(ap->y + 1 + vy - cb->scroll.scroll_y, ap->x + 1 + vx);
    } else curs_set(0);
}

void calculate_layout(AppState *state) {
    int my, mx; getmaxyx(stdscr, my, mx);
    if (my < 5 || mx < 10) return;
    
    int terminal_h = state->panels[PANEL_TERMINAL].visible ? (my - 2) / 4 : 0;
    if (terminal_h > 0 && terminal_h < 3) terminal_h = 3;
    
    int input_h = state->panels[PANEL_CHAT_INPUT].visible ? (my - 2) / 4 : 0;
    if (input_h > 0 && input_h < 3) input_h = 3;

    // We'll keep bottom row heights aligned if both are visible
    if (terminal_h > 0 && input_h > 0) {
        if (terminal_h > input_h) input_h = terminal_h;
        else terminal_h = input_h;
    }

    int uh_left = my - 2 - terminal_h; if (uh_left < 3) uh_left = 3;
    int uh_ai = my - 2 - input_h; if (uh_ai < 3) uh_ai = 3;
    
    int vc = 0; for (int i=0; i<3; i++) if (state->panels[i].visible) vc++;
    int x=0, fw = state->panels[PANEL_FILE_TREE].visible ? mx/5 : 0; if (fw>0 && fw<20) fw=20;
    
    // Top Row
    if (state->panels[PANEL_FILE_TREE].visible) { 
        state->panels[PANEL_FILE_TREE] = (Panel){x, 1, fw, uh_left, true, "Files"}; x+=fw; 
    }
    int rem = mx-x; if (rem < 5) rem = 5;
    int side_panels_visible = vc - (state->panels[PANEL_FILE_TREE].visible?1:0);
    int ow = (side_panels_visible > 0) ? rem / side_panels_visible : rem;
    
    int split_x = mx - ow; 
    if (state->panels[PANEL_EDITOR].visible && state->panels[PANEL_AI].visible) {
        split_x = x + ow;
    } else if (state->panels[PANEL_EDITOR].visible) {
        split_x = mx;
    } else if (state->panels[PANEL_AI].visible) {
        split_x = x;
    }

    if (state->panels[PANEL_EDITOR].visible) { 
        state->panels[PANEL_EDITOR] = (Panel){x, 1, ow, uh_left, true, "Editor"}; x+=ow; 
    }
    if (state->panels[PANEL_AI].visible) { 
        state->panels[PANEL_AI] = (Panel){x, 1, mx-x, uh_ai, true, "Chat History"}; 
    }

    // Bottom Row
    int tw = split_x; if (tw < 0) tw = 0;
    int ciw = mx - tw; if (ciw < 0) ciw = 0;

    state->panels[PANEL_TERMINAL] = (Panel){0, my - 1 - terminal_h, tw, terminal_h, state->panels[PANEL_TERMINAL].visible, "Terminal"};
    state->panels[PANEL_CHAT_INPUT] = (Panel){tw, my - 1 - input_h, ciw, input_h, state->panels[PANEL_CHAT_INPUT].visible, "Chat Input"};
}


ScrollState* get_panel_scroll(AppState *state, PanelType type) {
    if (type == PANEL_FILE_TREE) return &state->tree_scroll;
    if (type == PANEL_EDITOR) return &state->editor.scroll;
    if (type == PANEL_AI) return &state->ai.scroll;
    if (type == PANEL_TERMINAL) return &state->terminal.base.scroll;
    if (type == PANEL_CHAT_INPUT) return &state->ai.input.scroll;
    return NULL;
}

void clamp_scroll(ScrollState *s) {
    if (!s) return;
    int max_s = s->content_height - s->viewport_height;
    if (max_s < 0) max_s = 0;
    if (s->scroll_y > max_s) s->scroll_y = max_s;
    if (s->scroll_y < 0) s->scroll_y = 0;
}

Selection* get_panel_selection(AppState *state, PanelType type) {
    if (type == PANEL_EDITOR) return &state->editor.selection;
    if (type == PANEL_AI) return &state->ai.selection;
    if (type == PANEL_TERMINAL) return &state->terminal.base.selection;
    if (type == PANEL_CHAT_INPUT) return &state->ai.input.selection;
    return NULL;
}

void map_mouse_to_panel(AppState *state, PanelType type, int mx, int my, int *line, int *col) {
    Panel *p = &state->panels[type]; ScrollState *ss = get_panel_scroll(state, type);
    int cmx = mx; int cmy = my;
    if (cmx < p->x + 1) cmx = p->x + 1;
    if (cmx >= p->x + p->width - 1) cmx = p->x + p->width - 2;
    if (cmy < p->y + 1) cmy = p->y + 1;
    if (cmy >= p->y + p->height - 1) cmy = p->y + p->height - 2;

    if (type == PANEL_FILE_TREE) {
        *line = cmy - p->y - 1 + ss->scroll_y;
        *col = cmx - p->x - 1;
        return;
    }
    if (type == PANEL_EDITOR) {
        EditorBuffer *eb = &state->editor;
        *line = cmy - p->y - 1 + ss->scroll_y;
        if (eb->line_count == 0) { *line = 0; *col = 0; return; }
        if (*line < 0) *line = 0;
        if (*line >= eb->line_count) *line = eb->line_count - 1;
        *col = cmx - p->x - 1 + eb->scroll_x;
        int len = strlen(eb->lines[*line]);
        if (*col < 0) *col = 0;
        if (*col > len) *col = len;
    } else if (type == PANEL_TERMINAL) {
        TerminalBuffer *tb = &state->terminal;
        *line = cmy - p->y - 1 + ss->scroll_y;
        if (tb->base.line_count == 0) { *line = 0; *col = 0; return; }
        if (*line < 0) *line = 0;
        if (*line >= tb->base.line_count) *line = tb->base.line_count - 1;
        *col = cmx - p->x - 1;
        int len = strlen(tb->base.lines[*line]);
        if (*col < 0) *col = 0;
        if (*col > len) *col = len;
    } else if (type == PANEL_AI) {
        int mouse_y_in_history = my - p->y - 1 + ss->scroll_y;
        int curr_y = 0; int mw = p->width - 6; if (mw < 1) mw = 1;
        bool found = false;
        pthread_mutex_lock(&state->ai.mutex);
        for (int i = 0; i < state->ai.message_count; i++) {
            AIMessage *m = &state->ai.messages[i];
            if (!m->content && !m->reasoning) continue;
            if (m->role && strcmp(m->role, "system") == 0) continue;
            int is_user = (m->role && strcmp(m->role, "user") == 0);
            
            // Handle Reasoning
            if (m->reasoning) {
                char *rc = strdup(m->reasoning); char *sp; char *l = strtok_r(rc, "\n", &sp);
                bool first_l = true;
                while (l) {
                    char *curr = l;
                    while (1) {
                        int avail = first_l ? (mw - 11) : mw; if (avail < 1) avail = 1;
                        int split = (strlen(curr) > (size_t)avail) ? avail : (int)strlen(curr);
                        if (strlen(curr) > (size_t)avail) { int s = split; while (s > 0 && curr[s] != ' ') s--; if (s > 0) split = s; }
                        if (curr_y == mouse_y_in_history) { 
                            *line = curr_y; *col = mx - p->x - (first_l ? 13 : 2); 
                            found = true; break; 
                        }
                        curr_y++; if (strlen(curr) <= (size_t)split) break;
                        curr += split; while (*curr == ' ') curr++; first_l = false;
                    }
                    if (found) break;
                    l = strtok_r(NULL, "\n", &sp); first_l = false;
                }
                free(rc); if (found) break;
            }

            // Handle Content
            if (m->content) {
                char *cc = strdup(m->content); char *sp; char *l = strtok_r(cc, "\n", &sp);
                bool first_l = true; int label_len = is_user ? 5 : 4;
                while (l) {
                    char *curr = l;
                    while (1) {
                        int avail = first_l ? (mw - label_len) : mw; if (avail < 1) avail = 1;
                        int split = (strlen(curr) > (size_t)avail) ? avail : (int)strlen(curr);
                        if (strlen(curr) > (size_t)avail) { int s = split; while (s > 0 && curr[s] != ' ') s--; if (s > 0) split = s; }
                        if (curr_y == mouse_y_in_history) { 
                            *line = curr_y; *col = mx - p->x - (first_l ? (label_len + 2) : 2); 
                            found = true; break; 
                        }
                        curr_y++; if (strlen(curr) <= (size_t)split) break;
                        curr += split; while (*curr == ' ') curr++; first_l = false;
                    }
                    if (found) break;
                    l = strtok_r(NULL, "\n", &sp); first_l = false;
                }
                free(cc); if (found) break;
            }
        }
        pthread_mutex_unlock(&state->ai.mutex);
        if (!found) { 
            *line = (curr_y > 0) ? curr_y - 1 : 0; 
            *col = 0; 
        }
    } else if (type == PANEL_CHAT_INPUT) {
        ChatInputBuffer *cb = &state->ai.input;
        int vw = p->width - 2; if (vw < 1) vw = 1;
        int target_vrow = my - p->y - 1 + cb->scroll.scroll_y;
        if (target_vrow >= cb->scroll.content_height) {
            *line = cb->line_count - 1; *col = strlen(cb->lines[*line]);
            return;
        }
        int current_vrow = 0;
        for (int i=0; i < cb->line_count; i++) {
            int len = strlen(cb->lines[i]);
            int rows_this_line = (len + vw - 1) / vw;
            if (len == 0) rows_this_line = 1;
            if (target_vrow >= current_vrow && target_vrow < current_vrow + rows_this_line) {
                *line = i;
                *col = (target_vrow - current_vrow) * vw + (mx - p->x - 1);
                if (*col < 0) *col = 0; if (*col > len) *col = len;
                return;
            }
            current_vrow += rows_this_line;
        }
        *line = cb->line_count - 1; *col = strlen(cb->lines[*line]);
    } else { *line = -1; *col = -1; }
}

Selection get_normalized_selection(Selection *sel) {
    Selection n = *sel;
    if (n.start_line > n.end_line || (n.start_line == n.end_line && n.start_col > n.end_col)) {
        int tl = n.start_line, tc = n.start_col;
        n.start_line = n.end_line; n.start_col = n.end_col;
        n.end_line = tl; n.end_col = tc;
    }
    return n;
}

char* extract_selection(AppState *state, PanelType type) {
    Selection sel;
    if (type == PANEL_EDITOR) sel = get_normalized_selection(&state->editor.selection);
    else if (type == PANEL_AI) sel = get_normalized_selection(&state->ai.selection);
    else if (type == PANEL_TERMINAL) sel = get_normalized_selection(&state->terminal.base.selection);
    else if (type == PANEL_CHAT_INPUT) sel = get_normalized_selection(&state->ai.input.selection);
    else return NULL;

    if (!sel.active) return NULL;

    char *buf = malloc(1); buf[0] = '\0'; int len = 0;
    if (type == PANEL_EDITOR) {
        EditorBuffer *eb = &state->editor;
        for (int i = sel.start_line; i <= sel.end_line; i++) {
            if (i < 0 || i >= eb->line_count) continue;
            int start = (i == sel.start_line) ? sel.start_col : 0;
            int end = (i == sel.end_line) ? sel.end_col : (int)strlen(eb->lines[i]);
            if (start < 0) start = 0; if (end > (int)strlen(eb->lines[i])) end = (int)strlen(eb->lines[i]);
            if (end < start) end = start;
            int chunk = end - start;
            buf = realloc(buf, len + chunk + 2);
            memcpy(buf + len, eb->lines[i] + start, chunk);
            len += chunk;
            if (i < sel.end_line) buf[len++] = '\n';
        }
    } else if (type == PANEL_AI) {
        AIContext *ai = &state->ai;
        int mw = state->panels[PANEL_AI].width - 4; if (mw < 1) mw = 1;
        pthread_mutex_lock(&ai->mutex);
        int curr_y = 0;
        for (int i = 0; i < ai->message_count; i++) {
            AIMessage *m = &ai->messages[i];
            if (!m->content || !m->role || strcmp(m->role, "system") == 0) continue;
            char *content = strdup(m->content); char *saveptr; char *line = strtok_r(content, "\n", &saveptr);
            while (line) {
                char *curr = line;
                while (*curr) {
                    int clen = strlen(curr);
                    int split = (clen <= mw) ? clen : mw;
                    if (clen > mw) { int s = split; while (s > 0 && curr[s] != ' ') s--; if (s > 0) split = s; }
                    if (curr_y >= sel.start_line && curr_y <= sel.end_line) {
                        int start = (curr_y == sel.start_line) ? sel.start_col : 0;
                        int end = (curr_y == sel.end_line) ? sel.end_col : split;
                        if (start < 0) start = 0; if (end > split) end = split;
                        if (end < start) end = start;
                        int chunk = end - start;
                        buf = realloc(buf, len + chunk + 2);
                        memcpy(buf + len, curr + start, chunk);
                        len += chunk;
                        if (end == split || split == clen) buf[len++] = '\n';
                    }
                    curr_y++; curr += split; while (*curr == ' ') curr++;
                }
                line = strtok_r(NULL, "\n", &saveptr);
            }
            free(content);
        }
        pthread_mutex_unlock(&ai->mutex);
    } else if (type == PANEL_TERMINAL) {
        TerminalBuffer *tb = &state->terminal;
        for (int i = sel.start_line; i <= sel.end_line; i++) {
            if (i < 0 || i >= tb->base.line_count) continue;
            int start = (i == sel.start_line) ? sel.start_col : 0;
            int end = (i == sel.end_line) ? sel.end_col : (int)strlen(tb->base.lines[i]);
            if (start < 0) start = 0; if (end > (int)strlen(tb->base.lines[i])) end = (int)strlen(tb->base.lines[i]);
            if (end < start) end = start;
            int chunk = end - start;
            buf = realloc(buf, len + chunk + 2);
            memcpy(buf + len, tb->base.lines[i] + start, chunk);
            len += chunk;
            if (i < sel.end_line) buf[len++] = '\n';
        }
    } else if (type == PANEL_CHAT_INPUT) {
        ChatInputBuffer *cb = &state->ai.input;
        for (int i = sel.start_line; i <= sel.end_line; i++) {
            if (i < 0 || i >= cb->line_count) continue;
            int start = (i == sel.start_line) ? sel.start_col : 0;
            int end = (i == sel.end_line) ? sel.end_col : (int)strlen(cb->lines[i]);
            if (start < 0) start = 0; if (end > (int)strlen(cb->lines[i])) end = (int)strlen(cb->lines[i]);
            if (end < start) end = start;
            int chunk = end - start;
            buf = realloc(buf, len + chunk + 2);
            memcpy(buf + len, cb->lines[i] + start, chunk);
            len += chunk;
            if (i < sel.end_line) buf[len++] = '\n';
        }
    }
    buf[len] = '\0';
    return buf;
}

void app_copy(AppState *state) {
    char *text = extract_selection(state, state->active_panel);
    if (!text) return;
    if (state->clipboard.data) free(state->clipboard.data);
    state->clipboard.data = text;
    
    // System clipboard integration
    FILE *f = popen("xclip -selection clipboard", "w");
    if (f) { fputs(text, f); pclose(f); }
    else {
        f = popen("xsel -i -b", "w");
        if (f) { fputs(text, f); pclose(f); }
    }
    snprintf(state->last_action, 128, "Copied to Clipboard");
}

void app_cut(AppState *state) {
    if (state->active_panel != PANEL_EDITOR && state->active_panel != PANEL_CHAT_INPUT) return;
    app_copy(state);
    if (state->active_panel == PANEL_EDITOR) {
        EditorBuffer *eb = &state->editor;
        Selection sel = get_normalized_selection(&eb->selection);
        if (!sel.active) return;
        eb->cursor_x = sel.end_col; eb->cursor_y = sel.end_line;
        while (eb->cursor_y > sel.start_line || (eb->cursor_y == sel.start_line && eb->cursor_x > sel.start_col)) {
            delete_char(state);
        }
        eb->selection.active = 0;
    } else if (state->active_panel == PANEL_CHAT_INPUT) {
        ChatInputBuffer *cb = &state->ai.input;
        Selection sel = get_normalized_selection(&cb->selection);
        if (!sel.active) return;
        cb->cursor_x = sel.end_col; cb->cursor_y = sel.end_line;
        while (cb->cursor_y > sel.start_line || (cb->cursor_y == sel.start_line && cb->cursor_x > sel.start_col)) {
            buffer_delete_char(cb);
        }
        cb->selection.active = 0;
    }
    snprintf(state->last_action, 128, "Text Cut");
}

void app_paste(AppState *state) {
    char *text = NULL;
    FILE *f = popen("xclip -selection clipboard -o", "r");
    if (f) {
        char buffer[1024]; int len = 0;
        while (fgets(buffer, sizeof(buffer), f)) {
            int blen = strlen(buffer);
            text = realloc(text, len + blen + 1);
            strcpy(text + len, buffer);
            len += blen;
        }
        pclose(f);
    }
    if (!text && state->clipboard.data) text = strdup(state->clipboard.data);
    if (!text) return;

    if (state->active_panel == PANEL_EDITOR) {
        for (char *c = text; *c; c++) {
            if (*c == '\n') insert_newline(state);
            else insert_char(state, *c);
        }
    } else if (state->active_panel == PANEL_TERMINAL) {
        if (write(state->terminal.master_fd, text, strlen(text)) < 0) {}
    } else if (state->active_panel == PANEL_CHAT_INPUT || state->active_panel == PANEL_AI) {
        ChatInputBuffer *cb = &state->ai.input;
        for (char *c = text; *c; c++) {
            if (*c == '\n') buffer_insert_newline(cb);
            else buffer_insert_char(cb, *c);
        }
    }
    free(text);
    snprintf(state->last_action, 128, "Pasted");
}

void app_select_all(AppState *state) {
    if (state->active_panel == PANEL_EDITOR) {
        EditorBuffer *eb = &state->editor;
        eb->selection.active = 1; eb->selection.start_line = 0; eb->selection.start_col = 0;
        eb->selection.end_line = eb->line_count - 1; 
        eb->selection.end_col = (eb->line_count > 0) ? strlen(eb->lines[eb->line_count-1]) : 0;
    } else if (state->active_panel == PANEL_TERMINAL) {
        TerminalBuffer *tb = &state->terminal;
        tb->base.selection.active = 1; tb->base.selection.start_line = 0; tb->base.selection.start_col = 0;
        tb->base.selection.end_line = tb->base.line_count - 1;
        tb->base.selection.end_col = (tb->base.line_count > 0) ? strlen(tb->base.lines[tb->base.line_count-1]) : 0;
    } else if (state->active_panel == PANEL_AI) {
        state->ai.selection.active = 1; state->ai.selection.start_line = 0; state->ai.selection.start_col = 0;
        state->ai.selection.end_line = state->ai.scroll.content_height; state->ai.selection.end_col = 0;
    } else if (state->active_panel == PANEL_CHAT_INPUT) {
        ChatInputBuffer *cb = &state->ai.input;
        cb->selection.active = 1; cb->selection.start_line = 0; cb->selection.start_col = 0;
        cb->selection.end_line = cb->line_count - 1;
        cb->selection.end_col = (cb->line_count > 0) ? strlen(cb->lines[cb->line_count-1]) : 0;
    }
}

typedef struct {
    char *label;
    char *shortcut;
} MenuItem;

void draw_menu(WINDOW *win, MenuItem *items, int count, int selected) {
    for (int i = 0; i < count; i++) {
        if (i == selected) wattron(win, A_REVERSE);
        mvwprintw(win, i + 1, 2, "%-20s %s", items[i].label, items[i].shortcut);
        if (i == selected) wattroff(win, A_REVERSE);
    }
}

void show_help_menu(AppState *state) {
    int count = 0;
    while (key_bindings[count].cmd != CMD_NONE) count++;
    
    // Convert key_bindings to MenuItems
    MenuItem *items = malloc(sizeof(MenuItem) * (count + 1));
    for (int i = 0; i < count; i++) {
        items[i].label = (char*)key_bindings[i].label;
        items[i].shortcut = (char*)key_bindings[i].shortcut_str;
    }
    items[count].label = "Close Menu";
    items[count].shortcut = "ESC";
    count++;

    int selected = 0;
    int max_y, max_x; getmaxyx(stdscr, max_y, max_x);
    int menu_h = count + 2;
    int menu_w = 40;
    WINDOW *win = newwin(menu_h, menu_w, (max_y - menu_h) / 2, (max_x - menu_w) / 2);
    keypad(win, TRUE);

    while (1) {
        box(win, 0, 0);
        draw_menu(win, items, count, selected);
        wrefresh(win);
        int ch = wgetch(win);
        if (ch == KEY_UP) { if (selected > 0) selected--; }
        else if (ch == KEY_DOWN) { if (selected < count - 1) selected++; }
        else if (ch == 10 || ch == '\r' || ch == '\n') {
            if (selected == count - 1) break; 
            execute_command(state, key_bindings[selected].cmd);
            break;
        }
        else if (ch == 27) break;
    }
    delwin(win); free(items); clear(); calculate_layout(state);
}

void execute_command(AppState *state, CommandType cmd) {
    switch (cmd) {
        case CMD_SAVE: editor_save(state); break;
        case CMD_SAVE_AS: {
            WINDOW* win = newwin(3, 60, 5, 10); box(win, 0, 0); mvwprintw(win, 1, 2, "Save As: ");
            char path[512] = {0}; curs_set(1); echo(); wgetnstr(win, path, 511); noecho(); curs_set(0); delwin(win);
            if (strlen(path) > 0) buffer_save_file(&state->editor, path);
            clear(); calculate_layout(state);
        } break;
        case CMD_FIND: app_find(state); break;
        case CMD_FIND_NEXT: app_find_next(state); break;
        case CMD_REPLACE: app_replace(state); break;
        case CMD_REPLACE_ALL: app_replace_all(state); break;
        case CMD_SELECT_ALL: app_select_all(state); break;
        case CMD_COPY: app_copy(state); break;
        case CMD_CUT: app_cut(state); break;
        case CMD_PASTE: app_paste(state); break;
        case CMD_TOGGLE_TREE: state->panels[PANEL_FILE_TREE].visible = !state->panels[PANEL_FILE_TREE].visible; calculate_layout(state); break;
        case CMD_TOGGLE_EDITOR: state->panels[PANEL_EDITOR].visible = !state->panels[PANEL_EDITOR].visible; calculate_layout(state); break;
        case CMD_TOGGLE_AI: state->panels[PANEL_AI].visible = !state->panels[PANEL_AI].visible; calculate_layout(state); break;
        case CMD_TOGGLE_TERMINAL: state->panels[PANEL_TERMINAL].visible = !state->panels[PANEL_TERMINAL].visible; calculate_layout(state); break;
        case CMD_QUIT: state->running = false; break;
        case CMD_MENU: show_help_menu(state); break;
        case CMD_RUN_PYTHON:
            if (strcmp(state->editor.filepath, "[No File]") != 0) {
                editor_save(state);
                char cmd_str[1024]; snprintf(cmd_str, sizeof(cmd_str), "python3 %s\n", state->editor.filepath);
                if (write(state->terminal.master_fd, cmd_str, strlen(cmd_str)) < 0) {}
                state->panels[PANEL_TERMINAL].visible = true; calculate_layout(state);
                state->active_panel = PANEL_TERMINAL;
                snprintf(state->last_action, 128, "Run: %s", state->editor.filepath);
            }
            break;
        default: break;
    }
}


bool app_modal_input(WINDOW *win, char *buf, int max_len, int y, int x) {
    int pos = strlen(buf);
    curs_set(1);
    noecho();
    keypad(win, TRUE);
    wtimeout(win, -1);

    while (1) {
        // Draw the current buffer content
        // Ensure we don't overflow the window width (simple horizontal scroll/clip)
        int ww = getmaxx(win);
        int avail = ww - x - 1;
        if (avail < 1) avail = 1;

        int start_disp = (pos >= avail) ? pos - avail + 1 : 0;
        mvwprintw(win, y, x, "%-*.*s", avail, avail, buf + start_disp);
        wmove(win, y, x + (pos - start_disp));
        wrefresh(win);

        int ch = wgetch(win);
        if (ch == 27) return false; // Escape
        if (ch == '\n' || ch == KEY_ENTER) return true; // Enter
        if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (pos > 0) {
                buf[--pos] = '\0';
            }
        } else if (ch >= 32 && ch <= 126 && pos < max_len - 1) {
            buf[pos++] = (char)ch;
            buf[pos] = '\0';
        } else if (ch == KEY_MOUSE) {
            continue; // Ignore mouse
        }
    }
}


void app_find(AppState *state) {
    int max_y, max_x; getmaxyx(stdscr, max_y, max_x);
    WINDOW *win = newwin(5, 60, (max_y - 5) / 2, (max_x - 60) / 2);
    keypad(win, TRUE);
    box(win, 0, 0);
    mvwprintw(win, 1, 2, "Find:");
    wrefresh(win);

    char term[256] = {0};
    bool ok = app_modal_input(win, term, 255, 2, 2);
    
    delwin(win);
    clear();
    calculate_layout(state);
    curs_set(0);
    
    if (!ok || strlen(term) == 0) return;
    strncpy(state->last_search, term, sizeof(state->last_search) - 1);

    EditorBuffer *eb = &state->editor;
    int start_y = eb->cursor_y;
    int start_x = eb->cursor_x + 1;

    for (int i = 0; i < eb->line_count; i++) {
        int y = (start_y + i) % eb->line_count;
        char *line = eb->lines[y];
        char *found = NULL;
        if (i == 0) {
            if (start_x < (int)strlen(line)) found = strcasestr(line + start_x, term);
        } else {
            found = strcasestr(line, term);
        }

        if (found) {
            eb->cursor_y = y;
            eb->cursor_x = found - line;
            // Center the found line in the viewport
            eb->scroll.scroll_y = eb->cursor_y - eb->scroll.viewport_height / 2;
            clamp_scroll(&eb->scroll);
            snprintf(state->last_action, 128, "Found: %s", term);
            return;
        }
    }
    snprintf(state->last_action, 128, "Not found: %s", term);
}

void app_find_next(AppState *state) {
    if (strlen(state->last_search) == 0) {
        snprintf(state->last_action, 128, "No previous search term");
        return;
    }

    EditorBuffer *eb = &state->editor;
    int start_y = eb->cursor_y;
    int start_x = eb->cursor_x + 1;

    for (int i = 0; i < eb->line_count; i++) {
        int y = (start_y + i) % eb->line_count;
        char *line = eb->lines[y];
        char *found = NULL;
        if (i == 0) {
            if (start_x < (int)strlen(line)) found = strcasestr(line + start_x, state->last_search);
        } else {
            found = strcasestr(line, state->last_search);
        }

        if (found) {
            eb->cursor_y = y;
            eb->cursor_x = found - line;
            eb->scroll.scroll_y = eb->cursor_y - eb->scroll.viewport_height / 2;
            clamp_scroll(&eb->scroll);
            snprintf(state->last_action, 128, "Found next: %s", state->last_search);
            return;
        }
    }
    snprintf(state->last_action, 128, "No more matches: %s", state->last_search);
}

void app_replace(AppState *state) {
    int max_y, max_x; getmaxyx(stdscr, max_y, max_x);
    WINDOW *win = newwin(6, 60, (max_y - 6) / 2, (max_x - 60) / 2);
    box(win, 0, 0);
    mvwprintw(win, 1, 2, "Search for:");
    mvwprintw(win, 3, 2, "Replace with:");
    
    char term[256] = {0};
    char rep[256] = {0};

    if (!app_modal_input(win, term, 255, 2, 2)) {
        delwin(win); clear(); calculate_layout(state); curs_set(0); return;
    }
    if (!app_modal_input(win, rep, 255, 4, 2)) {
        delwin(win); clear(); calculate_layout(state); curs_set(0); return;
    }
    
    delwin(win); clear(); calculate_layout(state); curs_set(0);

    if (strlen(term) == 0) return;
    strncpy(state->last_search, term, sizeof(state->last_search)-1);
    strncpy(state->last_replace, rep, sizeof(state->last_replace)-1);

    EditorBuffer *eb = &state->editor;
    int start_y = eb->cursor_y;
    int start_x = eb->cursor_x;

    for (int i = 0; i < eb->line_count; i++) {
        int y = (start_y + i) % eb->line_count;
        char *line = eb->lines[y];
        char *found = NULL;
        if (i == 0) found = strcasestr(line + start_x, term);
        else found = strcasestr(line, term);

        if (found) {
            int x = found - line;
            buffer_replace_text(eb, y, x, strlen(term), rep);
            eb->cursor_y = y;
            eb->cursor_x = x + strlen(rep);
            eb->scroll.scroll_y = eb->cursor_y - eb->scroll.viewport_height / 2;
            clamp_scroll(&eb->scroll);
            snprintf(state->last_action, 128, "Replaced match for: %s", term);
            return;
        }
    }
    snprintf(state->last_action, 128, "Not found to replace: %s", term);
}

void app_replace_all(AppState *state) {
    int max_y, max_x; getmaxyx(stdscr, max_y, max_x);
    WINDOW *win = newwin(6, 60, (max_y - 6) / 2, (max_x - 60) / 2);
    box(win, 0, 0);
    mvwprintw(win, 1, 2, "Search for (Replace ALL):");
    mvwprintw(win, 3, 2, "Replace with:");
    
    char term[256] = {0};
    char rep[256] = {0};

    if (!app_modal_input(win, term, 255, 2, 2)) {
        delwin(win); clear(); calculate_layout(state); curs_set(0); return;
    }
    if (!app_modal_input(win, rep, 255, 4, 2)) {
        delwin(win); clear(); calculate_layout(state); curs_set(0); return;
    }
    
    delwin(win); clear(); calculate_layout(state); curs_set(0);

    if (strlen(term) == 0) return;
    strncpy(state->last_search, term, sizeof(state->last_search)-1);
    strncpy(state->last_replace, rep, sizeof(state->last_replace)-1);

    EditorBuffer *eb = &state->editor;
    int replacements = 0;
    for (int y = 0; y < eb->line_count; y++) {
        int x = 0;
        char *found;
        while ((found = strcasestr(eb->lines[y] + x, term))) {
            int pos = found - eb->lines[y];
            buffer_replace_text(eb, y, pos, strlen(term), rep);
            x = pos + strlen(rep);
            replacements++;
            if (strlen(term) == 0) break; // Should not happen but safety
        }
    }
    snprintf(state->last_action, 128, "Replaced %d occurrences of: %s", replacements, term);
}

void init_app(AppState *state) {
    memset(state, 0, sizeof(AppState));
    for (int i=0; i<PANEL_COUNT; i++) { state->panels[i].visible = true; }

    state->panels[PANEL_FILE_TREE].name = "Files"; state->panels[PANEL_EDITOR].name = "Editor";
    state->panels[PANEL_AI].name = "Chat"; state->panels[PANEL_TERMINAL].name = "Terminal";
    state->active_panel = PANEL_FILE_TREE; state->running = true; state->last_action[0] = '\0';
    state->root = create_file_node(".", ".", true, 0); state->root->is_expanded = true; load_directory(state->root);
    struct stat dst; if (stat(".", &dst) == 0) state->last_dir_modified = dst.st_mtime;
    state->tree_selection = 0; memset(&state->tree_scroll, 0, sizeof(ScrollState));
    state->editor.lines = malloc(sizeof(char*)); state->editor.lines[0] = strdup(""); state->editor.line_count = 1;
    state->editor.cursor_x = 0; state->editor.cursor_y = 0; memset(&state->editor.scroll, 0, sizeof(ScrollState));
    strcpy(state->editor.filepath, "[No File]");
    state->ai.messages = NULL; state->ai.message_count = 0; memset(&state->ai.scroll, 0, sizeof(ScrollState)); state->ai.is_waiting = 0;
    state->ai.is_waiting_approval = false; state->ai.pending_content = NULL; state->ai.diff_text = NULL;
    state->ai.stream_buffer = NULL; state->ai.stream_buffer_len = 0;
    memset(&state->editor.selection, 0, sizeof(Selection));
    memset(&state->terminal.base.selection, 0, sizeof(Selection));
    memset(&state->ai.selection, 0, sizeof(Selection));
    state->ai.input.lines = malloc(sizeof(char*)); state->ai.input.lines[0] = strdup(""); state->ai.input.line_count = 1;
    state->ai.input.cursor_x = 0; state->ai.input.cursor_y = 0; memset(&state->ai.input.scroll, 0, sizeof(ScrollState));
    memset(&state->ai.input.selection, 0, sizeof(Selection));
    state->clipboard.data = NULL;
    state->mouse_dragging = false; state->drag_start_panel = PANEL_FILE_TREE;
    pthread_mutex_init(&state->ai.mutex, NULL); init_terminal(state); curl_global_init(CURL_GLOBAL_ALL);
    mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, NULL); mouseinterval(0);
    printf("\033[?1003h"); fflush(stdout); // Enable all mouse tracking for smoother drag
}


void app_show_approval_dialog(AppState *state) {
    int my, mx; getmaxyx(stdscr, my, mx);
    int hw = my / 2; if (hw < 10) hw = 10; if (hw > my - 4) hw = my - 4;
    int ww = mx / 2; if (ww < 40) ww = 40; if (ww > mx - 4) ww = mx - 4;
    int wy = (my - hw) / 2;
    int wx = (mx - ww) / 2;

    WINDOW *win = newwin(hw, ww, wy, wx);
    keypad(win, TRUE);
    box(win, 0, 0);
    wattron(win, A_BOLD | COLOR_PAIR(1));
    mvwprintw(win, 0, 2, " [ AI Proposed Changes ] ");
    wattroff(win, A_BOLD | COLOR_PAIR(1));

    int dlines = 0;
    if (state->ai.diff_text) {
        char *diff_copy = strdup(state->ai.diff_text);
        char *sp;
        char *lc = strtok_r(diff_copy, "\n", &sp);
        while (lc) { dlines++; lc = strtok_r(NULL, "\n", &sp); }
        free(diff_copy);
    }

    int scroll_y = 0;
    int view_h = hw - 2;

    while (1) {
        // Clear inner window area
        for (int i=1; i<hw-1; i++) {
            mvwhline(win, i, 1, ' ', ww - 2);
        }

        if (state->ai.diff_text) {
            char *dt = strdup(state->ai.diff_text);
            char *spd;
            char *l = strtok_r(dt, "\n", &spd);
            int current_line = 0;
            int draw_line = 1;

            while (l && draw_line < hw - 1) {
                if (current_line >= scroll_y) {
                    if (l[0] == '+') wattron(win, COLOR_PAIR(1));
                    else if (l[0] == '-') wattron(win, A_DIM);
                    mvwprintw(win, draw_line, 1, "%.*s", ww - 2, l);
                    if (l[0] == '+' || l[0] == '-') wattroff(win, COLOR_PAIR(1) | A_DIM);
                    draw_line++;
                }
                l = strtok_r(NULL, "\n", &spd);
                current_line++;
            }
            free(dt);
        }
        
        box(win, 0, 0);
        wattron(win, A_BOLD | COLOR_PAIR(1));
        mvwprintw(win, 0, 2, " [ AI Proposed Changes ] ");
        wattroff(win, A_BOLD | COLOR_PAIR(1));
        mvwprintw(win, hw-1, (ww - 22) / 2, " [y] Accept  [n] Reject ");
        wrefresh(win);

        int ch = wgetch(win);
        if (ch == 'y' || ch == 'Y') {
            apply_pending_edit(state, true);
            break;
        } else if (ch == 'n' || ch == 'N' || ch == 27) {
            apply_pending_edit(state, false);
            break;
        } else if (ch == KEY_UP && scroll_y > 0) {
            scroll_y--;
        } else if (ch == KEY_DOWN && scroll_y < dlines - view_h + 1) {
            scroll_y++;
        } else if (ch == KEY_MOUSE) {
            continue;
        }
    }

    delwin(win);
    calculate_layout(state);
    refresh();
}


void handle_input(AppState *state) {
    if (state->ai.is_waiting_approval) {
        app_show_approval_dialog(state);
        return;
    }

    timeout(10); int ch = getch(); if (ch == ERR) return; MEVENT ev;
    
    bool shift = false; int base_ch = ch;
    if (ch == KEY_SLEFT || ch == KEY_SRIGHT || ch == KEY_SR || ch == KEY_SF) {
        shift = true;
        if (ch == KEY_SLEFT) base_ch = KEY_LEFT; else if (ch == KEY_SRIGHT) base_ch = KEY_RIGHT;
        else if (ch == KEY_SR) base_ch = KEY_UP; else if (ch == KEY_SF) base_ch = KEY_DOWN;
    }

    if (ch == 27) { // ESC
        timeout(25); int n = getch();
        if (n == ERR) {
            Selection *sel = get_panel_selection(state, state->active_panel);
            if (sel) sel->active = 0;
            state->mouse_dragging = false; state->button1_down = false;
            snprintf(state->last_action, 128, "Selection Cleared");
            return;
        }
        ungetch(n);
    }

    // Dispatch standard key bindings
    for (int i = 0; key_bindings[i].cmd != CMD_NONE; i++) {
        const KeyBinding *b = &key_bindings[i];
        if (b->key == ch) {
            if (ch == 27) { // Potential Alt combination
                int next = wgetch(stdscr);
                if (next == b->alt_key) {
                    execute_command(state, b->cmd);
                    return;
                } else {
                    ungetch(next);
                }
            } else {
                execute_command(state, b->cmd);
                return;
            }
        }
    }


    if (ch == KEY_MOUSE && getmouse(&ev) == OK) {

        if (ev.bstate & (BUTTON1_PRESSED | BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED)) {
            struct timeval tv; gettimeofday(&tv, NULL);
            long long now_ms = (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
            bool is_double = false;
            
            for (int i=0; i<PANEL_COUNT; i++) {
                Panel *p = &state->panels[i];
                if (p->visible && ev.x>=p->x && ev.x<p->x+p->width && ev.y>=p->y && ev.y<p->y+p->height) {
                    state->active_panel = (PanelType)i;
                    int ml, mc; map_mouse_to_panel(state, (PanelType)i, ev.x, ev.y, &ml, &mc);
                    if (ml != -1) {
                        if (now_ms - state->last_click_ms < 300 && state->last_click_panel == (PanelType)i && state->last_click_line == ml) {
                            is_double = true;
                        }
                        state->last_click_ms = now_ms; state->last_click_line = ml; state->last_click_panel = (PanelType)i;

                        state->press_line = ml; state->press_col = mc; state->press_panel = (PanelType)i;
                        if (i == PANEL_FILE_TREE) {
                            FileNode *fl[1000]; int count=0; flatten_tree(state->root, fl, &count, 1000);
                            int idx = ml;
                            if (idx >= 0 && idx < count) {
                                if (is_double) {
                                     FileNode *s = fl[idx];
                                     if (s->is_dir) { s->is_expanded = !s->is_expanded; if (s->is_expanded && s->child_count == 0) load_directory(s); }
                                     else { load_file(state, s->path); state->active_panel = PANEL_EDITOR; }
                                     state->last_click_ms = 0; // Prevent triple-click
                                }
                                state->tree_selection = idx;
                            }
                        } else {
                            Selection *sel = get_panel_selection(state, (PanelType)i);
                            if (sel) sel->active = 0;
                            if (i == PANEL_EDITOR) { state->editor.cursor_y = ml; state->editor.cursor_x = mc; }
                            else if (i == PANEL_CHAT_INPUT) { state->ai.input.cursor_y = ml; state->ai.input.cursor_x = mc; }
                        }
                    }
                    state->button1_down = true; 
                    state->mouse_dragging = false; state->drag_start_panel = (PanelType)i;
                    break;
                }
            }
        }

        if (ev.bstate & REPORT_MOUSE_POSITION || ev.bstate == 0) {
            if (state->button1_down) {
                int ml, mc; map_mouse_to_panel(state, state->drag_start_panel, ev.x, ev.y, &ml, &mc);
                if (ml != -1) {
                    if (!state->mouse_dragging && (ml != state->press_line || mc != state->press_col)) {
                        state->mouse_dragging = true;
                        Selection *sel = get_panel_selection(state, state->drag_start_panel);
                        if (sel) { sel->active = 1; sel->start_line = state->press_line; sel->start_col = state->press_col; }
                    }
                    if (state->mouse_dragging) {
                        Selection *sel = get_panel_selection(state, state->drag_start_panel);
                        if (sel) { sel->end_line = ml; sel->end_col = mc; }
                        if (state->drag_start_panel == PANEL_EDITOR) { state->editor.cursor_y = ml; state->editor.cursor_x = mc; }
                        else if (state->drag_start_panel == PANEL_CHAT_INPUT) { state->ai.input.cursor_y = ml; state->ai.input.cursor_x = mc; }
                    }
                }
            }
        }

        if (ev.bstate & BUTTON1_RELEASED) {
            state->button1_down = false; state->mouse_dragging = false;
        }

        for (int i=0; i<PANEL_COUNT; i++) {
            Panel *p = &state->panels[i];
            if (p->visible && ev.x>=p->x && ev.x<p->x+p->width && ev.y>=p->y && ev.y<p->y+p->height) {
                ScrollState *ss = get_panel_scroll(state, (PanelType)i);
                if (ev.bstate & 0x10000 || ev.bstate & 0x800) { if (ss) ss->scroll_y -= 3; } 
                else if (ev.bstate & 0x200000 || ev.bstate & 0x8000000) { if (ss) ss->scroll_y += 3; } 
                clamp_scroll(ss);
                break;
            }
        }
    } else {
        if (state->active_panel == PANEL_FILE_TREE) handle_tree_input(state, ch);
        else if (state->active_panel == PANEL_EDITOR) handle_editor_input(state, ch, base_ch, shift);
        else if (state->active_panel == PANEL_CHAT_INPUT) handle_chat_input(state, ch, base_ch, shift);
        else if (state->active_panel == PANEL_AI) {
            ScrollState *ss = &state->ai.scroll;
            if (ch == KEY_UP) ss->scroll_y--; else if (ch == KEY_DOWN) ss->scroll_y++;
            else if (ch == KEY_PPAGE) ss->scroll_y -= ss->viewport_height;
            else if (ch == KEY_NPAGE) ss->scroll_y += ss->viewport_height;
            clamp_scroll(ss);
        } else if (state->active_panel == PANEL_TERMINAL) handle_terminal_input(state, ch);
    }
}
