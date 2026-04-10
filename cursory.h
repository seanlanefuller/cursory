#ifndef CURSORY_H
#define CURSORY_H

#include <ncurses.h>
#include <stdbool.h>
#include <time.h>
#include "config.h"
#include <curl/curl.h>
#include <pthread.h>
#include <sys/types.h>

typedef enum { PANEL_FILE_TREE, PANEL_EDITOR, PANEL_AI, PANEL_TERMINAL, PANEL_CHAT_INPUT, PANEL_COUNT } PanelType;

typedef struct {
    int scroll_y;
    int content_height;
    int viewport_height;
} ScrollState;

void clamp_scroll(ScrollState *s);

typedef struct {
    int active;
    int start_line, start_col;
    int end_line, end_col;
} Selection;

typedef struct {
    char *data;
} Clipboard;


typedef struct {
    int x, y, width, height;
    bool visible;
    const char *name;
} Panel;

typedef struct FileNode {
    char *name;
    char *path;
    bool is_dir;
    bool is_expanded;
    struct FileNode **children;
    int child_count;
    int depth;
} FileNode;

typedef struct {
    char **lines;
    int line_count;
    int cursor_x;
    int cursor_y;
    int scroll_x;
    ScrollState scroll;
    Selection selection;
    char filepath[512]; // Only used if buffer is associated with a file
    time_t last_modified;
} TextBuffer;

typedef TextBuffer EditorBuffer;
typedef TextBuffer ChatInputBuffer;

typedef struct {
    int master_fd;
    pid_t shell_pid;
    TextBuffer base;
} TerminalBuffer;


typedef struct {
    char *role;
    char *content;
    char *reasoning;
} AIMessage;

typedef struct {
    char type[16]; // "replace", "insert", "delete"
    int line;
    char *content;
} PatchOp;

typedef struct {
    AIMessage *messages;
    int message_count;
    ChatInputBuffer input;
    ScrollState scroll;
    int is_waiting;
    pthread_mutex_t mutex;
    char pending_path[1024];
    char *pending_content;
    char *diff_text;
    bool is_waiting_approval;
    Selection selection;
    char *stream_buffer;
    int stream_buffer_len;
} AIContext;


typedef struct {
    Panel panels[PANEL_COUNT];
    PanelType active_panel;
    char last_action[1024];

    int mouse_x, mouse_y;
    int press_line, press_col;
    PanelType press_panel;
    bool button1_down;
    bool running;
    bool mouse_dragging;
    PanelType drag_start_panel;
    long long last_click_ms;
    int last_click_line;
    PanelType last_click_panel;
    FileNode *root;
    ScrollState tree_scroll;
    int tree_selection;
    time_t last_dir_modified;
    EditorBuffer editor;
    TerminalBuffer terminal;
    AIContext ai;
    Clipboard clipboard;
    char last_search[256];
    char last_replace[256];
} AppState;


typedef struct {
    AppState *state;
    char *prompt;
} AIThreadData;

// --- Utils ---
void log_debug(const char *fmt, ...);
void log_chat(const char *role, const char *content);
void segv_handler(int sig);

char* parse_json_value(const char *json, const char *key);
int parse_patch_ops(const char *json, PatchOp **ops);
char** apply_patch_to_buffer(char **lines, int *count, PatchOp *ops, int op_count);
char* escape_json_string(const char *in);
char* generate_line_diff(const char *old_ctx, const char *new_ctx);
bool is_safe_path(const char *path);

// --- File Tree ---
FileNode* create_file_node(const char *name, const char *path, bool is_dir, int depth);
void free_file_tree(FileNode *node);
void load_directory(FileNode *node);
int flatten_tree(FileNode *node, FileNode **flat_list, int *count, int max_count);

// --- Buffer ---
void buffer_init(TextBuffer *buf);
void buffer_free(TextBuffer *buf);
void buffer_clear(TextBuffer *buf);
void buffer_load_file(TextBuffer *buf, const char *path);
void buffer_save_file(TextBuffer *buf, const char *path);
void buffer_insert_char(TextBuffer *buf, int ch);
void buffer_delete_char(TextBuffer *buf);
void buffer_insert_newline(TextBuffer *buf);
void buffer_insert_line(TextBuffer *buf, int row, const char *content);
void buffer_delete_line(TextBuffer *buf, int row);
void buffer_set_content(TextBuffer *buf, const char *content);
void buffer_replace_text(TextBuffer *buf, int y, int x, int old_len, const char *new_text);

// --- UI Modular Components ---
void draw_file_tree(AppState *state, Panel *p);
void handle_tree_input(AppState *state, int ch);

void draw_editor(AppState *state, Panel *p);
void handle_editor_input(AppState *state, int ch, int base_ch, bool shift);

void draw_ai_chat(AppState *state, Panel *p);
void draw_chat_input(AppState *state, Panel *p);
void handle_chat_input(AppState *state, int ch, int base_ch, bool shift);
void app_show_approval_dialog(AppState *state);

void draw_terminal(AppState *state, Panel *p);
void handle_terminal_input(AppState *state, int ch);

// --- Terminal Logic ---
void init_terminal(AppState *state);
void update_terminal(AppState *state);

// --- AI Chat Logic ---
void* ai_thread_func(void *arg);
size_t ai_write_callback(void *contents, size_t size, size_t nmemb, void *userp);
void apply_pending_edit(AppState *state, bool approved);
void clear_ai_history(AppState *state);
void AppSendChat(AppState *state, const char *prompt);

// --- Global UI & App Logic ---
void init_app(AppState *state);
void calculate_layout(AppState *state);
void draw_panels(AppState *state);
void handle_input(AppState *state);
void draw_scrollbar(AppState *state, Panel *p, ScrollState *ss);
ScrollState* get_panel_scroll(AppState *state, PanelType type);
Selection* get_panel_selection(AppState *state, PanelType type);
Selection get_normalized_selection(Selection *s);

void app_cut(AppState *state);
void app_copy(AppState *state);
void app_paste(AppState *state);
void app_select_all(AppState *state);
char* extract_selection(AppState *state, PanelType type);
void editor_save(AppState *state);
void app_find(AppState *state);
void app_find_next(AppState *state);
void app_replace(AppState *state);
void app_replace_all(AppState *state);

// --- Tool Callbacks ---
char* tool_list_dir(const char *path);
char* tool_read_file(const char *path);

#endif
