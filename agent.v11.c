/*
 * mini-agent-c v10: v9 + http_request, checkpoint/undo, process management,
 *                       clipboard, preview_edit, run_tests.
 *
 * New in v10:
 *  - http_request(method, url, body, headers)  — general HTTP tool (GET/POST/PUT/DELETE/PATCH)
 *  - checkpoint()                              — snapshot modified files to ~/.mini-agent/checkpoints/
 *  - undo()                                    — restore from latest checkpoint
 *  - process_start(name, command)              — background process management
 *  - process_status(name)                      — check process status + log tail
 *  - process_stop(name)                        — terminate background process
 *  - clipboard_get()                           — read macOS clipboard (pbpaste)
 *  - clipboard_set(text)                       — write macOS clipboard (pbcopy)
 *  - preview_edit(path, old, new)              — preview diff without modifying file
 *  - run_tests(path)                           — auto-detect & run test framework
 *
 * Inherited from v9:
 *  - Parallel tool execution, ask_user, git, diff_files, notify
 *  - --stream-bash, --think, auto-mkdir, --no-parallel
 *
 * Inherited from v8:
 *  - grep_files, glob_files, http_get (kept as alias), todo, SIGINT, cost tracking, REPL
 *
 * Inherited from v7:
 *  - Anthropic Messages API + tool_use loop
 *  - Prompt caching (system + tools with cache_control)
 *  - Persistent memory (~/.mini-agent/memory.md)
 *  - Dynamic tool loading (.agent/tools/x.sh)
 *  - Subagent recursion (spawn_agent, depth-limited)
 *  - Context compaction via Haiku summarizer
 *  - Plan mode (--plan)
 *  - Sandbox mode (--sandbox: sandbox-exec)
 *  - SSE streaming (--stream)
 *  - OpenAI-compatible backend (--backend openai)
 *
 * Safety:
 *  - Token & cost budget caps (hard exit)
 *  - Max turns cap
 *  - Path confinement (no '..', no '~', CWD-only)
 *  - Dangerous command denylist for bash
 *  - Spawn depth cap (MINI_AGENT_DEPTH env)
 *  - .agent/STOP kill switch polled each turn
 *  - .agent/audit.log JSONL of every tool call
 *  - API key redaction on all output
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <curl/curl.h>
#include "cJSON.h"

/* ---------- constants ---------- */
#define MODEL_DEFAULT          "claude-sonnet-4-5"
#define MODEL_COMPACT          "claude-haiku-4-5"
#define MAX_TURNS_DEFAULT      30
#define MAX_TOOL_OUT_BYTES     (128 * 1024)
#define COMPACT_AFTER_MESSAGES 24
#define COMPACT_KEEP_LAST      6
#define MAX_DYN_TOOLS          32
#define MAX_SPAWN_DEPTH        2
#define MAX_RETRIES            3
#define RETRY_SLEEP_SEC        2
#define TOKEN_BUDGET_DEFAULT   300000

/* Cost per million tokens (Sonnet 4.5 pricing) */
#define COST_INPUT_PER_M       3.00
#define COST_OUTPUT_PER_M      15.00
#define COST_CACHE_WRITE_PER_M 3.75
#define COST_CACHE_READ_PER_M  0.30

/* ---------- globals ---------- */
#define BACKEND_ANTHROPIC 0
#define BACKEND_OPENAI    1
static int   g_backend         = BACKEND_ANTHROPIC;
static char  g_api_base[512]   = {0};
static int   g_plan_mode       = 0;
static int   g_use_sandbox     = 0;
static int   g_stream_mode     = 0;
static int   g_quiet           = 0;
static int   g_interactive     = 0;
static int   g_allow_http      = 0;
static int   g_no_parallel     = 0;
static int   g_stream_bash     = 0;
static int   g_approve         = 0;   /* --approve: confirm every tool call */
static int   g_approve_bash    = 0;   /* --approve-bash: confirm only bash */
static int   g_think_mode      = 0;
static long  g_think_budget    = 8000;
static int   g_max_turns       = MAX_TURNS_DEFAULT;
static long  g_token_budget    = TOKEN_BUDGET_DEFAULT;
static long  g_total_in_tokens       = 0;
static long  g_total_out_tokens      = 0;
static long  g_total_cache_write     = 0;
static long  g_total_cache_read      = 0;
static int   g_spawn_depth     = 0;
static char  g_self_path[1024] = {0};
static char  g_cwd[1024]       = {0};
static char  g_memory_path[1024] = {0};
static char  g_checkpoint_dir[1024] = {0};
static volatile int g_interrupted = 0;
static time_t       g_session_start = 0;   /* for electricity cost tracking */

typedef struct {
    char name[64];
    char description[512];
    char script_path[1024];
    cJSON *args;
} DynTool;

static DynTool g_dyn_tools[MAX_DYN_TOOLS];
static int g_n_dyn_tools = 0;

/* ---------- buffer ---------- */
typedef struct { char *data; size_t size, cap; } Buf;

static void buf_grow(Buf *b, size_t need) {
    if (b->cap >= need) return;
    size_t nc = b->cap ? b->cap : 1024;
    while (nc < need) nc *= 2;
    char *p = realloc(b->data, nc);
    if (!p) { perror("realloc"); exit(1); }
    b->data = p;
    b->cap = nc;
}
static void buf_append(Buf *b, const void *s, size_t n) {
    buf_grow(b, b->size + n + 1);
    memcpy(b->data + b->size, s, n);
    b->size += n;
    b->data[b->size] = 0;
}
static void buf_free(Buf *b) { free(b->data); b->data = NULL; b->size = b->cap = 0; }

/* ---------- signal handler ---------- */
static void sigint_handler(int sig) { (void)sig; g_interrupted = 1; }

/* ---------- safety helpers ---------- */
static void redact_secrets(char *s) {
    if (!s) return;
    const char *needle = "sk-ant-";
    char *p = s;
    while ((p = strstr(p, needle)) != NULL) {
        char *end = p + strlen(needle);
        while (*end && !isspace((unsigned char)*end) && *end != '"' && *end != '\'' && *end != ',') end++;
        if (end - p > (ssize_t)strlen(needle) + 4) {
            for (char *q = p + strlen(needle) + 2; q < end; q++) *q = '*';
        }
        p = end;
    }
}
static int should_stop_now(void) {
    struct stat st;
    return stat(".agent/STOP", &st) == 0;
}
static int path_is_safe(const char *p) {
    if (!p || !*p) return 0;
    if (strstr(p, "..") != NULL) return 0;
    if (p[0] == '~') return 0;
    if (p[0] == '/') {
        if (!*g_cwd) return 0;
        size_t cl = strlen(g_cwd);
        if (strncmp(p, g_cwd, cl) != 0) return 0;
        if (p[cl] != '\0' && p[cl] != '/') return 0;
    }
    return 1;
}
static const char *DANGEROUS_PATTERNS[] = {
    "rm -rf /", "rm -rf /*", "rm -rf ~", "rm -rf $HOME", "rm -rf /Users",
    ":(){", "sudo ", "doas ",
    "dd if=", "mkfs", "fdisk",
    "> /dev/sd", "> /dev/disk", "> /dev/rdisk",
    "chown -R / ", "chmod -R 777 /",
    "/etc/shadow", "/etc/sudoers", "launchctl unload",
    "pkill -9 ", "killall -9 ",
    "shutdown ", "halt ", "reboot ",
    "diskutil erase", "diskutil unmount",
    "git push --force origin main", "git push -f origin main",
    "rm .git/", "rm -rf .git",
    "curl -s ",
    NULL
};
static const char *check_dangerous(const char *cmd) {
    if (!cmd) return NULL;
    for (int i = 0; DANGEROUS_PATTERNS[i]; i++)
        if (strstr(cmd, DANGEROUS_PATTERNS[i])) return DANGEROUS_PATTERNS[i];
    return NULL;
}
static void ensure_agent_dir(void) {
    mkdir(".agent", 0755);
    mkdir(".agent/tools", 0755);
}
static void audit(const char *tool, const char *inp_json, const char *result_preview) {
    ensure_agent_dir();
    FILE *f = fopen(".agent/audit.log", "a");
    if (!f) return;
    time_t t = time(NULL);
    struct tm tmv; localtime_r(&t, &tmv);
    char ts[32]; strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", &tmv);
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "time", ts);
    cJSON_AddNumberToObject(e, "pid", getpid());
    cJSON_AddNumberToObject(e, "depth", g_spawn_depth);
    cJSON_AddStringToObject(e, "tool", tool ? tool : "");
    char inp_buf[2048];
    snprintf(inp_buf, sizeof(inp_buf), "%.2000s", inp_json ? inp_json : "");
    redact_secrets(inp_buf);
    cJSON_AddStringToObject(e, "input", inp_buf);
    char prev[512];
    snprintf(prev, sizeof(prev), "%.500s", result_preview ? result_preview : "");
    redact_secrets(prev);
    cJSON_AddStringToObject(e, "result_preview", prev);
    char *s = cJSON_PrintUnformatted(e);
    fprintf(f, "%s\n", s);
    free(s); cJSON_Delete(e); fclose(f);
}
static void logerr(const char *fmt, ...) {
    if (g_quiet) return;
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
}

/* ---------- cost estimate ---------- */
static void print_cost(void) {
    double cost = (g_total_in_tokens    / 1e6 * COST_INPUT_PER_M)
                + (g_total_out_tokens   / 1e6 * COST_OUTPUT_PER_M)
                + (g_total_cache_write  / 1e6 * COST_CACHE_WRITE_PER_M)
                + (g_total_cache_read   / 1e6 * COST_CACHE_READ_PER_M);
    logerr("[cost] ~$%.4f  in=%ld out=%ld cache_write=%ld cache_read=%ld\n",
           cost, g_total_in_tokens, g_total_out_tokens,
           g_total_cache_write, g_total_cache_read);
}

/* ---------- HTTP / API ---------- */
static size_t curl_write_cb(void *p, size_t sz, size_t nm, void *ud) {
    Buf *b = (Buf *)ud;
    size_t n = sz * nm;
    buf_append(b, p, n);
    return n;
}

/* ==== OpenAI-compatible bridge ==== */
static char *anthropic_body_to_openai(const char *anthro_body) {
    cJSON *a = cJSON_Parse(anthro_body);
    if (!a) return NULL;
    cJSON *o = cJSON_CreateObject();
    cJSON *m = cJSON_GetObjectItem(a, "model");
    if (m) cJSON_AddItemToObject(o, "model", cJSON_Duplicate(m, 1));
    cJSON *mt = cJSON_GetObjectItem(a, "max_tokens");
    if (mt) cJSON_AddItemToObject(o, "max_tokens", cJSON_Duplicate(mt, 1));
    cJSON *omsgs = cJSON_CreateArray();
    cJSON *sys = cJSON_GetObjectItem(a, "system");
    if (sys) {
        Buf sb = {0};
        if (cJSON_IsString(sys)) {
            const char *s = cJSON_GetStringValue(sys);
            if (s) buf_append(&sb, s, strlen(s));
        } else if (cJSON_IsArray(sys)) {
            cJSON *blk;
            cJSON_ArrayForEach(blk, sys) {
                const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(blk, "text"));
                if (t) { if (sb.size > 0) buf_append(&sb, "\n\n", 2); buf_append(&sb, t, strlen(t)); }
            }
        }
        if (sb.size > 0) {
            cJSON *sm = cJSON_CreateObject();
            cJSON_AddStringToObject(sm, "role", "system");
            cJSON_AddStringToObject(sm, "content", sb.data);
            cJSON_AddItemToArray(omsgs, sm);
        }
        free(sb.data);
    }
    cJSON *amsgs = cJSON_GetObjectItem(a, "messages");
    cJSON *msg;
    cJSON_ArrayForEach(msg, amsgs) {
        const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "role"));
        cJSON *content = cJSON_GetObjectItem(msg, "content");
        if (!role || !content) continue;
        if (cJSON_IsString(content)) {
            cJSON *om = cJSON_CreateObject();
            cJSON_AddStringToObject(om, "role", role);
            cJSON_AddStringToObject(om, "content", cJSON_GetStringValue(content));
            cJSON_AddItemToArray(omsgs, om);
            continue;
        }
        if (!cJSON_IsArray(content)) continue;
        if (strcmp(role, "assistant") == 0) {
            Buf text_buf = {0};
            cJSON *tool_calls = cJSON_CreateArray();
            cJSON *blk;
            cJSON_ArrayForEach(blk, content) {
                const char *bt = cJSON_GetStringValue(cJSON_GetObjectItem(blk, "type"));
                if (!bt) continue;
                if (strcmp(bt, "text") == 0) {
                    const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(blk, "text"));
                    if (t) buf_append(&text_buf, t, strlen(t));
                } else if (strcmp(bt, "tool_use") == 0) {
                    const char *id   = cJSON_GetStringValue(cJSON_GetObjectItem(blk, "id"));
                    const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(blk, "name"));
                    cJSON *input = cJSON_GetObjectItem(blk, "input");
                    char *args_str = input ? cJSON_PrintUnformatted(input) : strdup("{}");
                    cJSON *tc = cJSON_CreateObject();
                    cJSON_AddStringToObject(tc, "id", id ? id : "");
                    cJSON_AddStringToObject(tc, "type", "function");
                    cJSON *fn = cJSON_CreateObject();
                    cJSON_AddStringToObject(fn, "name", name ? name : "");
                    cJSON_AddStringToObject(fn, "arguments", args_str ? args_str : "{}");
                    cJSON_AddItemToObject(tc, "function", fn);
                    cJSON_AddItemToArray(tool_calls, tc);
                    free(args_str);
                }
            }
            cJSON *om = cJSON_CreateObject();
            cJSON_AddStringToObject(om, "role", "assistant");
            cJSON_AddStringToObject(om, "content", text_buf.data ? text_buf.data : "");
            if (cJSON_GetArraySize(tool_calls) > 0)
                cJSON_AddItemToObject(om, "tool_calls", tool_calls);
            else
                cJSON_Delete(tool_calls);
            cJSON_AddItemToArray(omsgs, om);
            free(text_buf.data);
        } else {
            int emitted_tool = 0;
            cJSON *blk;
            cJSON_ArrayForEach(blk, content) {
                const char *bt = cJSON_GetStringValue(cJSON_GetObjectItem(blk, "type"));
                if (bt && strcmp(bt, "tool_result") == 0) {
                    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(blk, "tool_use_id"));
                    cJSON *ci = cJSON_GetObjectItem(blk, "content");
                    const char *ct = ci && cJSON_IsString(ci) ? cJSON_GetStringValue(ci) : "";
                    cJSON *om = cJSON_CreateObject();
                    cJSON_AddStringToObject(om, "role", "tool");
                    cJSON_AddStringToObject(om, "tool_call_id", id ? id : "");
                    cJSON_AddStringToObject(om, "content", ct ? ct : "");
                    cJSON_AddItemToArray(omsgs, om);
                    emitted_tool = 1;
                } else if (bt && strcmp(bt, "text") == 0 && !emitted_tool) {
                    const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(blk, "text"));
                    if (t) {
                        cJSON *om = cJSON_CreateObject();
                        cJSON_AddStringToObject(om, "role", "user");
                        cJSON_AddStringToObject(om, "content", t);
                        cJSON_AddItemToArray(omsgs, om);
                    }
                }
            }
        }
    }
    cJSON_AddItemToObject(o, "messages", omsgs);
    cJSON *atools = cJSON_GetObjectItem(a, "tools");
    if (atools) {
        cJSON *otools = cJSON_CreateArray();
        cJSON *tool;
        cJSON_ArrayForEach(tool, atools) {
            cJSON *ot = cJSON_CreateObject();
            cJSON_AddStringToObject(ot, "type", "function");
            cJSON *fn = cJSON_CreateObject();
            cJSON *n = cJSON_GetObjectItem(tool, "name");
            cJSON *d = cJSON_GetObjectItem(tool, "description");
            cJSON *s = cJSON_GetObjectItem(tool, "input_schema");
            if (n) cJSON_AddItemToObject(fn, "name", cJSON_Duplicate(n, 1));
            if (d) cJSON_AddItemToObject(fn, "description", cJSON_Duplicate(d, 1));
            if (s) cJSON_AddItemToObject(fn, "parameters", cJSON_Duplicate(s, 1));
            cJSON_AddItemToObject(ot, "function", fn);
            cJSON_AddItemToArray(otools, ot);
        }
        cJSON_AddItemToObject(o, "tools", otools);
    }
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(a); cJSON_Delete(o);
    return s;
}

static char *openai_response_to_anthropic(const char *oai_resp) {
    cJSON *o = cJSON_Parse(oai_resp);
    if (!o) return NULL;
    cJSON *err = cJSON_GetObjectItem(o, "error");
    if (err) {
        cJSON *a = cJSON_CreateObject();
        cJSON_AddItemToObject(a, "error", cJSON_Duplicate(err, 1));
        char *s = cJSON_PrintUnformatted(a);
        cJSON_Delete(o); cJSON_Delete(a);
        return s;
    }
    cJSON *a = cJSON_CreateObject();
    cJSON *content = cJSON_CreateArray();
    const char *sr = "end_turn";
    cJSON *choices = cJSON_GetObjectItem(o, "choices");
    if (choices) {
        cJSON *ch = cJSON_GetArrayItem(choices, 0);
        if (ch) {
            cJSON *msg = cJSON_GetObjectItem(ch, "message");
            if (msg) {
                const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "content"));
                if (text && *text) {
                    cJSON *tb = cJSON_CreateObject();
                    cJSON_AddStringToObject(tb, "type", "text");
                    cJSON_AddStringToObject(tb, "text", text);
                    cJSON_AddItemToArray(content, tb);
                }
                cJSON *tcs = cJSON_GetObjectItem(msg, "tool_calls");
                if (tcs) {
                    cJSON *tc;
                    cJSON_ArrayForEach(tc, tcs) {
                        const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(tc, "id"));
                        cJSON *fn = cJSON_GetObjectItem(tc, "function");
                        if (!fn) continue;
                        const char *name     = cJSON_GetStringValue(cJSON_GetObjectItem(fn, "name"));
                        const char *args_str = cJSON_GetStringValue(cJSON_GetObjectItem(fn, "arguments"));
                        cJSON *input = NULL;
                        if (args_str) input = cJSON_Parse(args_str);
                        if (!input) input = cJSON_CreateObject();
                        cJSON *tub = cJSON_CreateObject();
                        cJSON_AddStringToObject(tub, "type", "tool_use");
                        cJSON_AddStringToObject(tub, "id", id ? id : "");
                        cJSON_AddStringToObject(tub, "name", name ? name : "");
                        cJSON_AddItemToObject(tub, "input", input);
                        cJSON_AddItemToArray(content, tub);
                    }
                }
            }
            const char *fr = cJSON_GetStringValue(cJSON_GetObjectItem(ch, "finish_reason"));
            if (fr) {
                if (strcmp(fr, "tool_calls") == 0) sr = "tool_use";
                else if (strcmp(fr, "length") == 0) sr = "max_tokens";
                else sr = "end_turn";
            }
        }
    }
    cJSON_AddStringToObject(a, "stop_reason", sr);
    cJSON_AddItemToObject(a, "content", content);
    cJSON *u = cJSON_GetObjectItem(o, "usage");
    if (u) {
        cJSON *au = cJSON_CreateObject();
        cJSON_AddNumberToObject(au, "input_tokens",
            cJSON_GetNumberValue(cJSON_GetObjectItem(u, "prompt_tokens")));
        cJSON_AddNumberToObject(au, "output_tokens",
            cJSON_GetNumberValue(cJSON_GetObjectItem(u, "completion_tokens")));
        cJSON_AddItemToObject(a, "usage", au);
    }
    char *s = cJSON_PrintUnformatted(a);
    cJSON_Delete(o); cJSON_Delete(a);
    return s;
}

static char *openai_api_once(const char *api_base, const char *api_key_maybe,
                              const char *body, long *http_code_out) {
    char *oai_body = anthropic_body_to_openai(body);
    if (!oai_body) return NULL;
    char url[1200];
    snprintf(url, sizeof(url), "%s/v1/chat/completions", api_base);
    CURL *curl = curl_easy_init();
    if (!curl) { free(oai_body); return NULL; }
    Buf buf = {0};
    struct curl_slist *h = NULL;
    h = curl_slist_append(h, "content-type: application/json");
    if (api_key_maybe && *api_key_maybe) {
        char auth[1024];
        snprintf(auth, sizeof(auth), "authorization: Bearer %s", api_key_maybe);
        h = curl_slist_append(h, auth);
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, oai_body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(oai_body));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 3600L);  /* local 122B can take a long time */
    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    if (http_code_out) *http_code_out = code;
    curl_slist_free_all(h); curl_easy_cleanup(curl); free(oai_body);
    if (rc != CURLE_OK) { logerr("[openai] curl: %s\n", curl_easy_strerror(rc)); free(buf.data); return NULL; }
    char *anthro_resp = openai_response_to_anthropic(buf.data);
    free(buf.data);
    return anthro_resp;
}

/* ==== SSE streaming ==== */
#define STREAM_MAX_BLOCKS 32
typedef struct {
    char *type; Buf text; char *id; char *name; Buf partial_json; int printed_nl;
} StreamBlock;
typedef struct {
    Buf recv;
    StreamBlock blocks[STREAM_MAX_BLOCKS];
    int n_blocks;
    char *stop_reason;
    long input_tokens, output_tokens;
    int message_stopped, print_text;
} StreamCtx;

static void sb_free(StreamBlock *b) {
    free(b->type); free(b->id); free(b->name);
    buf_free(&b->text); buf_free(&b->partial_json);
    memset(b, 0, sizeof(*b));
}
static void sc_init(StreamCtx *sc) { memset(sc, 0, sizeof(*sc)); }
static void sc_free(StreamCtx *sc) {
    for (int i = 0; i < sc->n_blocks; i++) sb_free(&sc->blocks[i]);
    buf_free(&sc->recv); free(sc->stop_reason);
    memset(sc, 0, sizeof(*sc));
}
static char *xstrdup(const char *s) { return s ? strdup(s) : NULL; }

static void handle_sse_event(StreamCtx *sc, const char *data_line) {
    cJSON *ev = cJSON_Parse(data_line);
    if (!ev) return;
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(ev, "type"));
    if (!type) { cJSON_Delete(ev); return; }
    if (strcmp(type, "message_start") == 0) {
        cJSON *m = cJSON_GetObjectItem(ev, "message");
        if (m) {
            cJSON *u = cJSON_GetObjectItem(m, "usage");
            if (u) sc->input_tokens += (long)cJSON_GetNumberValue(cJSON_GetObjectItem(u, "input_tokens"));
        }
    } else if (strcmp(type, "content_block_start") == 0) {
        int idx = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(ev, "index"));
        if (idx < 0 || idx >= STREAM_MAX_BLOCKS) { cJSON_Delete(ev); return; }
        cJSON *cb = cJSON_GetObjectItem(ev, "content_block");
        if (cb) {
            const char *bt = cJSON_GetStringValue(cJSON_GetObjectItem(cb, "type"));
            sc->blocks[idx].type = xstrdup(bt);
            if (bt && strcmp(bt, "tool_use") == 0) {
                sc->blocks[idx].id   = xstrdup(cJSON_GetStringValue(cJSON_GetObjectItem(cb, "id")));
                sc->blocks[idx].name = xstrdup(cJSON_GetStringValue(cJSON_GetObjectItem(cb, "name")));
            }
        }
        if (idx + 1 > sc->n_blocks) sc->n_blocks = idx + 1;
    } else if (strcmp(type, "content_block_delta") == 0) {
        int idx = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(ev, "index"));
        if (idx < 0 || idx >= STREAM_MAX_BLOCKS) { cJSON_Delete(ev); return; }
        cJSON *delta = cJSON_GetObjectItem(ev, "delta");
        if (!delta) { cJSON_Delete(ev); return; }
        const char *dtype = cJSON_GetStringValue(cJSON_GetObjectItem(delta, "type"));
        if (!dtype) { cJSON_Delete(ev); return; }
        if (strcmp(dtype, "text_delta") == 0) {
            const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(delta, "text"));
            if (t) {
                buf_append(&sc->blocks[idx].text, t, strlen(t));
                /* don't print if thinking or redacted_thinking */
                if (sc->print_text && sc->blocks[idx].type && 
                    strcmp(sc->blocks[idx].type, "thinking") != 0 &&
                    strcmp(sc->blocks[idx].type, "redacted_thinking") != 0) {
                    fputs(t, stdout); fflush(stdout);
                }
            }
        } else if (strcmp(dtype, "input_json_delta") == 0) {
            const char *p = cJSON_GetStringValue(cJSON_GetObjectItem(delta, "partial_json"));
            if (p) buf_append(&sc->blocks[idx].partial_json, p, strlen(p));
        }
    } else if (strcmp(type, "content_block_stop") == 0) {
        int idx = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(ev, "index"));
        if (idx >= 0 && idx < STREAM_MAX_BLOCKS &&
            sc->print_text && sc->blocks[idx].type &&
            strcmp(sc->blocks[idx].type, "text") == 0 && !sc->blocks[idx].printed_nl) {
            fputs("\n", stdout); fflush(stdout);
            sc->blocks[idx].printed_nl = 1;
        }
    } else if (strcmp(type, "message_delta") == 0) {
        cJSON *d = cJSON_GetObjectItem(ev, "delta");
        if (d) {
            const char *sr = cJSON_GetStringValue(cJSON_GetObjectItem(d, "stop_reason"));
            if (sr) { free(sc->stop_reason); sc->stop_reason = strdup(sr); }
        }
        cJSON *u = cJSON_GetObjectItem(ev, "usage");
        if (u) sc->output_tokens += (long)cJSON_GetNumberValue(cJSON_GetObjectItem(u, "output_tokens"));
    } else if (strcmp(type, "message_stop") == 0) {
        sc->message_stopped = 1;
    }
    cJSON_Delete(ev);
}

static void process_sse_buffer(StreamCtx *sc) {
    char *start = sc->recv.data;
    if (!start) return;
    char *end;
    while ((end = strstr(start, "\n\n")) != NULL) {
        *end = 0;
        char *line = start;
        while (line && *line) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = 0;
            if (strncmp(line, "data: ", 6) == 0) handle_sse_event(sc, line + 6);
            if (!nl) break;
            line = nl + 1;
        }
        start = end + 2;
    }
    if (start > sc->recv.data) {
        size_t rem = sc->recv.size - (start - sc->recv.data);
        if (rem > 0) memmove(sc->recv.data, start, rem);
        sc->recv.size = rem;
        if (sc->recv.data) sc->recv.data[rem] = 0;
    }
}

static size_t stream_write_cb(void *ptr, size_t sz, size_t nm, void *ud) {
    StreamCtx *sc = (StreamCtx *)ud;
    size_t n = sz * nm;
    buf_append(&sc->recv, ptr, n);
    process_sse_buffer(sc);
    return n;
}

static char *stream_to_anthropic_json(StreamCtx *sc) {
    cJSON *root = cJSON_CreateObject();
    cJSON *content = cJSON_CreateArray();
    for (int i = 0; i < sc->n_blocks; i++) {
        StreamBlock *b = &sc->blocks[i];
        if (!b->type) continue;
        /* log thinking blocks to stderr and skip from content */
        if (strcmp(b->type, "thinking") == 0 || strcmp(b->type, "redacted_thinking") == 0) {
            if (b->text.data && b->text.size > 0) {
                logerr("[thinking] %.*s\n", (int)b->text.size, b->text.data);
            }
            continue;
        }
        cJSON *blk = cJSON_CreateObject();
        cJSON_AddStringToObject(blk, "type", b->type);
        if (strcmp(b->type, "text") == 0) {
            cJSON_AddStringToObject(blk, "text", b->text.data ? b->text.data : "");
        } else if (strcmp(b->type, "tool_use") == 0) {
            cJSON_AddStringToObject(blk, "id",   b->id   ? b->id   : "");
            cJSON_AddStringToObject(blk, "name",  b->name ? b->name : "");
            cJSON *input = NULL;
            if (b->partial_json.size > 0) input = cJSON_Parse(b->partial_json.data);
            if (!input) input = cJSON_CreateObject();
            cJSON_AddItemToObject(blk, "input", input);
        }
        cJSON_AddItemToArray(content, blk);
    }
    cJSON_AddItemToObject(root, "content", content);
    cJSON_AddStringToObject(root, "stop_reason", sc->stop_reason ? sc->stop_reason : "end_turn");
    cJSON *usage = cJSON_CreateObject();
    cJSON_AddNumberToObject(usage, "input_tokens",  (double)sc->input_tokens);
    cJSON_AddNumberToObject(usage, "output_tokens", (double)sc->output_tokens);
    cJSON_AddItemToObject(root, "usage", usage);
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;
}

static char *claude_api_stream(const char *api_key, const char *body, long *http_code_out) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;
    StreamCtx sc; sc_init(&sc);
    sc.print_text = !g_quiet;
    cJSON *bj = cJSON_Parse(body);
    if (!bj) { sc_free(&sc); curl_easy_cleanup(curl); return NULL; }
    cJSON_DeleteItemFromObject(bj, "stream");
    cJSON_AddBoolToObject(bj, "stream", 1);
    char *stream_body = cJSON_PrintUnformatted(bj);
    cJSON_Delete(bj);
    struct curl_slist *h = NULL;
    char auth[1024];
    snprintf(auth, sizeof(auth), "x-api-key: %s", api_key);
    h = curl_slist_append(h, auth);
    h = curl_slist_append(h, "anthropic-version: 2023-06-01");
    h = curl_slist_append(h, g_think_mode
        ? "anthropic-beta: prompt-caching-2024-07-31,interleaved-thinking-2025-05-14"
        : "anthropic-beta: prompt-caching-2024-07-31");
    h = curl_slist_append(h, "content-type: application/json");
    h = curl_slist_append(h, "accept: text/event-stream");
    { char url[512]; snprintf(url, sizeof(url), "%s/v1/messages",
        g_api_base[0] ? g_api_base : "https://api.anthropic.com");
      curl_easy_setopt(curl, CURLOPT_URL, url); }
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, stream_body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(stream_body));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sc);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);
    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    if (http_code_out) *http_code_out = code;
    curl_slist_free_all(h); curl_easy_cleanup(curl); free(stream_body);
    if (rc != CURLE_OK) { logerr("[stream] curl: %s\n", curl_easy_strerror(rc)); sc_free(&sc); return NULL; }
    if (code >= 400) {
        char *raw = sc.recv.data ? strdup(sc.recv.data) : strdup("{\"error\":{\"message\":\"stream error\"}}");
        sc_free(&sc);
        return raw;
    }
    char *resp = stream_to_anthropic_json(&sc);
    sc_free(&sc);
    return resp;
}

static char *claude_api_once(const char *api_key, const char *body, long *http_code_out) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;
    Buf buf = {0};
    struct curl_slist *h = NULL;
    char auth[1024];
    snprintf(auth, sizeof(auth), "x-api-key: %s", api_key);
    h = curl_slist_append(h, auth);
    h = curl_slist_append(h, "anthropic-version: 2023-06-01");
    h = curl_slist_append(h, g_think_mode
        ? "anthropic-beta: prompt-caching-2024-07-31,interleaved-thinking-2025-05-14"
        : "anthropic-beta: prompt-caching-2024-07-31");
    h = curl_slist_append(h, "content-type: application/json");
    { char url[512]; snprintf(url, sizeof(url), "%s/v1/messages",
        g_api_base[0] ? g_api_base : "https://api.anthropic.com");
      curl_easy_setopt(curl, CURLOPT_URL, url); }
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);
    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    if (http_code_out) *http_code_out = code;
    curl_slist_free_all(h); curl_easy_cleanup(curl);
    if (rc != CURLE_OK) { logerr("[curl] %s\n", curl_easy_strerror(rc)); free(buf.data); return NULL; }
    return buf.data;
}

static char *claude_api(const char *api_key, const char *body) {
    const char *api_base_eff = g_api_base[0] ? g_api_base :
        (g_backend == BACKEND_OPENAI ? "http://localhost:4004" : "https://api.anthropic.com");
    for (int attempt = 0; attempt < MAX_RETRIES; attempt++) {
        long code = 0;
        char *resp;
        if (g_backend == BACKEND_OPENAI)
            resp = openai_api_once(api_base_eff, api_key, body, &code);
        else if (g_stream_mode)
            resp = claude_api_stream(api_key, body, &code);
        else
            resp = claude_api_once(api_key, body, &code);
        if (!resp) { sleep(RETRY_SLEEP_SEC); continue; }
        if (code == 529 || (code >= 500 && code < 600) ||
            (resp && strstr(resp, "\"overloaded_error\""))) {
            logerr("[retry %d] http=%ld sleeping %ds\n", attempt, code, RETRY_SLEEP_SEC);
            free(resp); sleep(RETRY_SLEEP_SEC); continue;
        }
        return resp;
    }
    return NULL;
}

/* ---------- read stream helper ---------- */
static char *read_stream_to_string(FILE *p) {
    size_t cap = 8192, len = 0;
    char *out = malloc(cap);
    size_t n;
    while ((n = fread(out + len, 1, cap - len - 1, p)) > 0) {
        len += n;
        if (len + 1 >= cap) {
            if (cap >= MAX_TOOL_OUT_BYTES) {
                const char *trunc = "\n[... output truncated ...]\n";
                size_t tl = strlen(trunc);
                if (len + tl + 1 < cap) { memcpy(out + len, trunc, tl + 1); len += tl; }
                break;
            }
            cap *= 2;
            if (cap > MAX_TOOL_OUT_BYTES) cap = MAX_TOOL_OUT_BYTES;
            char *np = realloc(out, cap);
            if (!np) break;
            out = np;
        }
    }
    out[len] = 0;
    return out;
}

/* ---------- shell single-quote escape helper ---------- */
/* appends shell-safe single-quoted version of s to buf */
static void buf_append_sq(Buf *b, const char *s) {
    buf_append(b, "'", 1);
    for (; *s; s++) {
        if (*s == '\'') buf_append(b, "'\\''", 4);
        else buf_append(b, s, 1);
    }
    buf_append(b, "'", 1);
}

/* ---------- builtin tools ---------- */

static char *tool_read_file(const char *path) {
    if (!path_is_safe(path)) {
        char *r = malloc(256);
        snprintf(r, 256, "Error: path '%s' not in sandboxed workspace", path ? path : "(null)");
        return r;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        char *r = malloc(512);
        snprintf(r, 512, "Error: cannot open %s (%s)", path, strerror(errno));
        return r;
    }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0) sz = 0;
    if (sz > MAX_TOOL_OUT_BYTES) sz = MAX_TOOL_OUT_BYTES;
    char *buf = malloc(sz + 64);
    size_t got = fread(buf, 1, sz, f);
    buf[got] = 0; fclose(f);
    redact_secrets(buf);
    return buf;
}

/* auto-mkdir: create parent directories for a path */
static void auto_mkdir(const char *path) {
    if (!path || !*path) return;
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    char *p = tmp;
    /* skip leading './' */
    if (p[0] == '.' && p[1] == '/') p += 2;
    for (; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (tmp[0]) mkdir(tmp, 0755);
            *p = '/';
        }
    }
}

static char *tool_write_file(const char *path, const char *content) {
    if (!path_is_safe(path)) {
        char *r = malloc(256);
        snprintf(r, 256, "Error: path '%s' not in sandboxed workspace", path ? path : "(null)");
        return r;
    }
    if (!content) content = "";
    auto_mkdir(path);
    FILE *f = fopen(path, "wb");
    if (!f) {
        char *r = malloc(512);
        snprintf(r, 512, "Error: cannot write %s (%s)", path, strerror(errno));
        return r;
    }
    size_t n = strlen(content);
    fwrite(content, 1, n, f); fclose(f);
    char *r = malloc(128);
    snprintf(r, 128, "Wrote %zu bytes to %s", n, path);
    return r;
}

static char *tool_edit_file(const char *path, const char *old_str, const char *new_str, int replace_all) {
    if (!path_is_safe(path)) {
        char *r = malloc(256);
        snprintf(r, 256, "Error: path '%s' not in sandboxed workspace", path ? path : "(null)");
        return r;
    }
    if (!old_str || !*old_str) return strdup("Error: old_string is empty");
    if (!new_str) new_str = "";
    FILE *f = fopen(path, "rb");
    if (!f) {
        char *r = malloc(512);
        snprintf(r, 512, "Error: cannot open %s (%s)", path, strerror(errno));
        return r;
    }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0) sz = 0;
    char *buf = malloc(sz + 1);
    size_t got = fread(buf, 1, sz, f);
    buf[got] = 0; fclose(f);
    size_t olen = strlen(old_str), nlen = strlen(new_str);
    int count = 0;
    for (char *p = buf; (p = strstr(p, old_str)) != NULL; p += olen) count++;
    if (count == 0) {
        free(buf);
        char *r = malloc(256);
        snprintf(r, 256, "Error: old_string not found in %s", path);
        return r;
    }
    if (count > 1 && !replace_all) {
        free(buf);
        char *r = malloc(256);
        snprintf(r, 256, "Error: old_string matches %d places; add context or set replace_all=true", count);
        return r;
    }
    int n_replace = replace_all ? count : 1;
    size_t out_cap = got + (n_replace * (nlen > olen ? (nlen - olen) : 0)) + 16;
    char *out = malloc(out_cap + 1);
    size_t oi = 0;
    char *src = buf;
    int done = 0;
    while (*src) {
        if (done < n_replace && strncmp(src, old_str, olen) == 0) {
            memcpy(out + oi, new_str, nlen); oi += nlen; src += olen; done++;
        } else {
            out[oi++] = *src++;
        }
    }
    out[oi] = 0;
    FILE *w = fopen(path, "wb");
    if (!w) {
        free(buf); free(out);
        char *r = malloc(512);
        snprintf(r, 512, "Error: cannot write %s (%s)", path, strerror(errno));
        return r;
    }
    fwrite(out, 1, oi, w); fclose(w);
    free(buf); free(out);
    char *r = malloc(160);
    snprintf(r, 160, "Edited %s: replaced %d occurrence%s", path, n_replace, n_replace == 1 ? "" : "s");
    return r;
}

static char *tool_bash(const char *cmd) {
    if (!cmd || !*cmd) return strdup("Error: empty command");
    const char *bad = check_dangerous(cmd);
    if (bad) {
        char *r = malloc(512);
        snprintf(r, 512, "BLOCKED: dangerous pattern '%s'", bad);
        return r;
    }
    char cmdfile[128];
    snprintf(cmdfile, sizeof(cmdfile), "/tmp/mini_agent_cmd_%d_%ld.sh", getpid(), (long)time(NULL));
    FILE *cf = fopen(cmdfile, "w");
    if (!cf) return strdup("Error: tempfile failed");
    fprintf(cf, "#!/bin/bash\nset -o pipefail\nulimit -t 60\nulimit -f 100000\n%s\n", cmd);
    fclose(cf); chmod(cmdfile, 0700);
    char full[2048];
    if (g_use_sandbox)
        snprintf(full, sizeof(full), "sandbox-exec -D CWD=%s -f .agent/sandbox.sb /bin/bash %s 2>&1", g_cwd, cmdfile);
    else
        snprintf(full, sizeof(full), "/bin/bash %s 2>&1", cmdfile);
    FILE *p = popen(full, "r");
    if (!p) { unlink(cmdfile); return strdup("Error: popen failed"); }
    char *out;
    if (g_stream_bash) {
        /* stream to stderr while reading */
        size_t cap = 8192, len = 0;
        out = malloc(cap);
        char chunk[4096];
        size_t n;
        while ((n = fread(chunk, 1, sizeof(chunk), p)) > 0) {
            fwrite(chunk, 1, n, stderr);
            fflush(stderr);
            if (len + n + 1 >= cap) {
                if (cap >= MAX_TOOL_OUT_BYTES) {
                    const char *trunc = "\n[... output truncated ...]\n";
                    size_t tl = strlen(trunc);
                    if (len + tl + 1 < cap) { memcpy(out + len, trunc, tl + 1); len += tl; }
                    break;
                }
                cap *= 2;
                if (cap > MAX_TOOL_OUT_BYTES) cap = MAX_TOOL_OUT_BYTES;
                char *np = realloc(out, cap);
                if (!np) break;
                out = np;
            }
            memcpy(out + len, chunk, n);
            len += n;
        }
        out[len] = 0;
    } else {
        out = read_stream_to_string(p);
    }
    int rc = pclose(p); unlink(cmdfile);
    char tail[64];
    snprintf(tail, sizeof(tail), "\n[exit=%d]", WIFEXITED(rc) ? WEXITSTATUS(rc) : -1);
    size_t ol = strlen(out), tl = strlen(tail);
    char *np = realloc(out, ol + tl + 1);
    if (np) { out = np; memcpy(out + ol, tail, tl + 1); }
    redact_secrets(out);
    return out;
}

static char *tool_list_dir(const char *path) {
    if (!path_is_safe(path)) {
        char *r = malloc(256);
        snprintf(r, 256, "Error: path '%s' not in sandboxed workspace", path ? path : "(null)");
        return r;
    }
    DIR *d = opendir(path);
    if (!d) {
        char *r = malloc(512);
        snprintf(r, 512, "Error: cannot open %s (%s)", path, strerror(errno));
        return r;
    }
    Buf b = {0};
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        buf_append(&b, e->d_name, strlen(e->d_name));
        buf_append(&b, "\n", 1);
    }
    closedir(d);
    if (!b.data) b.data = strdup("(empty)");
    return b.data;
}

/* NEW: grep_files — regex search across files */
static char *tool_grep_files(const char *pattern, const char *path, const char *include_glob) {
    if (!pattern || !*pattern) return strdup("Error: pattern required");
    const char *p = (path && *path) ? path : ".";
    if (!path_is_safe(p)) {
        char *r = malloc(256);
        snprintf(r, 256, "Error: path '%s' not in workspace", p);
        return r;
    }
    Buf cmd = {0};
    /* use -E for extended regex, -n for line numbers, -m 300 limit matches */
    buf_append(&cmd, "grep -rn --color=never -E -m 300 ", 33);
    if (include_glob && *include_glob) {
        buf_append(&cmd, "--include=", 10);
        buf_append_sq(&cmd, include_glob);
        buf_append(&cmd, " ", 1);
    }
    buf_append(&cmd, "-- ", 3);
    buf_append_sq(&cmd, pattern);
    buf_append(&cmd, " ", 1);
    buf_append_sq(&cmd, p);
    buf_append(&cmd, " 2>&1 | head -300", 17);

    FILE *pipe = popen(cmd.data, "r");
    buf_free(&cmd);
    if (!pipe) return strdup("Error: popen failed");
    char *out = read_stream_to_string(pipe);
    pclose(pipe);
    if (!out || !*out) { free(out); return strdup("(no matches)"); }
    return out;
}

/* NEW: glob_files — find files matching a pattern */
static char *tool_glob_files(const char *pattern, const char *path) {
    if (!pattern || !*pattern) return strdup("Error: pattern required");
    const char *p = (path && *path) ? path : ".";
    if (!path_is_safe(p)) {
        char *r = malloc(256);
        snprintf(r, 256, "Error: path '%s' not in workspace", p);
        return r;
    }
    Buf cmd = {0};
    buf_append(&cmd, "find ", 5);
    buf_append_sq(&cmd, p);
    buf_append(&cmd, " -name ", 7);
    buf_append_sq(&cmd, pattern);
    buf_append(&cmd, " -not -path '*/.git/*' -not -path '*/node_modules/*'"
                     " 2>&1 | sort | head -500", 72);
    FILE *pipe = popen(cmd.data, "r");
    buf_free(&cmd);
    if (!pipe) return strdup("Error: popen failed");
    char *out = read_stream_to_string(pipe);
    pclose(pipe);
    if (!out || !*out) { free(out); return strdup("(no files found)"); }
    return out;
}

/* NEW: http_get — fetch a URL (requires --allow-http) */
static char *tool_http_get(const char *url) {
    if (!g_allow_http)
        return strdup("Error: http_get is disabled. Restart with --allow-http to enable.");
    if (!url || (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0))
        return strdup("Error: only http:// and https:// URLs are allowed.");
    CURL *curl = curl_easy_init();
    if (!curl) return strdup("Error: curl init failed");
    Buf buf = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "mini-agent-c/8.0");
    CURLcode rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) {
        free(buf.data);
        char *r = malloc(256);
        snprintf(r, 256, "Error: %s", curl_easy_strerror(rc));
        return r;
    }
    if (!buf.data) return strdup("(empty response)");
    /* truncate to 32KB */
    if (buf.size > 32 * 1024) { buf.data[32 * 1024] = 0; }
    redact_secrets(buf.data);
    return buf.data;
}

/* NEW: todo — task list management in .agent/todo.md */
#define TODO_FILE ".agent/todo.md"

static char *tool_todo(const char *op, const char *item) {
    if (!op || !*op) return strdup("Error: op required: add | list | done | clear");
    ensure_agent_dir();

    if (strcmp(op, "list") == 0) {
        FILE *f = fopen(TODO_FILE, "r");
        if (!f) return strdup("(no todos)");
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        if (sz <= 0) { fclose(f); return strdup("(no todos)"); }
        char *buf = malloc(sz + 1);
        size_t got = fread(buf, 1, sz, f);
        buf[got] = 0; fclose(f);
        return buf;
    }

    if (strcmp(op, "add") == 0) {
        if (!item || !*item) return strdup("Error: item text required for add");
        FILE *f = fopen(TODO_FILE, "a");
        if (!f) return strdup("Error: cannot open todo file");
        fprintf(f, "- [ ] %s\n", item);
        fclose(f);
        return strdup("Added.");
    }

    if (strcmp(op, "done") == 0) {
        if (!item || !*item) return strdup("Error: item text required for done");
        FILE *f = fopen(TODO_FILE, "rb");
        if (!f) return strdup("Error: no todo file");
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        char *buf = malloc(sz + 1);
        size_t got = fread(buf, 1, sz, f); buf[got] = 0; fclose(f);
        /* replace first "- [ ] <item>" with "- [x] <item>" */
        char needle[1024];
        snprintf(needle, sizeof(needle), "- [ ] %s", item);
        char *found = strstr(buf, needle);
        if (!found) { free(buf); return strdup("Error: item not found in todo list"); }
        found[3] = 'x'; /* [ ] -> [x] */
        FILE *w = fopen(TODO_FILE, "wb");
        if (!w) { free(buf); return strdup("Error: cannot write todo file"); }
        fwrite(buf, 1, got, w); fclose(w); free(buf);
        return strdup("Marked done.");
    }

    if (strcmp(op, "clear") == 0) {
        /* keep only uncompleted items */
        FILE *f = fopen(TODO_FILE, "r");
        if (!f) return strdup("(nothing to clear)");
        Buf out = {0};
        char line[2048];
        int cleared = 0;
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "- [x]", 5) == 0) { cleared++; continue; }
            buf_append(&out, line, strlen(line));
        }
        fclose(f);
        FILE *w = fopen(TODO_FILE, "wb");
        if (w) { if (out.data) fwrite(out.data, 1, out.size, w); fclose(w); }
        buf_free(&out);
        char *r = malloc(64);
        snprintf(r, 64, "Cleared %d completed item%s.", cleared, cleared == 1 ? "" : "s");
        return r;
    }

    return strdup("Error: unknown op. Use: add | list | done | clear");
}

/* NEW v9: ask_user — prompt user mid-task */
static char *tool_ask_user(const char *question, const char *default_val) {
    if (!question || !*question) return strdup("Error: question required");
    int tty = open("/dev/tty", O_RDWR);
    if (tty < 0) return strdup("Error: cannot open /dev/tty");
    
    char prompt[2048];
    if (default_val && *default_val)
        snprintf(prompt, sizeof(prompt), "\n[ask_user] %s [%s]: ", question, default_val);
    else
        snprintf(prompt, sizeof(prompt), "\n[ask_user] %s: ", question);
    
    write(tty, prompt, strlen(prompt));
    
    char buf[4096];
    ssize_t n = read(tty, buf, sizeof(buf) - 1);
    close(tty);
    
    if (n <= 0) {
        if (default_val && *default_val) return strdup(default_val);
        return strdup("");
    }
    
    buf[n] = 0;
    /* trim newline */
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = 0;
    
    if (n == 0 && default_val && *default_val) return strdup(default_val);
    return strdup(buf);
}

/* NEW v9: diff_files — diff between two files */
static char *tool_diff_files(const char *path_a, const char *path_b) {
    if (!path_is_safe(path_a)) {
        char *r = malloc(256);
        snprintf(r, 256, "Error: path_a '%s' not in workspace", path_a ? path_a : "(null)");
        return r;
    }
    if (!path_is_safe(path_b)) {
        char *r = malloc(256);
        snprintf(r, 256, "Error: path_b '%s' not in workspace", path_b ? path_b : "(null)");
        return r;
    }
    
    Buf cmd = {0};
    buf_append(&cmd, "diff -u ", 8);
    buf_append_sq(&cmd, path_a);
    buf_append(&cmd, " ", 1);
    buf_append_sq(&cmd, path_b);
    buf_append(&cmd, " 2>&1", 5);
    
    FILE *p = popen(cmd.data, "r");
    buf_free(&cmd);
    if (!p) return strdup("Error: popen failed");
    
    char *out = read_stream_to_string(p);
    int rc = pclose(p);
    
    if (!out || !*out) {
        free(out);
        if (WIFEXITED(rc) && WEXITSTATUS(rc) == 0)
            return strdup("(files are identical)");
        return strdup("(diff failed)");
    }
    return out;
}

/* NEW v9: git — safe git operations */
static char *tool_git(const char *subcommand, const char *args) {
    if (!subcommand || !*subcommand) return strdup("Error: subcommand required");
    
    /* whitelist of allowed subcommands */
    const char *allowed[] = {"status", "log", "diff", "add", "commit", "branch", 
                             "checkout", "show", "stash", "reset", NULL};
    int ok = 0;
    for (int i = 0; allowed[i]; i++) {
        if (strcmp(subcommand, allowed[i]) == 0) { ok = 1; break; }
    }
    if (!ok) {
        char *r = malloc(512);
        snprintf(r, 512, "Error: git subcommand '%s' not allowed. Use: status, log, diff, add, commit, branch, checkout, show, stash, reset (soft/mixed only)", subcommand);
        return r;
    }
    
    /* block dangerous patterns */
    if (args) {
        if (strstr(args, "--force")) return strdup("Error: --force not allowed");
        if (strstr(args, "rm -rf")) return strdup("Error: rm -rf not allowed");
        if (strcmp(subcommand, "reset") == 0) {
            if (strstr(args, "--hard") && !strstr(args, "HEAD"))
                return strdup("Error: git reset --hard only allowed with HEAD");
        }
    }
    
    Buf cmd = {0};
    buf_append(&cmd, "git ", 4);
    buf_append(&cmd, subcommand, strlen(subcommand));
    if (args && *args) {
        buf_append(&cmd, " ", 1);
        buf_append(&cmd, args, strlen(args));
    }
    buf_append(&cmd, " 2>&1", 5);
    
    FILE *p = popen(cmd.data, "r");
    buf_free(&cmd);
    if (!p) return strdup("Error: popen failed");
    
    char *out = read_stream_to_string(p);
    pclose(p);
    if (!out || !*out) { free(out); return strdup("(no output)"); }
    return out;
}

/* NEW v9: notify — macOS notification */
static char *tool_notify(const char *title, const char *message) {
    if (!title) title = "mini-agent";
    if (!message) message = "";
    
    /* escape single quotes for shell */
    Buf t = {0}, m = {0};
    for (const char *p = title; *p; p++) {
        if (*p == '\'') buf_append(&t, "'\\''", 4);
        else buf_append(&t, p, 1);
    }
    for (const char *p = message; *p; p++) {
        if (*p == '\'') buf_append(&m, "'\\''", 4);
        else buf_append(&m, p, 1);
    }
    
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), 
             "osascript -e 'display notification \"%s\" with title \"%s\"' 2>&1",
             m.data ? m.data : "", t.data ? t.data : "");
    
    buf_free(&t);
    buf_free(&m);
    
    FILE *p = popen(cmd, "r");
    if (!p) return strdup("Error: osascript failed");
    
    char buf[512];
    size_t n = fread(buf, 1, sizeof(buf) - 1, p);
    buf[n] = 0;
    int rc = pclose(p);
    
    if (WIFEXITED(rc) && WEXITSTATUS(rc) == 0)
        return strdup("Notification sent");
    
    char *r = malloc(600);
    snprintf(r, 600, "Error: %s", buf[0] ? buf : "osascript failed");
    return r;
}

static char *tool_save_memory(const char *key, const char *value) {
    if (!key || !value) return strdup("Error: key and value required");
    FILE *f = fopen(g_memory_path, "a");
    if (!f) {
        char *r = malloc(512);
        snprintf(r, 512, "Error: cannot append memory (%s)", strerror(errno));
        return r;
    }
    time_t t = time(NULL);
    struct tm tmv; localtime_r(&t, &tmv);
    char ts[32]; strftime(ts, sizeof(ts), "%Y-%m-%d", &tmv);
    fprintf(f, "\n## %s [%s]\n%s\n", key, ts, value);
    fclose(f);
    return strdup("Memory saved");
}

static char *tool_recall_memory(void) {
    FILE *f = fopen(g_memory_path, "rb");
    if (!f) return strdup("(no memory yet)");
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return strdup("(empty memory)"); }
    if (sz > 32 * 1024) sz = 32 * 1024;
    char *buf = malloc(sz + 1);
    fread(buf, 1, sz, f); buf[sz] = 0; fclose(f);
    return buf;
}

static char *tool_spawn_agent(const char *task, const char *model) {
    if (g_spawn_depth + 1 > MAX_SPAWN_DEPTH) {
        char *r = malloc(256);
        snprintf(r, 256, "Error: spawn depth limit reached (%d)", MAX_SPAWN_DEPTH);
        return r;
    }
    if (!task || !*task) return strdup("Error: empty task");
    int pipefd[2];
    if (pipe(pipefd) < 0) return strdup("Error: pipe failed");
    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return strdup("Error: fork failed"); }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO); close(pipefd[1]);
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) { dup2(dn, STDERR_FILENO); close(dn); }
        char depth_str[16];
        snprintf(depth_str, sizeof(depth_str), "%d", g_spawn_depth + 1);
        setenv("MINI_AGENT_DEPTH", depth_str, 1);
        if (model && *model)
            execl(g_self_path, "agent.v8", "--quiet", "--model", model, task, (char *)NULL);
        else
            execl(g_self_path, "agent.v8", "--quiet", task, (char *)NULL);
        _exit(127);
    }
    close(pipefd[1]);
    Buf b = {0};
    char tmp[4096]; ssize_t rd;
    while ((rd = read(pipefd[0], tmp, sizeof(tmp))) > 0) {
        buf_append(&b, tmp, rd);
        if (b.size > 32 * 1024) break;
    }
    close(pipefd[0]);
    int status; waitpid(pid, &status, 0);
    if (!b.data) b.data = strdup("(no output from subagent)");
    return b.data;
}

/* ============ NEW v10 TOOLS ============ */

/* http_request — general HTTP tool with method, body, headers */
static char *tool_http_request(const char *method, const char *url, const char *body, const char *headers) {
    if (!g_allow_http)
        return strdup("Error: http_request is disabled. Restart with --allow-http to enable.");
    if (!url || (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0))
        return strdup("Error: only http:// and https:// URLs are allowed.");
    if (!method || !*method) method = "GET";
    
    /* validate method */
    if (strcmp(method, "GET") != 0 && strcmp(method, "POST") != 0 &&
        strcmp(method, "PUT") != 0 && strcmp(method, "DELETE") != 0 &&
        strcmp(method, "PATCH") != 0)
        return strdup("Error: method must be GET, POST, PUT, DELETE, or PATCH");
    
    CURL *curl = curl_easy_init();
    if (!curl) return strdup("Error: curl init failed");
    
    Buf buf = {0};
    struct curl_slist *h = NULL;
    
    /* parse headers if provided */
    if (headers && *headers) {
        char *hdrs = strdup(headers);
        char *line = hdrs, *next;
        while (line) {
            next = strchr(line, ',');
            if (next) { *next = 0; next++; }
            /* trim leading/trailing spaces */
            while (*line && isspace((unsigned char)*line)) line++;
            char *end = line + strlen(line) - 1;
            while (end > line && isspace((unsigned char)*end)) *end-- = 0;
            if (*line) h = curl_slist_append(h, line);
            line = next;
        }
        free(hdrs);
    }
    
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "mini-agent-c/10.0");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, h);
    
    if (strcmp(method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        if (body) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
        }
    } else if (strcmp(method, "PUT") == 0) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
        if (body) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
        }
    } else if (strcmp(method, "DELETE") == 0) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
        if (body) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
        }
    } else if (strcmp(method, "PATCH") == 0) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PATCH");
        if (body) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
        }
    }
    
    CURLcode rc = curl_easy_perform(curl);
    if (h) curl_slist_free_all(h);
    curl_easy_cleanup(curl);
    
    if (rc != CURLE_OK) {
        free(buf.data);
        char *r = malloc(256);
        snprintf(r, 256, "Error: %s", curl_easy_strerror(rc));
        return r;
    }
    if (!buf.data) return strdup("(empty response)");
    /* truncate to 32KB */
    if (buf.size > 32 * 1024) { buf.data[32 * 1024] = 0; }
    redact_secrets(buf.data);
    return buf.data;
}

/* checkpoint — snapshot modified files */
static char *tool_checkpoint(void) {
    /* create checkpoint directory with timestamp */
    time_t now = time(NULL);
    char ts[64];
    struct tm tm_val;
    localtime_r(&now, &tm_val);
    strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tm_val);
    
    char ckpt_path[1200];
    snprintf(ckpt_path, sizeof(ckpt_path), "%s/%s", g_checkpoint_dir, ts);
    if (mkdir(ckpt_path, 0755) < 0) {
        char *r = malloc(512);
        snprintf(r, 512, "Error: cannot create checkpoint directory (%s)", strerror(errno));
        return r;
    }
    
    /* find modified files by reading audit.log */
    cJSON *manifest = cJSON_CreateObject();
    cJSON_AddStringToObject(manifest, "timestamp", ts);
    cJSON *files = cJSON_CreateArray();
    
    FILE *audit = fopen(".agent/audit.log", "r");
    if (audit) {
        char line[8192];
        while (fgets(line, sizeof(line), audit)) {
            cJSON *entry = cJSON_Parse(line);
            if (!entry) continue;
            const char *tool = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "tool"));
            if (tool && (strcmp(tool, "write_file") == 0 || strcmp(tool, "edit_file") == 0)) {
                cJSON *input_obj = cJSON_Parse(cJSON_GetStringValue(cJSON_GetObjectItem(entry, "input")));
                if (input_obj) {
                    const char *path = cJSON_GetStringValue(cJSON_GetObjectItem(input_obj, "path"));
                    if (path && path_is_safe(path)) {
                        /* check if already in list */
                        int found = 0;
                        cJSON *item;
                        cJSON_ArrayForEach(item, files) {
                            if (strcmp(cJSON_GetStringValue(item), path) == 0) {
                                found = 1; break;
                            }
                        }
                        if (!found) {
                            /* copy file to checkpoint */
                            char dest[1200];
                            snprintf(dest, sizeof(dest), "%s/%s", ckpt_path, path);
                            /* create parent directories */
                            char *last_slash = strrchr(dest, '/');
                            if (last_slash) {
                                *last_slash = 0;
                                char cmd[2048];
                                snprintf(cmd, sizeof(cmd), "mkdir -p '%s' 2>&1", dest);
                                system(cmd);
                                *last_slash = '/';
                            }
                            /* copy file */
                            char cmd[2048];
                            snprintf(cmd, sizeof(cmd), "cp '%s' '%s' 2>&1", path, dest);
                            system(cmd);
                            cJSON_AddItemToArray(files, cJSON_CreateString(path));
                        }
                    }
                    cJSON_Delete(input_obj);
                }
            }
            cJSON_Delete(entry);
        }
        fclose(audit);
    }
    
    cJSON_AddItemToObject(manifest, "files", files);
    
    /* write manifest */
    char manifest_path[1200];
    snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.json", ckpt_path);
    char *manifest_str = cJSON_Print(manifest);
    FILE *mf = fopen(manifest_path, "w");
    if (mf) {
        fprintf(mf, "%s\n", manifest_str);
        fclose(mf);
    }
    free(manifest_str);
    cJSON_Delete(manifest);
    
    char *r = malloc(256);
    snprintf(r, 256, "Checkpoint %s saved (%d files)", ts, cJSON_GetArraySize(files));
    return r;
}

/* undo — restore from latest checkpoint */
static char *tool_undo(void) {
    /* find latest checkpoint */
    DIR *d = opendir(g_checkpoint_dir);
    if (!d) return strdup("Error: no checkpoints found");
    
    char latest[256] = "";
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (strcmp(e->d_name, latest) > 0)
            snprintf(latest, sizeof(latest), "%s", e->d_name);
    }
    closedir(d);
    
    if (!latest[0]) return strdup("Error: no checkpoints found");
    
    /* read manifest */
    char manifest_path[1200];
    snprintf(manifest_path, sizeof(manifest_path), "%s/%s/manifest.json", g_checkpoint_dir, latest);
    FILE *mf = fopen(manifest_path, "r");
    if (!mf) return strdup("Error: cannot read checkpoint manifest");
    
    fseek(mf, 0, SEEK_END); long sz = ftell(mf); fseek(mf, 0, SEEK_SET);
    char *manifest_str = malloc(sz + 1);
    fread(manifest_str, 1, sz, mf);
    manifest_str[sz] = 0;
    fclose(mf);
    
    cJSON *manifest = cJSON_Parse(manifest_str);
    free(manifest_str);
    if (!manifest) return strdup("Error: invalid manifest");
    
    cJSON *files = cJSON_GetObjectItem(manifest, "files");
    Buf result = {0};
    buf_append(&result, "Restored files:\n", 16);
    
    cJSON *item;
    cJSON_ArrayForEach(item, files) {
        const char *path = cJSON_GetStringValue(item);
        if (!path) continue;
        
        char src[1200];
        snprintf(src, sizeof(src), "%s/%s/%s", g_checkpoint_dir, latest, path);
        
        char cmd[2048];
        snprintf(cmd, sizeof(cmd), "cp '%s' '%s' 2>&1", src, path);
        system(cmd);
        
        buf_append(&result, "  ", 2);
        buf_append(&result, path, strlen(path));
        buf_append(&result, "\n", 1);
    }
    
    cJSON_Delete(manifest);
    if (!result.data) result.data = strdup("Error: no files restored");
    return result.data;
}

/* process_start — start background process */
static char *tool_process_start(const char *name, const char *command) {
    if (!name || !*name) return strdup("Error: name required");
    if (!command || !*command) return strdup("Error: command required");
    
    /* create .agent/procs directory */
    mkdir(".agent/procs", 0755);
    
    char pid_path[1024], log_path[1024];
    snprintf(pid_path, sizeof(pid_path), ".agent/procs/%s.pid", name);
    snprintf(log_path, sizeof(log_path), ".agent/procs/%s.log", name);
    
    /* check if already running */
    FILE *pf = fopen(pid_path, "r");
    if (pf) {
        int old_pid;
        if (fscanf(pf, "%d", &old_pid) == 1) {
            fclose(pf);
            if (kill(old_pid, 0) == 0)
                return strdup("Error: process already running");
        } else {
            fclose(pf);
        }
    }
    
    pid_t pid = fork();
    if (pid < 0) return strdup("Error: fork failed");
    
    if (pid == 0) {
        /* child process */
        int logfd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (logfd >= 0) {
            dup2(logfd, STDOUT_FILENO);
            dup2(logfd, STDERR_FILENO);
            close(logfd);
        }
        setsid(); /* detach from parent */
        execl("/bin/bash", "bash", "-c", command, (char *)NULL);
        _exit(127);
    }
    
    /* parent: write PID file */
    pf = fopen(pid_path, "w");
    if (pf) {
        fprintf(pf, "%d\n", pid);
        fclose(pf);
    }
    
    char *r = malloc(128);
    snprintf(r, 128, "Started PID=%d", pid);
    return r;
}

/* process_status — check process status */
static char *tool_process_status(const char *name) {
    if (!name || !*name) return strdup("Error: name required");
    
    char pid_path[1024], log_path[1024];
    snprintf(pid_path, sizeof(pid_path), ".agent/procs/%s.pid", name);
    snprintf(log_path, sizeof(log_path), ".agent/procs/%s.log", name);
    
    FILE *pf = fopen(pid_path, "r");
    if (!pf) return strdup("Error: process not found");
    
    int pid;
    if (fscanf(pf, "%d", &pid) != 1) {
        fclose(pf);
        return strdup("Error: invalid PID file");
    }
    fclose(pf);
    
    /* check if alive */
    int alive = (kill(pid, 0) == 0);
    
    Buf result = {0};
    char status[128];
    snprintf(status, sizeof(status), "PID=%d status=%s\n\nLog tail:\n", pid, alive ? "running" : "stopped");
    buf_append(&result, status, strlen(status));
    
    /* tail last 20 lines */
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "tail -20 '%s' 2>&1", log_path);
    FILE *p = popen(cmd, "r");
    if (p) {
        char line[1024];
        while (fgets(line, sizeof(line), p))
            buf_append(&result, line, strlen(line));
        pclose(p);
    }
    
    if (!result.data) result.data = strdup("(no data)");
    return result.data;
}

/* process_stop — stop background process */
static char *tool_process_stop(const char *name) {
    if (!name || !*name) return strdup("Error: name required");
    
    char pid_path[1024];
    snprintf(pid_path, sizeof(pid_path), ".agent/procs/%s.pid", name);
    
    FILE *pf = fopen(pid_path, "r");
    if (!pf) return strdup("Error: process not found");
    
    int pid;
    if (fscanf(pf, "%d", &pid) != 1) {
        fclose(pf);
        return strdup("Error: invalid PID file");
    }
    fclose(pf);
    
    kill(pid, SIGTERM);
    unlink(pid_path);
    
    char *r = malloc(128);
    snprintf(r, 128, "Stopped PID=%d", pid);
    return r;
}

/* clipboard_get — read macOS clipboard */
static char *tool_clipboard_get(void) {
    FILE *p = popen("pbpaste 2>&1", "r");
    if (!p) return strdup("Error: pbpaste failed");
    
    char *out = read_stream_to_string(p);
    pclose(p);
    
    if (!out || !*out) {
        free(out);
        return strdup("(clipboard empty)");
    }
    return out;
}

/* clipboard_set — write macOS clipboard */
static char *tool_clipboard_set(const char *text) {
    if (!text) text = "";
    
    FILE *p = popen("pbcopy", "w");
    if (!p) return strdup("Error: pbcopy failed");
    
    size_t len = strlen(text);
    fwrite(text, 1, len, p);
    int rc = pclose(p);
    
    if (rc != 0) return strdup("Error: pbcopy failed");
    
    char *r = malloc(128);
    snprintf(r, 128, "Copied %zu bytes", len);
    return r;
}

/* preview_edit — show diff without modifying file */
static char *tool_preview_edit(const char *path, const char *old_str, const char *new_str) {
    if (!path_is_safe(path)) {
        char *r = malloc(256);
        snprintf(r, 256, "Error: path '%s' not in sandboxed workspace", path ? path : "(null)");
        return r;
    }
    if (!old_str || !*old_str) return strdup("Error: old_string is empty");
    if (!new_str) new_str = "";
    
    FILE *f = fopen(path, "rb");
    if (!f) {
        char *r = malloc(512);
        snprintf(r, 512, "Error: cannot open %s (%s)", path, strerror(errno));
        return r;
    }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0) sz = 0;
    char *buf = malloc(sz + 1);
    size_t got = fread(buf, 1, sz, f);
    buf[got] = 0; fclose(f);
    
    /* perform edit in memory */
    size_t olen = strlen(old_str), nlen = strlen(new_str);
    char *p = strstr(buf, old_str);
    if (!p) {
        free(buf);
        return strdup("Error: old_string not found");
    }
    
    size_t out_cap = got + (nlen > olen ? (nlen - olen) : 0) + 16;
    char *out = malloc(out_cap + 1);
    size_t oi = 0;
    char *src = buf;
    
    /* single replacement */
    while (*src) {
        if (src == p) {
            memcpy(out + oi, new_str, nlen);
            oi += nlen;
            src += olen;
        } else {
            out[oi++] = *src++;
        }
    }
    out[oi] = 0;
    
    /* write to temp file */
    char tmpfile[256];
    snprintf(tmpfile, sizeof(tmpfile), "/tmp/preview_edit_%d.tmp", getpid());
    FILE *tf = fopen(tmpfile, "w");
    if (!tf) {
        free(buf); free(out);
        return strdup("Error: cannot create temp file");
    }
    fwrite(out, 1, oi, tf);
    fclose(tf);
    free(out);
    free(buf);
    
    /* run diff */
    Buf cmd = {0};
    buf_append(&cmd, "diff -u ", 8);
    buf_append_sq(&cmd, path);
    buf_append(&cmd, " ", 1);
    buf_append_sq(&cmd, tmpfile);
    buf_append(&cmd, " 2>&1", 5);
    
    FILE *dp = popen(cmd.data, "r");
    buf_free(&cmd);
    if (!dp) {
        unlink(tmpfile);
        return strdup("Error: diff failed");
    }
    
    char *diff_out = read_stream_to_string(dp);
    pclose(dp);
    unlink(tmpfile);
    
    if (!diff_out || !*diff_out) {
        free(diff_out);
        return strdup("(no changes)");
    }
    return diff_out;
}

/* run_tests — auto-detect and run test framework */
static char *tool_run_tests(const char *path) {
    const char *test_path = (path && *path) ? path : ".";
    if (!path_is_safe(test_path)) {
        char *r = malloc(256);
        snprintf(r, 256, "Error: path '%s' not in workspace", test_path);
        return r;
    }
    
    /* auto-detect test framework */
    char cmd[2048] = "";
    struct stat st;
    
    /* Rust: Cargo.toml */
    if (stat("Cargo.toml", &st) == 0) {
        snprintf(cmd, sizeof(cmd), "cd '%s' && cargo test 2>&1", test_path);
    }
    /* Node.js: package.json + jest */
    else if (stat("package.json", &st) == 0) {
        FILE *pj = fopen("package.json", "r");
        if (pj) {
            char buf[4096];
            size_t n = fread(buf, 1, sizeof(buf) - 1, pj);
            buf[n] = 0;
            fclose(pj);
            if (strstr(buf, "\"jest\"") || strstr(buf, "\"test\""))
                snprintf(cmd, sizeof(cmd), "cd '%s' && npm test 2>&1", test_path);
        }
    }
    /* Python: pytest or setup.py */
    else if (stat("pytest.ini", &st) == 0 || stat("setup.py", &st) == 0 || stat("pyproject.toml", &st) == 0) {
        snprintf(cmd, sizeof(cmd), "cd '%s' && python -m pytest 2>&1", test_path);
    }
    /* Makefile with test target */
    else if (stat("Makefile", &st) == 0) {
        FILE *mf = fopen("Makefile", "r");
        if (mf) {
            char line[256];
            int has_test = 0;
            while (fgets(line, sizeof(line), mf)) {
                if (strncmp(line, "test:", 5) == 0) {
                    has_test = 1;
                    break;
                }
            }
            fclose(mf);
            if (has_test)
                snprintf(cmd, sizeof(cmd), "cd '%s' && make test 2>&1", test_path);
        }
    }
    
    if (!cmd[0]) return strdup("Error: no test framework detected (Cargo.toml, package.json+jest, pytest, or Makefile with test target)");
    
    /* run with timeout */
    char timeout_cmd[2200];
    snprintf(timeout_cmd, sizeof(timeout_cmd), "timeout 120 bash -c %s 2>&1 || echo '[exit='$?']'", cmd);
    
    FILE *p = popen(timeout_cmd, "r");
    if (!p) return strdup("Error: popen failed");
    
    char *out = read_stream_to_string(p);
    pclose(p);
    
    if (!out || !*out) {
        free(out);
        return strdup("(no test output)");
    }
    return out;
}

/* ---------- dynamic tools ---------- */
static void load_dynamic_tools(void) {
    const char *dir = ".agent/tools";
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && g_n_dyn_tools < MAX_DYN_TOOLS) {
        size_t el = strlen(e->d_name);
        if (el < 4 || strcmp(e->d_name + el - 3, ".sh") != 0) continue;
        DynTool *t = &g_dyn_tools[g_n_dyn_tools];
        snprintf(t->script_path, sizeof(t->script_path), "%s/%s", dir, e->d_name);
        snprintf(t->name, sizeof(t->name), "%.*s", (int)(el - 3), e->d_name);
        t->description[0] = 0;
        t->args = cJSON_CreateArray();
        FILE *f = fopen(t->script_path, "r");
        if (!f) continue;
        char line[1024];
        for (int i = 0; i < 40 && fgets(line, sizeof(line), f); i++) {
            size_t ll = strlen(line);
            while (ll > 0 && (line[ll-1] == '\n' || line[ll-1] == '\r')) line[--ll] = 0;
            const char *p = line;
            if (strncmp(p, "#@name:", 7) == 0) { p += 7; while (*p == ' ') p++; snprintf(t->name, sizeof(t->name), "%s", p); }
            else if (strncmp(p, "#@description:", 14) == 0) { p += 14; while (*p == ' ') p++; snprintf(t->description, sizeof(t->description), "%s", p); }
            else if (strncmp(p, "#@arg:", 6) == 0) { p += 6; while (*p == ' ') p++; cJSON_AddItemToArray(t->args, cJSON_CreateString(p)); }
        }
        fclose(f);
        if (!t->description[0]) strcpy(t->description, "Dynamic shell tool");
        g_n_dyn_tools++;
    }
    closedir(d);
    if (g_n_dyn_tools > 0) logerr("[dyn] loaded %d dynamic tools\n", g_n_dyn_tools);
}

static char *exec_dyn_tool(DynTool *t, cJSON *input) {
    Buf cmd = {0};
    buf_append(&cmd, "/bin/bash ", 10);
    buf_append(&cmd, t->script_path, strlen(t->script_path));
    int n = cJSON_GetArraySize(t->args);
    for (int i = 0; i < n; i++) {
        const char *spec = cJSON_GetStringValue(cJSON_GetArrayItem(t->args, i));
        if (!spec) continue;
        const char *colon = strchr(spec, ':');
        char aname[96];
        size_t nl = colon ? (size_t)(colon - spec) : strlen(spec);
        if (nl >= sizeof(aname)) nl = sizeof(aname) - 1;
        memcpy(aname, spec, nl); aname[nl] = 0;
        cJSON *v = cJSON_GetObjectItem(input, aname);
        const char *s = v ? cJSON_GetStringValue(v) : "";
        if (!s) s = "";
        buf_append(&cmd, " ", 1);
        buf_append_sq(&cmd, s);
    }
    buf_append(&cmd, " 2>&1", 6);
    FILE *p = popen(cmd.data, "r");
    buf_free(&cmd);
    if (!p) return strdup("Error: popen failed");
    char *out = read_stream_to_string(p);
    pclose(p);
    redact_secrets(out);
    return out;
}

/* ---------- tool schema ---------- */
static cJSON *make_simple_tool(const char *name, const char *desc, const char **params, const char **pdesc, int n) {
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
        if (pdesc && pdesc[i]) cJSON_AddStringToObject(p, "description", pdesc[i]);
        cJSON_AddItemToObject(props, params[i], p);
        cJSON_AddItemToArray(req, cJSON_CreateString(params[i]));
    }
    cJSON_AddItemToObject(s, "properties", props);
    cJSON_AddItemToObject(s, "required", req);
    cJSON_AddItemToObject(t, "input_schema", s);
    return t;
}

static cJSON *build_tools_array(void) {
    cJSON *tools = cJSON_CreateArray();

    /* read_file */
    { const char *p[] = {"path"}, *pd[] = {"Relative path inside workspace"};
      cJSON_AddItemToArray(tools, make_simple_tool("read_file", "Read contents of a file.", p, pd, 1)); }

    /* write_file */
    { const char *p[] = {"path","content"}, *pd[] = {"Relative path","Full file content (overwrites)"};
      cJSON_AddItemToArray(tools, make_simple_tool("write_file", "Write content to a file (overwrites).", p, pd, 2)); }

    /* edit_file */
    { const char *p[] = {"path","old_string","new_string"};
      const char *pd[] = {"Relative path","Exact substring to replace (must be unique unless replace_all=true)","Replacement text"};
      cJSON *edit = make_simple_tool("edit_file",
          "Precise in-place edit: replace old_string with new_string. "
          "old_string must be unique. Pass replace_all=\"true\" for all occurrences.", p, pd, 3);
      cJSON *schema = cJSON_GetObjectItem(edit, "input_schema");
      cJSON *props  = cJSON_GetObjectItem(schema, "properties");
      cJSON *ra = cJSON_CreateObject();
      cJSON_AddStringToObject(ra, "type", "string");
      cJSON_AddStringToObject(ra, "description", "\"true\" to replace every occurrence; default \"false\"");
      cJSON_AddItemToObject(props, "replace_all", ra);
      cJSON_AddItemToArray(tools, edit); }

    /* bash */
    { const char *p[] = {"command"}, *pd[] = {"Bash command. Dangerous patterns blocked."};
      cJSON_AddItemToArray(tools, make_simple_tool("bash",
          "Run a bash command. Output captured, 60s CPU limit.", p, pd, 1)); }

    /* list_dir */
    { const char *p[] = {"path"}, *pd[] = {"Directory path"};
      cJSON_AddItemToArray(tools, make_simple_tool("list_dir", "List non-hidden entries in a directory.", p, pd, 1)); }

    /* grep_files (NEW) */
    { cJSON *t = cJSON_CreateObject();
      cJSON_AddStringToObject(t, "name", "grep_files");
      cJSON_AddStringToObject(t, "description",
          "Search files for a regex pattern (extended regex). "
          "Returns file:line:match lines. "
          "Faster and safer than bash grep for codebase search.");
      cJSON *s = cJSON_CreateObject(); cJSON_AddStringToObject(s, "type", "object");
      cJSON *props = cJSON_CreateObject(); cJSON *req = cJSON_CreateArray();
      { cJSON *p = cJSON_CreateObject(); cJSON_AddStringToObject(p, "type", "string");
        cJSON_AddStringToObject(p, "description", "Extended regex pattern to search for");
        cJSON_AddItemToObject(props, "pattern", p); cJSON_AddItemToArray(req, cJSON_CreateString("pattern")); }
      { cJSON *p = cJSON_CreateObject(); cJSON_AddStringToObject(p, "type", "string");
        cJSON_AddStringToObject(p, "description", "Directory to search (default: .)");
        cJSON_AddItemToObject(props, "path", p); }
      { cJSON *p = cJSON_CreateObject(); cJSON_AddStringToObject(p, "type", "string");
        cJSON_AddStringToObject(p, "description", "File glob filter e.g. '*.c', '*.{js,ts}'");
        cJSON_AddItemToObject(props, "include_glob", p); }
      cJSON_AddItemToObject(s, "properties", props); cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToObject(t, "input_schema", s); cJSON_AddItemToArray(tools, t); }

    /* glob_files (NEW) */
    { cJSON *t = cJSON_CreateObject();
      cJSON_AddStringToObject(t, "name", "glob_files");
      cJSON_AddStringToObject(t, "description",
          "Find files matching a glob pattern (e.g. '*.c', 'test_*.py'). "
          "Skips .git and node_modules.");
      cJSON *s = cJSON_CreateObject(); cJSON_AddStringToObject(s, "type", "object");
      cJSON *props = cJSON_CreateObject(); cJSON *req = cJSON_CreateArray();
      { cJSON *p = cJSON_CreateObject(); cJSON_AddStringToObject(p, "type", "string");
        cJSON_AddStringToObject(p, "description", "Glob pattern e.g. '*.c'");
        cJSON_AddItemToObject(props, "pattern", p); cJSON_AddItemToArray(req, cJSON_CreateString("pattern")); }
      { cJSON *p = cJSON_CreateObject(); cJSON_AddStringToObject(p, "type", "string");
        cJSON_AddStringToObject(p, "description", "Root directory (default: .)");
        cJSON_AddItemToObject(props, "path", p); }
      cJSON_AddItemToObject(s, "properties", props); cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToObject(t, "input_schema", s); cJSON_AddItemToArray(tools, t); }

    /* http_get (NEW) */
    { const char *p[] = {"url"}, *pd[] = {"Full URL (http:// or https://)"};
      cJSON_AddItemToArray(tools, make_simple_tool("http_get",
          "Fetch a URL and return the response body (max 32KB). "
          "Requires --allow-http flag at startup.", p, pd, 1)); }

    /* todo (NEW) */
    { cJSON *t = cJSON_CreateObject();
      cJSON_AddStringToObject(t, "name", "todo");
      cJSON_AddStringToObject(t, "description",
          "Manage a task list in .agent/todo.md. "
          "op: 'add' (item required), 'list', 'done' (item required), 'clear' (removes [x] items).");
      cJSON *s = cJSON_CreateObject(); cJSON_AddStringToObject(s, "type", "object");
      cJSON *props = cJSON_CreateObject(); cJSON *req = cJSON_CreateArray();
      { cJSON *p = cJSON_CreateObject(); cJSON_AddStringToObject(p, "type", "string");
        cJSON_AddStringToObject(p, "description", "add | list | done | clear");
        cJSON_AddItemToObject(props, "op", p); cJSON_AddItemToArray(req, cJSON_CreateString("op")); }
      { cJSON *p = cJSON_CreateObject(); cJSON_AddStringToObject(p, "type", "string");
        cJSON_AddStringToObject(p, "description", "Item text (for add/done)");
        cJSON_AddItemToObject(props, "item", p); }
      cJSON_AddItemToObject(s, "properties", props); cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToObject(t, "input_schema", s); cJSON_AddItemToArray(tools, t); }

    /* ask_user (NEW v9) */
    { cJSON *t = cJSON_CreateObject();
      cJSON_AddStringToObject(t, "name", "ask_user");
      cJSON_AddStringToObject(t, "description",
          "Opens /dev/tty to prompt the user mid-task. Returns user input or default.");
      cJSON *s = cJSON_CreateObject(); cJSON_AddStringToObject(s, "type", "object");
      cJSON *props = cJSON_CreateObject(); cJSON *req = cJSON_CreateArray();
      { cJSON *p = cJSON_CreateObject(); cJSON_AddStringToObject(p, "type", "string");
        cJSON_AddStringToObject(p, "description", "Question to ask the user");
        cJSON_AddItemToObject(props, "question", p); cJSON_AddItemToArray(req, cJSON_CreateString("question")); }
      { cJSON *p = cJSON_CreateObject(); cJSON_AddStringToObject(p, "type", "string");
        cJSON_AddStringToObject(p, "description", "Default value if user presses Enter");
        cJSON_AddItemToObject(props, "default", p); }
      cJSON_AddItemToObject(s, "properties", props); cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToObject(t, "input_schema", s); cJSON_AddItemToArray(tools, t); }

    /* diff_files (NEW v9) */
    { const char *p[] = {"path_a","path_b"}, *pd[] = {"First file path","Second file path"};
      cJSON_AddItemToArray(tools, make_simple_tool("diff_files",
          "Run 'diff -u path_a path_b'. Both paths must be in workspace.", p, pd, 2)); }

    /* git (NEW v9) */
    { cJSON *t = cJSON_CreateObject();
      cJSON_AddStringToObject(t, "name", "git");
      cJSON_AddStringToObject(t, "description",
          "Safe git operations. Allowed: status, log, diff, add, commit, branch, checkout, show, stash, reset (soft/mixed only). "
          "Blocks --force, rm -rf, reset --hard to non-HEAD.");
      cJSON *s = cJSON_CreateObject(); cJSON_AddStringToObject(s, "type", "object");
      cJSON *props = cJSON_CreateObject(); cJSON *req = cJSON_CreateArray();
      { cJSON *p = cJSON_CreateObject(); cJSON_AddStringToObject(p, "type", "string");
        cJSON_AddStringToObject(p, "description", "Git subcommand");
        cJSON_AddItemToObject(props, "subcommand", p); cJSON_AddItemToArray(req, cJSON_CreateString("subcommand")); }
      { cJSON *p = cJSON_CreateObject(); cJSON_AddStringToObject(p, "type", "string");
        cJSON_AddStringToObject(p, "description", "Arguments to pass to git subcommand");
        cJSON_AddItemToObject(props, "args", p); }
      cJSON_AddItemToObject(s, "properties", props); cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToObject(t, "input_schema", s); cJSON_AddItemToArray(tools, t); }

    /* notify (NEW v9) */
    { const char *p[] = {"title","message"}, *pd[] = {"Notification title","Notification message"};
      cJSON_AddItemToArray(tools, make_simple_tool("notify",
          "macOS notification via osascript.", p, pd, 2)); }

    /* save_memory */
    { const char *p[] = {"key","value"}, *pd[] = {"Short title","Fact to remember"};
      cJSON_AddItemToArray(tools, make_simple_tool("save_memory",
          "Persist a fact to long-term memory (~/.mini-agent/memory.md).", p, pd, 2)); }

    /* recall_memory */
    { cJSON *rm = cJSON_CreateObject();
      cJSON_AddStringToObject(rm, "name", "recall_memory");
      cJSON_AddStringToObject(rm, "description", "Read the full persistent memory file.");
      cJSON *rms = cJSON_CreateObject(); cJSON_AddStringToObject(rms, "type", "object");
      cJSON_AddItemToObject(rms, "properties", cJSON_CreateObject());
      cJSON_AddItemToObject(rms, "required", cJSON_CreateArray());
      cJSON_AddItemToObject(rm, "input_schema", rms);
      cJSON_AddItemToArray(tools, rm); }

    /* spawn_agent */
    { const char *p[] = {"task"}, *pd[] = {"Self-contained task for a sub-agent"};
      cJSON_AddItemToArray(tools, make_simple_tool("spawn_agent",
          "Spawn an isolated sub-agent for a subtask (depth-limited).", p, pd, 1)); }

    /* === NEW v10 TOOLS === */

    /* http_request */
    { cJSON *t = cJSON_CreateObject();
      cJSON_AddStringToObject(t, "name", "http_request");
      cJSON_AddStringToObject(t, "description",
          "General HTTP tool. Supports GET/POST/PUT/DELETE/PATCH. "
          "Send body for non-GET requests. Parse comma-separated headers 'Key: Value'. "
          "Requires --allow-http flag. http_get is kept as alias for GET requests.");
      cJSON *s = cJSON_CreateObject(); cJSON_AddStringToObject(s, "type", "object");
      cJSON *props = cJSON_CreateObject(); cJSON *req = cJSON_CreateArray();
      { cJSON *p = cJSON_CreateObject(); cJSON_AddStringToObject(p, "type", "string");
        cJSON_AddStringToObject(p, "description", "HTTP method: GET, POST, PUT, DELETE, PATCH");
        cJSON_AddItemToObject(props, "method", p); cJSON_AddItemToArray(req, cJSON_CreateString("method")); }
      { cJSON *p = cJSON_CreateObject(); cJSON_AddStringToObject(p, "type", "string");
        cJSON_AddStringToObject(p, "description", "Full URL (http:// or https://)");
        cJSON_AddItemToObject(props, "url", p); cJSON_AddItemToArray(req, cJSON_CreateString("url")); }
      { cJSON *p = cJSON_CreateObject(); cJSON_AddStringToObject(p, "type", "string");
        cJSON_AddStringToObject(p, "description", "Request body (for POST/PUT/DELETE/PATCH)");
        cJSON_AddItemToObject(props, "body", p); }
      { cJSON *p = cJSON_CreateObject(); cJSON_AddStringToObject(p, "type", "string");
        cJSON_AddStringToObject(p, "description", "Comma-separated headers 'Key: Value, Key2: Value2'");
        cJSON_AddItemToObject(props, "headers", p); }
      cJSON_AddItemToObject(s, "properties", props); cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToObject(t, "input_schema", s); cJSON_AddItemToArray(tools, t); }

    /* checkpoint */
    { cJSON *t = cJSON_CreateObject();
      cJSON_AddStringToObject(t, "name", "checkpoint");
      cJSON_AddStringToObject(t, "description",
          "Snapshot all modified files to ~/.mini-agent/checkpoints/<timestamp>/. "
          "Writes manifest.json listing files. Returns 'Checkpoint <id> saved'.");
      cJSON *s = cJSON_CreateObject(); cJSON_AddStringToObject(s, "type", "object");
      cJSON_AddItemToObject(s, "properties", cJSON_CreateObject());
      cJSON_AddItemToObject(s, "required", cJSON_CreateArray());
      cJSON_AddItemToObject(t, "input_schema", s); cJSON_AddItemToArray(tools, t); }

    /* undo */
    { cJSON *t = cJSON_CreateObject();
      cJSON_AddStringToObject(t, "name", "undo");
      cJSON_AddStringToObject(t, "description",
          "Restore files from the latest checkpoint. "
          "Reads manifest.json, restores each file. Returns list of restored files.");
      cJSON *s = cJSON_CreateObject(); cJSON_AddStringToObject(s, "type", "object");
      cJSON_AddItemToObject(s, "properties", cJSON_CreateObject());
      cJSON_AddItemToObject(s, "required", cJSON_CreateArray());
      cJSON_AddItemToObject(t, "input_schema", s); cJSON_AddItemToArray(tools, t); }

    /* process_start */
    { const char *p[] = {"name","command"};
      const char *pd[] = {"Process name identifier","Shell command to run in background"};
      cJSON_AddItemToArray(tools, make_simple_tool("process_start",
          "Fork+exec command in background. Writes PID to .agent/procs/<name>.pid "
          "and log to .agent/procs/<name>.log. Returns 'Started PID=X'.", p, pd, 2)); }

    /* process_status */
    { const char *p[] = {"name"}, *pd[] = {"Process name identifier"};
      cJSON_AddItemToArray(tools, make_simple_tool("process_status",
          "Check if process is alive (kill -0), tail last 20 lines of log. "
          "Returns status + log tail.", p, pd, 1)); }

    /* process_stop */
    { const char *p[] = {"name"}, *pd[] = {"Process name identifier"};
      cJSON_AddItemToArray(tools, make_simple_tool("process_stop",
          "Send SIGTERM to background process. Returns 'Stopped PID=X'.", p, pd, 1)); }

    /* clipboard_get */
    { cJSON *t = cJSON_CreateObject();
      cJSON_AddStringToObject(t, "name", "clipboard_get");
      cJSON_AddStringToObject(t, "description",
          "Read macOS clipboard contents via 'pbpaste 2>&1'. Returns clipboard text.");
      cJSON *s = cJSON_CreateObject(); cJSON_AddStringToObject(s, "type", "object");
      cJSON_AddItemToObject(s, "properties", cJSON_CreateObject());
      cJSON_AddItemToObject(s, "required", cJSON_CreateArray());
      cJSON_AddItemToObject(t, "input_schema", s); cJSON_AddItemToArray(tools, t); }

    /* clipboard_set */
    { const char *p[] = {"text"}, *pd[] = {"Text to copy to clipboard"};
      cJSON_AddItemToArray(tools, make_simple_tool("clipboard_set",
          "Write text to macOS clipboard via 'pbcopy'. Returns 'Copied N bytes'.", p, pd, 1)); }

    /* preview_edit */
    { const char *p[] = {"path","old_string","new_string"};
      const char *pd[] = {"File path","Exact substring to replace","Replacement text"};
      cJSON_AddItemToArray(tools, make_simple_tool("preview_edit",
          "Apply edit in memory, write to temp file, run 'diff -u <original> <tempfile>', "
          "return diff WITHOUT modifying file. Good for review before committing changes.", p, pd, 3)); }

    /* run_tests */
    { cJSON *t = cJSON_CreateObject();
      cJSON_AddStringToObject(t, "name", "run_tests");
      cJSON_AddStringToObject(t, "description",
          "Auto-detect test framework and run tests. "
          "Checks for: Cargo.toml (cargo test), package.json+jest (npm test), "
          "pytest/setup.py (python -m pytest), Makefile with 'test' target (make test). "
          "Returns test output. Timeout 120s.");
      cJSON *s = cJSON_CreateObject(); cJSON_AddStringToObject(s, "type", "object");
      cJSON *props = cJSON_CreateObject();
      { cJSON *p = cJSON_CreateObject(); cJSON_AddStringToObject(p, "type", "string");
        cJSON_AddStringToObject(p, "description", "Test directory (default: '.')");
        cJSON_AddItemToObject(props, "path", p); }
      cJSON_AddItemToObject(s, "properties", props);
      cJSON_AddItemToObject(s, "required", cJSON_CreateArray());
      cJSON_AddItemToObject(t, "input_schema", s); cJSON_AddItemToArray(tools, t); }

    /* dynamic tools */
    for (int i = 0; i < g_n_dyn_tools; i++) {
        DynTool *dt = &g_dyn_tools[i];
        cJSON *tj = cJSON_CreateObject();
        cJSON_AddStringToObject(tj, "name", dt->name);
        char dd[600]; snprintf(dd, sizeof(dd), "[dynamic] %s", dt->description);
        cJSON_AddStringToObject(tj, "description", dd);
        cJSON *sc = cJSON_CreateObject(); cJSON_AddStringToObject(sc, "type", "object");
        cJSON *props = cJSON_CreateObject(); cJSON *req = cJSON_CreateArray();
        int na = cJSON_GetArraySize(dt->args);
        for (int j = 0; j < na; j++) {
            const char *spec = cJSON_GetStringValue(cJSON_GetArrayItem(dt->args, j));
            if (!spec) continue;
            const char *colon = strchr(spec, ':');
            char aname[96], adesc[256] = "";
            size_t nl = colon ? (size_t)(colon - spec) : strlen(spec);
            if (nl >= sizeof(aname)) nl = sizeof(aname) - 1;
            memcpy(aname, spec, nl); aname[nl] = 0;
            if (colon && colon[1]) snprintf(adesc, sizeof(adesc), "%s", colon + 1);
            cJSON *ap = cJSON_CreateObject(); cJSON_AddStringToObject(ap, "type", "string");
            if (*adesc) cJSON_AddStringToObject(ap, "description", adesc);
            cJSON_AddItemToObject(props, aname, ap);
            cJSON_AddItemToArray(req, cJSON_CreateString(aname));
        }
        cJSON_AddItemToObject(sc, "properties", props); cJSON_AddItemToObject(sc, "required", req);
        cJSON_AddItemToObject(tj, "input_schema", sc);
        cJSON_AddItemToArray(tools, tj);
    }

    /* cache_control on last tool → caches all tools */
    int total = cJSON_GetArraySize(tools);
    if (total > 0) {
        cJSON *last = cJSON_GetArrayItem(tools, total - 1);
        cJSON *cc = cJSON_CreateObject();
        cJSON_AddStringToObject(cc, "type", "ephemeral");
        cJSON_AddItemToObject(last, "cache_control", cc);
    }
    return tools;
}

/* ---------- approve prompt ---------- */
/* Returns 1=execute, 0=skip, -1=abort */
static int approve_tool(const char *name, cJSON *input) {
    char *inp_str = cJSON_PrintUnformatted(input);
    fprintf(stderr, "\n\033[33m[approve]\033[0m \033[1m%s\033[0m %s\n",
            name, inp_str ? inp_str : "{}");
    free(inp_str);
    fprintf(stderr, "  [y]es  [n]o/skip  [a]bort  [!]yes-to-all ? ");
    fflush(stderr);
    char buf[64];
    if (!fgets(buf, sizeof(buf), stdin)) return -1;
    char c = buf[0];
    if (c == 'a' || c == 'A') return -1;
    if (c == '!') { g_approve = 0; g_approve_bash = 0; return 1; } /* disable for rest */
    if (c == 'n' || c == 'N' || c == 's' || c == 'S') return 0;
    return 1; /* y or Enter */
}

/* ---------- tool dispatch ---------- */
static char *execute_tool(const char *name, cJSON *input) {
    if (g_plan_mode) {
        char *inp_str = cJSON_PrintUnformatted(input);
        char *r = malloc(512 + (inp_str ? strlen(inp_str) : 0));
        snprintf(r, 512 + (inp_str ? strlen(inp_str) : 0),
                 "[PLAN MODE] would call %s with %s", name, inp_str ? inp_str : "{}");
        free(inp_str);
        return r;
    }

    /* --approve / --approve-bash gate */
    int need_approve = g_approve ||
        (g_approve_bash && strcmp(name, "bash") == 0);
    if (need_approve) {
        int ok = approve_tool(name, input);
        if (ok == -1) {
            logerr("[abort] user aborted agent\n");
            exit(0);
        }
        if (ok == 0) {
            char *r = strdup("[skipped by user]");
            return r;
        }
    }

    cJSON *path    = cJSON_GetObjectItem(input, "path");
    cJSON *content = cJSON_GetObjectItem(input, "content");
    cJSON *command = cJSON_GetObjectItem(input, "command");
    cJSON *key     = cJSON_GetObjectItem(input, "key");
    cJSON *value   = cJSON_GetObjectItem(input, "value");
    cJSON *task    = cJSON_GetObjectItem(input, "task");
    cJSON *old_s   = cJSON_GetObjectItem(input, "old_string");
    cJSON *new_s   = cJSON_GetObjectItem(input, "new_string");
    cJSON *ra      = cJSON_GetObjectItem(input, "replace_all");
    cJSON *pattern = cJSON_GetObjectItem(input, "pattern");
    cJSON *incglob = cJSON_GetObjectItem(input, "include_glob");
    cJSON *url       = cJSON_GetObjectItem(input, "url");
    cJSON *op        = cJSON_GetObjectItem(input, "op");
    cJSON *item      = cJSON_GetObjectItem(input, "item");
    cJSON *question  = cJSON_GetObjectItem(input, "question");
    cJSON *def_val   = cJSON_GetObjectItem(input, "default");
    cJSON *path_a    = cJSON_GetObjectItem(input, "path_a");
    cJSON *path_b    = cJSON_GetObjectItem(input, "path_b");
    cJSON *subcmd    = cJSON_GetObjectItem(input, "subcommand");
    cJSON *args      = cJSON_GetObjectItem(input, "args");
    cJSON *title     = cJSON_GetObjectItem(input, "title");
    cJSON *message   = cJSON_GetObjectItem(input, "message");
    // v10 parameters
    cJSON *label     = cJSON_GetObjectItem(input, "label");
    cJSON *method    = cJSON_GetObjectItem(input, "method");
    cJSON *headers   = cJSON_GetObjectItem(input, "headers");
    cJSON *body      = cJSON_GetObjectItem(input, "body");
    cJSON *proc_name = cJSON_GetObjectItem(input, "name");
    cJSON *text      = cJSON_GetObjectItem(input, "text");
    cJSON *framework = cJSON_GetObjectItem(input, "framework");

    if (strcmp(name, "read_file") == 0 && path)
        return tool_read_file(cJSON_GetStringValue(path));
    if (strcmp(name, "write_file") == 0 && path)
        return tool_write_file(cJSON_GetStringValue(path),
                               content ? cJSON_GetStringValue(content) : "");
    if (strcmp(name, "edit_file") == 0 && path && old_s && new_s) {
        const char *ra_s = ra ? cJSON_GetStringValue(ra) : NULL;
        int ra_bool = ra_s && (strcmp(ra_s, "true") == 0 || strcmp(ra_s, "1") == 0);
        return tool_edit_file(cJSON_GetStringValue(path),
                              cJSON_GetStringValue(old_s),
                              cJSON_GetStringValue(new_s), ra_bool);
    }
    if (strcmp(name, "bash") == 0 && command)
        return tool_bash(cJSON_GetStringValue(command));
    if (strcmp(name, "list_dir") == 0 && path)
        return tool_list_dir(cJSON_GetStringValue(path));
    if (strcmp(name, "grep_files") == 0 && pattern)
        return tool_grep_files(cJSON_GetStringValue(pattern),
                               path ? cJSON_GetStringValue(path) : ".",
                               incglob ? cJSON_GetStringValue(incglob) : NULL);
    if (strcmp(name, "glob_files") == 0 && pattern)
        return tool_glob_files(cJSON_GetStringValue(pattern),
                               path ? cJSON_GetStringValue(path) : ".");
    if (strcmp(name, "http_get") == 0 && url)
        return tool_http_get(cJSON_GetStringValue(url));
    if (strcmp(name, "todo") == 0 && op)
        return tool_todo(cJSON_GetStringValue(op),
                         item ? cJSON_GetStringValue(item) : NULL);
    if (strcmp(name, "save_memory") == 0 && key && value)
        return tool_save_memory(cJSON_GetStringValue(key), cJSON_GetStringValue(value));
    if (strcmp(name, "recall_memory") == 0)
        return tool_recall_memory();
    if (strcmp(name, "spawn_agent") == 0 && task)
        return tool_spawn_agent(cJSON_GetStringValue(task), NULL);
    if (strcmp(name, "ask_user") == 0 && question)
        return tool_ask_user(cJSON_GetStringValue(question),
                             def_val ? cJSON_GetStringValue(def_val) : NULL);
    if (strcmp(name, "diff_files") == 0 && path_a && path_b)
        return tool_diff_files(cJSON_GetStringValue(path_a), cJSON_GetStringValue(path_b));
    if (strcmp(name, "git") == 0 && subcmd)
        return tool_git(cJSON_GetStringValue(subcmd),
                        args ? cJSON_GetStringValue(args) : NULL);
    if (strcmp(name, "notify") == 0 && (title || message))
        return tool_notify(title ? cJSON_GetStringValue(title) : "mini-agent",
                           message ? cJSON_GetStringValue(message) : "");

    // v10 tools
    if (strcmp(name, "checkpoint") == 0)
        return tool_checkpoint();
    if (strcmp(name, "undo") == 0)
        return tool_undo();
    if (strcmp(name, "http_request") == 0 && url)
        return tool_http_request(method ? cJSON_GetStringValue(method) : "GET",
                                 cJSON_GetStringValue(url),
                                 body ? cJSON_GetStringValue(body) : NULL,
                                 headers ? cJSON_GetStringValue(headers) : NULL);
    if (strcmp(name, "process_start") == 0 && proc_name && command)
        return tool_process_start(cJSON_GetStringValue(proc_name),
                                  cJSON_GetStringValue(command));
    if (strcmp(name, "process_stop") == 0 && proc_name)
        return tool_process_stop(cJSON_GetStringValue(proc_name));
    if (strcmp(name, "process_status") == 0)
        return tool_process_status(proc_name ? cJSON_GetStringValue(proc_name) : NULL);
    if (strcmp(name, "clipboard_get") == 0)
        return tool_clipboard_get();
    if (strcmp(name, "clipboard_set") == 0 && text)
        return tool_clipboard_set(cJSON_GetStringValue(text));
    if (strcmp(name, "preview_edit") == 0 && path && old_s && new_s)
        return tool_preview_edit(cJSON_GetStringValue(path),
                                 cJSON_GetStringValue(old_s),
                                 cJSON_GetStringValue(new_s));
    if (strcmp(name, "run_tests") == 0)
        return tool_run_tests(path ? cJSON_GetStringValue(path) : ".");

    for (int i = 0; i < g_n_dyn_tools; i++)
        if (strcmp(g_dyn_tools[i].name, name) == 0)
            return exec_dyn_tool(&g_dyn_tools[i], input);

    char *r = malloc(256);
    snprintf(r, 256, "Error: unknown tool '%s'", name);
    return r;
}

/* ---------- memory ---------- */
static char *load_persistent_memory(void) {
    const char *home = getenv("HOME");
    if (!home) home = ".";
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/.mini-agent", home);
    mkdir(dir, 0755);
    snprintf(g_memory_path, sizeof(g_memory_path), "%s/memory.md", dir);
    FILE *f = fopen(g_memory_path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    if (sz > 16 * 1024) sz = 16 * 1024;
    char *buf = malloc(sz + 1);
    fread(buf, 1, sz, f); buf[sz] = 0; fclose(f);
    return buf;
}

/* ---------- compaction ---------- */
static cJSON *compact_messages(const char *api_key, cJSON *messages) {
    int n = cJSON_GetArraySize(messages);
    if (n < COMPACT_AFTER_MESSAGES) return messages;
    int start = n - COMPACT_KEEP_LAST;
    while (start < n) {
        cJSON *m = cJSON_GetArrayItem(messages, start);
        const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(m, "role"));
        if (role && strcmp(role, "assistant") == 0) break;
        start++;
    }
    if (start >= n || start < 4) return messages;
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "model", MODEL_COMPACT);
    cJSON_AddNumberToObject(body, "max_tokens", 1500);
    cJSON_AddStringToObject(body, "system",
        "You compress conversation history for an AI agent. "
        "Extract: (1) key facts, (2) decisions made, (3) files created/modified, "
        "(4) pending work, (5) errors to avoid. Be dense. <=400 words.");
    cJSON *sub = cJSON_CreateArray();
    for (int i = 0; i < start; i++)
        cJSON_AddItemToArray(sub, cJSON_Duplicate(cJSON_GetArrayItem(messages, i), 1));
    cJSON *ask = cJSON_CreateObject();
    cJSON_AddStringToObject(ask, "role", "user");
    cJSON_AddStringToObject(ask, "content", "Summarize the above for context compaction.");
    cJSON_AddItemToArray(sub, ask);
    cJSON_AddItemToObject(body, "messages", sub);
    char *bs = cJSON_PrintUnformatted(body); cJSON_Delete(body);
    logerr("[compact] summarizing %d messages...\n", start);
    char *resp = claude_api(api_key, bs); free(bs);
    if (!resp) return messages;
    cJSON *rj = cJSON_Parse(resp); free(resp);
    if (!rj) return messages;
    cJSON *content_arr = cJSON_GetObjectItem(rj, "content");
    char *summary = NULL;
    cJSON *block;
    cJSON_ArrayForEach(block, content_arr) {
        const char *ty = cJSON_GetStringValue(cJSON_GetObjectItem(block, "type"));
        if (ty && strcmp(ty, "text") == 0) {
            const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(block, "text"));
            if (t) summary = strdup(t);
            break;
        }
    }
    cJSON_Delete(rj);
    if (!summary) return messages;
    cJSON *nm = cJSON_CreateArray();
    char *wrapped = malloc(strlen(summary) + 64);
    sprintf(wrapped, "[COMPACTED CONTEXT]\n%s", summary);
    cJSON *sm = cJSON_CreateObject();
    cJSON_AddStringToObject(sm, "role", "user");
    cJSON_AddStringToObject(sm, "content", wrapped);
    cJSON_AddItemToArray(nm, sm);
    free(wrapped); free(summary);
    for (int i = start; i < n; i++)
        cJSON_AddItemToArray(nm, cJSON_Duplicate(cJSON_GetArrayItem(messages, i), 1));
    cJSON_Delete(messages);
    logerr("[compact] %d -> %d messages\n", n, cJSON_GetArraySize(nm));
    return nm;
}

/* ---------- power / battery info (macOS) ---------- */
/*
 * Returns a malloc'd string like:
 *   "Battery: 78% (discharging, ~2h30m remaining). "
 *   "Battery: 100% (charging). "
 *   "Battery: AC power (no battery). "
 * Returns NULL if pmset not available.
 */
static char *get_power_info(void) {
#ifndef __APPLE__
    return NULL;
#else
    FILE *fp = popen("pmset -g batt 2>/dev/null", "r");
    if (!fp) return NULL;

    char line[256];
    char result[512] = {0};
    int found = 0;

    while (fgets(line, sizeof(line), fp)) {
        /* Look for lines like:
         *  -InternalBattery-0 (id=...)  78%; discharging; 2:30 remaining present: true
         *  -InternalBattery-0 (id=...)  100%; charging; (no estimate) present: true
         *  Now drawing from 'AC Power'   (no battery line)
         */
        if (strstr(line, "InternalBattery") || strstr(line, "Battery")) {
            int pct = -1;
            char status[64] = "unknown";
            char remain[64] = "";
            /* parse percentage */
            char *pct_pos = strstr(line, "%");
            if (pct_pos) {
                char *p = pct_pos - 1;
                while (p > line && (*p == ' ' || (*p >= '0' && *p <= '9'))) p--;
                pct = atoi(p + 1);
            }
            /* parse status keyword */
            if (strstr(line, "discharging")) strcpy(status, "discharging");
            else if (strstr(line, "charging"))   strcpy(status, "charging");
            else if (strstr(line, "finishing"))  strcpy(status, "finishing charge");
            else if (strstr(line, "AC"))         strcpy(status, "AC");

            /* parse remaining time h:mm */
            char *semi = pct_pos ? strstr(pct_pos, "; ") : NULL;
            if (semi) {
                char *next = strstr(semi + 2, "; ");
                if (next) {
                    char *timestr = next + 2;
                    /* trim trailing whitespace */
                    int h = 0, m = 0;
                    if (sscanf(timestr, "%d:%d", &h, &m) == 2) {
                        if (h > 0)
                            snprintf(remain, sizeof(remain), ", ~%dh%02dm remaining", h, m);
                        else
                            snprintf(remain, sizeof(remain), ", ~%dm remaining", m);
                    }
                }
            }

            if (pct >= 0) {
                if (strcmp(status, "AC") == 0)
                    snprintf(result, sizeof(result), "Battery: %d%% (AC power)", pct);
                else
                    snprintf(result, sizeof(result), "Battery: %d%% (%s%s)", pct, status, remain);
            }
            found = 1;
            break;
        }
        if (strstr(line, "AC Power") && !found) {
            snprintf(result, sizeof(result), "Battery: AC power (no battery)");
            found = 1;
        }
    }
    pclose(fp);

    if (!found || result[0] == '\0') return NULL;

    /* Append electricity cost estimate */
    if (g_session_start > 0) {
        time_t now = time(NULL);
        double elapsed_min = difftime(now, g_session_start) / 60.0;
        /* Apple M5 estimated TDP under LLM load: ~25W */
        double kwh = 25.0 * (elapsed_min / 60.0) / 1000.0;
        double cost_jpy = kwh * 31.0;  /* ¥31/kWh Japan average */
        char cost_buf[128];
        snprintf(cost_buf, sizeof(cost_buf),
            " Electricity: ~¥%.2f (%.0fmin × 25W × ¥31/kWh).",
            cost_jpy, elapsed_min);
        strncat(result, cost_buf, sizeof(result) - strlen(result) - 1);
    }

    return strdup(result);
#endif
}

/* ---------- system prompt ---------- */
static cJSON *build_system_array(const char *memory) {
    cJSON *arr = cJSON_CreateArray();
    cJSON *base = cJSON_CreateObject();
    cJSON_AddStringToObject(base, "type", "text");

    /* Build system text, optionally injecting power info */
    char *power_info = get_power_info();
    char sys_text[2048];
    snprintf(sys_text, sizeof(sys_text),
        "You are mini-agent-c v11, a minimal autonomous agent in pure C. "
        "Tools: read_file, write_file, edit_file, bash, list_dir, "
        "grep_files (regex search), glob_files (find by pattern), "
        "http_get (fetch URLs, if enabled), todo (task tracking), "
        "save_memory, recall_memory, spawn_agent, checkpoint, undo, "
        "http_request (full HTTP with headers/body), process_start/stop/status, "
        "clipboard_read/write, preview_edit, run_tests, "
        "plus dynamic tools from .agent/tools/*.sh. "
        "Prefer edit_file over write_file for existing files. "
        "Use grep_files and glob_files to explore codebases efficiently — "
        "prefer them over bash grep/find. "
        "Use todo to track multi-step plans. "
        "Work inside the current directory only. "
        "When the task is complete, respond with a concise summary and STOP calling tools."
        "%s%s",
        power_info ? " [Host status: " : "",
        power_info ? power_info       : "");
    if (power_info) {
        /* close the bracket */
        size_t len = strlen(sys_text);
        if (len < sizeof(sys_text) - 2) { sys_text[len] = ']'; sys_text[len+1] = '\0'; }
        free(power_info);
    }

    cJSON_AddStringToObject(base, "text", sys_text);
    cJSON_AddItemToArray(arr, base);
    if (memory && *memory) {
        cJSON *mb = cJSON_CreateObject();
        cJSON_AddStringToObject(mb, "type", "text");
        char *wrap = malloc(strlen(memory) + 128);
        sprintf(wrap, "# Persistent memory\n%s", memory);
        cJSON_AddStringToObject(mb, "text", wrap);
        free(wrap);
        cJSON *cc = cJSON_CreateObject();
        cJSON_AddStringToObject(cc, "type", "ephemeral");
        cJSON_AddItemToObject(mb, "cache_control", cc);
        cJSON_AddItemToArray(arr, mb);
    } else {
        cJSON *cc = cJSON_CreateObject();
        cJSON_AddStringToObject(cc, "type", "ephemeral");
        cJSON_AddItemToObject(base, "cache_control", cc);
    }
    return arr;
}

/* ---------- agent loop (extracted for REPL reuse) ---------- */
static int run_agent(const char *api_key, const char *model,
                     const char *task, cJSON *tools, cJSON *system) {
    cJSON *messages = cJSON_CreateArray();
    cJSON *um = cJSON_CreateObject();
    cJSON_AddStringToObject(um, "role", "user");
    cJSON_AddStringToObject(um, "content", task);
    cJSON_AddItemToArray(messages, um);

    logerr("[start] model=%s turns<=%d budget=%ld depth=%d plan=%d sandbox=%d http=%d\n",
           model, g_max_turns, g_token_budget, g_spawn_depth,
           g_plan_mode, g_use_sandbox, g_allow_http);

    int turn;
    int final_ok = 0;
    char *final_text = NULL;
    for (turn = 0; turn < g_max_turns; turn++) {
        if (g_interrupted) {
            logerr("\n[interrupted] Ctrl-C, exiting cleanly\n");
            break;
        }
        if (should_stop_now()) {
            logerr("[stop] .agent/STOP detected\n");
            break;
        }
        if (g_total_in_tokens + g_total_out_tokens > g_token_budget) {
            logerr("[budget] token cap exceeded (%ld > %ld)\n",
                   g_total_in_tokens + g_total_out_tokens, g_token_budget);
            break;
        }

        messages = compact_messages(api_key, messages);

        cJSON *body = cJSON_CreateObject();
        cJSON_AddStringToObject(body, "model", model);
        cJSON_AddNumberToObject(body, "max_tokens", 16384);
        cJSON_AddItemToObject(body, "system",  cJSON_Duplicate(system, 1));
        cJSON_AddItemToObject(body, "tools",   cJSON_Duplicate(tools, 1));
        cJSON_AddItemToObject(body, "messages", cJSON_Duplicate(messages, 1));
        /* add extended thinking if enabled */
        if (g_think_mode && g_backend == BACKEND_ANTHROPIC) {
            cJSON *thinking = cJSON_CreateObject();
            cJSON_AddStringToObject(thinking, "type", "enabled");
            cJSON_AddNumberToObject(thinking, "budget_tokens", (double)g_think_budget);
            cJSON_AddItemToObject(body, "thinking", thinking);
        }
        char *body_str = cJSON_PrintUnformatted(body);
        cJSON_Delete(body);

        logerr("\n=== turn %d === (in=%ld out=%ld msgs=%d)\n",
               turn, g_total_in_tokens, g_total_out_tokens, cJSON_GetArraySize(messages));
        char *resp = claude_api(api_key, body_str);
        free(body_str);
        if (!resp) { logerr("[err] API call failed\n"); break; }

        cJSON *rj = cJSON_Parse(resp);
        if (!rj) { logerr("[err] bad JSON: %.400s\n", resp); free(resp); break; }
        free(resp);

        cJSON *err = cJSON_GetObjectItem(rj, "error");
        if (err) {
            char *e = cJSON_PrintUnformatted(err);
            logerr("[api-error] %s\n", e); free(e);
            cJSON_Delete(rj); break;
        }

        cJSON *usage = cJSON_GetObjectItem(rj, "usage");
        if (usage) {
            g_total_in_tokens  += (long)cJSON_GetNumberValue(cJSON_GetObjectItem(usage, "input_tokens"));
            g_total_out_tokens += (long)cJSON_GetNumberValue(cJSON_GetObjectItem(usage, "output_tokens"));
            cJSON *crc = cJSON_GetObjectItem(usage, "cache_read_input_tokens");
            cJSON *ccc = cJSON_GetObjectItem(usage, "cache_creation_input_tokens");
            if (crc) g_total_cache_read  += (long)cJSON_GetNumberValue(crc);
            if (ccc) g_total_cache_write += (long)cJSON_GetNumberValue(ccc);
            if (!g_quiet && (crc || ccc))
                logerr("[cache] read=%g create=%g\n",
                       cJSON_GetNumberValue(crc), cJSON_GetNumberValue(ccc));
        }

        cJSON *content = cJSON_GetObjectItem(rj, "content");
        if (!content) { logerr("[err] no content\n"); cJSON_Delete(rj); break; }

        cJSON *am = cJSON_CreateObject();
        cJSON_AddStringToObject(am, "role", "assistant");
        cJSON_AddItemToObject(am, "content", cJSON_Duplicate(content, 1));
        cJSON_AddItemToArray(messages, am);

        /* collect all tool_use blocks first */
        typedef struct { const char *name; const char *id; cJSON *inp; } ToolCall;
        ToolCall tool_calls[64];
        int n_tools = 0;
        cJSON *block;
        cJSON_ArrayForEach(block, content) {
            const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(block, "type"));
            if (!type) continue;
            /* handle thinking blocks - log to stderr, skip from final_text */
            if (strcmp(type, "thinking") == 0 || strcmp(type, "redacted_thinking") == 0) {
                const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(block, "text"));
                if (t) logerr("[thinking] %s\n", t);
                continue;
            }
            if (strcmp(type, "text") == 0) {
                const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(block, "text"));
                if (t) {
                    free(final_text); final_text = strdup(t);
                    if (!g_quiet && !g_stream_mode) printf("\n[assistant] %s\n", t);
                }
            } else if (strcmp(type, "tool_use") == 0 && n_tools < 64) {
                tool_calls[n_tools].name = cJSON_GetStringValue(cJSON_GetObjectItem(block, "name"));
                tool_calls[n_tools].id   = cJSON_GetStringValue(cJSON_GetObjectItem(block, "id"));
                tool_calls[n_tools].inp  = cJSON_GetObjectItem(block, "input");
                n_tools++;
            }
        }

        cJSON *tool_results = cJSON_CreateArray();
        int has_tool = n_tools > 0;
        
        /* parallel execution if multiple tools and not disabled */
        if (n_tools > 1 && !g_no_parallel) {
            logerr("[parallel] executing %d tools in parallel\n", n_tools);
            typedef struct { pid_t pid; int pipefd[2]; const char *name; const char *id; char *inp_str; } ProcInfo;
            ProcInfo procs[64];
            
            /* spawn all tool processes */
            for (int i = 0; i < n_tools; i++) {
                procs[i].name = tool_calls[i].name;
                procs[i].id = tool_calls[i].id;
                procs[i].inp_str = cJSON_PrintUnformatted(tool_calls[i].inp);
                
                if (pipe(procs[i].pipefd) < 0) {
                    logerr("[err] pipe failed for tool %d\n", i);
                    procs[i].pid = -1;
                    continue;
                }
                
                pid_t pid = fork();
                if (pid < 0) {
                    logerr("[err] fork failed for tool %d\n", i);
                    close(procs[i].pipefd[0]); close(procs[i].pipefd[1]);
                    procs[i].pid = -1;
                    continue;
                }
                
                if (pid == 0) {
                    /* child process */
                    close(procs[i].pipefd[0]);
                    char *out = execute_tool(tool_calls[i].name, tool_calls[i].inp);
                    if (strlen(out) > MAX_TOOL_OUT_BYTES) out[MAX_TOOL_OUT_BYTES] = 0;
                    write(procs[i].pipefd[1], out, strlen(out));
                    free(out);
                    close(procs[i].pipefd[1]);
                    _exit(0);
                }
                
                /* parent */
                procs[i].pid = pid;
                close(procs[i].pipefd[1]);
            }
            
            /* collect all results */
            for (int i = 0; i < n_tools; i++) {
                if (procs[i].pid <= 0) {
                    /* create error result */
                    cJSON *tr = cJSON_CreateObject();
                    cJSON_AddStringToObject(tr, "type", "tool_result");
                    cJSON_AddStringToObject(tr, "tool_use_id", procs[i].id ? procs[i].id : "");
                    cJSON_AddStringToObject(tr, "content", "Error: failed to spawn tool process");
                    cJSON_AddItemToArray(tool_results, tr);
                    continue;
                }
                
                logerr("[tool] %s %.200s\n", procs[i].name, procs[i].inp_str ? procs[i].inp_str : "");
                
                /* read output from pipe */
                Buf out_buf = {0};
                char chunk[8192];
                ssize_t n;
                while ((n = read(procs[i].pipefd[0], chunk, sizeof(chunk))) > 0) {
                    buf_append(&out_buf, chunk, n);
                    if (out_buf.size > MAX_TOOL_OUT_BYTES) break;
                }
                close(procs[i].pipefd[0]);
                
                /* wait for child */
                waitpid(procs[i].pid, NULL, 0);
                
                if (!out_buf.data) out_buf.data = strdup("");
                
                audit(procs[i].name, procs[i].inp_str, out_buf.data);
                
                cJSON *tr = cJSON_CreateObject();
                cJSON_AddStringToObject(tr, "type", "tool_result");
                cJSON_AddStringToObject(tr, "tool_use_id", procs[i].id ? procs[i].id : "");
                cJSON_AddStringToObject(tr, "content", out_buf.data);
                cJSON_AddItemToArray(tool_results, tr);
                
                free(out_buf.data);
                free(procs[i].inp_str);
            }
        } else {
            /* sequential execution (original behavior) */
            for (int i = 0; i < n_tools; i++) {
                const char *name = tool_calls[i].name;
                const char *id   = tool_calls[i].id;
                cJSON *inp       = tool_calls[i].inp;
                char *inp_str = cJSON_PrintUnformatted(inp);
                logerr("[tool] %s %.200s\n", name, inp_str ? inp_str : "");
                char *out = execute_tool(name, inp);
                audit(name, inp_str, out);
                free(inp_str);
                if (strlen(out) > MAX_TOOL_OUT_BYTES) out[MAX_TOOL_OUT_BYTES] = 0;
                cJSON *tr = cJSON_CreateObject();
                cJSON_AddStringToObject(tr, "type", "tool_result");
                cJSON_AddStringToObject(tr, "tool_use_id", id ? id : "");
                cJSON_AddStringToObject(tr, "content", out);
                cJSON_AddItemToArray(tool_results, tr);
                free(out);
            }
        }

        if (!has_tool) {
            cJSON_Delete(tool_results);
            const char *stop = cJSON_GetStringValue(cJSON_GetObjectItem(rj, "stop_reason"));
            logerr("\n[done] stop=%s turns=%d total_in=%ld total_out=%ld\n",
                   stop ? stop : "?", turn + 1, g_total_in_tokens, g_total_out_tokens);
            cJSON_Delete(rj);
            final_ok = 1;
            break;
        }

        cJSON *trm = cJSON_CreateObject();
        cJSON_AddStringToObject(trm, "role", "user");
        cJSON_AddItemToObject(trm, "content", tool_results);
        cJSON_AddItemToArray(messages, trm);
        cJSON_Delete(rj);
    }

    if (turn >= g_max_turns) logerr("[halt] max turns reached\n");
    if (g_quiet && final_text) printf("%s\n", final_text);
    free(final_text);
    cJSON_Delete(messages);
    return final_ok;
}

/* ---------- usage ---------- */
static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options] \"<task>\"\n"
        "       %s --interactive [options]\n"
        "\n"
        "Options:\n"
        "  --model MODEL         claude-sonnet-4-5 (default), claude-haiku-4-5, claude-opus-4-5\n"
        "  --max-turns N         turn cap (default %d)\n"
        "  --budget N            token budget cap (default %d)\n"
        "  --approve  --step     confirm every tool call before executing\n"
        "  --approve-bash        confirm only bash commands\n"
        "  --plan                plan mode, no side effects\n"
        "  --sandbox             bash runs under sandbox-exec\n"
        "  --stream              SSE streaming (Anthropic backend only)\n"
        "  --stream-bash         real-time bash output to stderr\n"
        "  --quiet               subagent mode (only final text to stdout)\n"
        "  --no-memory           don't load persistent memory\n"
        "  --allow-http          enable http_get tool\n"
        "  --no-parallel         disable parallel tool execution\n"
        "  --think               enable extended thinking mode\n"
        "  --think-budget N      thinking token budget (default 8000)\n"
        "  --interactive / -i    REPL mode (multi-task conversation)\n"
        "  --backend B           anthropic (default) | openai\n"
        "  --api-base URL        override API base URL\n"
        "  --version             print version\n"
        "  --help                show this help\n"
        "\n"
        "New in v9: parallel tools, ask_user, git, diff_files, notify, streaming bash,\n"
        "           extended thinking, auto-mkdir\n",
        prog, prog, MAX_TURNS_DEFAULT, TOKEN_BUDGET_DEFAULT);
}

/* ---------- main ---------- */
int main(int argc, char **argv) {
    g_session_start = time(NULL);
    signal(SIGINT, sigint_handler);
    realpath(argv[0], g_self_path);
    if (!getcwd(g_cwd, sizeof(g_cwd))) strcpy(g_cwd, ".");
    const char *depth_env = getenv("MINI_AGENT_DEPTH");
    if (depth_env) g_spawn_depth = atoi(depth_env);

    const char *model = MODEL_DEFAULT;
    int no_memory = 0;
    const char *task = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) { usage(argv[0]); return 0; }
        else if (strcmp(a, "--version") == 0) { printf("mini-agent-c v11.0\n"); return 0; }
        else if (strcmp(a, "--model") == 0 && i + 1 < argc) { model = argv[++i]; }
        else if (strcmp(a, "--max-turns") == 0 && i + 1 < argc) { g_max_turns = atoi(argv[++i]); }
        else if (strcmp(a, "--budget") == 0 && i + 1 < argc) { g_token_budget = atol(argv[++i]); }
        else if (strcmp(a, "--plan") == 0) { g_plan_mode = 1; }
        else if (strcmp(a, "--sandbox") == 0) { g_use_sandbox = 1; }
        else if (strcmp(a, "--stream") == 0) { g_stream_mode = 1; }
        else if (strcmp(a, "--quiet") == 0) { g_quiet = 1; }
        else if (strcmp(a, "--no-memory") == 0) { no_memory = 1; }
        else if (strcmp(a, "--allow-http") == 0) { g_allow_http = 1; }
        else if (strcmp(a, "--no-parallel") == 0) { g_no_parallel = 1; }
        else if (strcmp(a, "--stream-bash") == 0) { g_stream_bash = 1; }
        else if (strcmp(a, "--think") == 0) { g_think_mode = 1; }
        else if (strcmp(a, "--think-budget") == 0 && i + 1 < argc) { g_think_budget = atol(argv[++i]); }
        else if (strcmp(a, "--interactive") == 0 || strcmp(a, "-i") == 0) { g_interactive = 1; }
        else if (strcmp(a, "--approve") == 0 || strcmp(a, "--step") == 0) { g_approve = 1; }
        else if (strcmp(a, "--approve-bash") == 0) { g_approve_bash = 1; }
        else if (strcmp(a, "--backend") == 0 && i + 1 < argc) {
            const char *b = argv[++i];
            if (strcmp(b, "openai") == 0) g_backend = BACKEND_OPENAI;
            else if (strcmp(b, "anthropic") == 0) g_backend = BACKEND_ANTHROPIC;
            else { fprintf(stderr, "unknown backend: %s\n", b); return 1; }
        }
        else if (strcmp(a, "--api-base") == 0 && i + 1 < argc) {
            snprintf(g_api_base, sizeof(g_api_base), "%s", argv[++i]);
        }
        else if (strcmp(a, "--allow-remote-backend") == 0) {
            setenv("MINI_AGENT_ALLOW_REMOTE", "1", 1);
        }
        else if (a[0] == '-') { fprintf(stderr, "unknown flag: %s\n", a); usage(argv[0]); return 1; }
        else if (!task) { task = a; }
    }

    if (!g_interactive && !task) { usage(argv[0]); return 1; }

    const char *api_key = NULL;
    if (g_backend == BACKEND_ANTHROPIC) {
        api_key = getenv("ANTHROPIC_API_KEY");
        if (!api_key || !*api_key) {
            fprintf(stderr, "ANTHROPIC_API_KEY not set\n");
            return 1;
        }
    } else {
        api_key = getenv("OPENAI_API_KEY");
        if (!api_key) api_key = "";
        if (g_api_base[0]) {
            int is_local = (strstr(g_api_base, "127.0.0.1") != NULL)
                        || (strstr(g_api_base, "localhost") != NULL)
                        || (strstr(g_api_base, "://[::1]") != NULL);
            if (!is_local && !getenv("MINI_AGENT_ALLOW_REMOTE")) {
                fprintf(stderr, "refused: --api-base '%s' is not localhost. "
                        "Pass --allow-remote-backend to override.\n", g_api_base);
                return 1;
            }
            if (!is_local) { logerr("[warn] remote backend — sandbox auto-enabled\n"); g_use_sandbox = 1; }
        }
    }

    ensure_agent_dir();
    // Initialize checkpoint directory for v10
    const char *home = getenv("HOME");
    if (home) {
        snprintf(g_checkpoint_dir, sizeof(g_checkpoint_dir), "%s/.mini-agent/checkpoints", home);
    } else {
        snprintf(g_checkpoint_dir, sizeof(g_checkpoint_dir), ".agent/checkpoints");
    }
    load_dynamic_tools();
    char *memory = no_memory ? NULL : load_persistent_memory();

    curl_global_init(CURL_GLOBAL_DEFAULT);

    cJSON *tools  = build_tools_array();
    cJSON *system = build_system_array(memory);
    free(memory);

    int exit_code = 0;

    if (g_interactive) {
        printf("mini-agent-c v8 interactive mode (model: %s)\n", model);
        printf("Type your task and press Enter. 'exit' or Ctrl-D to quit.\n\n");
        char line[8192];
        while (!g_interrupted) {
            printf("> ");
            fflush(stdout);
            if (!fgets(line, sizeof(line), stdin)) break;
            size_t ll = strlen(line);
            while (ll > 0 && (line[ll-1] == '\n' || line[ll-1] == '\r')) line[--ll] = 0;
            if (!ll) continue;
            if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) break;
            if (strcmp(line, "/cost") == 0) { print_cost(); continue; }
            if (strcmp(line, "/reset") == 0) {
                g_total_in_tokens = g_total_out_tokens = 0;
                g_total_cache_read = g_total_cache_write = 0;
                printf("[reset] token counters cleared\n");
                continue;
            }
            int ok = run_agent(api_key, model, line, tools, system);
            if (!ok) exit_code = 2;
            printf("\n");
        }
    } else {
        int ok = run_agent(api_key, model, task, tools, system);
        if (!ok) exit_code = 2;
    }

    print_cost();

    cJSON_Delete(tools);
    cJSON_Delete(system);
    curl_global_cleanup();
    return exit_code;
}
