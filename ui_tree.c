#define _GNU_SOURCE
#include "cursory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

FileNode* create_file_node(const char *name, const char *path, bool is_dir, int depth) {
    FileNode *node = malloc(sizeof(FileNode));
    node->name = strdup(name);
    node->path = strdup(path);
    node->is_dir = is_dir;
    node->is_expanded = false;
    node->children = NULL;
    node->child_count = 0;
    node->depth = depth;
    return node;
}

void free_file_tree(FileNode *node) {
    if (!node) return;
    for (int i = 0; i < node->child_count; i++) free_file_tree(node->children[i]);
    free(node->children);
    free(node->name);
    free(node->path);
    free(node);
}

void load_directory(FileNode *node) {
    if (!node->is_dir) return; 
    DIR *dir = opendir(node->path);
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/%s", node->path, entry->d_name);
        struct stat st;
        stat(full_path, &st);
        node->children = realloc(node->children, sizeof(FileNode*) * (node->child_count + 1));
        node->children[node->child_count++] = create_file_node(entry->d_name, full_path, S_ISDIR(st.st_mode), node->depth + 1);
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

void draw_file_tree(AppState *state, Panel *p) {
    static FileNode *flat[1000];

    // Optimization: Only re-flatten if root has changed or some time has passed
    // For now, let's just do it simply but keep the function separate
    int count = 0;
    flatten_tree(state->root, flat, &count, 1000);

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
}

void handle_tree_input(AppState *state, int ch) {
    FileNode *fl[1000]; int c=0; flatten_tree(state->root, fl, &c, 1000);
    if (ch == KEY_UP && state->tree_selection > 0) state->tree_selection--; 
    else if (ch == KEY_DOWN && state->tree_selection < c-1) state->tree_selection++;
    else if (ch == KEY_PPAGE) { state->tree_selection -= state->tree_scroll.viewport_height; if (state->tree_selection < 0) state->tree_selection = 0; }
    else if (ch == KEY_NPAGE) { state->tree_selection += state->tree_scroll.viewport_height; if (state->tree_selection >= c) state->tree_selection = c-1; }
    else if (ch == '\n' || ch == KEY_ENTER) {
        FileNode *s = fl[state->tree_selection];
        if (s->is_dir) { s->is_expanded = !s->is_expanded; if (s->is_expanded && s->child_count == 0) load_directory(s); }
        else { buffer_load_file(&state->editor, s->path); state->active_panel = PANEL_EDITOR; }
    }
}
