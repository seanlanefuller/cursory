#ifndef CURSORY_H
#define CURSORY_H

#include <ncurses.h>
#include <stdbool.h>
#include <curl/curl.h>
#include <pthread.h>
#include <sys/types.h>

typedef enum { PANEL_FILE_TREE, PANEL_EDITOR, PANEL_AI, PANEL_TERMINAL, PANEL_CHAT_INPUT, PANEL_COUNT } PanelType;

typedef struct {
    int scroll_y;
    int content_height;
    int viewport_height;
} ScrollState;

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
    char filepath[512];
    time_t last_modified;
    Selection selection;
} EditorBuffer;

typedef struct {
    char **lines;
    int line_count;
    int cursor_x;
    int cursor_y;
    ScrollState scroll;
    Selection selection;
} ChatInputBuffer;


typedef struct {
    int master_fd;
    pid_t shell_pid;
    char **lines;
    int line_count;
    ScrollState scroll;
    Selection selection;
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

// --- Editor ---
void load_file(AppState *state, const char *path);
void log_debug(const char *fmt, ...);
void log_chat(const char *role, const char *content);
char* parse_json_value(const char *json, const char *key);
void save_file(AppState *state);
void insert_char(AppState *state, int ch);
void delete_char(AppState *state);
void insert_newline(AppState *state);
void free_buffer(EditorBuffer *buf);

// --- Terminal ---
void init_terminal(AppState *state);
void update_terminal(AppState *state);
void handle_terminal_input(AppState *state, int ch);

// --- AI ---
void* ai_thread_func(void *arg);
size_t ai_write_callback(void *contents, size_t size, size_t nmemb, void *userp);
void apply_pending_edit(AppState *state, bool approved);
char* tool_list_dir(const char *path);
char* tool_read_file(const char *path);
void clear_ai_history(AppState *state);
void AppSendChat(AppState *state);
void app_cut(AppState *state);
void app_copy(AppState *state);
void app_paste(AppState *state);
void app_select_all(AppState *state);
char* extract_selection(AppState *state, PanelType type);
Selection get_normalized_selection(Selection *sel);
void editor_save(AppState *state);

// --- UI ---
void init_app(AppState *state);
void calculate_layout(AppState *state);
void draw_panels(AppState *state);
void draw_file_tree(AppState *state, Panel *p);
void draw_editor(AppState *state, Panel *p);
void draw_terminal(AppState *state, Panel *p);
void draw_ai_chat(AppState *state, Panel *p);
void handle_input(AppState *state);
void handle_ai_input(AppState *state, int ch);

#endif
