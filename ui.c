#define _GNU_SOURCE
#include "cursory.h"

// Prototypes
ScrollState* get_panel_scroll(AppState *state, PanelType type);
Selection* get_panel_selection(AppState *state, PanelType type);
void map_mouse_to_panel(AppState *state, PanelType type, int mx, int my, int *line, int *col);
Selection get_normalized_selection(Selection *sel);
char* extract_selection(AppState *state, PanelType type);
void draw_chat_input(AppState *state, Panel *p);
void draw_ai_chat(AppState *state, Panel *p);
void handle_chat_input(AppState *state, int ch);
void clamp_scroll(ScrollState *s);
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

// --- File Tree ---
FileNode* create_file_node(const char *name, const char *path, bool is_dir, int depth) {
    FileNode *node = malloc(sizeof(FileNode)); node->name = strdup(name); node->path = strdup(path); node->is_dir = is_dir; node->is_expanded = false; node->children = NULL; node->child_count = 0; node->depth = depth; return node;
}
void free_file_tree(FileNode *node) {
    if (!node) return;
    for (int i = 0; i < node->child_count; i++) free_file_tree(node->children[i]);
    free(node->children); free(node->name); free(node->path); free(node);
}
void load_directory(FileNode *node) {
    if (!node->is_dir) return; 
    DIR *dir = opendir(node->path); if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char full_path[1024]; snprintf(full_path, sizeof(full_path), "%s/%s", node->path, entry->d_name); struct stat st; stat(full_path, &st);
        node->children = realloc(node->children, sizeof(FileNode*) * (node->child_count + 1)); node->children[node->child_count++] = create_file_node(entry->d_name, full_path, S_ISDIR(st.st_mode), node->depth + 1);
    }
    closedir(dir);
}
int flatten_tree(FileNode *node, FileNode **flat_list, int *count, int max_count) {
    if (!node || *count >= max_count) return *count;
    if (node->depth > 0) flat_list[(*count)++] = node;
    if (node->is_expanded) {
        for (int i=0; i<node->child_count; i++) flatten_tree(node->children[i], flat_list, count, max_count);
    }
    return *count;
}


// --- Editor ---
void free_buffer(EditorBuffer *buf) {
    if (buf->lines) { for (int i = 0; i < buf->line_count; i++) free(buf->lines[i]); free(buf->lines); buf->lines = NULL; } buf->line_count = 0; buf->cursor_x = 0; buf->cursor_y = 0; buf->scroll_x = 0; memset(&buf->scroll, 0, sizeof(ScrollState));
}

void load_file(AppState *state, const char *path) {
    free_buffer(&state->editor); snprintf(state->editor.filepath, sizeof(state->editor.filepath), "%s", path); 
    FILE *f = fopen(path, "r");
    if (!f) {
        state->editor.lines = malloc(sizeof(char*)); state->editor.lines[0] = strdup(""); state->editor.line_count = 1;
        return;
    }
    struct stat st; if (stat(path, &st) == 0) state->editor.last_modified = st.st_mtime;
    char *line = NULL; size_t len = 0; ssize_t read;
    while ((read = getline(&line, &len, f)) != -1) {
        if (read > 0 && line[read-1] == '\n') line[read-1] = '\0';
        state->editor.lines = realloc(state->editor.lines, sizeof(char*) * (state->editor.line_count + 1)); state->editor.lines[state->editor.line_count++] = strdup(line);
    }
    if (state->editor.line_count == 0) {
        state->editor.lines = malloc(sizeof(char*)); state->editor.lines[0] = strdup(""); state->editor.line_count = 1;
    }
    free(line); fclose(f);
}

void save_file(AppState *state) {
    EditorBuffer *eb = &state->editor;
    if (eb->filepath[0] == '\0' || strcmp(eb->filepath, "[No File]") == 0) return;
    FILE *f = fopen(eb->filepath, "w");
    if (!f) {
        pthread_mutex_lock(&state->ai.mutex);
        snprintf(state->last_action, 128, "Error: Save failed");
        pthread_mutex_unlock(&state->ai.mutex);
    } else {

        for (int i=0; i<eb->line_count; i++) fprintf(f, "%s\n", eb->lines[i]);
        fclose(f);
        struct stat st; if (stat(eb->filepath, &st) == 0) eb->last_modified = st.st_mtime;
        snprintf(state->last_action, sizeof(state->last_action), "Saved: %s", eb->filepath);
        if (state->root) {
            free_file_tree(state->root);
            state->root = create_file_node(".", ".", true, 0);
            state->root->is_expanded = true;
            load_directory(state->root);
            struct stat dst;
            if (stat(".", &dst) == 0) state->last_dir_modified = dst.st_mtime;
        }
    }
}

void insert_char(AppState *state, int ch) {
    EditorBuffer *eb = &state->editor; if (eb->line_count == 0) { eb->lines = malloc(sizeof(char*)); eb->lines[0] = strdup(""); eb->line_count = 1; }
    char *l = eb->lines[eb->cursor_y]; int len = strlen(l);
    eb->lines[eb->cursor_y] = realloc(eb->lines[eb->cursor_y], len + 2);
    memmove(eb->lines[eb->cursor_y] + eb->cursor_x + 1, eb->lines[eb->cursor_y] + eb->cursor_x, len - eb->cursor_x + 1);
    eb->lines[eb->cursor_y][eb->cursor_x++] = (char)ch;
}

void delete_char(AppState *state) {
    EditorBuffer *eb = &state->editor; if (eb->line_count == 0 || (eb->cursor_x == 0 && eb->cursor_y == 0)) return;
    if (eb->cursor_x > 0) {
        char *l = eb->lines[eb->cursor_y]; int len = strlen(l);
        memmove(l + eb->cursor_x - 1, l + eb->cursor_x, len - eb->cursor_x + 1); eb->cursor_x--;
    } else {
        char *prev = eb->lines[eb->cursor_y-1]; char *curr = eb->lines[eb->cursor_y];
        int plen = strlen(prev); int clen = strlen(curr);
        eb->lines[eb->cursor_y-1] = realloc(eb->lines[eb->cursor_y-1], plen + clen + 1);
        strcat(eb->lines[eb->cursor_y-1], curr); eb->cursor_x = plen;
        free(curr); memmove(eb->lines + eb->cursor_y, eb->lines + eb->cursor_y + 1, sizeof(char*) * (eb->line_count - eb->cursor_y - 1));
        eb->line_count--; eb->cursor_y--;
    }
}

void insert_newline(AppState *state) {
    EditorBuffer *eb = &state->editor; if (eb->line_count == 0) { eb->lines = malloc(sizeof(char*)); eb->lines[0] = strdup(""); eb->line_count = 1; }
    char *curr = eb->lines[eb->cursor_y];

    char *next = strdup(curr + eb->cursor_x); curr[eb->cursor_x] = '\0';
    eb->lines = realloc(eb->lines, sizeof(char*) * (eb->line_count + 1));
    memmove(eb->lines + eb->cursor_y + 2, eb->lines + eb->cursor_y + 1, sizeof(char*) * (eb->line_count - eb->cursor_y - 1));
    eb->lines[eb->cursor_y+1] = next; eb->line_count++; eb->cursor_y++; eb->cursor_x = 0;
}

void editor_save(AppState *state) { save_file(state); }

void draw_editor(AppState *state, Panel *p) {
    EditorBuffer *eb = &state->editor; int vh = p->height-2; int vw = p->width-2;
    eb->scroll.viewport_height = vh; eb->scroll.content_height = eb->line_count;

    if (eb->cursor_y < eb->scroll.scroll_y) eb->scroll.scroll_y = eb->cursor_y;
    if (eb->cursor_y >= eb->scroll.scroll_y + vh) eb->scroll.scroll_y = eb->cursor_y - vh + 1;
    clamp_scroll(&eb->scroll);
    if (eb->cursor_x < eb->scroll_x) eb->scroll_x = eb->cursor_x;
    if (eb->cursor_x >= eb->scroll_x + vw) eb->scroll_x = eb->cursor_x - vw + 1;
    if (eb->scroll_x < 0) eb->scroll_x = 0;

    for (int i=0; i<vh; i++) {
        int ly = eb->scroll.scroll_y + i;
        if (ly>=0 && ly<eb->line_count && eb->lines[ly]) {
            char *line = eb->lines[ly];
            int len = strlen(line);
            move(p->y + 1 + i, p->x + 1);
            for (int x = 0; x < vw && (x + eb->scroll_x) < len; x++) {
                int lx = x + eb->scroll_x;
                bool selected = false;
                if (eb->selection.active) {
                    Selection n = get_normalized_selection(&eb->selection);
                    if (ly > n.start_line && ly < n.end_line) selected = true;
                    else if (ly == n.start_line && ly == n.end_line) selected = (lx >= n.start_col && lx < n.end_col);
                    else if (ly == n.start_line) selected = (lx >= n.start_col);
                    else if (ly == n.end_line) selected = (lx < n.end_col);
                }
                if (selected) attron(A_REVERSE);
                addch(line[lx]);
                if (selected) attroff(A_REVERSE);
            }
        }
    }
}


// --- Terminal ---
void init_terminal(AppState *state) {
    state->terminal.line_count = 0; state->terminal.lines = NULL; memset(&state->terminal.scroll, 0, sizeof(ScrollState)); state->terminal.shell_pid = forkpty(&state->terminal.master_fd, NULL, NULL, NULL);
    if (state->terminal.shell_pid == 0) { setenv("TERM", "vt100", 1); execlp("/bin/bash", "bash", NULL); exit(1); }
    fcntl(state->terminal.master_fd, F_SETFL, fcntl(state->terminal.master_fd, F_GETFL) | O_NONBLOCK);
    state->terminal.lines = malloc(sizeof(char*)); state->terminal.lines[0] = strdup(""); state->terminal.line_count = 1;
}

void update_terminal(AppState *state) {
    char buf[4012]; ssize_t n; static int esc = 0;
    while ((n = read(state->terminal.master_fd, buf, sizeof(buf)-1)) > 0) {
        buf[n] = '\0';
        for (int i=0; i<n; i++) {
            if (esc == 0) {
                if (buf[i] == '\x1b') esc = 1; 
                else if (buf[i] == '\n') { 
                    char **tmp = realloc(state->terminal.lines, sizeof(char*)*(++state->terminal.line_count));
                    if (tmp) { state->terminal.lines = tmp; state->terminal.lines[state->terminal.line_count-1] = strdup(""); }
                }
                else if (buf[i] == 8 || buf[i] == 127) {
                    int last = state->terminal.line_count-1;
                    if (last >= 0 && state->terminal.lines[last]) {
                        int len = strlen(state->terminal.lines[last]);
                        if (len > 0) state->terminal.lines[last][len-1] = '\0';
                    }
                }
                else if (buf[i] >= 32 || buf[i] == '\t') { 
                    int last = state->terminal.line_count-1;
                    if (last >= 0 && state->terminal.lines[last]) {
                        int len = strlen(state->terminal.lines[last]); 
                        char *tmp = realloc(state->terminal.lines[last], len+2); 
                        if (tmp) { state->terminal.lines[last] = tmp; state->terminal.lines[last][len] = buf[i]; state->terminal.lines[last][len+1] = '\0'; }
                    }
                }
            } else if (esc == 1) esc = (buf[i] == '[') ? 2 : 0; else if (esc == 2) if (buf[i] >= '@' && buf[i] <= '~') esc = 0;
        }
        int vh = state->terminal.scroll.viewport_height > 0 ? state->terminal.scroll.viewport_height : (state->panels[PANEL_TERMINAL].height - 2);
        state->terminal.scroll.content_height = state->terminal.line_count;
        state->terminal.scroll.scroll_y = state->terminal.line_count - vh;
        clamp_scroll(&state->terminal.scroll);
    }
}


void handle_terminal_input(AppState *state, int ch) {
    bool shift = (ch == KEY_SR || ch == KEY_SF || ch == KEY_SLEFT || ch == KEY_SRIGHT);
    int base_ch = ch;
    if (shift) switch(ch) { case KEY_SR: base_ch=KEY_UP; break; case KEY_SF: base_ch=KEY_DOWN; break; case KEY_SLEFT: base_ch=KEY_LEFT; break; case KEY_SRIGHT: base_ch=KEY_RIGHT; break; }

    if (base_ch == KEY_PPAGE) { state->terminal.scroll.scroll_y -= state->terminal.scroll.viewport_height/2; } 
    else if (base_ch == KEY_NPAGE) { state->terminal.scroll.scroll_y += state->terminal.scroll.viewport_height/2; }
    else { 
        char c = (ch == KEY_BACKSPACE || ch == 127) ? 127 : (ch == '\n' || ch == KEY_ENTER) ? '\n' : (char)ch; 
        if (!shift && write(state->terminal.master_fd, &c, 1) < 0) {} 
        if (!shift) {
            state->terminal.scroll.content_height = state->terminal.line_count;
            state->terminal.scroll.scroll_y = state->terminal.line_count - state->terminal.scroll.viewport_height;
        }
    }
    
    clamp_scroll(&state->terminal.scroll);
}


void draw_terminal(AppState *state, Panel *p) {
    if (p->height < 3) return;
    TerminalBuffer *buf = &state->terminal; int vis = p->height-2; 
    buf->scroll.viewport_height = vis; buf->scroll.content_height = buf->line_count;
    for (int i=0; i<vis; i++) {
        int ly = buf->scroll.scroll_y + i;
        if (ly>=0 && ly<buf->line_count && buf->lines[ly]) {
            char *line = buf->lines[ly];
            int len = strlen(line);
            move(p->y + 1 + i, p->x + 1);
            for (int x = 0; x < p->width - 2 && x < len; x++) {
                bool selected = false;
                if (buf->selection.active) {
                    Selection n = get_normalized_selection(&buf->selection);
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


// --- AI Chat ---
void handle_ai_input(AppState *state, int ch) {
    if (ch == KEY_UP) state->ai.scroll.scroll_y--;
    else if (ch == KEY_DOWN) state->ai.scroll.scroll_y++;
    else if (ch == KEY_PPAGE) state->ai.scroll.scroll_y -= 10;
    else if (ch == KEY_NPAGE) state->ai.scroll.scroll_y += 10;
}



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

void draw_ai_chat(AppState *state, Panel *p) {
    pthread_mutex_lock(&state->ai.mutex);
    int dy_boundary = p->y + p->height - 1;

    if (state->ai.is_waiting_approval && state->ai.diff_text) {
        int dlines = 0; char *dtc = strdup(state->ai.diff_text); char *sp; char *lc = strtok_r(dtc, "\n", &sp);
        while (lc) { dlines++; lc = strtok_r(NULL, "\n", &sp); } free(dtc);
        int max_h = p->height / 3; if (max_h < 3) max_h = 3;
        int diff_h = (dlines + 2 > max_h) ? max_h : dlines + 2;
        dy_boundary -= diff_h;
        attron(COLOR_PAIR(1) | A_BOLD); mvprintw(dy_boundary + 1, p->x + 1, "APPLY? (y/n)"); attroff(COLOR_PAIR(1) | A_BOLD);
        int ly = dy_boundary + 2; char *dt = strdup(state->ai.diff_text); char *spd; char *l = strtok_r(dt, "\n", &spd);
        int cnt = 0; while (l && cnt < diff_h - 2) {
            if (l[0] == '+') attron(COLOR_PAIR(1)); else if (l[0] == '-') attron(A_DIM);
            mvprintw(ly++, p->x + 1, "%.*s", p->width - 2, l);
            if (l[0] == '+' || l[0] == '-') attroff(COLOR_PAIR(1) | A_DIM);
            l = strtok_r(NULL, "\n", &spd); cnt++;
        }
        free(dt);
    }

    int ew = p->width - 6; if (ew < 1) ew = 1;
    int total_h = 0;
    for (int i=0; i<state->ai.message_count; i++) {
        AIMessage *m = &state->ai.messages[i]; if (!m->content && !m->reasoning) continue;
        if (m->role && strcmp(m->role, "system") == 0) continue;
        
        int is_user = (m->role && strcmp(m->role, "user") == 0);
        
        // Reasoning Block (only if present and streaming)
        if (m->reasoning) {
            char *rc = strdup(m->reasoning); char *sp; char *l = strtok_r(rc, "\n", &sp);
            int label_len = 11; // "Thinking: "
            while (l) {
                char *curr = l; bool first_inner = (l == rc);
                while (1) {
                    int available = first_inner ? (ew - label_len) : ew; if (available < 1) available = 1;
                    int split = (strlen(curr) > (size_t)available) ? available : (int)strlen(curr);
                    if (strlen(curr) > (size_t)available) { int s = split; while (s > 0 && curr[s] != ' ') s--; if (s > 0) split = s; }
                    total_h++;
                    if (strlen(curr) <= (size_t)split) break;
                    curr += split; while (*curr == ' ') curr++;
                    first_inner = false;
                }
                l = strtok_r(NULL, "\n", &sp);
            }
            free(rc);
        }

        // Content Block
        if (m->content) {
            char *cc = strdup(m->content); char *sp; char *l = strtok_r(cc, "\n", &sp);
            int label_len = is_user ? 5 : 4; // "You: " or "AI: "
            while (l) {
                char *curr = l; bool first_inner = (l == cc);
                while (1) {
                    int available = first_inner ? (ew - label_len) : ew; if (available < 1) available = 1;
                    int split = (strlen(curr) > (size_t)available) ? available : (int)strlen(curr);
                    if (strlen(curr) > (size_t)available) { int s = split; while (s > 0 && curr[s] != ' ') s--; if (s > 0) split = s; }
                    total_h++;
                    if (strlen(curr) <= (size_t)split) break;
                    curr += split; while (*curr == ' ') curr++;
                    first_inner = false;
                }
                l = strtok_r(NULL, "\n", &sp);
            }
            free(cc);
        }
    }

    state->ai.scroll.viewport_height = dy_boundary - (p->y + 1);
    if (state->ai.scroll.viewport_height < 0) state->ai.scroll.viewport_height = 0;
    state->ai.scroll.content_height = total_h;
    clamp_scroll(&state->ai.scroll);

    int cur_vrow = 0; int sc = state->ai.scroll.scroll_y;
    for (int i=0; i<state->ai.message_count; i++) {
        AIMessage *m = &state->ai.messages[i]; if (!m->content && !m->reasoning) continue;
        if (m->role && strcmp(m->role, "system") == 0) continue;
        int is_user = (m->role && strcmp(m->role, "user") == 0);

        // Draw Reasoning
        if (m->reasoning) {
            char *rc = strdup(m->reasoning); char *sp; char *l = strtok_r(rc, "\n", &sp);
            bool first_l = true;
            while (l) {
                char *curr = l;
                while (1) {
                    int available = first_l ? (ew - 11) : ew; if (available < 1) available = 1;
                    int split = (strlen(curr) > (size_t)available) ? available : (int)strlen(curr);
                    if (strlen(curr) > (size_t)available) { int s = split; while (s > 0 && curr[s] != ' ') s--; if (s > 0) split = s; }
                    int dy = p->y + 1 + (cur_vrow - sc);
                    if (dy > p->y && dy < dy_boundary) {
                        bool sel = false;
                        if (state->ai.selection.active) {
                            Selection n = get_normalized_selection(&state->ai.selection);
                            if (cur_vrow > n.start_line && cur_vrow < n.end_line) sel = true;
                            else if (cur_vrow == n.start_line && cur_vrow == n.end_line) sel = (0 >= n.start_col && 0 < n.end_col); // Simplified col
                            else if (cur_vrow == n.start_line) sel = true; 
                            else if (cur_vrow == n.end_line) sel = true;
                        }
                        if (sel) attron(A_REVERSE);
                        if (first_l) { attron(A_BOLD | A_DIM); mvprintw(dy, p->x + 2, "Thinking: "); attroff(A_BOLD | A_DIM); }
                        mvprintw(dy, first_l ? (p->x + 2 + 11) : (p->x + 2), "%.*s", split, curr);
                        if (sel) attroff(A_REVERSE);
                    }
                    cur_vrow++;
                    if (strlen(curr) <= (size_t)split) break;
                    curr += split; while (*curr == ' ') curr++;
                    first_l = false;
                }
                l = strtok_r(NULL, "\n", &sp); first_l = false;
            }
            free(rc);
        }

        // Draw Content
        if (m->content) {
            char *cc = strdup(m->content); char *sp; char *l = strtok_r(cc, "\n", &sp);
            bool first_l = true; int label_len = is_user ? 5 : 4;
            while (l) {
                char *curr = l;
                while (1) {
                    int available = first_l ? (ew - label_len) : ew; if (available < 1) available = 1;
                    int split = (strlen(curr) > (size_t)available) ? available : (int)strlen(curr);
                    if (strlen(curr) > (size_t)available) { int s = split; while (s > 0 && curr[s] != ' ') s--; if (s > 0) split = s; }
                    int dy = p->y + 1 + (cur_vrow - sc);
                    if (dy > p->y && dy < dy_boundary) {
                        bool sel = false;
                        if (state->ai.selection.active) {
                            Selection n = get_normalized_selection(&state->ai.selection);
                            if (cur_vrow > n.start_line && cur_vrow < n.end_line) sel = true;
                            else if (cur_vrow == n.start_line && cur_vrow == n.end_line) sel = true;
                            else if (cur_vrow == n.start_line || cur_vrow == n.end_line) sel = true;
                        }
                        if (sel) attron(A_REVERSE);
                        if (first_l) { attron(A_BOLD); mvprintw(dy, p->x + 2, is_user ? "You: " : "AI: "); attroff(A_BOLD); }
                        mvprintw(dy, first_l ? (p->x + 2 + label_len) : (p->x + 2), "%.*s", split, curr);
                        if (sel) attroff(A_REVERSE);
                    }
                    cur_vrow++;
                    if (strlen(curr) <= (size_t)split) break;
                    curr += split; while (*curr == ' ') curr++;
                    first_l = false;
                }
                l = strtok_r(NULL, "\n", &sp); first_l = false;
            }
            free(cc);
        }
    }
    pthread_mutex_unlock(&state->ai.mutex);
}

void draw_chat_input(AppState *state, Panel *p) {
    ChatInputBuffer *cb = &state->ai.input;
    int vh = p->height - 2; int vw = p->width - 2;
    if (vw < 1) return;

    int total_vrows = 0;
    for (int i=0; i<cb->line_count; i++) {
        int r = (strlen(cb->lines[i]) + vw - 1) / vw;
        total_vrows += (r > 0 ? r : 1);
    }
    cb->scroll.viewport_height = vh; cb->scroll.content_height = total_vrows;

    int visual_cursor_y = 0;
    for (int i=0; i < cb->cursor_y; i++) {
        int r = (strlen(cb->lines[i]) + vw - 1) / vw;
        visual_cursor_y += (r > 0 ? r : 1);
    }
    visual_cursor_y += cb->cursor_x / vw;

    if (visual_cursor_y < cb->scroll.scroll_y) cb->scroll.scroll_y = visual_cursor_y;
    if (visual_cursor_y >= cb->scroll.scroll_y + vh) cb->scroll.scroll_y = visual_cursor_y - vh + 1;
    clamp_scroll(&cb->scroll);

    int current_vrow = 0;
    int draw_y = 0;
    for (int i=0; i<cb->line_count; i++) {
        char *line = cb->lines[i]; int len = strlen(line);
        int rows_this_line = (len + vw - 1) / vw;
        if (len == 0) rows_this_line = 1;

        for (int r=0; r < rows_this_line; r++) {
            if (current_vrow >= cb->scroll.scroll_y && draw_y < vh) {
                move(p->y + 1 + draw_y, p->x + 1);
                int start_x = r * vw;
                for (int x=0; x < vw && (start_x + x) < len; x++) {
                    int lx = start_x + x;
                    bool selected = false;
                    if (cb->selection.active) {
                        Selection n = get_normalized_selection(&cb->selection);
                        if (i > n.start_line && i < n.end_line) selected = true;
                        else if (i == n.start_line && i == n.end_line) selected = (lx >= n.start_col && lx < n.end_col);
                        else if (i == n.start_line) selected = (lx >= n.start_col);
                        else if (i == n.end_line) selected = (lx < n.end_col);
                    }
                    if (selected) attron(A_REVERSE);
                    addch(line[lx]);
                    if (selected) attroff(A_REVERSE);
                }
                draw_y++;
            }
            current_vrow++;
        }
    }
}


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

        if (i == PANEL_FILE_TREE) {
            FileNode *flat[1000]; int count = 0; flatten_tree(state->root, flat, &count, 1000);
            state->tree_scroll.viewport_height = p->height - 2;
            state->tree_scroll.content_height = count;
            if (state->tree_selection < state->tree_scroll.scroll_y) state->tree_scroll.scroll_y = state->tree_selection;
            if (state->tree_selection >= state->tree_scroll.scroll_y + state->tree_scroll.viewport_height) state->tree_scroll.scroll_y = state->tree_selection - state->tree_scroll.viewport_height + 1;
            clamp_scroll(&state->tree_scroll);

            for (int j=0; j<state->tree_scroll.viewport_height && (j+state->tree_scroll.scroll_y)<count; j++) {
                int idx = j+state->tree_scroll.scroll_y; 
                if (idx >= 0 && idx < count && flat[idx]) {
                    if (idx==state->tree_selection && state->active_panel==PANEL_FILE_TREE) attron(A_REVERSE);
                    int indent = (flat[idx]->depth - 1) * 2; if (indent < 0) indent = 0;
                    mvprintw(p->y+1+j, p->x+1, "%*s%.*s", indent, "", p->width-2-indent, flat[idx]->name);
                    if (idx==state->tree_selection && state->active_panel==PANEL_FILE_TREE) attroff(A_REVERSE);
                }
            }
        } else if (i == PANEL_EDITOR) draw_editor(state, p);
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

    attron(A_REVERSE);
    const char *status = (state->ai.is_waiting) ? "Streaming response..." : "Ready";
    mvprintw(LINES-1, 1, " [%s] ", status);
    attroff(A_REVERSE);

    Panel *ap = &state->panels[(int)state->active_panel];
    if (state->active_panel == PANEL_EDITOR) {
        curs_set(1); move(ap->y+1+state->editor.cursor_y-state->editor.scroll.scroll_y, ap->x+1+state->editor.cursor_x-state->editor.scroll_x);
    } else if (state->active_panel == PANEL_TERMINAL) {
        curs_set(1); int last = state->terminal.line_count - 1;
        int sy = ap->y+1+(last-state->terminal.scroll.scroll_y);
        if (sy>ap->y && sy<ap->y+ap->height-1 && last >= 0 && state->terminal.lines[last]) 
            move(sy, ap->x+1+(int)strlen(state->terminal.lines[last]));
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
    int th = state->panels[PANEL_TERMINAL].visible ? (my-2)/4 : 0; if (th<3 && th>0) th=3;
    int uh = my-2-th; if (uh < 3) uh = 3;
    
    int vc = 0; for (int i=0; i<3; i++) if (state->panels[i].visible) vc++;
    int x=0, fw = state->panels[PANEL_FILE_TREE].visible ? mx/5 : 0; if (fw>0 && fw<20) fw=20;
    
    // Top Row
    if (state->panels[PANEL_FILE_TREE].visible) { state->panels[PANEL_FILE_TREE] = (Panel){x, 1, fw, uh, true, "Files"}; x+=fw; }
    int rem = mx-x; if (rem < 5) rem = 5;
    int side_panels_visible = vc - (state->panels[PANEL_FILE_TREE].visible?1:0);
    int ow = (side_panels_visible > 0) ? rem / side_panels_visible : rem;
    
    int split_x = mx - ow; // Default split if AI is the only side panel
    if (state->panels[PANEL_EDITOR].visible && state->panels[PANEL_AI].visible) {
        split_x = x + ow;
    } else if (state->panels[PANEL_EDITOR].visible) {
        split_x = mx; // No AI, split at end
    } else if (state->panels[PANEL_AI].visible) {
        split_x = x; // AI takes all besides Files
    }

    if (state->panels[PANEL_EDITOR].visible) { state->panels[PANEL_EDITOR] = (Panel){x, 1, ow, uh, true, "Editor"}; x+=ow; }
    if (state->panels[PANEL_AI].visible) { state->panels[PANEL_AI] = (Panel){x, 1, mx-x, uh, true, "Chat History"}; }

    // Bottom Row
    // If AI is hidden but Chat Input is visible, we should still use a reasonable width (0.3 of screen or split_x)
    if (!state->panels[PANEL_AI].visible && state->panels[PANEL_CHAT_INPUT].visible) {
        split_x = mx - (int)(mx * 0.3);
        if (split_x < 20) split_x = mx - 20;
    }
    
    int tw = split_x; if (tw < 0) tw = 0;
    int ciw = mx - tw; if (ciw < 0) ciw = 0;

    state->panels[PANEL_TERMINAL] = (Panel){0, my - 1 - th, tw, th, state->panels[PANEL_TERMINAL].visible, "Terminal"};
    state->panels[PANEL_CHAT_INPUT] = (Panel){tw, my - 1 - th, ciw, th, state->panels[PANEL_CHAT_INPUT].visible, "Chat Input"};
}


ScrollState* get_panel_scroll(AppState *state, PanelType type) {
    if (type == PANEL_FILE_TREE) return &state->tree_scroll;
    if (type == PANEL_EDITOR) return &state->editor.scroll;
    if (type == PANEL_AI) return &state->ai.scroll;
    if (type == PANEL_TERMINAL) return &state->terminal.scroll;
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
    if (type == PANEL_TERMINAL) return &state->terminal.selection;
    if (type == PANEL_CHAT_INPUT) return &state->ai.input.selection;
    return NULL;
}

void map_mouse_to_panel(AppState *state, PanelType type, int mx, int my, int *line, int *col) {
    Panel *p = &state->panels[type]; ScrollState *ss = get_panel_scroll(state, type);
    int cmx = mx; int cmy = my;
    if (cmx < p->x + 1) cmx = p->x + 1; if (cmx >= p->x + p->width - 1) cmx = p->x + p->width - 2;
    if (cmy < p->y + 1) cmy = p->y + 1; if (cmy >= p->y + p->height - 1) cmy = p->y + p->height - 2;

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
        if (tb->line_count == 0) { *line = 0; *col = 0; return; }
        if (*line < 0) *line = 0;
        if (*line >= tb->line_count) *line = tb->line_count - 1;
        *col = cmx - p->x - 1;
        int len = strlen(tb->lines[*line]);
        if (*col < 0) *col = 0;
        if (*col > len) *col = len;
    } else if (type == PANEL_AI) {
        int mw = p->width - 4; if (mw < 1) mw = 1;
        int mouse_y_in_history = my - p->y - 1 + ss->scroll_y;
        int curr_y = 0; bool found = false;
        pthread_mutex_lock(&state->ai.mutex);
        for (int i = 0; i < state->ai.message_count; i++) {
            AIMessage *m = &state->ai.messages[i];
            if (!m->content || !m->role || strcmp(m->role, "system") == 0) continue;
            char *content = strdup(m->content); char *saveptr; char *cl = strtok_r(content, "\n", &saveptr);
            while (cl) {
                char *curr = cl;
                while (*curr) {
                    int clen = strlen(curr);
                    int split = (clen <= mw) ? clen : mw;
                    if (clen > mw) { int s = split; while (s > 0 && curr[s] != ' ') s--; if (s > 0) split = s; }
                    if (curr_y == mouse_y_in_history) {
                        int label_start = (strcmp(m->role, "user") == 0 && curr == cl) ? 5 : 4;
                        if (m->reasoning && curr_y < (state->ai.scroll.content_height)) { // Adjusted for reasoning label
                             // This is complex, but let's simplify: 
                             // Just use mx relative to p->x
                        }
                        *line = curr_y; *col = mx - p->x - 2;
                        found = true; break;
                    }
                    curr_y++; curr += split; while (*curr == ' ') curr++;
                }
                if (found) break; cl = strtok_r(NULL, "\n", &saveptr);
            }
            free(content); if (found) break;
        }
        pthread_mutex_unlock(&state->ai.mutex);
        if (!found) { // Clamp to last message if clicking below
            if (state->ai.message_count > 0) { *line = state->ai.message_count - 1; *col = 0; }
            else { *line = -1; *col = -1; }
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
    else if (type == PANEL_TERMINAL) sel = get_normalized_selection(&state->terminal.selection);
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
            if (i < 0 || i >= tb->line_count) continue;
            int start = (i == sel.start_line) ? sel.start_col : 0;
            int end = (i == sel.end_line) ? sel.end_col : (int)strlen(tb->lines[i]);
            if (start < 0) start = 0; if (end > (int)strlen(tb->lines[i])) end = (int)strlen(tb->lines[i]);
            if (end < start) end = start;
            int chunk = end - start;
            buf = realloc(buf, len + chunk + 2);
            memcpy(buf + len, tb->lines[i] + start, chunk);
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
            // Delete manually for chat input since delete_char is for editor
            if (cb->cursor_x > 0) {
                int len = strlen(cb->lines[cb->cursor_y]);
                memmove(cb->lines[cb->cursor_y] + cb->cursor_x - 1, cb->lines[cb->cursor_y] + cb->cursor_x, len - cb->cursor_x + 1);
                cb->cursor_x--;
            } else if (cb->cursor_y > 0) {
                int prev_len = strlen(cb->lines[cb->cursor_y-1]);
                cb->lines[cb->cursor_y-1] = realloc(cb->lines[cb->cursor_y-1], prev_len + strlen(cb->lines[cb->cursor_y]) + 1);
                strcat(cb->lines[cb->cursor_y-1], cb->lines[cb->cursor_y]);
                free(cb->lines[cb->cursor_y]);
                memmove(cb->lines + cb->cursor_y, cb->lines + cb->cursor_y + 1, (cb->line_count - cb->cursor_y - 1) * sizeof(char*));
                cb->line_count--; cb->cursor_y--; cb->cursor_x = prev_len;
            }
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
        for (char *c = text; *c; c++) {
            if (*c == '\n') {
                ChatInputBuffer *cb = &state->ai.input;
                cb->lines = realloc(cb->lines, sizeof(char*) * (cb->line_count + 1));
                char *rem = strdup(cb->lines[cb->cursor_y] + cb->cursor_x);
                cb->lines[cb->cursor_y][cb->cursor_x] = '\0';
                memmove(cb->lines + cb->cursor_y + 2, cb->lines + cb->cursor_y + 1, sizeof(char*) * (cb->line_count - cb->cursor_y - 1));
                cb->lines[cb->cursor_y + 1] = rem;
                cb->line_count++; cb->cursor_y++; cb->cursor_x = 0;
            } else {
                ChatInputBuffer *cb = &state->ai.input;
                char *l = cb->lines[cb->cursor_y]; int len = strlen(l);
                cb->lines[cb->cursor_y] = realloc(cb->lines[cb->cursor_y], len + 2);
                memmove(cb->lines[cb->cursor_y] + cb->cursor_x + 1, cb->lines[cb->cursor_y] + cb->cursor_x, len - cb->cursor_x + 1);
                cb->lines[cb->cursor_y][cb->cursor_x++] = *c;
            }
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
        tb->selection.active = 1; tb->selection.start_line = 0; tb->selection.start_col = 0;
        tb->selection.end_line = tb->line_count - 1;
        tb->selection.end_col = (tb->line_count > 0) ? strlen(tb->lines[tb->line_count-1]) : 0;
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
    char *name;
    char *shortcut;
} MenuItem;

void show_help_menu(AppState *state) {
    MenuItem items[] = {
        {"Save File", ""},
        {"Save As...", ""},
        {"Select All", "Ctrl+A"},
        {"Copy", "Ctrl+C"},
        {"Cut", "Ctrl+X"},
        {"Paste", "Ctrl+V"},
        {"Run Python Script", "Ctrl+R"},
        {"Toggle File Tree", "Alt+f"},
        {"Toggle Editor", "Alt+e"},
        {"Toggle AI Chat", "Alt+a"},
        {"Toggle Terminal", "Alt+t"},
        {"Quit Application", "Alt+q"},
        {"Close Menu", "ESC"}
    };
    int num_items = sizeof(items) / sizeof(MenuItem);
    
    int max_y, max_x; getmaxyx(stdscr, max_y, max_x);
    int hw = 60, hh = num_items + 6;
    int hy = (max_y - hh) / 2; int hx = (max_x - hw) / 2;
    WINDOW *win = newwin(hh, hw, hy, hx);
    keypad(win, TRUE);
    
    int selection = 0;
    while (1) {
        box(win, 0, 0);
        attron(A_BOLD);
        mvwprintw(win, 1, (hw - 16) / 2, "Cursory Commands");
        attroff(A_BOLD);
        
        for (int i=0; i<num_items; i++) {
            if (i == selection) wattron(win, A_REVERSE);
            mvwprintw(win, 3 + i, 4, "%-35s %15s", items[i].name, items[i].shortcut);
            if (i == selection) wattroff(win, A_REVERSE);
        }
        
        mvwprintw(win, hh - 2, (hw - 30) / 2, "Press Enter to execute, or ESC");
        
        wrefresh(win);
        int c = wgetch(win);
        if (c == KEY_MOUSE) continue;
        
        if (c == KEY_UP && selection > 0) selection--;
        else if (c == KEY_DOWN && selection < num_items - 1) selection++;
        else if (c == 27) { break; } // ESC
        else if (c == '\n' || c == KEY_ENTER) {
            delwin(win); clear(); calculate_layout(state);
            switch (selection) {
                case 0: editor_save(state); return;
                case 1: 
                {
                    WINDOW *pwin = newwin(5, 60, (max_y-5)/2, (max_x-60)/2);
                    box(pwin, 0, 0);
                    mvwprintw(pwin, 1, 2, "Save As (File Path):");
                    wmove(pwin, 2, 2);
                    wrefresh(pwin);
                    
                    char path[1024] = {0};
                    echo(); curs_set(1);
                    wtimeout(pwin, -1);
                    wgetnstr(pwin, path, 1023);
                    noecho(); curs_set(0);
                    delwin(pwin);
                    clear(); calculate_layout(state);
                    
                    if (strlen(path) > 0) {
                        snprintf(state->editor.filepath, sizeof(state->editor.filepath), "%s", path);
                        editor_save(state);
                    }
                    return;
                }
                case 2: app_select_all(state); return;
                case 3: app_copy(state); return;
                case 4: app_cut(state); return;
                case 5: app_paste(state); return;
                case 6: 
                    if (strcmp(state->editor.filepath, "[No File]") != 0) {
                        editor_save(state);
                        char cmd[1024]; snprintf(cmd, sizeof(cmd), "python3 %s\n", state->editor.filepath);
                        if (write(state->terminal.master_fd, cmd, strlen(cmd)) < 0) {}
                        state->panels[PANEL_TERMINAL].visible = true; calculate_layout(state);
                        state->active_panel = PANEL_TERMINAL;
                        snprintf(state->last_action, 128, "Run: %s", state->editor.filepath);
                    }
                    return;
                case 7: state->panels[PANEL_FILE_TREE].visible = !state->panels[PANEL_FILE_TREE].visible; calculate_layout(state); return;
                case 8: state->panels[PANEL_EDITOR].visible = !state->panels[PANEL_EDITOR].visible; calculate_layout(state); return;
                case 9: state->panels[PANEL_AI].visible = !state->panels[PANEL_AI].visible; calculate_layout(state); return;
                case 10: state->panels[PANEL_TERMINAL].visible = !state->panels[PANEL_TERMINAL].visible; calculate_layout(state); return;
                case 11: state->running = false; return;
                case 12: return;
            }
        }
    }
    delwin(win);
    clear();
    calculate_layout(state);
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
    memset(&state->terminal.selection, 0, sizeof(Selection));
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


void handle_input(AppState *state) {
    timeout(10); int ch = getch(); if (ch == ERR) return; MEVENT ev;
    
    bool shift = false; int base_ch = ch;
    if (ch == KEY_SLEFT || ch == KEY_SRIGHT || ch == KEY_SR || ch == KEY_SF) {
        shift = true;
        if (ch == KEY_SLEFT) base_ch = KEY_LEFT; else if (ch == KEY_SRIGHT) base_ch = KEY_RIGHT;
        else if (ch == KEY_SR) base_ch = KEY_UP; else if (ch == KEY_SF) base_ch = KEY_DOWN;
    }

    if (ch == KEY_F(1)) { show_help_menu(state); return; }
    if (ch == 1) { app_select_all(state); return; }
    if (ch == 3) { app_copy(state); return; }
    if (ch == 24) { app_cut(state); return; }
    if (ch == 22) { app_paste(state); return; }
    if (ch == 18) { // Ctrl+R
        if (strcmp(state->editor.filepath, "[No File]") != 0) {
            editor_save(state);
            char cmd[1024];
            snprintf(cmd, sizeof(cmd), "python3 %s\n", state->editor.filepath);
            if (write(state->terminal.master_fd, cmd, strlen(cmd)) < 0) {}
            state->panels[PANEL_TERMINAL].visible = true;
            calculate_layout(state);
            state->active_panel = PANEL_TERMINAL;
            snprintf(state->last_action, 128, "Run: %s", state->editor.filepath);
        }
        return;
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
        if (n == 'q') state->running = false;
        else if (n == 'f') state->panels[PANEL_FILE_TREE].visible = !state->panels[PANEL_FILE_TREE].visible;
        else if (n == 'e') state->panels[PANEL_EDITOR].visible = !state->panels[PANEL_EDITOR].visible;
        else if (n == 'a') state->panels[PANEL_AI].visible = !state->panels[PANEL_AI].visible;
        else if (n == 't') state->panels[PANEL_TERMINAL].visible = !state->panels[PANEL_TERMINAL].visible;
        calculate_layout(state);
        return;
    }

    if (state->ai.is_waiting_approval) {
        if (ch == 'y' || ch == 'Y') { apply_pending_edit(state, true); return; }
        if (ch == 'n' || ch == 'N') { apply_pending_edit(state, false); return; }
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
        if (state->active_panel == PANEL_FILE_TREE) {
            FileNode *fl[1000]; int c=0; flatten_tree(state->root, fl, &c, 1000);
            if (ch == KEY_UP && state->tree_selection > 0) state->tree_selection--; 
            else if (ch == KEY_DOWN && state->tree_selection < c-1) state->tree_selection++;
            else if (ch == KEY_PPAGE) { state->tree_selection -= state->tree_scroll.viewport_height; if (state->tree_selection < 0) state->tree_selection = 0; }
            else if (ch == KEY_NPAGE) { state->tree_selection += state->tree_scroll.viewport_height; if (state->tree_selection >= c) state->tree_selection = c-1; }
            else if (ch == '\n' || ch == KEY_ENTER) {
                FileNode *s = fl[state->tree_selection];
                if (s->is_dir) { s->is_expanded = !s->is_expanded; if (s->is_expanded && s->child_count == 0) load_directory(s); }
                else { load_file(state, s->path); state->active_panel = PANEL_EDITOR; }
            }
        } else if (state->active_panel == PANEL_EDITOR) {
            EditorBuffer *eb = &state->editor; Selection *sel = &eb->selection;
            int old_y = eb->cursor_y, old_x = eb->cursor_x;

            if (base_ch == KEY_UP && eb->cursor_y > 0) {
                eb->cursor_y--;
                int len = strlen(eb->lines[eb->cursor_y]);
                if (eb->cursor_x > len) eb->cursor_x = len;
            } else if (base_ch == KEY_DOWN && eb->cursor_y < eb->line_count-1) {
                eb->cursor_y++;
                int len = strlen(eb->lines[eb->cursor_y]);
                if (eb->cursor_x > len) eb->cursor_x = len;
            } else if (base_ch == KEY_PPAGE) {
                eb->cursor_y -= eb->scroll.viewport_height;
                if (eb->cursor_y < 0) eb->cursor_y = 0;
                int len = strlen(eb->lines[eb->cursor_y]);
                if (eb->cursor_x > len) eb->cursor_x = len;
            } else if (base_ch == KEY_NPAGE) {
                eb->cursor_y += eb->scroll.viewport_height;
                if (eb->cursor_y >= eb->line_count) eb->cursor_y = eb->line_count - 1;
                int len = strlen(eb->lines[eb->cursor_y]);
                if (eb->cursor_x > len) eb->cursor_x = len;
            }
            else if (base_ch == KEY_LEFT && eb->cursor_x > 0) eb->cursor_x--;
            else if (base_ch == KEY_RIGHT && eb->cursor_x < (int)strlen(eb->lines[eb->cursor_y])) eb->cursor_x++;
            else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8 || ch == KEY_DC) {
                if (sel->active) {
                    Selection n = get_normalized_selection(sel);
                    eb->cursor_x = n.end_col; eb->cursor_y = n.end_line;
                    while (eb->cursor_y > n.start_line || (eb->cursor_y == n.start_line && eb->cursor_x > n.start_col)) {
                        delete_char(state);
                    }
                    sel->active = 0;
                } else if (ch != KEY_DC) {
                    delete_char(state);
                } else {
                    if (eb->cursor_x < (int)strlen(eb->lines[eb->cursor_y]) || eb->cursor_y < eb->line_count - 1) {
                        if (eb->cursor_x == (int)strlen(eb->lines[eb->cursor_y])) { eb->cursor_y++; eb->cursor_x = 0; }
                        else { eb->cursor_x++; }
                        delete_char(state);
                    }
                }
            }
            else if (ch == '\n' || ch == KEY_ENTER) insert_newline(state);
            else if (ch >= 32 && ch < 127) insert_char(state, ch);

            if (shift) {
                if (!sel->active) { sel->active = 1; sel->start_line = old_y; sel->start_col = old_x; }
                sel->end_line = eb->cursor_y; sel->end_col = eb->cursor_x;
            } else if (ch != ERR && ch != KEY_MOUSE) { sel->active = 0; }
        } else if (state->active_panel == PANEL_CHAT_INPUT) {
            ChatInputBuffer *cb = &state->ai.input; Selection *sel = &cb->selection;
            int old_y = cb->cursor_y, old_x = cb->cursor_x;
            Panel *p = &state->panels[PANEL_CHAT_INPUT]; int vw = p->width - 2; if (vw < 1) vw = 1;

            if (base_ch == KEY_UP) {
                if (cb->cursor_x >= vw) cb->cursor_x -= vw;
                else if (cb->cursor_y > 0) {
                    cb->cursor_y--; int plen = strlen(cb->lines[cb->cursor_y]);
                    int last_vrow_start = (plen / vw) * vw;
                    cb->cursor_x = last_vrow_start + (cb->cursor_x % vw);
                    if (cb->cursor_x > plen) cb->cursor_x = plen;
                }
            } else if (base_ch == KEY_DOWN) {
                int len = strlen(cb->lines[cb->cursor_y]);
                if (cb->cursor_x + vw <= len) cb->cursor_x += vw;
                else if (cb->cursor_y < cb->line_count - 1) {
                    cb->cursor_y++; cb->cursor_x %= vw;
                    int nlen = strlen(cb->lines[cb->cursor_y]);
                    if (cb->cursor_x > nlen) cb->cursor_x = nlen;
                }
            } else if (base_ch == KEY_PPAGE) {
                cb->cursor_y -= cb->scroll.viewport_height;
                if (cb->cursor_y < 0) cb->cursor_y = 0;
                int plen = strlen(cb->lines[cb->cursor_y]);
                int last_vrow_start = (plen / vw) * vw;
                cb->cursor_x = last_vrow_start + (cb->cursor_x % vw);
                if (cb->cursor_x > plen) cb->cursor_x = plen;
            } else if (base_ch == KEY_NPAGE) {
                cb->cursor_y += cb->scroll.viewport_height;
                if (cb->cursor_y >= cb->line_count) cb->cursor_y = cb->line_count - 1;
                cb->cursor_x %= vw;
                int nlen = strlen(cb->lines[cb->cursor_y]);
                if (cb->cursor_x > nlen) cb->cursor_x = nlen;
            } else if (base_ch == KEY_LEFT && cb->cursor_x > 0) cb->cursor_x--;
            else if (base_ch == KEY_RIGHT && cb->cursor_x < (int)strlen(cb->lines[cb->cursor_y])) cb->cursor_x++;
            else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8 || ch == KEY_DC) {
                if (sel->active) {
                    Selection n = get_normalized_selection(sel);
                    cb->cursor_x = n.end_col; cb->cursor_y = n.end_line;
                    while (cb->cursor_y > n.start_line || (cb->cursor_y == n.start_line && cb->cursor_x > n.start_col)) {
                        if (cb->cursor_x > 0) {
                            int len = strlen(cb->lines[cb->cursor_y]);
                            memmove(cb->lines[cb->cursor_y] + cb->cursor_x - 1, cb->lines[cb->cursor_y] + cb->cursor_x, len - cb->cursor_x + 1);
                            cb->cursor_x--;
                        } else if (cb->cursor_y > 0) {
                            int prev_len = strlen(cb->lines[cb->cursor_y-1]);
                            cb->lines[cb->cursor_y-1] = realloc(cb->lines[cb->cursor_y-1], prev_len + strlen(cb->lines[cb->cursor_y]) + 1);
                            strcat(cb->lines[cb->cursor_y-1], cb->lines[cb->cursor_y]);
                            free(cb->lines[cb->cursor_y]);
                            memmove(cb->lines + cb->cursor_y, cb->lines + cb->cursor_y + 1, (cb->line_count - cb->cursor_y - 1) * sizeof(char*));
                            cb->line_count--; cb->cursor_y--; cb->cursor_x = prev_len;
                        }
                    }
                    sel->active = 0;
                } else {
                    bool do_del = false;
                    if (ch == KEY_DC) {
                        if (cb->cursor_x < (int)strlen(cb->lines[cb->cursor_y]) || cb->cursor_y < cb->line_count - 1) {
                            if (cb->cursor_x == (int)strlen(cb->lines[cb->cursor_y])) { cb->cursor_y++; cb->cursor_x = 0; }
                            else { cb->cursor_x++; }
                            do_del = true;
                        }
                    } else do_del = true;
                    
                    if (do_del) {
                        if (cb->cursor_x > 0) {
                            char *l = cb->lines[cb->cursor_y]; int len = strlen(l);
                            memmove(l + cb->cursor_x - 1, l + cb->cursor_x, len - cb->cursor_x + 1); cb->cursor_x--;
                        } else if (cb->cursor_y > 0) {
                            char *prev = cb->lines[cb->cursor_y-1]; char *curr = cb->lines[cb->cursor_y];
                            int plen = strlen(prev); int clen = strlen(curr);
                            cb->lines[cb->cursor_y-1] = realloc(cb->lines[cb->cursor_y-1], plen + clen + 1);
                            strcat(cb->lines[cb->cursor_y-1], curr); cb->cursor_x = plen;
                            free(curr); memmove(cb->lines + cb->cursor_y, cb->lines + cb->cursor_y + 1, sizeof(char*) * (cb->line_count - cb->cursor_y - 1));
                            cb->line_count--; cb->cursor_y--;
                        }
                    }
                }
            } else if (ch == '\n' || ch == KEY_ENTER) {
                AppSendChat(state);
            } else if (ch >= 32 && ch <= 126) {
                char *l = cb->lines[cb->cursor_y]; int len = strlen(l);
                cb->lines[cb->cursor_y] = realloc(cb->lines[cb->cursor_y], len + 2);
                memmove(cb->lines[cb->cursor_y] + cb->cursor_x + 1, cb->lines[cb->cursor_y] + cb->cursor_x, len - cb->cursor_x + 1);
                cb->lines[cb->cursor_y][cb->cursor_x++] = (char)ch;
            }

            if (shift) {
                if (!sel->active) { sel->active = 1; sel->start_line = old_y; sel->start_col = old_x; }
                sel->end_line = cb->cursor_y; sel->end_col = cb->cursor_x;
            } else if (ch != ERR && ch != KEY_MOUSE) { sel->active = 0; }
        } else if (state->active_panel == PANEL_AI) {
            ScrollState *ss = &state->ai.scroll;
            if (ch == KEY_UP) ss->scroll_y--; else if (ch == KEY_DOWN) ss->scroll_y++;
            else if (ch == KEY_PPAGE) ss->scroll_y -= ss->viewport_height;
            else if (ch == KEY_NPAGE) ss->scroll_y += ss->viewport_height;
            clamp_scroll(ss);
        } else if (state->active_panel == PANEL_TERMINAL) handle_terminal_input(state, ch);
    }
}
