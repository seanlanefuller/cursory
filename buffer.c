#define _GNU_SOURCE
#include "cursory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void buffer_init(TextBuffer *buf) {
    memset(buf, 0, sizeof(TextBuffer));
    buf->lines = malloc(sizeof(char*));
    buf->lines[0] = strdup("");
    buf->line_count = 1;
}

void buffer_free(TextBuffer *buf) {
    if (!buf->lines) return;
    for (int i = 0; i < buf->line_count; i++) {
        free(buf->lines[i]);
    }
    free(buf->lines);
    buf->lines = NULL;
    buf->line_count = 0;
}

void buffer_clear(TextBuffer *buf) {
    buffer_free(buf);
    buffer_init(buf);
}

void buffer_load_file(TextBuffer *buf, const char *path) {
    buffer_clear(buf);
    strncpy(buf->filepath, path, sizeof(buf->filepath) - 1);
    
    FILE *f = fopen(path, "r");
    if (!f) return;

    struct stat st;
    if (stat(path, &st) == 0) buf->last_modified = st.st_mtime;

    char *line = NULL;
    size_t len = 0;
    ssize_t read;
    
    // Remove the initial empty line from buffer_init
    free(buf->lines[0]);
    buf->line_count = 0;

    while ((read = getline(&line, &len, f)) != -1) {
        if (read > 0 && line[read-1] == '\n') line[read-1] = '\0';
        buf->lines = realloc(buf->lines, sizeof(char*) * (buf->line_count + 1));
        buf->lines[buf->line_count++] = strdup(line);
    }
    
    if (buf->line_count == 0) {
        buf->lines = realloc(buf->lines, sizeof(char*) * 1);
        buf->lines[0] = strdup("");
        buf->line_count = 1;
    }

    free(line);
    fclose(f);
}

void buffer_save_file(TextBuffer *buf, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return;

    for (int i = 0; i < buf->line_count; i++) {
        fprintf(f, "%s\n", buf->lines[i]);
    }
    fclose(f);

    struct stat st;
    if (stat(path, &st) == 0) buf->last_modified = st.st_mtime;
    if (buf != (void*)path) strncpy(buf->filepath, path, sizeof(buf->filepath) - 1);
}

void buffer_insert_char(TextBuffer *buf, int ch) {
    if (buf->line_count == 0) {
        buf->lines = malloc(sizeof(char*));
        buf->lines[0] = strdup("");
        buf->line_count = 1;
    }
    
    char *l = buf->lines[buf->cursor_y];
    int len = strlen(l);
    buf->lines[buf->cursor_y] = realloc(buf->lines[buf->cursor_y], len + 2);
    memmove(buf->lines[buf->cursor_y] + buf->cursor_x + 1, buf->lines[buf->cursor_y] + buf->cursor_x, len - buf->cursor_x + 1);
    buf->lines[buf->cursor_y][buf->cursor_x++] = (char)ch;
}

void buffer_delete_char(TextBuffer *buf) {
    if (buf->line_count == 0 || (buf->cursor_x == 0 && buf->cursor_y == 0)) return;
    
    if (buf->cursor_x > 0) {
        char *l = buf->lines[buf->cursor_y];
        int len = strlen(l);
        memmove(l + buf->cursor_x - 1, l + buf->cursor_x, len - buf->cursor_x + 1);
        buf->cursor_x--;
    } else {
        char *prev = buf->lines[buf->cursor_y-1];
        char *curr = buf->lines[buf->cursor_y];
        int plen = strlen(prev);
        int clen = strlen(curr);
        
        buf->lines[buf->cursor_y-1] = realloc(buf->lines[buf->cursor_y-1], plen + clen + 1);
        strcat(buf->lines[buf->cursor_y-1], curr);
        buf->cursor_x = plen;
        
        free(curr);
        memmove(buf->lines + buf->cursor_y, buf->lines + buf->cursor_y + 1, sizeof(char*) * (buf->line_count - buf->cursor_y - 1));
        buf->line_count--;
        buf->cursor_y--;
    }
}

void buffer_insert_newline(TextBuffer *buf) {
    if (buf->line_count == 0) {
        buf->lines = malloc(sizeof(char*));
        buf->lines[0] = strdup("");
        buf->line_count = 1;
    }
    
    char *curr = buf->lines[buf->cursor_y];
    char *next = strdup(curr + buf->cursor_x);
    curr[buf->cursor_x] = '\0';
    
    buf->lines = realloc(buf->lines, sizeof(char*) * (buf->line_count + 1));
    memmove(buf->lines + buf->cursor_y + 2, buf->lines + buf->cursor_y + 1, sizeof(char*) * (buf->line_count - buf->cursor_y - 1));
    buf->lines[buf->cursor_y+1] = next;
    buf->line_count++;
    buf->cursor_y++;
    buf->cursor_x = 0;
}

void buffer_insert_line(TextBuffer *buf, int row, const char *content) {
    if (row < 0) row = 0;
    if (row > buf->line_count) row = buf->line_count;
    
    buf->lines = realloc(buf->lines, sizeof(char*) * (buf->line_count + 1));
    memmove(buf->lines + row + 1, buf->lines + row, sizeof(char*) * (buf->line_count - row));
    buf->lines[row] = strdup(content ? content : "");
    buf->line_count++;
}

void buffer_delete_line(TextBuffer *buf, int row) {
    if (row < 0 || row >= buf->line_count) return;
    
    free(buf->lines[row]);
    memmove(buf->lines + row, buf->lines + row + 1, sizeof(char*) * (buf->line_count - row - 1));
    buf->line_count--;
    
    if (buf->line_count == 0) {
        buf->lines = realloc(buf->lines, sizeof(char*) * 1);
        buf->lines[0] = strdup("");
        buf->line_count = 1;
    }
    
    if (buf->cursor_y >= buf->line_count) buf->cursor_y = buf->line_count - 1;
}

void buffer_set_content(TextBuffer *buf, const char *content) {
    buffer_free(buf);
    if (!content || strlen(content) == 0) {
        buffer_init(buf);
        return;
    }

    char *copy = strdup(content);
    char *start = copy;
    char *newline;
    while ((newline = strchr(start, '\n'))) {
        *newline = '\0';
        buffer_insert_line(buf, buf->line_count, start);
        start = newline + 1;
    }
    buffer_insert_line(buf, buf->line_count, start);
    free(copy);

}

void buffer_replace_text(TextBuffer *buf, int y, int x, int old_len, const char *new_text) {
    if (y < 0 || y >= buf->line_count) return;
    char *line = buf->lines[y];
    int line_len = strlen(line);
    if (x < 0 || x > line_len) return;
    if (x + old_len > line_len) old_len = line_len - x;

    int new_len = new_text ? strlen(new_text) : 0;
    int diff = new_len - old_len;

    if (diff != 0) {
        buf->lines[y] = realloc(buf->lines[y], line_len + diff + 1);
        line = buf->lines[y];
        memmove(line + x + new_len, line + x + old_len, line_len - (x + old_len) + 1);
    }
    if (new_len > 0) {
        memcpy(line + x, new_text, new_len);
    }
    line[line_len + diff] = '\0';
}
