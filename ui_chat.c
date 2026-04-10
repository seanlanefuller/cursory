#define _GNU_SOURCE
#include "cursory.h"
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

void draw_ai_chat(AppState *state, Panel *p) {
    if (p->height < 3) return;
    AIContext *ai = &state->ai; 
    pthread_mutex_lock(&ai->mutex);
    int dy_boundary = p->y + p->height - 1;


    int ew = p->width - 6; if (ew < 1) ew = 1;
    int total_h = 0;
    for (int i=0; i<ai->message_count; i++) {
        AIMessage *m = &ai->messages[i]; if (!m->content && !m->reasoning) continue;
        if (m->role && strcmp(m->role, "system") == 0) continue;
        int is_user = (m->role && strcmp(m->role, "user") == 0);
        if (m->reasoning) {
            char *rc = strdup(m->reasoning); char *sp; char *l = strtok_r(rc, "\n", &sp);
            while (l) {
                char *curr = l; bool first = (l == rc);
                while (1) {
                    int avail = first ? (ew - 11) : ew; if (avail < 1) avail = 1;
                    int split = (strlen(curr) > (size_t)avail) ? avail : (int)strlen(curr);
                    if (strlen(curr) > (size_t)avail) { int s = split; while (s > 0 && curr[s] != ' ') s--; if (s > 0) split = s; }
                    total_h++; if (strlen(curr) <= (size_t)split) break;
                    curr += split; while (*curr == ' ') curr++; first = false;
                }
                l = strtok_r(NULL, "\n", &sp);
            }
            free(rc);
        }
        if (m->content) {
            char *cc = strdup(m->content); char *sp; char *l = strtok_r(cc, "\n", &sp);
            int label_len = is_user ? 5 : 4;
            while (l) {
                char *curr = l; bool first = (l == cc);
                while (1) {
                    int avail = first ? (ew - label_len) : ew; if (avail < 1) avail = 1;
                    int split = (strlen(curr) > (size_t)avail) ? avail : (int)strlen(curr);
                    if (strlen(curr) > (size_t)avail) { int s = split; while (s > 0 && curr[s] != ' ') s--; if (s > 0) split = s; }
                    total_h++; if (strlen(curr) <= (size_t)split) break;
                    curr += split; while (*curr == ' ') curr++; first = false;
                }
                l = strtok_r(NULL, "\n", &sp);
            }
            free(cc);
        }
    }

    ai->scroll.viewport_height = dy_boundary - (p->y + 1);
    if (ai->scroll.viewport_height < 0) ai->scroll.viewport_height = 0;
    ai->scroll.content_height = total_h;
    clamp_scroll(&ai->scroll);

    int curr_vrow = 0; int sc = ai->scroll.scroll_y;
    for (int i=0; i<ai->message_count; i++) {
        AIMessage *m = &ai->messages[i]; if (!m->content && !m->reasoning) continue;
        if (m->role && strcmp(m->role, "system") == 0) continue;
        int is_user = (m->role && strcmp(m->role, "user") == 0);
        if (m->reasoning) {
            char *rc = strdup(m->reasoning); char *sp; char *l = strtok_r(rc, "\n", &sp);
            bool first_l = true;
            while (l) {
                char *curr = l;
                while (1) {
                    int avail = first_l ? (ew - 11) : ew; if (avail < 1) avail = 1;
                    int split = (strlen(curr) > (size_t)avail) ? avail : (int)strlen(curr);
                    if (strlen(curr) > (size_t)avail) { int s = split; while (s > 0 && curr[s] != ' ') s--; if (s > 0) split = s; }
                    int dy = p->y + 1 + (curr_vrow - sc);
                    if (dy > p->y && dy < dy_boundary) {
                        bool sel = false;
                        if (ai->selection.active) {
                            Selection n = get_normalized_selection(&ai->selection);
                            if (curr_vrow >= n.start_line && curr_vrow <= n.end_line) sel = true;
                        }
                        if (sel) attron(A_REVERSE);
                        if (first_l) { attron(A_BOLD | A_DIM); mvprintw(dy, p->x + 2, "Thinking: "); attroff(A_BOLD | A_DIM); }
                        mvprintw(dy, first_l ? (p->x + 2 + 11) : (p->x + 2), "%.*s", split, curr);
                        if (sel) attroff(A_REVERSE);
                    }
                    curr_vrow++; if (strlen(curr) <= (size_t)split) break;
                    curr += split; while (*curr == ' ') curr++; first_l = false;
                }
                l = strtok_r(NULL, "\n", &sp); first_l = false;
            }
            free(rc);
        }
        if (m->content) {
            char *cc = strdup(m->content); char *sp; char *l = strtok_r(cc, "\n", &sp);
            bool first_l = true; int label_len = is_user ? 5 : 4;
            while (l) {
                char *curr = l;
                while (1) {
                    int avail = first_l ? (ew - label_len) : ew; if (avail < 1) avail = 1;
                    int split = (strlen(curr) > (size_t)avail) ? avail : (int)strlen(curr);
                    if (strlen(curr) > (size_t)avail) { int s = split; while (s > 0 && curr[s] != ' ') s--; if (s > 0) split = s; }
                    int dy = p->y + 1 + (curr_vrow - sc);
                    if (dy > p->y && dy < dy_boundary) {
                        bool sel = false;
                        if (ai->selection.active) {
                            Selection n = get_normalized_selection(&ai->selection);
                            if (curr_vrow >= n.start_line && curr_vrow <= n.end_line) sel = true;
                        }
                        if (sel) attron(A_REVERSE);
                        if (first_l) { attron(A_BOLD); mvprintw(dy, p->x + 2, is_user ? "You: " : "AI: "); attroff(A_BOLD); }
                        mvprintw(dy, first_l ? (p->x + 2 + label_len) : (p->x + 2), "%.*s", split, curr);
                        if (sel) attroff(A_REVERSE);
                    }
                    curr_vrow++; if (strlen(curr) <= (size_t)split) break;
                    curr += split; while (*curr == ' ') curr++; first_l = false;
                }
                l = strtok_r(NULL, "\n", &sp); first_l = false;
            }
            free(cc);
        }
    }
    pthread_mutex_unlock(&ai->mutex);
}

void draw_chat_input(AppState *state, Panel *p) {
    ChatInputBuffer *cb = &state->ai.input;
    int vh = p->height - 2; int vw = p->width - 2; if (vw < 1) return;
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
    int current_vrow = 0; int draw_y = 0;
    for (int i=0; i<cb->line_count; i++) {
        char *line = cb->lines[i]; int len = strlen(line);
        int rows = (len + vw - 1) / vw; if (len == 0) rows = 1;
        for (int r=0; r < rows; r++) {
            if (current_vrow >= cb->scroll.scroll_y && draw_y < vh) {
                move(p->y + 1 + draw_y, p->x + 1);
                int sx = r * vw;
                for (int x=0; x < vw && (sx + x) < len; x++) {
                    int lx = sx + x; bool sel = false;
                    if (cb->selection.active) {
                        Selection n = get_normalized_selection(&cb->selection);
                        if (i > n.start_line && i < n.end_line) sel = true;
                        else if (i == n.start_line && i == n.end_line) sel = (lx >= n.start_col && lx < n.end_col);
                        else if (i == n.start_line) sel = (lx >= n.start_col);
                        else if (i == n.end_line) sel = (lx < n.end_col);
                    }
                    if (sel) attron(A_REVERSE);
                    addch(line[lx]);
                    if (sel) attroff(A_REVERSE);
                }
                draw_y++;
            }
            current_vrow++;
        }
    }
}

void handle_chat_input(AppState *state, int ch, int base_ch, bool shift) {
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
        cb->cursor_y -= cb->scroll.viewport_height; if (cb->cursor_y < 0) cb->cursor_y = 0;
        int plen = strlen(cb->lines[cb->cursor_y]); int last_start = (plen / vw) * vw;
        cb->cursor_x = last_start + (cb->cursor_x % vw); if (cb->cursor_x > plen) cb->cursor_x = plen;
    } else if (base_ch == KEY_NPAGE) {
        cb->cursor_y += cb->scroll.viewport_height; if (cb->cursor_y >= cb->line_count) cb->cursor_y = cb->line_count - 1;
        cb->cursor_x %= vw; int nlen = strlen(cb->lines[cb->cursor_y]); if (cb->cursor_x > nlen) cb->cursor_x = nlen;
    } else if (base_ch == KEY_LEFT && cb->cursor_x > 0) cb->cursor_x--;
    else if (base_ch == KEY_RIGHT && cb->cursor_x < (int)strlen(cb->lines[cb->cursor_y])) cb->cursor_x++;
    else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8 || ch == KEY_DC) {
        if (sel->active) {
            Selection n = get_normalized_selection(sel);
            cb->cursor_x = n.end_col; cb->cursor_y = n.end_line;
            while (cb->cursor_y > n.start_line || (cb->cursor_y == n.start_line && cb->cursor_x > n.start_col)) buffer_delete_char(cb);
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
            if (do_del) buffer_delete_char(cb);
        }
    } else if (ch == '\n' || ch == KEY_ENTER) {
        if (!shift && cb->line_count > 0 && strlen(cb->lines[cb->line_count-1]) > 0) {
            char *full = malloc(1); full[0] = '\0'; int flen = 0;
            for (int i=0; i<cb->line_count; i++) { int cl = strlen(cb->lines[i]); full = realloc(full, flen+cl+2); strcat(full, cb->lines[i]); if (i<cb->line_count-1) strcat(full, "\n"); flen += cl+1; }
            AppSendChat(state, full);
        } else buffer_insert_newline(cb);
    } else if (ch >= 32 && ch <= 126) buffer_insert_char(cb, (char)ch);

    if (shift) {
        if (!sel->active) { sel->active = 1; sel->start_line = old_y; sel->start_col = old_x; }
        sel->end_line = cb->cursor_y; sel->end_col = cb->cursor_x;
    } else if (ch != ERR && ch != KEY_MOUSE) { sel->active = 0; }
}
