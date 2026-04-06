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
    FILE *lf = fopen("/tmp/cursory_ai_raw.log", "a");
    if (lf) { fwrite(contents, 1, rs, lf); fclose(lf); }

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
        state->ai.messages[state->ai.message_count++] = (AIMessage){strdup("user"), strdup("Edit rejected by user.")};
        state->ai.messages[state->ai.message_count++] = (AIMessage){strdup("assistant"), strdup("")};
        goto cleanup;
    }
    if (!is_safe_path(state->ai.pending_path)) {
        state->ai.messages = realloc(state->ai.messages, sizeof(AIMessage)*(state->ai.message_count+2));
        state->ai.messages[state->ai.message_count++] = (AIMessage){strdup("user"), strdup("Error: Path forbidden.")};
        state->ai.messages[state->ai.message_count++] = (AIMessage){strdup("assistant"), strdup("")};
        goto cleanup;
    }
    FILE *f = fopen(state->ai.pending_path, "w");
    if (!f) {
        state->ai.messages = realloc(state->ai.messages, sizeof(AIMessage)*(state->ai.message_count+2));
        state->ai.messages[state->ai.message_count++] = (AIMessage){strdup("user"), strdup("Error: Cannot write.")};
        state->ai.messages[state->ai.message_count++] = (AIMessage){strdup("assistant"), strdup("")};
    } else {
        fwrite(state->ai.pending_content, 1, strlen(state->ai.pending_content), f); fclose(f);
        state->ai.messages = realloc(state->ai.messages, sizeof(AIMessage)*(state->ai.message_count+2));
        state->ai.messages[state->ai.message_count++] = (AIMessage){strdup("user"), strdup("Applied successfully.")};
        state->ai.messages[state->ai.message_count++] = (AIMessage){strdup("assistant"), strdup("")};
        if (strcmp(state->editor.filepath, state->ai.pending_path) == 0) load_file(state, state->ai.pending_path);
        free_file_tree(state->root); state->root = create_file_node(".", ".", true, 0); state->root->is_expanded = true; load_directory(state->root);
    }
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

    char *full_p;
    if (asprintf(&full_p, "You are a helpful AI coding assistant running an an IDE in Linux.\n"
                     "### CONVERSATION HISTORY:\n%s\n### NEW USER MESSAGE: %s\n### YOUR RESPONSE:", 
                     hist, td->prompt) == -1) {
        free(hist);
        s->ai.is_waiting = 0;
        pthread_mutex_unlock(&s->ai.mutex);
        free(td->prompt); free(td);
        return NULL;
    }
    free(hist);

    int last = s->ai.message_count - 1;
    if (last >= 0) { free(s->ai.messages[last].content); s->ai.messages[last].content = strdup(""); }
    snprintf(s->last_action, 128, "AI Thinking...");
    pthread_mutex_unlock(&s->ai.mutex);

    char *model = strdup("deepseek-coder:latest");
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
        if (asprintf(&pay, "{\"model\": \"%s\", \"messages\": [{\"role\": \"user\", \"content\": \"%s\"}], \"stream\": true, \"options\": {\"temperature\": 0.7}}", model, esc) != -1) {
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
                if (midx >= 0) {
                    if (s->ai.messages[midx].reasoning) { free(s->ai.messages[midx].reasoning); s->ai.messages[midx].reasoning = NULL; }
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

void AppSendChat(AppState *state) {
    pthread_mutex_lock(&state->ai.mutex);
    if (state->ai.is_waiting) { pthread_mutex_unlock(&state->ai.mutex); return; }
    if (state->ai.stream_buffer) { free(state->ai.stream_buffer); state->ai.stream_buffer = NULL; state->ai.stream_buffer_len = 0; }
    
    int total_len = 0;
    for (int i=0; i<state->ai.input.line_count; i++) total_len += strlen(state->ai.input.lines[i]) + 1;
    char *prompt = malloc(total_len + 1); prompt[0] = '\0';
    for (int i=0; i<state->ai.input.line_count; i++) {
        strcat(prompt, state->ai.input.lines[i]);
        if (i < state->ai.input.line_count - 1) strcat(prompt, "\n");
    }
    
    if (strlen(prompt) == 0) { free(prompt); pthread_mutex_unlock(&state->ai.mutex); return; }

    for (int i=0; i<state->ai.input.line_count; i++) free(state->ai.input.lines[i]);
    free(state->ai.input.lines);
    state->ai.input.lines = malloc(sizeof(char*)); state->ai.input.lines[0] = strdup("");
    state->ai.input.line_count = 1; state->ai.input.cursor_x = 0; state->ai.input.cursor_y = 0;

    state->ai.messages = realloc(state->ai.messages, sizeof(AIMessage) * (state->ai.message_count + 2));
    state->ai.messages[state->ai.message_count++] = (AIMessage){strdup("user"), strdup(prompt), NULL};
    state->ai.messages[state->ai.message_count++] = (AIMessage){strdup("assistant"), strdup(""), NULL};
    
    AIThreadData *td = malloc(sizeof(AIThreadData));
    td->state = state; td->prompt = prompt;
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
