#define _GNU_SOURCE
#include "cursory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <curl/curl.h>

#include <pthread.h>
#include <unistd.h>
#include <dirent.h>
#include <signal.h>

static void process_ai_line(AppState *s, char *line) {
    if (strncmp(line, "data: ", 6) == 0) {
        char *json = line + 6;
        if (strncmp(json, "[DONE]", 6) != 0) {
            char *reasoning = parse_json_value(json, "reasoning_content");
            if (!reasoning) reasoning = parse_json_value(json, "reasoning");
            if (!reasoning) {
                char *choices = parse_json_value(json, "choices");
                if (choices) {
                    char *delta = parse_json_value(choices, "delta");
                    if (delta) { reasoning = parse_json_value(delta, "reasoning_content"); if (!reasoning) reasoning = parse_json_value(delta, "reasoning"); free(delta); }
                    free(choices);
                }
            }
            
            char *content = parse_json_value(json, "content");
            if (!content) {
                char *choices = parse_json_value(json, "choices");
                if (choices) {
                    char *delta = parse_json_value(choices, "delta");
                    if (delta) { content = parse_json_value(delta, "content"); free(delta); }
                    free(choices);
                }
            }

            int last = s->ai.message_count - 1;
            if (last >= 0) {
                AIMessage *m = &s->ai.messages[last];
                if (reasoning) {
                    int rlen = m->reasoning ? strlen(m->reasoning) : 0;
                    m->reasoning = realloc(m->reasoning, rlen + strlen(reasoning) + 1);
                    if (rlen == 0) m->reasoning[0] = '\0';
                    strcpy(m->reasoning + rlen, reasoning);
                    free(reasoning);
                }
                if (content) {
                    int clen = m->content ? strlen(m->content) : 0;
                    m->content = realloc(m->content, clen + strlen(content) + 1);
                    if (clen == 0) m->content[0] = '\0';
                    strcpy(m->content + clen, content);
                    free(content);
                }
                s->ai.scroll.scroll_y = 999999;
            }
        }
    } else if (strlen(line) > 0) {
        int last = s->ai.message_count - 1;
        if (last >= 0) {
            AIMessage *m = &s->ai.messages[last];
            int clen = m->content ? strlen(m->content) : 0;
            m->content = realloc(m->content, clen + strlen(line) + 1);
            if (clen == 0) m->content[0] = '\0';
            strcpy(m->content + clen, line);
            s->ai.scroll.scroll_y = 999999;
        }
    }
}

size_t ai_write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t rs = size * nmemb; AppState *s = (AppState*)userp;

    pthread_mutex_lock(&s->ai.mutex);
    s->ai.stream_buffer = realloc(s->ai.stream_buffer, s->ai.stream_buffer_len + rs + 1);
    memcpy(s->ai.stream_buffer + s->ai.stream_buffer_len, contents, rs);
    s->ai.stream_buffer_len += rs;
    s->ai.stream_buffer[s->ai.stream_buffer_len] = '\0';

    char *start = s->ai.stream_buffer; char *newline;
    while ((newline = strchr(start, '\n'))) {
        *newline = '\0'; process_ai_line(s, start);
        start = newline + 1;
    }
    int rem = s->ai.stream_buffer_len - (start - s->ai.stream_buffer);
    if (rem > 0) { memmove(s->ai.stream_buffer, start, rem); s->ai.stream_buffer_len = rem; s->ai.stream_buffer[s->ai.stream_buffer_len] = '\0'; }
    else s->ai.stream_buffer_len = 0;
    pthread_mutex_unlock(&s->ai.mutex);
    return rs;
}


void apply_pending_edit(AppState *state, bool approved) {
    pthread_mutex_lock(&state->ai.mutex);
    if (!approved) {
        state->ai.is_waiting_approval = false;
        state->ai.messages = realloc(state->ai.messages, sizeof(AIMessage)*(state->ai.message_count+2));
        state->ai.messages[state->ai.message_count++] = (AIMessage){strdup("user"), strdup("Edit rejected by user."), NULL};
        state->ai.messages[state->ai.message_count++] = (AIMessage){strdup("assistant"), strdup(""), NULL};
        goto cleanup;
    }

    if (state->ai.pending_content) {
        buffer_set_content(&state->editor, state->ai.pending_content);
    }

    if (strlen(state->ai.pending_path) > 0) {
        strncpy(state->editor.filepath, state->ai.pending_path, sizeof(state->editor.filepath)-1);
    }

    state->ai.messages = realloc(state->ai.messages, sizeof(AIMessage)*(state->ai.message_count+2));
    state->ai.messages[state->ai.message_count++] = (AIMessage){strdup("user"), strdup("Applied successfully."), NULL};
    state->ai.messages[state->ai.message_count++] = (AIMessage){strdup("assistant"), strdup(""), NULL};
cleanup:
    state->ai.is_waiting_approval = false;
    if (state->ai.pending_content) { free(state->ai.pending_content); state->ai.pending_content = NULL; }
    if (state->ai.diff_text) { free(state->ai.diff_text); state->ai.diff_text = NULL; }
    AIThreadData *td = malloc(sizeof(AIThreadData)); td->state = state; td->prompt = strdup("System: Process last tool result.");
    pthread_t tid; pthread_create(&tid, NULL, ai_thread_func, td); pthread_detach(tid);
    state->ai.is_waiting = 1;
    pthread_mutex_unlock(&state->ai.mutex);
}

char* tool_list_dir(const char *path) {
    if (!is_safe_path(path)) return strdup("Error: Forbidden.");
    DIR *d = opendir(path); if (!d) return strdup("Error: Cannot open.");
    char *res = strdup(""); int rlen = 0; struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        int l = strlen(e->d_name); char *tmp = realloc(res, rlen + l + 3);
        if (tmp) { res = tmp; strcat(res, e->d_name); strcat(res, ", "); rlen += l + 2; }
    }
    closedir(d); return res;
}

char* tool_read_file(const char *path) {
    if (!is_safe_path(path)) return strdup("Error: Forbidden.");
    FILE *f = fopen(path, "r"); if (!f) return strdup("Error: Cannot open.");
    char *buf = malloc(16385); int n = fread(buf, 1, 16384, f); buf[n] = '\0'; fclose(f); return buf;
}

char* tool_grep_file(const char *path, const char *pattern) {
    if (!is_safe_path(path)) return strdup("Error: Forbidden.");
    FILE *f = fopen(path, "r"); if (!f) return strdup("Error: Cannot open.");
    char line[1024]; char *res = strdup(""); int rlen = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, pattern)) {
            int l = strlen(line); char *tmp = realloc(res, rlen + l + 1);
            if (tmp) { res = tmp; strcpy(res + rlen, line); rlen += l; }
            if (rlen > 8000) break;
        }
    }
    fclose(f); return res;
}

void* ai_thread_func(void *arg) {
    AIThreadData *td = (AIThreadData*)arg; AppState *s = td->state;
    if (!td || !s) { if (td) { free(td->prompt); free(td); } return NULL; }
    signal(SIGSEGV, segv_handler); signal(SIGABRT, segv_handler);
    log_debug("AI Thread Start (Ollama): prompt='%s'", td->prompt ? td->prompt : "NULL");

    pthread_mutex_lock(&s->ai.mutex);
    char cwd[512]; if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, ".");
    
    // Build context for the AI
    char *hist = strdup(""); int hl = 0;
    // Keep more history (last 20 messages)
    int start = s->ai.message_count > 20 ? s->ai.message_count - 20 : 0;
    for (int i=start; i<s->ai.message_count; i++) {
        char *r = s->ai.messages[i].role ? s->ai.messages[i].role : "user";
        char *c = s->ai.messages[i].content ? s->ai.messages[i].content : "";
        if (strlen(c) == 0) continue;
        int l = strlen(r) + strlen(c) + 4; 
        char *tmp = realloc(hist, hl + l + 1);
        if (tmp) { 
            hist = tmp;
            if (hl == 0) hist[0] = '\0';
            strcat(hist, r); strcat(hist, ": "); strcat(hist, c); strcat(hist, "\n"); hl += l; 
        }
    }

    char *file_ctx = strdup("");
    if (strcmp(s->editor.filepath, "[No File]") != 0) {
        int f_len = 0;
        char *tmp_f;
        if (asprintf(&tmp_f, "### ACTIVE FILE (%s):\n", s->editor.filepath) != -1) {
            free(file_ctx);
            file_ctx = tmp_f;
            f_len = strlen(tmp_f);
        }
        for (int i=0; i<s->editor.line_count; i++) {
            char *line;
            int l = asprintf(&line, "%d: %s\n", i + 1, s->editor.lines[i]);
            if (l != -1) {
                char *tmp = realloc(file_ctx, f_len + l + 1);
                if (tmp) {
                    file_ctx = tmp;
                    strcpy(file_ctx + f_len, line);
                    f_len += l;
                }
                free(line);
            }
        }
    }

    char *tools_ctx = strdup("### TOOLS:\n"
                             "You can edit the buffer using the following tools (output JSON to use):\n"
                             "1. Patch existing content: {\"tool\": \"patch\", \"patch\": [{\"type\": \"replace\"|\"insert\"|\"delete\", \"line\": <line_number>, \"content\": \"<line_content>\"}]}\n"
                             "2. Overwrite or Create new content in buffer: {\"tool\": \"write_file\", \"path\": \"<filename>\", \"content\": \"<entire_file_content>\"}\n"
                             "   Note: You can use \"mode\" instead of \"tool\", and \"code\" or \"\" instead of \"content\".\n"
                             "3. Read a file from disk: {\"tool\": \"read_file\", \"path\": \"<filename>\"}\n"
                             "4. Search for a pattern in a file: {\"tool\": \"grep_file\", \"path\": \"<filename>\", \"pattern\": \"<string_to_find>\"}\n"
                             "5. List files in a directory: {\"tool\": \"list_dir\", \"path\": \"<directory_path>\"}\n"
                             "CRITICAL: Do NOT invent tools like `append_file`. Use `patch` with `insert` instead.\n"
                             "CRITICAL: All changes are applied to the ACTIVE EDITOR BUFFER ONLY. They do NOT touch the filesystem (except for read-only tools).\n"
                             "CRITICAL: When talking back to the user, you MUST respond in normal conversational plaintext! Do NOT wrap your conversational replies dynamically into explicit JSON structures!\n");

    char *full_p;
    if (asprintf(&full_p, "You are a helpful AI coding assistant running in an IDE in Linux.\n"
                          "%s\n%s\n"
                          "### CONVERSATION HISTORY:\n%s\n"
                          "### NEW USER MESSAGE: %s\n"
                          "### YOUR RESPONSE:", 
                          tools_ctx, file_ctx, hist, td->prompt) == -1) {
        free(hist); free(tools_ctx); free(file_ctx);
        s->ai.is_waiting = 0;
        pthread_mutex_unlock(&s->ai.mutex);
        free(td->prompt); free(td);
        return NULL;
    }
    free(hist); free(tools_ctx); free(file_ctx);

    int last = s->ai.message_count - 1;
    if (last >= 0) { free(s->ai.messages[last].content); s->ai.messages[last].content = strdup(""); }
    snprintf(s->last_action, 128, "AI Thinking...");
    pthread_mutex_unlock(&s->ai.mutex);

    char *model = strdup("qwen2.5-coder:7b");
    FILE *cf = fopen("config.txt", "r");
    if (cf) {
        char buf[128];
        if (fgets(buf, sizeof(buf), cf)) {
            char *p = strchr(buf, '\n'); if (p) *p = '\0';
            p = strchr(buf, '\r'); if (p) *p = '\0';
            if (strlen(buf) > 0) { free(model); model = strdup(buf); }
        }
        fclose(cf);
    }

    CURL *curl = curl_easy_init(); 
    if (curl) {
        char *esc = escape_json_string(full_p); free(full_p);
        char *pay; 
        if (asprintf(&pay, "{\"model\": \"%s\", \"messages\": [{\"role\": \"user\", \"content\": \"%s\"}], \"stream\": true, \"options\": {\"temperature\": 0.3}}", model, esc) != -1) {
            struct curl_slist *h = NULL; h = curl_slist_append(h, "Content-Type: application/json");
            curl_easy_setopt(curl, CURLOPT_URL, "http://localhost:11434/v1/chat/completions");
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, h); curl_easy_setopt(curl, CURLOPT_POSTFIELDS, pay);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ai_write_callback); curl_easy_setopt(curl, CURLOPT_WRITEDATA, s);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L); curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
            
            CURLcode res_curl = curl_easy_perform(curl);
            log_debug("CURL Result: %d", res_curl);
            
            if (res_curl == CURLE_OK) {
                pthread_mutex_lock(&s->ai.mutex);
                if (s->ai.stream_buffer_len > 0) { process_ai_line(s, s->ai.stream_buffer); s->ai.stream_buffer_len = 0; }
                int midx = s->ai.message_count - 1;
                if (midx >= 0 && s->ai.messages[midx].content) {
                    log_debug("Full AI Response: %s", s->ai.messages[midx].content);
                    
                    if (s->ai.messages[midx].reasoning) { free(s->ai.messages[midx].reasoning); s->ai.messages[midx].reasoning = NULL; }
                    
                    if (1) {
                        PatchOp *ops = NULL;
                        int num_ops = parse_patch_ops(s->ai.messages[midx].content, &ops);
                        if (num_ops > 0) {
                            int line_count = s->editor.line_count;
                            char **res_lines = apply_patch_to_buffer(s->editor.lines, &line_count, ops, num_ops);
                            if (res_lines) {
                                int tlen = 0;
                                for (int i=0; i<line_count; i++) tlen += strlen(res_lines[i]) + 1;
                                char *joined = malloc(tlen + 1); joined[0] = '\0';
                                for (int i=0; i<line_count; i++) {
                                    strcat(joined, res_lines[i]);
                                    if (i < line_count - 1) strcat(joined, "\n");
                                }
                                s->ai.pending_content = joined;
                                strncpy(s->ai.pending_path, s->editor.filepath, sizeof(s->ai.pending_path)-1);
                                
                                char *old_joined = malloc(1); old_joined[0] = '\0'; int old_len = 0;
                                for(int i=0; i<s->editor.line_count; i++) {
                                    int l = strlen(s->editor.lines[i]);
                                    old_joined = realloc(old_joined, old_len + l + 2);
                                    strcpy(old_joined + old_len, s->editor.lines[i]);
                                    old_len += l;
                                    if (i < s->editor.line_count - 1) { old_joined[old_len++] = '\n'; old_joined[old_len] = '\0'; }
                                }
                                s->ai.diff_text = generate_line_diff(old_joined, joined);
                                free(old_joined);
                                
                                for(int i=0; i<line_count; i++) free(res_lines[i]);
                                free(res_lines);
                                s->ai.is_waiting_approval = true;
                            }
                            for (int i=0; i<num_ops; i++) if (ops[i].content) free(ops[i].content);
                            free(ops);
                        }
                    }

                    if (!s->ai.is_waiting_approval && s->ai.messages[midx].content) {
                        char *tool = parse_json_value(s->ai.messages[midx].content, "tool");
                        if (!tool) tool = parse_json_value(s->ai.messages[midx].content, "mode");
                        if (tool) {
                            char *path = parse_json_value(s->ai.messages[midx].content, "path");
                            char *pattern = parse_json_value(s->ai.messages[midx].content, "pattern");
                            char *result = NULL;
                            
                            if (strcmp(tool, "list_dir") == 0 && path) result = tool_list_dir(path);
                            else if (strcmp(tool, "read_file") == 0 && path) result = tool_read_file(path);
                            else if (strcmp(tool, "grep_file") == 0 && path && pattern) result = tool_grep_file(path, pattern);
                            else if (strcmp(tool, "write_file") == 0) {
                                char *content = parse_json_value(s->ai.messages[midx].content, "content");
                                if (!content) content = parse_json_value(s->ai.messages[midx].content, "code");
                                if (!content) content = parse_json_value(s->ai.messages[midx].content, "");
                                
                                if (content) {
                                    s->ai.pending_content = content;
                                    if (path) strncpy(s->ai.pending_path, path, sizeof(s->ai.pending_path)-1);
                                    else strcpy(s->ai.pending_path, s->editor.filepath);
                                    
                                    char *old_joined = malloc(1); old_joined[0] = '\0'; int old_len = 0;
                                    for(int i=0; i<s->editor.line_count; i++) {
                                        int l = strlen(s->editor.lines[i]);
                                        old_joined = realloc(old_joined, old_len + l + 2);
                                        strcpy(old_joined + old_len, s->editor.lines[i]);
                                        old_len += l;
                                        if (i < s->editor.line_count - 1) { old_joined[old_len++] = '\n'; old_joined[old_len] = '\0'; }
                                    }
                                    s->ai.diff_text = generate_line_diff(old_joined, content);
                                    free(old_joined);
                                    s->ai.is_waiting_approval = true;
                                }
                            }
                            
                            if (result) {
                                s->ai.messages = realloc(s->ai.messages, sizeof(AIMessage)*(s->ai.message_count+2));
                                s->ai.messages[s->ai.message_count++] = (AIMessage){strdup("system"), result, NULL};
                                s->ai.messages[s->ai.message_count++] = (AIMessage){strdup("assistant"), strdup(""), NULL};
                                
                                AIThreadData *td_new = malloc(sizeof(AIThreadData)); td_new->state = s; 
                                td_new->prompt = strdup("System: Process tool result.");
                                pthread_t tid; pthread_create(&tid, NULL, ai_thread_func, td_new); pthread_detach(tid);
                                s->ai.is_waiting = 1;
                            }
                            free(tool); if (path) free(path); if (pattern) free(pattern);
                            if (result) {
                                pthread_mutex_unlock(&s->ai.mutex);
                                curl_slist_free_all(h); free(pay); curl_easy_cleanup(curl); free(model);
                                return NULL;
                            }
                        }
                    }
                }
                pthread_mutex_unlock(&s->ai.mutex);
            } else {
                pthread_mutex_lock(&s->ai.mutex);
                free(s->ai.messages[s->ai.message_count-1].content);
                s->ai.messages[s->ai.message_count-1].content = strdup("Error: AI Timeout or Connection issue.");
                pthread_mutex_unlock(&s->ai.mutex);
            }
            curl_slist_free_all(h); free(pay);
        } else {
            free(esc);
        }
        curl_easy_cleanup(curl);
    }
    free(model);

    pthread_mutex_lock(&s->ai.mutex);
    if (s->ai.message_count > 0) log_chat("AI", s->ai.messages[s->ai.message_count-1].content);
    s->ai.is_waiting = 0; 
    snprintf(s->last_action, 128, "AI: Ready"); 
    pthread_mutex_unlock(&s->ai.mutex);

    free(td->prompt); free(td); return NULL;
}

void AppSendChat(AppState *state, const char *prompt) {
    pthread_mutex_lock(&state->ai.mutex);
    if (state->ai.is_waiting) { pthread_mutex_unlock(&state->ai.mutex); return; }
    if (state->ai.stream_buffer) { free(state->ai.stream_buffer); state->ai.stream_buffer = NULL; state->ai.stream_buffer_len = 0; }
    
    if (!prompt || strlen(prompt) == 0) { pthread_mutex_unlock(&state->ai.mutex); return; }

    buffer_clear(&state->ai.input);

    state->ai.messages = realloc(state->ai.messages, sizeof(AIMessage) * (state->ai.message_count + 2));
    state->ai.messages[state->ai.message_count++] = (AIMessage){strdup("user"), strdup(prompt), NULL};
    state->ai.messages[state->ai.message_count++] = (AIMessage){strdup("assistant"), strdup(""), NULL};
    
    AIThreadData *td = malloc(sizeof(AIThreadData));
    td->state = state; td->prompt = strdup(prompt);
    pthread_t tid; pthread_create(&tid, NULL, ai_thread_func, td); pthread_detach(tid);
    state->ai.is_waiting = 1; state->ai.scroll.scroll_y = 999999;
    pthread_mutex_unlock(&state->ai.mutex);
}

void clear_ai_history(AppState *state) {
    pthread_mutex_lock(&state->ai.mutex);
    for (int i=0; i<state->ai.message_count; i++) { free(state->ai.messages[i].role); free(state->ai.messages[i].content); }
    free(state->ai.messages); state->ai.messages = NULL; state->ai.message_count = 0;
    pthread_mutex_unlock(&state->ai.mutex);
}
