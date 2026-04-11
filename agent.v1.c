/*
 * mini-agent-c: Minimal Claude Code-style agent in pure C.
 * Agent loop: prompt -> API -> tool_use -> execute -> loop.
 * Tools: read_file, write_file, bash
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/wait.h>
#include <curl/curl.h>
#include "cJSON.h"

#define MODEL_DEFAULT "claude-sonnet-4-5"
#define MAX_TURNS 40
#define MAX_TOOL_OUT (200 * 1024)

typedef struct { char *data; size_t size; } Buf;

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    Buf *b = userdata;
    size_t n = size * nmemb;
    char *p = realloc(b->data, b->size + n + 1);
    if (!p) return 0;
    b->data = p;
    memcpy(b->data + b->size, ptr, n);
    b->size += n;
    b->data[b->size] = 0;
    return n;
}

static char *claude_api(const char *api_key, const char *body) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;
    Buf buf = {0};
    struct curl_slist *h = NULL;
    char auth[1024];
    snprintf(auth, sizeof(auth), "x-api-key: %s", api_key);
    h = curl_slist_append(h, auth);
    h = curl_slist_append(h, "anthropic-version: 2023-06-01");
    h = curl_slist_append(h, "content-type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, "https://api.anthropic.com/v1/messages");
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);
    CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(h);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) { free(buf.data); return NULL; }
    return buf.data;
}

/* ---- tools ---- */

static char *tool_read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        char *r = malloc(512);
        snprintf(r, 512, "Error: cannot open %s (%s)", path, strerror(errno));
        return r;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) sz = 0;
    char *buf = malloc(sz + 1);
    size_t got = fread(buf, 1, sz, f);
    buf[got] = 0;
    fclose(f);
    return buf;
}

static char *tool_write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        char *r = malloc(512);
        snprintf(r, 512, "Error: cannot write %s (%s)", path, strerror(errno));
        return r;
    }
    size_t n = strlen(content);
    fwrite(content, 1, n, f);
    fclose(f);
    char *r = malloc(128);
    snprintf(r, 128, "Wrote %zu bytes to %s", n, path);
    return r;
}

static char *tool_bash(const char *cmd) {
    char *full = malloc(strlen(cmd) + 16);
    sprintf(full, "%s 2>&1", cmd);
    FILE *p = popen(full, "r");
    free(full);
    if (!p) return strdup("Error: popen failed");
    size_t cap = 8192, len = 0;
    char *out = malloc(cap);
    size_t n;
    while ((n = fread(out + len, 1, cap - len - 1, p)) > 0) {
        len += n;
        if (len + 1 >= cap) {
            cap *= 2;
            if (cap > MAX_TOOL_OUT) cap = MAX_TOOL_OUT;
            out = realloc(out, cap);
            if (len + 1 >= cap) break;
        }
    }
    out[len] = 0;
    int rc = pclose(p);
    char tail[64];
    snprintf(tail, sizeof(tail), "\n[exit=%d]", WEXITSTATUS(rc));
    size_t tlen = strlen(tail);
    if (len + tlen + 1 < cap) { memcpy(out + len, tail, tlen + 1); }
    return out;
}

/* ---- tools JSON schema ---- */

static cJSON *make_tool(const char *name, const char *desc, const char **params, int n) {
    cJSON *t = cJSON_CreateObject();
    cJSON_AddStringToObject(t, "name", name);
    cJSON_AddStringToObject(t, "description", desc);
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "type", "object");
    cJSON *props = cJSON_CreateObject();
    cJSON *req = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "type", "string");
        cJSON_AddItemToObject(props, params[i], p);
        cJSON_AddItemToArray(req, cJSON_CreateString(params[i]));
    }
    cJSON_AddItemToObject(s, "properties", props);
    cJSON_AddItemToObject(s, "required", req);
    cJSON_AddItemToObject(t, "input_schema", s);
    return t;
}

static cJSON *make_tools(void) {
    cJSON *tools = cJSON_CreateArray();
    const char *p1[] = {"path"};
    cJSON_AddItemToArray(tools, make_tool("read_file", "Read contents of a file on disk.", p1, 1));
    const char *p2[] = {"path", "content"};
    cJSON_AddItemToArray(tools, make_tool("write_file", "Write content to a file (overwrites).", p2, 2));
    const char *p3[] = {"command"};
    cJSON_AddItemToArray(tools, make_tool("bash", "Run a bash command, return stdout+stderr+exit.", p3, 1));
    return tools;
}

static char *execute_tool(const char *name, cJSON *input) {
    cJSON *path = cJSON_GetObjectItem(input, "path");
    cJSON *content = cJSON_GetObjectItem(input, "content");
    cJSON *command = cJSON_GetObjectItem(input, "command");
    if (strcmp(name, "read_file") == 0 && path)
        return tool_read_file(cJSON_GetStringValue(path));
    if (strcmp(name, "write_file") == 0 && path && content)
        return tool_write_file(cJSON_GetStringValue(path), cJSON_GetStringValue(content));
    if (strcmp(name, "bash") == 0 && command)
        return tool_bash(cJSON_GetStringValue(command));
    char *r = malloc(256);
    snprintf(r, 256, "Error: unknown or invalid tool call: %s", name);
    return r;
}

/* ---- main loop ---- */

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s \"<prompt>\" [model]\n", argv[0]);
        return 1;
    }
    const char *api_key = getenv("ANTHROPIC_API_KEY");
    if (!api_key || !*api_key) {
        fprintf(stderr, "ANTHROPIC_API_KEY env var not set\n");
        return 1;
    }
    const char *model = (argc >= 3) ? argv[2] : MODEL_DEFAULT;

    curl_global_init(CURL_GLOBAL_DEFAULT);

    cJSON *messages = cJSON_CreateArray();
    cJSON *um = cJSON_CreateObject();
    cJSON_AddStringToObject(um, "role", "user");
    cJSON_AddStringToObject(um, "content", argv[1]);
    cJSON_AddItemToArray(messages, um);

    cJSON *tools = make_tools();
    const char *sys_prompt =
        "You are mini-agent-c, a minimal autonomous agent written in C. "
        "You have read_file, write_file, and bash tools. "
        "Proceed step by step. Use bash to test your work. "
        "When the task is complete, respond with a short text summary and STOP calling tools.";

    int turn;
    for (turn = 0; turn < MAX_TURNS; turn++) {
        cJSON *body = cJSON_CreateObject();
        cJSON_AddStringToObject(body, "model", model);
        cJSON_AddNumberToObject(body, "max_tokens", 8192);
        cJSON_AddStringToObject(body, "system", sys_prompt);
        cJSON_AddItemToObject(body, "tools", cJSON_Duplicate(tools, 1));
        cJSON_AddItemToObject(body, "messages", cJSON_Duplicate(messages, 1));

        char *body_str = cJSON_PrintUnformatted(body);
        cJSON_Delete(body);

        fprintf(stderr, "\n=== turn %d ===\n", turn);
        char *resp = claude_api(api_key, body_str);
        free(body_str);
        if (!resp) { fprintf(stderr, "API call failed\n"); break; }

        cJSON *rj = cJSON_Parse(resp);
        if (!rj) {
            fprintf(stderr, "JSON parse failed: %.500s\n", resp);
            free(resp);
            break;
        }
        free(resp);

        cJSON *err = cJSON_GetObjectItem(rj, "error");
        if (err) {
            char *e = cJSON_PrintUnformatted(err);
            fprintf(stderr, "API error: %s\n", e);
            free(e);
            cJSON_Delete(rj);
            break;
        }

        cJSON *content = cJSON_GetObjectItem(rj, "content");
        if (!content) {
            fprintf(stderr, "no content in response\n");
            cJSON_Delete(rj);
            break;
        }

        /* append assistant message */
        cJSON *am = cJSON_CreateObject();
        cJSON_AddStringToObject(am, "role", "assistant");
        cJSON_AddItemToObject(am, "content", cJSON_Duplicate(content, 1));
        cJSON_AddItemToArray(messages, am);

        cJSON *tool_results = cJSON_CreateArray();
        int has_tool = 0;
        cJSON *block;
        cJSON_ArrayForEach(block, content) {
            const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(block, "type"));
            if (!type) continue;
            if (strcmp(type, "text") == 0) {
                const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(block, "text"));
                if (t) printf("\n[assistant] %s\n", t);
            } else if (strcmp(type, "tool_use") == 0) {
                has_tool = 1;
                const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(block, "name"));
                const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(block, "id"));
                cJSON *input = cJSON_GetObjectItem(block, "input");
                char *inp_str = cJSON_PrintUnformatted(input);
                fprintf(stderr, "[tool] %s %.200s\n", name, inp_str ? inp_str : "");
                free(inp_str);
                char *out = execute_tool(name, input);
                if (strlen(out) > MAX_TOOL_OUT) out[MAX_TOOL_OUT] = 0;
                cJSON *tr = cJSON_CreateObject();
                cJSON_AddStringToObject(tr, "type", "tool_result");
                cJSON_AddStringToObject(tr, "tool_use_id", id);
                cJSON_AddStringToObject(tr, "content", out);
                cJSON_AddItemToArray(tool_results, tr);
                free(out);
            }
        }

        if (!has_tool) {
            cJSON_Delete(tool_results);
            const char *stop = cJSON_GetStringValue(cJSON_GetObjectItem(rj, "stop_reason"));
            fprintf(stderr, "\n[done] stop_reason=%s turns=%d\n", stop ? stop : "?", turn + 1);
            cJSON_Delete(rj);
            break;
        }

        cJSON *trm = cJSON_CreateObject();
        cJSON_AddStringToObject(trm, "role", "user");
        cJSON_AddItemToObject(trm, "content", tool_results);
        cJSON_AddItemToArray(messages, trm);

        cJSON_Delete(rj);
    }

    if (turn >= MAX_TURNS) fprintf(stderr, "\n[halt] max turns reached\n");

    cJSON_Delete(messages);
    cJSON_Delete(tools);
    curl_global_cleanup();
    return 0;
}
