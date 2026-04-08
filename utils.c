#define _GNU_SOURCE
#include "cursory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include <time.h>
#include <stdarg.h>
#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>

void log_debug(const char *fmt, ...) {
    FILE *f = fopen("/tmp/cursory.log", "a");
    if (!f) return;
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char ts[64]; strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", t);
    fprintf(f, "[%s] ", ts);
    va_list args; va_start(args, fmt); vfprintf(f, fmt, args); va_end(args);
    fprintf(f, "\n"); fclose(f);
}

void log_chat(const char *role, const char *content) {
    FILE *f = fopen("/tmp/cursory_chat.log", "a");
    if (!f) return;
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char ts[64]; strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", t);
    fprintf(f, "[%s] %s: %s\n\n", ts, role, content);
    fclose(f);
}


void segv_handler(int sig) {
    log_debug("!!! CRITICAL: SIGSEGV (%d) !!!", sig);
    endwin();
    fprintf(stderr, "Fatal error: Segmentation Fault. See /tmp/cursory.log\n");
    exit(1);
}



char* generate_line_diff(const char *old_ctx, const char *new_ctx) {
    char *res = strdup(""); int rlen = 0;
    char *oc = strdup(old_ctx ? old_ctx : ""), *nc = strdup(new_ctx ? new_ctx : "");
    char *saveptr_ol = NULL, *saveptr_nl = NULL;
    char *ol = strtok_r(oc, "\n", &saveptr_ol), *nl = strtok_r(nc, "\n", &saveptr_nl);
    while (ol || nl) {
        if (ol && nl && strcmp(ol, nl) == 0) {
            int l = strlen(ol) + 3; res = realloc(res, rlen + l + 1); strcat(res, "  "); strcat(res, ol); strcat(res, "\n"); rlen += l;
        } else {
            if (ol) { int l = strlen(ol) + 3; res = realloc(res, rlen + l + 1); strcat(res, "- "); strcat(res, ol); strcat(res, "\n"); rlen += l; }
            if (nl) { int l = strlen(nl) + 3; res = realloc(res, rlen + l + 1); strcat(res, "+ "); strcat(res, nl); strcat(res, "\n"); rlen += l; }
        }
        ol = strtok_r(NULL, "\n", &saveptr_ol); nl = strtok_r(NULL, "\n", &saveptr_nl);
    }
    free(oc); free(nc); return res;
}



char* escape_json_string(const char *in) {
    int l = strlen(in); char *out = malloc(l*2+1), *p = out;
    for (int i=0; i<l; i++) { 
        if (in[i]=='\"') { *p++='\\'; *p++='\"'; } 
        else if (in[i]=='\n') { *p++='\\'; *p++='n'; } 
        else if (in[i]=='\\') { *p++='\\'; *p++='\\'; } 
        else *p++=in[i]; 
    } 
    *p = '\0'; return out;
}

const char* find_json_block_end(const char *start) {
    if (!start) return NULL;
    char open = *start; 
    char close = (open == '{') ? '}' : (open == '[' ? ']' : '\0');
    if (!close) return NULL;
    
    int depth = 0; bool in_quote = false; char quote_char = '\0'; bool esc = false;
    const char *p = start;
    while (*p) {
        if (!in_quote) {
            if (*p == '\'' || *p == '\"') { in_quote = true; quote_char = *p; esc = false; }
            else if (*p == open) depth++;
            else if (*p == close) { depth--; if (depth == 0) return p; }
        } else {
            if (!esc && *p == '\\') esc = true;
            else if (!esc && *p == quote_char) in_quote = false;
            else esc = false;
        }
        p++;
    }
    return NULL;
}

char* parse_json_value(const char *json, const char *key) {
    if (!json || !key) return NULL;
    char key_match[256]; snprintf(key_match, sizeof(key_match), "\"%s\"", key);
    const char *last = NULL; const char *cur = json;
    while ((cur = strcasestr(cur, key_match))) { last = cur; cur++; }
    if (!last) return NULL;

    const char *p = last + strlen(key_match);
    while (*p && isspace(*p)) p++;
    if (*p == ':') p++;
    else if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBC && (unsigned char)p[2] == 0x9A) p += 3;
    else return NULL;
    while (*p && isspace(*p)) p++;
    const char *st = p;
    while (*st && *st != '\"' && *st != '\'' && *st != '{' && *st != '[') st++;
    if (!*st) return NULL;

    if (*st == '{' || *st == '[') {
        const char *en = find_json_block_end(st);
        if (en) {
            int len = en + 1 - st; char *val = malloc(len + 1); strncpy(val, st, len); val[len] = '\0'; return val;
        }
        return NULL;
    }

    // String value
    char quote_char = *st;
    st++; const char *en = st;
    bool esc = false;
    while (*en) {
        if (!esc && *en == '\\') esc = true;
        else if (!esc && *en == quote_char) break;
        else esc = false;
        en++;
    }
    if (!*en) return NULL;

    bool has_real_newlines = false;
    for (const char *tmp = st; tmp < en; tmp++) {
        if (*tmp == '\n') { has_real_newlines = true; break; }
    }

    int len = en - st; char *val = malloc(len * 2 + 1); // alloc enough space for \n expansions
    char *d = val; const char *s = st;
    while (s < en) {
        if (*s == '\\') {
            s++;
            if (*s == 'n') {
                if (has_real_newlines) { *d++ = '\\'; *d++ = 'n'; }
                else { *d++ = '\n'; }
                s++;
            }
            else if (*s == 'r') { *d++ = '\r'; s++; }
            else if (*s == 't') { *d++ = '\t'; s++; }
            else if (*s == '\"') { *d++ = '\"'; s++; }
            else if (*s == '\'') { *d++ = '\''; s++; }
            else if (*s == '\\') { *d++ = '\\'; s++; }
            else if (*s == 'u') {
                s++; if (s+4 > en) break;
                char h[5] = {s[0], s[1], s[2], s[3], '\0'};
                unsigned int code = (unsigned int)strtol(h, NULL, 16);
                if (code <= 127) *d++ = (char)code; else *d++ = '?';
                s += 4;
            }
            else *d++ = *s++;
        } else *d++ = *s++;
    }
    *d = '\0'; return val;
}



int parse_patch_ops(const char *json, PatchOp **ops) {
    if (!json) return 0;
    int count = 0; PatchOp *list = NULL;
    
    const char *p = json;
    const char *patch_start = strcasestr(p, "\"patch\"");
    const char *arr_start = NULL;

    if (patch_start) {
        arr_start = strchr(patch_start, '[');
    } else {
        // Fallback: ONLY if the string starts with '[' (ignoring whitespace)
        const char *tmp = json;
        while (*tmp == ' ' || *tmp == '\n' || *tmp == '\r' || *tmp == '\t') tmp++;
        if (*tmp == '[') arr_start = tmp;
    }

    if (arr_start) {
        const char *end_arr = find_json_block_end(arr_start); 
        if (end_arr) {
            const char *curr = arr_start + 1;
            while (curr < end_arr) {
                curr = strchr(curr, '{');
                if (!curr || curr >= end_arr) break;
                const char *obj_end = find_json_block_end(curr); 
                if (!obj_end || obj_end > end_arr) break;
                
                int len = obj_end - curr + 1; char *obj = malloc(len + 1); strncpy(obj, curr, len); obj[len] = '\0';
                
                char *type = parse_json_value(obj, "type");
                char *content = parse_json_value(obj, "content");
                if (!content) content = parse_json_value(obj, "code");
                if (!content) content = parse_json_value(obj, "text");
                if (!content) content = parse_json_value(obj, "");

                int line = 0;
                char *lpos = strcasestr(obj, "\"line\"");
                if (lpos) {
                    lpos += 6;
                    while (*lpos && !isdigit(*lpos)) lpos++;
                    if (isdigit(*lpos)) line = atoi(lpos);
                }

                if (type) {
                    list = realloc(list, sizeof(PatchOp) * (count + 1));
                    strncpy(list[count].type, type, 15); list[count].type[15] = '\0';
                    list[count].line = line;
                    if (content) {
                        int clen = strlen(content);
                        while (clen > 0 && (content[clen-1] == '\n' || content[clen-1] == '\r')) {
                            content[clen-1] = '\0';
                            clen--;
                        }
                        list[count].content = strdup(content);
                    } else {
                        list[count].content = NULL;
                    }
                    count++;
                }
                free(obj); free(type); free(content);
                curr = obj_end + 1;
            }
        }
    }
    *ops = list; return count;
}

char** apply_patch_to_buffer(char **lines, int *count, PatchOp *ops, int op_count) {
    if (op_count > 1) {
        for (int i=1; i<op_count; i++) {
            PatchOp key = ops[i]; int j = i - 1;
            while (j >= 0 && ops[j].line <= key.line) { ops[j+1] = ops[j]; j--; }
            ops[j+1] = key;
        }
    }
    int original_count = *count;
    int cur_count = *count; char **cur_lines = malloc(sizeof(char*) * cur_count);
    for (int i=0; i<cur_count; i++) cur_lines[i] = strdup(lines[i]);
    for (int i=0; i<op_count; i++) {
        PatchOp *op = &ops[i]; int idx = op->line - 1;
        if (idx < 0) idx = 0;
        
        if (strcmp(op->type, "insert") == 0 && idx >= original_count) {
            strcpy(op->type, "replace");
        }

        char *lines_arr[1000]; int num_lines = 0;
        char *p = op->content ? op->content : "";

        while (*p && num_lines < 1000) {
            const char *nl = strchr(p, '\n');
            if (!nl) { lines_arr[num_lines++] = strdup(p); break; }
            int slen = nl - p; char *sl = malloc(slen + 1);
            strncpy(sl, p, slen); sl[slen] = '\0';
            lines_arr[num_lines++] = sl; p = nl + 1;
        }
        if (num_lines == 0) lines_arr[num_lines++] = strdup("");

        if (strcmp(op->type, "replace") == 0) {
            if (idx >= 0) {
                while (idx >= cur_count) {
                    cur_lines = realloc(cur_lines, sizeof(char*) * (cur_count + 1));
                    cur_lines[cur_count++] = strdup("");
                }
                free(cur_lines[idx]); cur_lines[idx] = lines_arr[0];
                for (int m = 1; m < num_lines; m++) {
                    cur_lines = realloc(cur_lines, sizeof(char*) * (cur_count + 1));
                    memmove(cur_lines + idx + m + 1, cur_lines + idx + m, sizeof(char*) * (cur_count - (idx + m)));
                    cur_lines[idx + m] = lines_arr[m]; cur_count++;
                }
            } else {
                for (int m=0; m<num_lines; m++) free(lines_arr[m]);
            }
        } else if (strcmp(op->type, "insert") == 0) {
            if (idx >= 0) {
                while (idx > cur_count) {
                    cur_lines = realloc(cur_lines, sizeof(char*) * (cur_count + 1));
                    cur_lines[cur_count++] = strdup("");
                }
                for (int m = 0; m < num_lines; m++) {
                    cur_lines = realloc(cur_lines, sizeof(char*) * (cur_count + 1));
                    memmove(cur_lines + idx + m + 1, cur_lines + idx + m, sizeof(char*) * (cur_count - (idx + m)));
                    cur_lines[idx + m] = lines_arr[m]; cur_count++;
                }
            } else {
                for (int m=0; m<num_lines; m++) free(lines_arr[m]);
            }
        } else if (strcmp(op->type, "delete") == 0) {
            if (idx >= 0 && idx < cur_count) {
                free(cur_lines[idx]); memmove(cur_lines + idx, cur_lines + idx + 1, sizeof(char*) * (cur_count - idx - 1));
                cur_count--;
            }
        }
    }
    *count = cur_count; return cur_lines;
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
