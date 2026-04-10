#define _GNU_SOURCE
#include "cursory.h"
#include <string.h>

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

    // Status line: Line and Column position on the bottom border
    char status[64];
    snprintf(status, sizeof(status), " Ln %d, Col %d ", eb->cursor_y + 1, eb->cursor_x + 1);
    int slen = strlen(status);
    if (p->width > slen + 4) {
        mvprintw(p->y + p->height - 1, p->x + p->width - slen - 2, "%s", status);
    }
}

void handle_editor_input(AppState *state, int ch, int base_ch, bool shift) {
    EditorBuffer *eb = &state->editor; Selection *sel = &eb->selection;
    int old_y = eb->cursor_y, old_x = eb->cursor_x;

    if (base_ch == KEY_UP && eb->cursor_y > 0) {
        eb->cursor_y--;
        int len = strlen(eb->lines[eb->cursor_y]); if (eb->cursor_x > len) eb->cursor_x = len;
    } else if (base_ch == KEY_DOWN && eb->cursor_y < eb->line_count-1) {
        eb->cursor_y++;
        int len = strlen(eb->lines[eb->cursor_y]); if (eb->cursor_x > len) eb->cursor_x = len;
    } else if (base_ch == KEY_PPAGE) {
        eb->cursor_y -= eb->scroll.viewport_height; if (eb->cursor_y < 0) eb->cursor_y = 0;
        int len = strlen(eb->lines[eb->cursor_y]); if (eb->cursor_x > len) eb->cursor_x = len;
    } else if (base_ch == KEY_NPAGE) {
        eb->cursor_y += eb->scroll.viewport_height; if (eb->cursor_y >= eb->line_count) eb->cursor_y = eb->line_count - 1;
        int len = strlen(eb->lines[eb->cursor_y]); if (eb->cursor_x > len) eb->cursor_x = len;
    } else if (base_ch == KEY_LEFT && eb->cursor_x > 0) eb->cursor_x--;
    else if (base_ch == KEY_RIGHT && eb->cursor_x < (int)strlen(eb->lines[eb->cursor_y])) eb->cursor_x++;
    else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8 || ch == KEY_DC) {
        if (sel->active) {
            Selection n = get_normalized_selection(sel);
            eb->cursor_x = n.end_col; eb->cursor_y = n.end_line;
            while (eb->cursor_y > n.start_line || (eb->cursor_y == n.start_line && eb->cursor_x > n.start_col)) buffer_delete_char(eb);
            sel->active = 0;
        } else {
            if (ch == KEY_DC) {
                if (eb->cursor_x < (int)strlen(eb->lines[eb->cursor_y]) || eb->cursor_y < eb->line_count - 1) {
                    if (eb->cursor_x == (int)strlen(eb->lines[eb->cursor_y])) { eb->cursor_y++; eb->cursor_x = 0; }
                    else eb->cursor_x++;
                    buffer_delete_char(eb);
                }
            } else buffer_delete_char(eb);
        }
    } else if (ch == '\n' || ch == KEY_ENTER) buffer_insert_newline(eb);
    else if (ch >= 32 && ch < 127) buffer_insert_char(eb, ch);

    if (shift) {
        if (!sel->active) { sel->active = 1; sel->start_line = old_y; sel->start_col = old_x; }
        sel->end_line = eb->cursor_y; sel->end_col = eb->cursor_x;
    } else if (ch != ERR && ch != KEY_MOUSE) sel->active = 0;
}
