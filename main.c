#include <ncurses.h>
#include <stdbool.h>
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
#include <curl/curl.h>
#include <pthread.h>
#include <time.h>
#include <stdarg.h>
#include <limits.h>

void log_debug(const char *fmt, ...) {
    FILE *f = fopen("cursory.log", "a");
    if (!f) return;
    time_t now = time(NULL);
    char *ts = ctime(&now); ts[strlen(ts)-1] = '\0';
    fprintf(f, "[%s] ", ts);
    va_list args; va_start(args, fmt); vfprintf(f, fmt, args); va_end(args);
    fprintf(f, "\n"); fclose(f);
}

typedef enum { PANEL_FILE_TREE, PANEL_EDITOR, PANEL_AI, PANEL_TERMINAL, PANEL_COUNT } PanelType;
typedef struct { int x, y, width, height; bool visible; const char *name; } Panel;
typedef struct FileNode { char *name; char *path; bool is_dir; bool is_expanded; struct FileNode **children; int child_count; int depth; } FileNode;
typedef struct { char **lines; int line_count; int cursor_x; int cursor_y; int scroll_x; int scroll_y; char filepath[512]; } EditorBuffer;
typedef struct { int master_fd; pid_t shell_pid; char **lines; int line_count; int scroll; } TerminalBuffer;
typedef struct { char *role; char *content; } AIMessage;
typedef struct { AIMessage *messages; int message_count; char input_buffer[1024]; int input_length; int scroll; int is_waiting; pthread_mutex_t mutex; } AIContext;
typedef struct { Panel panels[PANEL_COUNT]; PanelType active_panel; char last_action[128]; int mouse_x, mouse_y; bool running; FileNode *root; int tree_scroll; int tree_selection; EditorBuffer editor; TerminalBuffer terminal; AIContext ai; } AppState;

typedef struct { AppState *state; char *prompt; } AIThreadData;

void load_file(AppState *state, const char *path);
void free_buffer(EditorBuffer *buf);
void update_terminal(AppState *state);
void handle_terminal_input(AppState *state, int ch);
void *ai_thread_func(void *arg);

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
    if (*count >= max_count) return 0; 
    flat_list[(*count)++] = node; 
    if (node->is_expanded) { for (int i = 0; i < node->child_count; i++) flatten_tree(node->children[i], flat_list, count, max_count); } return *count;
}

// --- Editor ---
void free_buffer(EditorBuffer *buf) {
    if (buf->lines) { for (int i = 0; i < buf->line_count; i++) free(buf->lines[i]); free(buf->lines); buf->lines = NULL; } buf->line_count = 0; buf->cursor_x = 0; buf->cursor_y = 0; buf->scroll_x = 0; buf->scroll_y = 0;
}
void load_file(AppState *state, const char *path) {
    free_buffer(&state->editor); snprintf(state->editor.filepath, sizeof(state->editor.filepath), "%s", path); FILE *f = fopen(path, "r"); if (!f) return;
    char *line = NULL; size_t len = 0; ssize_t read;
    while ((read = getline(&line, &len, f)) != -1) {
        if (read > 0 && line[read-1] == '\n') line[read-1] = '\0';
        state->editor.lines = realloc(state->editor.lines, sizeof(char*) * (state->editor.line_count + 1)); state->editor.lines[state->editor.line_count++] = strdup(line);
    }
    free(line); fclose(f);
}
void draw_editor(AppState *state, Panel *p) {
    EditorBuffer *eb = &state->editor;
    for (int k = 0; k < p->height - 2 && (k + eb->scroll_y) < eb->line_count; k++) {
        char *l = eb->lines[k+eb->scroll_y]; if ((int)strlen(l) > eb->scroll_x) mvprintw(p->y+1+k, p->x+1, "%.*s", p->width-2, l + eb->scroll_x);
    }
}

// --- Terminal ---
void init_terminal(AppState *state) {
    state->terminal.line_count = 0; state->terminal.lines = NULL; state->terminal.scroll = 0; state->terminal.shell_pid = forkpty(&state->terminal.master_fd, NULL, NULL, NULL);
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
                if (buf[i] == '\x1b') esc = 1; else if (buf[i] == '\n') { state->terminal.lines = realloc(state->terminal.lines, sizeof(char*)*(++state->terminal.line_count)); state->terminal.lines[state->terminal.line_count-1] = strdup(""); }
                else if (buf[i] >= 32 || buf[i] == '\t') { int last = state->terminal.line_count-1; int len = strlen(state->terminal.lines[last]); state->terminal.lines[last] = realloc(state->terminal.lines[last], len+2); state->terminal.lines[last][len] = buf[i]; state->terminal.lines[last][len+1] = '\0'; }
            } else if (esc == 1) esc = (buf[i] == '[') ? 2 : 0; else if (esc == 2) if (buf[i] >= '@' && buf[i] <= '~') esc = 0;
        }
    }
}
void handle_terminal_input(AppState *state, int ch) {
    if (ch == KEY_PPAGE) { state->terminal.scroll += 5; } else if (ch == KEY_NPAGE) { if (state->terminal.scroll > 0) state->terminal.scroll -= 5; }
    else { char c = (ch == KEY_BACKSPACE || ch == 127) ? 127 : (ch == '\n' || ch == KEY_ENTER) ? '\n' : (char)ch; if (write(state->terminal.master_fd, &c, 1) < 0) {} state->terminal.scroll = 0; }
}

// --- AI Logic ---
size_t ai_write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t rs = size * nmemb; AppState *s = (AppState*)userp; char *raw = malloc(rs + 1); memcpy(raw, contents, rs); raw[rs] = '\0';
    pthread_mutex_lock(&s->ai.mutex);
    char *k = strstr(raw, "\"response\"");
    if (!k) {
        char *msg = strstr(raw, "\"message\"");
        if (msg) k = strstr(msg, "\"content\"");
    }
    if (k) {
        char *c = strchr(k, ':');
        if (c) {
            char *st = strchr(c, '\"');
            if (st) {
                st++; char *en = st;
                while (*en && (*en != '\"' || (*(en-1) == '\\'))) en++;
                if (*en) {
                    int len = en - st; int last = s->ai.message_count - 1;
                    int cur_l = strlen(s->ai.messages[last].content);
                    s->ai.messages[last].content = realloc(s->ai.messages[last].content, cur_l + len + 1);
                    char *d = s->ai.messages[last].content + cur_l; const char *src = st;
                    while (src < en) {
                        if (*src == '\\' && *(src+1) == 'n') { *d++ = '\n'; src += 2; }
                        else if (*src == '\\' && *(src+1) == '\"') { *d++ = '\"'; src += 2; }
                        else if (*src == '\\' && *(src+1) == '\\') { *d++ = '\\'; src += 2; }
                        else *d++ = *src++;
                    }
                    *d = '\0';
                }
            }
        }
    }
    pthread_mutex_unlock(&s->ai.mutex); free(raw); return rs;
}

char* escape_json_string(const char *in) {
    int l = strlen(in); char *out = malloc(l*2+1), *p = out;
    for (int i=0; i<l; i++) { if (in[i]=='\"') { *p++='\\'; *p++='\"'; } else if (in[i]=='\n') { *p++='\\'; *p++='n'; } else if (in[i]=='\\') { *p++='\\'; *p++='\\'; } else *p++=in[i]; } *p = '\0'; return out;
}

char* parse_json_value(const char *json, const char *key) {
    if (!json) return NULL;
    char search[128]; snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search); if (!p) return NULL;
    const char *last = p; while ((p = strstr(last + 1, search))) last = p;
    const char *colon = strchr(last, ':'); if (!colon) return NULL;
    const char *st = strchr(colon, '\"'); if (!st) return NULL;
    st++; const char *en = st;
    while (*en && (*en != '\"' || (*(en-1) == '\\'))) en++;
    if (!*en) return NULL;
    int len = en - st; char *val = malloc(len + 1);
    char *d = val; const char *s = st;
    while (s < en) {
        if (*s == '\\' && *(s+1) == 'n') { *d++ = '\n'; s += 2; }
        else if (*s == '\\' && *(s+1) == '\"') { *d++ = '\"'; s += 2; }
        else if (*s == '\\' && *(s+1) == '\\') { *d++ = '\\'; s += 2; }
        else *d++ = *s++;
    }
    *d = '\0'; return val;
}

bool is_safe_path(const char *path) {
    char resolved[PATH_MAX]; char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) == NULL) return false;
    if (realpath(path, resolved) == NULL) {
        if (strstr(path, "..")) return false;
        return true; 
    }
    int cwd_len = strlen(cwd);
    int res_len = strlen(resolved);
    if (res_len < cwd_len) return false;
    if (strncmp(resolved, cwd, cwd_len) != 0) return false;
    if (res_len > cwd_len && resolved[cwd_len] != '/') return false;
    return true;
}

char* tool_list_dir(const char *path) {
    if (!is_safe_path(path)) return strdup("Error: Parent directory access forbidden.");
    DIR *d = opendir(path); if (!d) return strdup("Error: Cannot open directory.");
    char *res = malloc(1); res[0] = '\0'; int res_len = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        int l = strlen(e->d_name);
        res = realloc(res, res_len + l + 3);
        strcat(res, e->d_name); strcat(res, ", "); res_len += l + 2;
    }
    closedir(d); return res;
}

char* tool_read_file(const char *path) {
    if (!is_safe_path(path)) return strdup("Error: Path forbidden.");
    FILE *f = fopen(path, "r"); if (!f) return strdup("Error: Cannot open.");
    char *buf = malloc(8193); int n = fread(buf, 1, 8192, f); buf[n] = '\0';
    fclose(f); return buf;
}

char* tool_grep_file(const char *path, const char *pattern) {
    if (!is_safe_path(path)) return strdup("Error: Path forbidden.");
    FILE *f = fopen(path, "r"); if (!f) return strdup("Error: Cannot open.");
    char line[1024]; char *res = malloc(1); res[0] = '\0'; int rlen = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, pattern)) {
            int llen = strlen(line); 
            if (rlen + llen > 8000) {
                res = realloc(res, rlen + 32); strcat(res, "\n... (Truncated)"); break;
            }
            res = realloc(res, rlen + llen + 1);
            strcpy(res + rlen, line); rlen += llen;
        }
    }
    fclose(f); if (rlen == 0) return strdup("Pattern not found.");
    return res;
}

void* ai_thread_func(void *arg) {
    AIThreadData *td = (AIThreadData*)arg; AppState *s = td->state;
    char last_path[512] = ""; char last_tool[64] = "";
    int iterations = 0;
    while (iterations < 5) {
        pthread_mutex_lock(&s->ai.mutex);
        char cwd[512]; if (getcwd(cwd,sizeof(cwd))==NULL) strcpy(cwd,".");
        char *full_p = malloc(350000);
        sprintf(full_p, "### PERSONALITY: You are a low-level project explorer. You respond ONLY with JSON.\n"
                        "### TOOLS: list_dir, read_file, grep_file\n"
                        "### TASK: To answer the query, choose ONE:\n"
                        "1. Call a tool: {\"action\": \"tool\", \"path\": \"relative_file\"}\n"
                        "2. Final reply: {\"response\": \"answer\"}\n"
                        "### PROJECT: CWD=%s, FILES=[", cwd);
        DIR *dir = opendir(".");
        if (dir) {
            struct dirent *e; while ((e = readdir(dir))) if (e->d_name[0]!='.') { strcat(full_p, e->d_name); strcat(full_p, ", "); }
            closedir(dir);
        }
        strcat(full_p, "]\n### HISTORY (Last 4):\n");
        int start = s->ai.message_count > 4 ? s->ai.message_count - 4 : 0;
        for (int i=start; i<s->ai.message_count; i++) {
            strcat(full_p, s->ai.messages[i].role); strcat(full_p, ": ");
            strcat(full_p, s->ai.messages[i].content); strcat(full_p, "\n");
        }
        strcat(full_p, "### RESPONSE: ");
        pthread_mutex_unlock(&s->ai.mutex);

        CURL *curl = curl_easy_init();
        if (!curl) { free(full_p); break; }
        struct curl_slist *h = curl_slist_append(NULL, "Content-Type: application/json");
        char *esc_p = escape_json_string(full_p); free(full_p);
        size_t pay_size = 1024000; char *pay = malloc(pay_size);
        snprintf(pay, pay_size, "{\"model\":\"llama3\",\"prompt\":\"%s\",\"stream\":true}", esc_p); free(esc_p);
        log_debug("Full JSON Payload: %s", pay);
        
        curl_easy_setopt(curl, CURLOPT_URL, "http://127.0.0.1:11434/api/generate");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, h); curl_easy_setopt(curl, CURLOPT_POSTFIELDS, pay);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ai_write_callback); curl_easy_setopt(curl, CURLOPT_WRITEDATA, s);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L); curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);

        pthread_mutex_lock(&s->ai.mutex);
        int last = s->ai.message_count - 1;
        free(s->ai.messages[last].content); s->ai.messages[last].content = strdup("");
        snprintf(s->last_action, 128, "Thinking (%d)...", iterations + 1);
        pthread_mutex_unlock(&s->ai.mutex);

        CURLcode res = curl_easy_perform(curl);
        curl_slist_free_all(h); free(pay); curl_easy_cleanup(curl);
        if (res != CURLE_OK) break;

        pthread_mutex_lock(&s->ai.mutex);
        char *content = strdup(s->ai.messages[s->ai.message_count-1].content);
        pthread_mutex_unlock(&s->ai.mutex);
        log_debug("AI Output: %s", content);

        char *action = parse_json_value(content, "action");
        char *path = parse_json_value(content, "path");
        char *pattern = parse_json_value(content, "pattern");
        char *ai_msg = parse_json_value(content, "response");
        bool loop = false;

        if (action && path && strcmp(action, last_tool) == 0 && strcmp(path, last_path) == 0) {
            log_debug("Loop detected. Stopping.");
            free(action); free(path); free(pattern); free(ai_msg); free(content); break;
        }
        if (path) strncpy(last_path, path, sizeof(last_path)-1);
        if (action) strncpy(last_tool, action, sizeof(last_tool)-1);

        if (action && strcmp(action, "list_dir") == 0 && path) {
            char *result = tool_list_dir(path);
            pthread_mutex_lock(&s->ai.mutex);
            free(s->ai.messages[s->ai.message_count-1].content);
            char info[256]; snprintf(info, sizeof(info), "[Used tool: list_dir %s]", path);
            s->ai.messages[s->ai.message_count-1].content = strdup(info);
            s->ai.messages = realloc(s->ai.messages, sizeof(AIMessage)*(s->ai.message_count+2));
            char final_res[10240]; snprintf(final_res, sizeof(final_res), "Tool result: %s", result);
            s->ai.messages[s->ai.message_count++] = (AIMessage){strdup("user"), strdup(final_res)};
            s->ai.messages[s->ai.message_count++] = (AIMessage){strdup("assistant"), strdup("")};
            pthread_mutex_unlock(&s->ai.mutex); free(result); loop = true;
        } else if (action && strcmp(action, "read_file") == 0 && path) {
            char *result = tool_read_file(path);
            pthread_mutex_lock(&s->ai.mutex);
            free(s->ai.messages[s->ai.message_count-1].content);
            char info[256]; snprintf(info, sizeof(info), "[Used tool: read_file %s]", path);
            s->ai.messages[s->ai.message_count-1].content = strdup(info);
            s->ai.messages = realloc(s->ai.messages, sizeof(AIMessage)*(s->ai.message_count+2));
            char final_res[12000]; snprintf(final_res, sizeof(final_res), "Tool result: %s", result);
            s->ai.messages[s->ai.message_count++] = (AIMessage){strdup("user"), strdup(final_res)};
            s->ai.messages[s->ai.message_count++] = (AIMessage){strdup("assistant"), strdup("")};
            pthread_mutex_unlock(&s->ai.mutex); free(result); loop = true;
        } else if (action && strcmp(action, "grep_file") == 0 && path && pattern) {
            char *result = tool_grep_file(path, pattern);
            pthread_mutex_lock(&s->ai.mutex);
            free(s->ai.messages[s->ai.message_count-1].content);
            char info[256]; snprintf(info, sizeof(info), "[Used tool: grep_file %s %s]", path, pattern);
            s->ai.messages[s->ai.message_count-1].content = strdup(info);
            s->ai.messages = realloc(s->ai.messages, sizeof(AIMessage)*(s->ai.message_count+2));
            char final_res[12000]; snprintf(final_res, sizeof(final_res), "Tool result: %s", result);
            s->ai.messages[s->ai.message_count++] = (AIMessage){strdup("user"), strdup(final_res)};
            s->ai.messages[s->ai.message_count++] = (AIMessage){strdup("assistant"), strdup("")};
            pthread_mutex_unlock(&s->ai.mutex); free(result); loop = true;
        } else if (action) {
            pthread_mutex_lock(&s->ai.mutex);
            free(s->ai.messages[s->ai.message_count-1].content);
            char info[256]; snprintf(info, sizeof(info), "[Rejected Tool: %s]", action);
            s->ai.messages[s->ai.message_count-1].content = strdup(info);
            s->ai.messages = realloc(s->ai.messages, sizeof(AIMessage)*(s->ai.message_count+2));
            char err[256]; snprintf(err, sizeof(err), "Error: Tool '%s' not found. Available: list_dir, read_file, grep_file.", action);
            s->ai.messages[s->ai.message_count++] = (AIMessage){strdup("user"), strdup(err)};
            s->ai.messages[s->ai.message_count++] = (AIMessage){strdup("assistant"), strdup("")};
            pthread_mutex_unlock(&s->ai.mutex); loop = true;
        } else if (ai_msg) {
            pthread_mutex_lock(&s->ai.mutex);
            free(s->ai.messages[s->ai.message_count-1].content);
            s->ai.messages[s->ai.message_count-1].content = strdup(ai_msg);
            pthread_mutex_unlock(&s->ai.mutex);
        }
        free(action); free(path); free(pattern); free(ai_msg); free(content);
        if (!loop) break;
        iterations++;
    }
    pthread_mutex_lock(&s->ai.mutex); s->ai.is_waiting = 0; snprintf(s->last_action, 128, "AI: Done."); pthread_mutex_unlock(&s->ai.mutex);
    free(td->prompt); free(td); return NULL;
}

void handle_ai_input(AppState *state, int ch) {
    pthread_mutex_lock(&state->ai.mutex); if (state->ai.is_waiting) { pthread_mutex_unlock(&state->ai.mutex); return; }
    if (ch == '\n' || ch == KEY_ENTER) {
        if (state->ai.input_length > 0) {
            state->ai.messages = realloc(state->ai.messages, sizeof(AIMessage)*(state->ai.message_count+2));
            state->ai.messages[state->ai.message_count++] = (AIMessage){strdup("user"), strdup(state->ai.input_buffer)};
            state->ai.messages[state->ai.message_count++] = (AIMessage){strdup("assistant"), strdup("")};
            AIThreadData *td = malloc(sizeof(AIThreadData)); td->state = state; td->prompt = strdup(state->ai.input_buffer);
            pthread_t tid; pthread_create(&tid, NULL, ai_thread_func, td); pthread_detach(tid);
            state->ai.is_waiting = 1; state->ai.input_length = 0; state->ai.input_buffer[0] = '\0';
        }
    } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) { if (state->ai.input_length > 0) state->ai.input_buffer[--state->ai.input_length] = '\0'; }
    else if (ch >= 32 && ch < 127 && state->ai.input_length < 1023) { state->ai.input_buffer[state->ai.input_length++] = (char)ch; state->ai.input_buffer[state->ai.input_length] = '\0'; }
    else if (ch == KEY_UP || ch == KEY_PPAGE) state->ai.scroll++; else if ((ch == KEY_DOWN || ch == KEY_NPAGE) && state->ai.scroll > 0) state->ai.scroll--;
    pthread_mutex_unlock(&state->ai.mutex);
}

void draw_terminal(AppState *state, Panel *p) {
    TerminalBuffer *buf = &state->terminal; int vis = p->height-2; 
    for (int i=0; i<vis; i++) {
        int idx = buf->line_count-1-buf->scroll-(vis-1-i); if (idx>=0 && idx<buf->line_count) mvprintw(p->y+1+i, p->x+1, "%.*s", p->width-2, buf->lines[idx]);
    }
}

void draw_ai_chat(AppState *state, Panel *p) {
    pthread_mutex_lock(&state->ai.mutex);
    int iw = p->width - 4; if (iw < 1) iw = 1;
    int in_lines = (state->ai.input_length / iw) + 1;
    for (int i=0; i<in_lines; i++) {
        int off = i*iw; int l = state->ai.input_length - off; if (l<0) l=0; if (l>iw) l=iw;
        mvprintw(p->y + p->height - 1 - in_lines + i, p->x + 1, (i==0 ? "> %.*s" : "  %.*s"), iw, state->ai.input_buffer + off);
    }
    
    int dy = p->y + p->height - 2 - in_lines, midx = state->ai.message_count - 1, sc = state->ai.scroll;
    while (midx >= 0 && dy > p->y) {
        AIMessage *m = &state->ai.messages[midx]; 
        if (strcmp(m->role, "system") == 0) { midx--; continue; } 
        char *content = strdup(m->content); const char *role = (strcmp(m->role, "user") == 0 ? "You" : "AI");
        char *line = strtok(content, "\n"); char **wrapped = NULL; int wc = 0;
        int mw = p->width - 6; if (mw < 1) mw = 1;
        while (line) {
            char *curr = line;
            while (*curr) {
                int clen = strlen(curr);
                if (clen <= mw) {
                    wrapped = realloc(wrapped, sizeof(char*)*(wc+1));
                    wrapped[wc++] = strdup(curr);
                    break;
                } else {
                    int split = mw; while (split > 0 && curr[split] != ' ') split--;
                    if (split == 0) split = mw; 
                    wrapped = realloc(wrapped, sizeof(char*)*(wc+1));
                    wrapped[wc] = malloc(split+1); strncpy(wrapped[wc], curr, split); wrapped[wc][split] = '\0'; wc++;
                    curr += split; while (*curr == ' ') curr++;
                }
            }
            line = strtok(NULL, "\n");
        }
        for (int i=wc-1; i>=0 && dy > p->y; i--) {
            if (sc > 0) sc--; else {
                if (i == 0) mvprintw(dy, p->x+1, "%s: %.*s", role, p->width-1-(int)strlen(role)-2, wrapped[i]);
                else mvprintw(dy, p->x+5, "%.*s", p->width-6, wrapped[i]);
                dy--;
            }
        }
        for (int i=0; i<wc; i++) {
            free(wrapped[i]);
        }
        free(wrapped); free(content); midx--;
    }
    pthread_mutex_unlock(&state->ai.mutex);
}

void draw_panels(AppState *state) {
    for (int i=0; i<PANEL_COUNT; i++) {
        Panel *p = &state->panels[i]; if (!p->visible) continue;
        if ((int)state->active_panel == i) attron(A_BOLD | COLOR_PAIR(1));
        for (int y=p->y; y<p->y+p->height; y++) { mvaddch(y, p->x, ACS_VLINE); mvaddch(y, p->x+p->width-1, ACS_VLINE); }
        for (int x=p->x; x<p->x+p->width; x++) { mvaddch(p->y, x, ACS_HLINE); mvaddch(p->y+p->height-1, x, ACS_HLINE); }
        mvaddch(p->y, p->x, ACS_ULCORNER); mvaddch(p->y, p->x+p->width-1, ACS_URCORNER);
        mvaddch(p->y+p->height-1, p->x, ACS_LLCORNER); mvaddch(p->y+p->height-1, p->x+p->width-1, ACS_LRCORNER);
        mvprintw(p->y, p->x+2, " %.*s ", p->width-4, p->name); if ((int)state->active_panel == i) attroff(A_BOLD | COLOR_PAIR(1));
        if (i == PANEL_FILE_TREE) {
            FileNode *flat[1000]; int count = 0; flatten_tree(state->root, flat, &count, 1000);
            for (int j=0; j<p->height-2 && (j+state->tree_scroll)<count; j++) {
                int idx = j+state->tree_scroll; if (idx==state->tree_selection && state->active_panel==PANEL_FILE_TREE) attron(A_REVERSE);
                mvprintw(p->y+1+j, p->x+1, "%.*s", p->width-2, flat[idx]->name);
                if (idx==state->tree_selection && state->active_panel==PANEL_FILE_TREE) attroff(A_REVERSE);
            }
        } else if (i == PANEL_EDITOR) draw_editor(state, p);
        else if (i == PANEL_AI) draw_ai_chat(state, p);
        else if (i == PANEL_TERMINAL) draw_terminal(state, p);
    }
    Panel *ap = &state->panels[state->active_panel];
    if (state->active_panel == PANEL_EDITOR) {
        curs_set(1); move(ap->y+1+state->editor.cursor_y-state->editor.scroll_y, ap->x+1+state->editor.cursor_x-state->editor.scroll_x);
    } else if (state->active_panel == PANEL_TERMINAL) {
        curs_set(1); int sy = ap->y+ap->height-2-state->terminal.scroll;
        if (sy>ap->y && sy<ap->y+ap->height-1) move(sy, ap->x+1+(int)strlen(state->terminal.lines[state->terminal.line_count-1]));
        else curs_set(0);
    } else if (state->active_panel == PANEL_AI) {
        curs_set(1); int iw = ap->width - 4; if (iw < 1) iw = 1;
        int in_lines = (state->ai.input_length / iw) + 1;
        int row = state->ai.input_length / iw; int col = state->ai.input_length % iw;
        move(ap->y + ap->height - 1 - (in_lines - row), ap->x + 3 + col);
    } else curs_set(0);
}

void init_app(AppState *state) {
    for (int i=0; i<PANEL_COUNT; i++) { state->panels[i].visible = true; }
    state->panels[PANEL_FILE_TREE].name = "Files"; state->panels[PANEL_EDITOR].name = "Editor";
    state->panels[PANEL_AI].name = "Chat"; state->panels[PANEL_TERMINAL].name = "Terminal";
    state->active_panel = PANEL_FILE_TREE; state->running = true; state->last_action[0] = '\0';
    state->root = create_file_node(".", ".", true, 0); state->root->is_expanded = true; load_directory(state->root);
    state->tree_selection = 0; state->tree_scroll = 0; state->editor.line_count = 0; state->editor.lines = NULL; strcpy(state->editor.filepath, "[No File]");
    state->ai.messages = NULL; state->ai.message_count = 0; state->ai.input_length = 0; state->ai.input_buffer[0] = '\0'; state->ai.scroll = 0; state->ai.is_waiting = 0;
    pthread_mutex_init(&state->ai.mutex, NULL); init_terminal(state); curl_global_init(CURL_GLOBAL_ALL);
}

void calculate_layout(AppState *state) {
    int my, mx; getmaxyx(stdscr, my, mx);
    int th = state->panels[PANEL_TERMINAL].visible ? (my-2)/4 : 0; if (th<3 && th>0) th=3; int uh = my-2-th;
    state->panels[PANEL_TERMINAL] = (Panel){0, my-1-th, mx, th, state->panels[PANEL_TERMINAL].visible, "Terminal"};
    int vc = 0; for (int i=0; i<3; i++) if (state->panels[i].visible) vc++;
    if (vc > 0) {
        int x=0, fw = state->panels[PANEL_FILE_TREE].visible ? mx/5 : 0; if (fw>0 && fw<20) fw=20;
        if (state->panels[PANEL_FILE_TREE].visible) { state->panels[PANEL_FILE_TREE] = (Panel){x, 1, fw, uh, true, "Files"}; x+=fw; }
        int rem = mx-x; int ow = (vc > (state->panels[PANEL_FILE_TREE].visible?1:0)) ? rem/(vc-(state->panels[PANEL_FILE_TREE].visible?1:0)) : rem;
        if (state->panels[PANEL_EDITOR].visible) { state->panels[PANEL_EDITOR] = (Panel){x, 1, ow, uh, true, "Editor"}; x+=ow; }
        if (state->panels[PANEL_AI].visible) { state->panels[PANEL_AI] = (Panel){x, 1, mx-x, uh, true, "Chat"}; }
    }
}

void handle_input(AppState *state) {
    timeout(10); int ch = getch(); if (ch == ERR) return; MEVENT ev;
    if (ch == 27) {
        timeout(25); int n = getch();
        if (n == 'q') state->running = false;
        else if (n == 'f') state->panels[PANEL_FILE_TREE].visible = !state->panels[PANEL_FILE_TREE].visible;
        else if (n == 'e') state->panels[PANEL_EDITOR].visible = !state->panels[PANEL_EDITOR].visible;
        else if (n == 'a') state->panels[PANEL_AI].visible = !state->panels[PANEL_AI].visible;
        else if (n == 't') state->panels[PANEL_TERMINAL].visible = !state->panels[PANEL_TERMINAL].visible;
        return;
    }
    if (ch == KEY_MOUSE && getmouse(&ev) == OK) {
        for (int i=0; i<PANEL_COUNT; i++) {
            Panel *p = &state->panels[i];
            if (p->visible && ev.x>=p->x && ev.x<p->x+p->width && ev.y>=p->y && ev.y<p->y+p->height) {
                state->active_panel = i;
                if (i == PANEL_FILE_TREE) {
                    FileNode *fl[1000]; int c=0; flatten_tree(state->root, fl, &c, 1000);
                    int sel = ev.y - p->y - 1 + state->tree_scroll;
                    if (sel >= 0 && sel < c) {
                        if (sel == state->tree_selection && (ev.bstate & BUTTON1_DOUBLE_CLICKED)) {
                             FileNode *s = fl[sel];
                             if (s->is_dir) { s->is_expanded = !s->is_expanded; if (s->is_expanded && s->child_count == 0) load_directory(s); }
                             else { load_file(state, s->path); state->active_panel = PANEL_EDITOR; }
                        }
                        state->tree_selection = sel;
                    }
                } else if (i == PANEL_EDITOR) {
                    EditorBuffer *eb = &state->editor; 
                    int cy = ev.y - p->y - 1 + eb->scroll_y; int cx = ev.x - p->x - 1 + eb->scroll_x;
                    if (cy >= 0 && cy < eb->line_count) { eb->cursor_y = cy; int len = strlen(eb->lines[cy]); eb->cursor_x = (cx > len) ? len : (cx < 0 ? 0 : cx); }
                }
                break;
            }
        }
    } else {
        if (state->active_panel == PANEL_FILE_TREE) {
            FileNode *fl[1000]; int c=0; flatten_tree(state->root, fl, &c, 1000); int vh = state->panels[PANEL_FILE_TREE].height - 2;
            if (ch == KEY_UP && state->tree_selection > 0) state->tree_selection--; else if (ch == KEY_DOWN && state->tree_selection < c-1) state->tree_selection++;
            else if (ch == KEY_PPAGE) { state->tree_selection -= vh; if (state->tree_selection < 0) state->tree_selection = 0; }
            else if (ch == KEY_NPAGE) { state->tree_selection += vh; if (state->tree_selection >= c) state->tree_selection = c - 1; }
            else if (ch == '\n' || ch == KEY_ENTER) {
                FileNode *s = fl[state->tree_selection];
                if (s->is_dir) { s->is_expanded = !s->is_expanded; if (s->is_expanded && s->child_count == 0) load_directory(s); }
                else { load_file(state, s->path); state->active_panel = PANEL_EDITOR; }
            }
            if (state->tree_selection < state->tree_scroll) state->tree_scroll = state->tree_selection;
            if (state->tree_selection >= state->tree_scroll + vh) state->tree_scroll = state->tree_selection - vh + 1;
        } else if (state->active_panel == PANEL_EDITOR) {
            EditorBuffer *eb = &state->editor; int vh = state->panels[PANEL_EDITOR].height - 2; int vw = state->panels[PANEL_EDITOR].width - 2;
            if (ch == KEY_UP && eb->cursor_y > 0) eb->cursor_y--; else if (ch == KEY_DOWN && eb->cursor_y < eb->line_count-1) eb->cursor_y++;
            else if (ch == KEY_PPAGE) { eb->cursor_y -= vh; if (eb->cursor_y < 0) eb->cursor_y = 0; }
            else if (ch == KEY_NPAGE) { eb->cursor_y += vh; if (eb->cursor_y >= eb->line_count) eb->cursor_y = eb->line_count - 1; }
            else if (ch == KEY_LEFT && eb->cursor_x > 0) eb->cursor_x--; else if (ch == KEY_RIGHT && eb->cursor_x < (eb->line_count>0 ? (int)strlen(eb->lines[eb->cursor_y]) : 0)) eb->cursor_x++;
            if (eb->cursor_y < eb->scroll_y) {
                eb->scroll_y = eb->cursor_y;
            }
            if (eb->cursor_y >= eb->scroll_y + vh) {
                eb->scroll_y = eb->cursor_y - vh + 1;
            }
            if (eb->cursor_x < eb->scroll_x) {
                eb->scroll_x = eb->cursor_x;
            }
            if (eb->cursor_x >= eb->scroll_x + vw) {
                eb->scroll_x = eb->cursor_x - vw + 1;
            }
        } else if (state->active_panel == PANEL_AI) handle_ai_input(state, ch); else if (state->active_panel == PANEL_TERMINAL) handle_terminal_input(state, ch);
    }
}

int main() {
    initscr(); raw(); keypad(stdscr, TRUE); noecho(); curs_set(0); mousemask(ALL_MOUSE_EVENTS, NULL); start_color(); init_pair(1, COLOR_CYAN, COLOR_BLACK);
    AppState state; init_app(&state);
    while (state.running) {
        update_terminal(&state); calculate_layout(&state); erase();
        attron(A_REVERSE); mvhline(0,0,' ',COLS); mvprintw(0,1," Cursory "); mvhline(LINES-1,0,' ',COLS); mvprintw(LINES-1,1," [%s] ", state.last_action); attroff(A_REVERSE);
        draw_panels(&state); refresh(); handle_input(&state);
    }
    endwin(); return 0;
}
