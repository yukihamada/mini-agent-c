/*
 * mini-agent-c v3: full-featured minimal autonomous agent in C.
 *
 * Features:
 *  - Anthropic Messages API + tool_use loop
 *  - Prompt caching (system + tools with cache_control)
 *  - Persistent memory (~/.mini-agent/memory.md)
 *  - Dynamic tool loading (./.agent/tools/*.sh with #@name/#@description/#@arg headers)
 *  - Subagent recursion (spawn_agent tool, depth-limited)
 *  - Context compaction via Haiku summarizer
 *  - Plan mode (--plan: no side effects)
 *  - Sandbox mode (--sandbox: bash runs under sandbox-exec with .agent/sandbox.sb)
 *  - Multi-model (--model)
 *  - Streaming (--stream) SSE parse
 *
 * Safety:
 *  - Token & cost budget caps (hard exit)
 *  - Max turns cap
 *  - Path confinement for file tools (no '..' no '~' absolute only if inside CWD)
 *  - Dangerous command denylist for bash
 *  - Spawn depth cap via MINI_AGENT_DEPTH env var
 *  - .agent/STOP kill switch polled each turn
 *  - .agent/audit.log JSONL of every tool call
 *  - API key redaction on any output
 *  - Secrets never written to audit log
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
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
/* ~$3 at Sonnet pricing, roughly */

/* ---------- globals ---------- */
#define BACKEND_ANTHROPIC 0
#define BACKEND_OPENAI    1   /* OpenAI-compatible: NOU, Ollama, LM Studio, vLLM, OpenAI */
static int   g_backend         = BACKEND_ANTHROPIC;
static char  g_api_base[512]   = {0};  /* empty = use backend default */
static int   g_plan_mode       = 0;
static int   g_use_sandbox     = 0;
static int   g_stream_mode     = 0;
static int   g_quiet           = 0;    /* subagent mode: only final text to stdout */
static int   g_max_turns       = MAX_TURNS_DEFAULT;
static long  g_token_budget    = TOKEN_BUDGET_DEFAULT;
static long  g_total_in_tokens = 0;
static long  g_total_out_tokens= 0;
static int   g_spawn_depth     = 0;
static char  g_self_path[1024] = {0};
static char  g_cwd[1024]       = {0};
static char  g_memory_path[1024] = {0};

typedef struct {
    char name[64];
    char description[512];
    char script_path[1024];
    cJSON *args;  /* array of "name:description" */
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
    "curl -s ", /* prevent silent-mode external posts; not silent is ok */
    NULL
};

static const char *check_dangerous(const char *cmd) {
    if (!cmd) return NULL;
    for (int i = 0; DANGEROUS_PATTERNS[i]; i++) {
        if (strstr(cmd, DANGEROUS_PATTERNS[i])) return DANGEROUS_PATTERNS[i];
    }
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
    struct tm tmv;
    localtime_r(&t, &tmv);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", &tmv);
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
    free(s);
    cJSON_Delete(e);
    fclose(f);
}

static void logerr(const char *fmt, ...) {
    if (g_quiet) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

/* ---------- HTTP / API ---------- */

static size_t curl_write_cb(void *p, size_t sz, size_t nm, void *ud) {
    Buf *b = (Buf *)ud;
    size_t n = sz * nm;
    buf_append(b, p, n);
    return n;
}

/* ==== OpenAI-compatible backend (NOU/Ollama/LM Studio/vLLM/OpenAI) ==== */

/* Convert Anthropic-format request body to OpenAI chat/completions body.
 * We accept cache_control / system-as-array / content-block messages and
 * flatten to OpenAI's simpler schema. Unknown fields dropped. */
static char *anthropic_body_to_openai(const char *anthro_body) {
    cJSON *a = cJSON_Parse(anthro_body);
    if (!a) return NULL;
    cJSON *o = cJSON_CreateObject();

    cJSON *m = cJSON_GetObjectItem(a, "model");
    if (m) cJSON_AddItemToObject(o, "model", cJSON_Duplicate(m, 1));

    cJSON *mt = cJSON_GetObjectItem(a, "max_tokens");
    if (mt) cJSON_AddItemToObject(o, "max_tokens", cJSON_Duplicate(mt, 1));

    cJSON *omsgs = cJSON_CreateArray();

    /* system → one prepended system message */
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
                if (t) {
                    if (sb.size > 0) buf_append(&sb, "\n\n", 2);
                    buf_append(&sb, t, strlen(t));
                }
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

    /* messages: convert content blocks */
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
            /* Split blocks into text + tool_calls */
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
                    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(blk, "id"));
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
            if (cJSON_GetArraySize(tool_calls) > 0) {
                cJSON_AddItemToObject(om, "tool_calls", tool_calls);
            } else {
                cJSON_Delete(tool_calls);
            }
            cJSON_AddItemToArray(omsgs, om);
            free(text_buf.data);
        } else {
            /* user role with tool_results → emit one tool message per result */
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
                    /* user text block */
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

    /* tools: anthropic → openai function wrapping */
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
    cJSON_Delete(a);
    cJSON_Delete(o);
    return s;
}

/* Convert OpenAI chat/completions response body → Anthropic Messages response body. */
static char *openai_response_to_anthropic(const char *oai_resp) {
    cJSON *o = cJSON_Parse(oai_resp);
    if (!o) return NULL;

    cJSON *err = cJSON_GetObjectItem(o, "error");
    if (err) {
        cJSON *a = cJSON_CreateObject();
        cJSON_AddItemToObject(a, "error", cJSON_Duplicate(err, 1));
        char *s = cJSON_PrintUnformatted(a);
        cJSON_Delete(o);
        cJSON_Delete(a);
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
                        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(fn, "name"));
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
    cJSON_Delete(o);
    cJSON_Delete(a);
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
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 3600L);
    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    if (http_code_out) *http_code_out = code;
    curl_slist_free_all(h);
    curl_easy_cleanup(curl);
    free(oai_body);
    if (rc != CURLE_OK) {
        logerr("[openai] curl: %s\n", curl_easy_strerror(rc));
        free(buf.data);
        return NULL;
    }
    char *anthro_resp = openai_response_to_anthropic(buf.data);
    free(buf.data);
    return anthro_resp;
}

/* ==== end OpenAI bridge ==== */

/* ==== SSE streaming (v6) ==== */
/*
 * Parse Anthropic Messages API server-sent events progressively.
 * Events of interest:
 *   message_start              → capture usage.input_tokens
 *   content_block_start        → init a new block (text or tool_use)
 *   content_block_delta        → text_delta (print to stdout) or input_json_delta (buffer)
 *   content_block_stop         → finalize block (parse partial_json for tool_use)
 *   message_delta              → stop_reason, usage.output_tokens
 *   message_stop               → done
 *
 * Rebuilds an Anthropic Messages response JSON at the end so the main loop
 * (which expects non-streaming format) works unchanged.
 */

#define STREAM_MAX_BLOCKS 32

typedef struct {
    char *type;         /* "text" or "tool_use" */
    Buf   text;         /* for text blocks */
    char *id;           /* for tool_use */
    char *name;         /* for tool_use */
    Buf   partial_json; /* accumulating input_json_delta */
    int   printed_nl;   /* we already printed a trailing newline */
} StreamBlock;

typedef struct {
    Buf         recv;              /* unparsed bytes accumulator */
    StreamBlock blocks[STREAM_MAX_BLOCKS];
    int         n_blocks;
    char       *stop_reason;
    long        input_tokens;
    long        output_tokens;
    int         message_stopped;
    int         print_text;        /* echo text_delta to stdout as they arrive */
} StreamCtx;

static void sb_free(StreamBlock *b) {
    free(b->type); free(b->id); free(b->name);
    buf_free(&b->text);
    buf_free(&b->partial_json);
    memset(b, 0, sizeof(*b));
}

static void sc_init(StreamCtx *sc) {
    memset(sc, 0, sizeof(*sc));
}

static void sc_free(StreamCtx *sc) {
    for (int i = 0; i < sc->n_blocks; i++) sb_free(&sc->blocks[i]);
    buf_free(&sc->recv);
    free(sc->stop_reason);
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
                if (sc->print_text) { fputs(t, stdout); fflush(stdout); }
            }
        } else if (strcmp(dtype, "input_json_delta") == 0) {
            const char *p = cJSON_GetStringValue(cJSON_GetObjectItem(delta, "partial_json"));
            if (p) buf_append(&sc->blocks[idx].partial_json, p, strlen(p));
        }
    } else if (strcmp(type, "content_block_stop") == 0) {
        int idx = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(ev, "index"));
        if (idx < 0 || idx >= STREAM_MAX_BLOCKS) { cJSON_Delete(ev); return; }
        if (sc->print_text && sc->blocks[idx].type &&
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
    /* Parse complete events (delimited by \n\n) from sc->recv. */
    char *start = sc->recv.data;
    if (!start) return;
    char *end;
    while ((end = strstr(start, "\n\n")) != NULL) {
        *end = 0;
        /* Event may have multiple lines; find the "data: " line(s). */
        char *line = start;
        while (line && *line) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = 0;
            if (strncmp(line, "data: ", 6) == 0) {
                handle_sse_event(sc, line + 6);
            }
            if (!nl) break;
            line = nl + 1;
        }
        start = end + 2;
    }
    /* Shift remaining unparsed bytes to the front. */
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

/* Rebuild an Anthropic-format Messages response from StreamCtx. */
static char *stream_to_anthropic_json(StreamCtx *sc) {
    cJSON *root = cJSON_CreateObject();
    cJSON *content = cJSON_CreateArray();
    for (int i = 0; i < sc->n_blocks; i++) {
        StreamBlock *b = &sc->blocks[i];
        if (!b->type) continue;
        cJSON *blk = cJSON_CreateObject();
        cJSON_AddStringToObject(blk, "type", b->type);
        if (strcmp(b->type, "text") == 0) {
            cJSON_AddStringToObject(blk, "text", b->text.data ? b->text.data : "");
        } else if (strcmp(b->type, "tool_use") == 0) {
            cJSON_AddStringToObject(blk, "id", b->id ? b->id : "");
            cJSON_AddStringToObject(blk, "name", b->name ? b->name : "");
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
    cJSON_AddNumberToObject(usage, "input_tokens", (double)sc->input_tokens);
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

    /* Mutate the body to ensure "stream": true */
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
    h = curl_slist_append(h, "anthropic-beta: prompt-caching-2024-07-31");
    h = curl_slist_append(h, "content-type: application/json");
    h = curl_slist_append(h, "accept: text/event-stream");
    curl_easy_setopt(curl, CURLOPT_URL, "https://api.anthropic.com/v1/messages");
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, stream_body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(stream_body));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sc);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 3600L);

    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    if (http_code_out) *http_code_out = code;
    curl_slist_free_all(h);
    curl_easy_cleanup(curl);
    free(stream_body);

    if (rc != CURLE_OK) {
        logerr("[stream] curl: %s\n", curl_easy_strerror(rc));
        sc_free(&sc);
        return NULL;
    }

    /* Non-2xx: the server probably returned a JSON error on a single line.
     * Just return the raw buffer so the caller can parse it normally. */
    if (code >= 400) {
        char *raw = sc.recv.data ? strdup(sc.recv.data) : strdup("{\"error\":{\"message\":\"stream error\"}}");
        sc_free(&sc);
        return raw;
    }

    char *resp = stream_to_anthropic_json(&sc);
    sc_free(&sc);
    return resp;
}
/* ==== end SSE streaming ==== */

static char *claude_api_once(const char *api_key, const char *body, long *http_code_out) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;
    Buf buf = {0};
    struct curl_slist *h = NULL;
    char auth[1024];
    snprintf(auth, sizeof(auth), "x-api-key: %s", api_key);
    h = curl_slist_append(h, auth);
    h = curl_slist_append(h, "anthropic-version: 2023-06-01");
    h = curl_slist_append(h, "anthropic-beta: prompt-caching-2024-07-31");
    h = curl_slist_append(h, "content-type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, "https://api.anthropic.com/v1/messages");
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 3600L);
    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    if (http_code_out) *http_code_out = code;
    curl_slist_free_all(h);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) {
        logerr("[curl] %s\n", curl_easy_strerror(rc));
        free(buf.data);
        return NULL;
    }
    return buf.data;
}

static char *claude_api(const char *api_key, const char *body) {
    const char *api_base_eff = g_api_base[0] ? g_api_base :
        (g_backend == BACKEND_OPENAI ? "http://localhost:4004" : "https://api.anthropic.com");

    for (int attempt = 0; attempt < MAX_RETRIES; attempt++) {
        long code = 0;
        char *resp;
        if (g_backend == BACKEND_OPENAI) {
            /* streaming for OpenAI backend not yet implemented — always non-stream */
            resp = openai_api_once(api_base_eff, api_key, body, &code);
        } else if (g_stream_mode) {
            resp = claude_api_stream(api_key, body, &code);
        } else {
            resp = claude_api_once(api_key, body, &code);
        }
        if (!resp) { sleep(RETRY_SLEEP_SEC); continue; }
        if (code == 529 || (code >= 500 && code < 600) ||
            (resp && strstr(resp, "\"overloaded_error\""))) {
            logerr("[retry %d] http=%ld sleeping %ds\n", attempt, code, RETRY_SLEEP_SEC);
            free(resp);
            sleep(RETRY_SLEEP_SEC);
            continue;
        }
        return resp;
    }
    return NULL;
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
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) sz = 0;
    if (sz > MAX_TOOL_OUT_BYTES) sz = MAX_TOOL_OUT_BYTES;
    char *buf = malloc(sz + 64);
    size_t got = fread(buf, 1, sz, f);
    buf[got] = 0;
    fclose(f);
    redact_secrets(buf);
    return buf;
}

static char *tool_write_file(const char *path, const char *content) {
    if (!path_is_safe(path)) {
        char *r = malloc(256);
        snprintf(r, 256, "Error: path '%s' not in sandboxed workspace", path ? path : "(null)");
        return r;
    }
    if (!content) content = "";
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

static char *tool_bash(const char *cmd) {
    if (!cmd || !*cmd) return strdup("Error: empty command");
    const char *bad = check_dangerous(cmd);
    if (bad) {
        char *r = malloc(512);
        snprintf(r, 512, "BLOCKED: command contains dangerous pattern '%s' — refused by safety layer", bad);
        return r;
    }
    char cmdfile[128];
    snprintf(cmdfile, sizeof(cmdfile), "/tmp/mini_agent_cmd_%d_%ld.sh", getpid(), (long)time(NULL));
    FILE *cf = fopen(cmdfile, "w");
    if (!cf) return strdup("Error: tempfile failed");
    fprintf(cf, "#!/bin/bash\nset -o pipefail\nulimit -t 60\nulimit -f 100000\n%s\n", cmd);
    fclose(cf);
    chmod(cmdfile, 0700);

    char full[2048];
    if (g_use_sandbox) {
        snprintf(full, sizeof(full),
                 "sandbox-exec -D CWD=%s -f .agent/sandbox.sb /bin/bash %s 2>&1",
                 g_cwd, cmdfile);
    } else {
        snprintf(full, sizeof(full), "/bin/bash %s 2>&1", cmdfile);
    }
    FILE *p = popen(full, "r");
    if (!p) { unlink(cmdfile); return strdup("Error: popen failed"); }
    char *out = read_stream_to_string(p);
    int rc = pclose(p);
    unlink(cmdfile);
    /* append exit code */
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
        if (e->d_name[0] == '.') continue;  /* skip dotfiles */
        buf_append(&b, e->d_name, strlen(e->d_name));
        buf_append(&b, "\n", 1);
    }
    closedir(d);
    if (!b.data) b.data = strdup("(empty)");
    return b.data;
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
    struct tm tmv;
    localtime_r(&t, &tmv);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d", &tmv);
    fprintf(f, "\n## %s [%s]\n%s\n", key, ts, value);
    fclose(f);
    return strdup("Memory saved");
}

static char *tool_recall_memory(void) {
    FILE *f = fopen(g_memory_path, "rb");
    if (!f) return strdup("(no memory yet)");
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return strdup("(empty memory)"); }
    if (sz > 32 * 1024) sz = 32 * 1024;
    char *buf = malloc(sz + 1);
    fread(buf, 1, sz, f);
    buf[sz] = 0;
    fclose(f);
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
    if (pid < 0) {
        close(pipefd[0]); close(pipefd[1]);
        return strdup("Error: fork failed");
    }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) { dup2(dn, STDERR_FILENO); close(dn); }
        char depth_str[16];
        snprintf(depth_str, sizeof(depth_str), "%d", g_spawn_depth + 1);
        setenv("MINI_AGENT_DEPTH", depth_str, 1);
        if (model && *model) {
            execl(g_self_path, "agent.v3", "--quiet", "--model", model, task, (char *)NULL);
        } else {
            execl(g_self_path, "agent.v3", "--quiet", task, (char *)NULL);
        }
        _exit(127);
    }
    close(pipefd[1]);
    Buf b = {0};
    char tmp[4096];
    ssize_t rd;
    while ((rd = read(pipefd[0], tmp, sizeof(tmp))) > 0) {
        buf_append(&b, tmp, rd);
        if (b.size > 32 * 1024) break;
    }
    close(pipefd[0]);
    int status;
    waitpid(pid, &status, 0);
    if (!b.data) b.data = strdup("(no output from subagent)");
    return b.data;
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
        /* default name from filename */
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
            if (strncmp(p, "#@name:", 7) == 0) {
                p += 7; while (*p == ' ') p++;
                snprintf(t->name, sizeof(t->name), "%s", p);
            } else if (strncmp(p, "#@description:", 14) == 0) {
                p += 14; while (*p == ' ') p++;
                snprintf(t->description, sizeof(t->description), "%s", p);
            } else if (strncmp(p, "#@arg:", 6) == 0) {
                p += 6; while (*p == ' ') p++;
                cJSON_AddItemToArray(t->args, cJSON_CreateString(p));
            }
        }
        fclose(f);
        if (!t->description[0]) strcpy(t->description, "Dynamic shell tool");
        g_n_dyn_tools++;
    }
    closedir(d);
    if (g_n_dyn_tools > 0) logerr("[dyn] loaded %d dynamic tools\n", g_n_dyn_tools);
}

static char *exec_dyn_tool(DynTool *t, cJSON *input) {
    /* build: /bin/bash script_path 'arg1' 'arg2' ... */
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
        memcpy(aname, spec, nl);
        aname[nl] = 0;
        cJSON *v = cJSON_GetObjectItem(input, aname);
        const char *s = v ? cJSON_GetStringValue(v) : "";
        if (!s) s = "";
        /* shell single-quote escape */
        buf_append(&cmd, " '", 2);
        for (const char *q = s; *q; q++) {
            if (*q == '\'') buf_append(&cmd, "'\\''", 4);
            else buf_append(&cmd, q, 1);
        }
        buf_append(&cmd, "'", 1);
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

    const char *rp[] = {"path"};
    const char *rpd[] = {"Relative path inside workspace"};
    cJSON_AddItemToArray(tools, make_simple_tool("read_file",
        "Read contents of a file inside the workspace.", rp, rpd, 1));

    const char *wp[] = {"path", "content"};
    const char *wpd[] = {"Relative path", "Full file content (overwrites)"};
    cJSON_AddItemToArray(tools, make_simple_tool("write_file",
        "Write content to a file inside the workspace (overwrites).", wp, wpd, 2));

    const char *bp[] = {"command"};
    const char *bpd[] = {"Bash command. Dangerous patterns blocked."};
    cJSON_AddItemToArray(tools, make_simple_tool("bash",
        "Run a bash command in the workspace. Output captured, 60s CPU limit.", bp, bpd, 1));

    const char *lp[] = {"path"};
    const char *lpd[] = {"Directory path"};
    cJSON_AddItemToArray(tools, make_simple_tool("list_dir",
        "List non-hidden entries in a directory.", lp, lpd, 1));

    const char *sp[] = {"key", "value"};
    const char *spd[] = {"Short title", "Fact or insight to remember across sessions"};
    cJSON_AddItemToArray(tools, make_simple_tool("save_memory",
        "Persist a fact to long-term memory (~/.mini-agent/memory.md).", sp, spd, 2));

    cJSON *rm = cJSON_CreateObject();
    cJSON_AddStringToObject(rm, "name", "recall_memory");
    cJSON_AddStringToObject(rm, "description", "Read the full persistent memory file.");
    cJSON *rms = cJSON_CreateObject();
    cJSON_AddStringToObject(rms, "type", "object");
    cJSON_AddItemToObject(rms, "properties", cJSON_CreateObject());
    cJSON_AddItemToObject(rms, "required", cJSON_CreateArray());
    cJSON_AddItemToObject(rm, "input_schema", rms);
    cJSON_AddItemToArray(tools, rm);

    const char *xp[] = {"task"};
    const char *xpd[] = {"Self-contained task description for a fresh sub-agent"};
    cJSON_AddItemToArray(tools, make_simple_tool("spawn_agent",
        "Spawn a fresh isolated sub-agent to work on a subtask (depth-limited).", xp, xpd, 1));

    /* dynamic tools */
    for (int i = 0; i < g_n_dyn_tools; i++) {
        DynTool *dt = &g_dyn_tools[i];
        cJSON *tj = cJSON_CreateObject();
        cJSON_AddStringToObject(tj, "name", dt->name);
        char dd[600];
        snprintf(dd, sizeof(dd), "[dynamic] %s", dt->description);
        cJSON_AddStringToObject(tj, "description", dd);
        cJSON *sc = cJSON_CreateObject();
        cJSON_AddStringToObject(sc, "type", "object");
        cJSON *props = cJSON_CreateObject();
        cJSON *req = cJSON_CreateArray();
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
            cJSON *ap = cJSON_CreateObject();
            cJSON_AddStringToObject(ap, "type", "string");
            if (*adesc) cJSON_AddStringToObject(ap, "description", adesc);
            cJSON_AddItemToObject(props, aname, ap);
            cJSON_AddItemToArray(req, cJSON_CreateString(aname));
        }
        cJSON_AddItemToObject(sc, "properties", props);
        cJSON_AddItemToObject(sc, "required", req);
        cJSON_AddItemToObject(tj, "input_schema", sc);
        cJSON_AddItemToArray(tools, tj);
    }

    /* cache_control on the last tool → caches all tools */
    int total = cJSON_GetArraySize(tools);
    if (total > 0) {
        cJSON *last = cJSON_GetArrayItem(tools, total - 1);
        cJSON *cc = cJSON_CreateObject();
        cJSON_AddStringToObject(cc, "type", "ephemeral");
        cJSON_AddItemToObject(last, "cache_control", cc);
    }
    return tools;
}

/* ---------- tool dispatch ---------- */

static char *execute_tool(const char *name, cJSON *input) {
    cJSON *path    = cJSON_GetObjectItem(input, "path");
    cJSON *content = cJSON_GetObjectItem(input, "content");
    cJSON *command = cJSON_GetObjectItem(input, "command");
    cJSON *key     = cJSON_GetObjectItem(input, "key");
    cJSON *value   = cJSON_GetObjectItem(input, "value");
    cJSON *task    = cJSON_GetObjectItem(input, "task");

    if (g_plan_mode) {
        char *inp_str = cJSON_PrintUnformatted(input);
        char *r = malloc(512 + (inp_str ? strlen(inp_str) : 0));
        snprintf(r, 512 + (inp_str ? strlen(inp_str) : 0),
                 "[PLAN MODE] would call %s with %s", name, inp_str ? inp_str : "{}");
        free(inp_str);
        return r;
    }

    if (strcmp(name, "read_file") == 0 && path)
        return tool_read_file(cJSON_GetStringValue(path));
    if (strcmp(name, "write_file") == 0 && path)
        return tool_write_file(cJSON_GetStringValue(path),
                               content ? cJSON_GetStringValue(content) : "");
    if (strcmp(name, "bash") == 0 && command)
        return tool_bash(cJSON_GetStringValue(command));
    if (strcmp(name, "list_dir") == 0 && path)
        return tool_list_dir(cJSON_GetStringValue(path));
    if (strcmp(name, "save_memory") == 0 && key && value)
        return tool_save_memory(cJSON_GetStringValue(key), cJSON_GetStringValue(value));
    if (strcmp(name, "recall_memory") == 0)
        return tool_recall_memory();
    if (strcmp(name, "spawn_agent") == 0 && task)
        return tool_spawn_agent(cJSON_GetStringValue(task), NULL);

    for (int i = 0; i < g_n_dyn_tools; i++) {
        if (strcmp(g_dyn_tools[i].name, name) == 0)
            return exec_dyn_tool(&g_dyn_tools[i], input);
    }

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
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    if (sz > 16 * 1024) sz = 16 * 1024;
    char *buf = malloc(sz + 1);
    fread(buf, 1, sz, f);
    buf[sz] = 0;
    fclose(f);
    return buf;
}

/* ---------- compaction ---------- */

static cJSON *compact_messages(const char *api_key, cJSON *messages) {
    int n = cJSON_GetArraySize(messages);
    if (n < COMPACT_AFTER_MESSAGES) return messages;
    /* find a safe start index: must be role=assistant so we can prepend a user summary */
    int start = n - COMPACT_KEEP_LAST;
    while (start < n) {
        cJSON *m = cJSON_GetArrayItem(messages, start);
        const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(m, "role"));
        if (role && strcmp(role, "assistant") == 0) break;
        start++;
    }
    if (start >= n) return messages;
    if (start < 4) return messages;

    /* build summarization request: first `start` messages */
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "model", MODEL_COMPACT);
    cJSON_AddNumberToObject(body, "max_tokens", 1500);
    cJSON_AddStringToObject(body, "system",
        "You compress conversation history for an AI agent. "
        "Extract: (1) key facts, (2) decisions made, (3) files created/modified, "
        "(4) pending work, (5) errors to avoid. Be dense. <=400 words.");
    cJSON *sub = cJSON_CreateArray();
    for (int i = 0; i < start; i++) {
        cJSON_AddItemToArray(sub, cJSON_Duplicate(cJSON_GetArrayItem(messages, i), 1));
    }
    cJSON *ask = cJSON_CreateObject();
    cJSON_AddStringToObject(ask, "role", "user");
    cJSON_AddStringToObject(ask, "content", "Summarize the above for context compaction.");
    cJSON_AddItemToArray(sub, ask);
    cJSON_AddItemToObject(body, "messages", sub);

    char *bs = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    logerr("[compact] summarizing %d messages...\n", start);
    char *resp = claude_api(api_key, bs);
    free(bs);
    if (!resp) return messages;

    cJSON *rj = cJSON_Parse(resp);
    free(resp);
    if (!rj) return messages;
    cJSON *content = cJSON_GetObjectItem(rj, "content");
    char *summary = NULL;
    cJSON *block;
    cJSON_ArrayForEach(block, content) {
        const char *ty = cJSON_GetStringValue(cJSON_GetObjectItem(block, "type"));
        if (ty && strcmp(ty, "text") == 0) {
            const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(block, "text"));
            if (t) summary = strdup(t);
            break;
        }
    }
    cJSON_Delete(rj);
    if (!summary) return messages;

    /* rebuild: [user: summary] + messages[start..n-1] */
    cJSON *nm = cJSON_CreateArray();
    char *wrapped = malloc(strlen(summary) + 64);
    sprintf(wrapped, "[COMPACTED CONTEXT]\n%s", summary);
    cJSON *sm = cJSON_CreateObject();
    cJSON_AddStringToObject(sm, "role", "user");
    cJSON_AddStringToObject(sm, "content", wrapped);
    cJSON_AddItemToArray(nm, sm);
    free(wrapped);
    free(summary);
    for (int i = start; i < n; i++) {
        cJSON_AddItemToArray(nm, cJSON_Duplicate(cJSON_GetArrayItem(messages, i), 1));
    }
    cJSON_Delete(messages);
    logerr("[compact] %d -> %d messages\n", n, cJSON_GetArraySize(nm));
    return nm;
}

/* ---------- system prompt ---------- */

static cJSON *build_system_array(const char *memory) {
    cJSON *arr = cJSON_CreateArray();
    cJSON *base = cJSON_CreateObject();
    cJSON_AddStringToObject(base, "type", "text");
    cJSON_AddStringToObject(base, "text",
        "You are mini-agent-c v3, a minimal autonomous agent implemented in ~900 lines of C. "
        "You have tools: read_file, write_file, bash, list_dir, save_memory, recall_memory, spawn_agent, "
        "plus any dynamic tools loaded from .agent/tools/*.sh. "
        "Work inside the current directory only. Paths with '..' or outside the workspace are rejected. "
        "Dangerous bash patterns (rm -rf /, sudo, dd, etc.) are blocked. "
        "For independent sub-tasks, prefer spawn_agent so the main context stays lean. "
        "When the task is complete, respond with a concise text summary and STOP calling tools.");
    cJSON_AddItemToArray(arr, base);
    if (memory && *memory) {
        cJSON *mb = cJSON_CreateObject();
        cJSON_AddStringToObject(mb, "type", "text");
        char *wrap = malloc(strlen(memory) + 128);
        sprintf(wrap, "# Persistent memory\n%s", memory);
        cJSON_AddStringToObject(mb, "text", wrap);
        free(wrap);
        /* cache the memory block */
        cJSON *cc = cJSON_CreateObject();
        cJSON_AddStringToObject(cc, "type", "ephemeral");
        cJSON_AddItemToObject(mb, "cache_control", cc);
        cJSON_AddItemToArray(arr, mb);
    } else {
        /* cache the base block */
        cJSON *cc = cJSON_CreateObject();
        cJSON_AddStringToObject(cc, "type", "ephemeral");
        cJSON_AddItemToObject(base, "cache_control", cc);
    }
    return arr;
}

/* ---------- main ---------- */

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options] \"<task>\"\n"
        "  --model MODEL       claude-sonnet-4-5 (default), claude-haiku-4-5, claude-opus-4-5\n"
        "  --max-turns N       turn cap (default 30)\n"
        "  --budget N          token budget cap (default 300000)\n"
        "  --plan              plan mode, no side effects\n"
        "  --sandbox           run bash under sandbox-exec (.agent/sandbox.sb)\n"
        "  --stream            enable SSE streaming (Anthropic backend only; text prints as it arrives)\n"
        "  --quiet             subagent mode (only final text to stdout)\n"
        "  --no-memory         don't load persistent memory\n"
        "  --backend B         anthropic (default) | openai (NOU/Ollama/etc)\n"
        "  --api-base URL      override API base (default: anthropic=api.anthropic.com, openai=localhost:4004)\n"
        "  --help              show this help\n",
        prog);
}

int main(int argc, char **argv) {
    realpath(argv[0], g_self_path);
    if (!getcwd(g_cwd, sizeof(g_cwd))) strcpy(g_cwd, ".");
    const char *depth_env = getenv("MINI_AGENT_DEPTH");
    if (depth_env) g_spawn_depth = atoi(depth_env);

    const char *model = MODEL_DEFAULT;
    int no_memory = 0;
    const char *task = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--help") == 0) { usage(argv[0]); return 0; }
        else if (strcmp(a, "--version") == 0) { printf("mini-agent-c v6.0\n"); return 0; }
        else if (strcmp(a, "--model") == 0 && i + 1 < argc) { model = argv[++i]; }
        else if (strcmp(a, "--max-turns") == 0 && i + 1 < argc) { g_max_turns = atoi(argv[++i]); }
        else if (strcmp(a, "--budget") == 0 && i + 1 < argc) { g_token_budget = atol(argv[++i]); }
        else if (strcmp(a, "--plan") == 0) { g_plan_mode = 1; }
        else if (strcmp(a, "--sandbox") == 0) { g_use_sandbox = 1; }
        else if (strcmp(a, "--stream") == 0) { g_stream_mode = 1; }
        else if (strcmp(a, "--quiet") == 0) { g_quiet = 1; }
        else if (strcmp(a, "--no-memory") == 0) { no_memory = 1; }
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
            /* required when api-base is not localhost; see hardening doc */
            setenv("MINI_AGENT_ALLOW_REMOTE", "1", 1);
        }
        else if (a[0] == '-') { fprintf(stderr, "unknown flag: %s\n", a); usage(argv[0]); return 1; }
        else if (!task) { task = a; }
    }
    if (!task) { usage(argv[0]); return 1; }

    const char *api_key = NULL;
    if (g_backend == BACKEND_ANTHROPIC) {
        api_key = getenv("ANTHROPIC_API_KEY");
        if (!api_key || !*api_key) {
            fprintf(stderr, "ANTHROPIC_API_KEY env var not set\n");
            return 1;
        }
    } else {
        /* OpenAI backend: key is optional (NOU/Ollama don't require it) */
        api_key = getenv("OPENAI_API_KEY");
        if (!api_key) api_key = "";

        /* Safety: if api-base points outside localhost, require explicit opt-in.
         * Reason: remote OpenAI-compat servers might be unauthenticated. */
        if (g_api_base[0]) {
            int is_local = (strstr(g_api_base, "127.0.0.1") != NULL)
                        || (strstr(g_api_base, "localhost") != NULL)
                        || (strstr(g_api_base, "://[::1]") != NULL);
            if (!is_local && !getenv("MINI_AGENT_ALLOW_REMOTE")) {
                fprintf(stderr,
                    "refused: --api-base '%s' is not localhost.\n"
                    "If you really want to talk to a remote LLM endpoint,\n"
                    "pass --allow-remote-backend AND make sure that endpoint\n"
                    "is authenticated — see docs/NOU-security-review.md.\n",
                    g_api_base);
                return 1;
            }
            if (!is_local) {
                logerr("[warn] remote backend %s — sandbox auto-enabled\n", g_api_base);
                g_use_sandbox = 1;
            }
        }
    }

    ensure_agent_dir();
    load_dynamic_tools();
    char *memory = no_memory ? NULL : load_persistent_memory();

    curl_global_init(CURL_GLOBAL_DEFAULT);

    cJSON *messages = cJSON_CreateArray();
    cJSON *um = cJSON_CreateObject();
    cJSON_AddStringToObject(um, "role", "user");
    cJSON_AddStringToObject(um, "content", task);
    cJSON_AddItemToArray(messages, um);

    cJSON *tools = build_tools_array();
    cJSON *system = build_system_array(memory);
    free(memory);

    logerr("[start] model=%s turns<=%d budget=%ld tokens depth=%d plan=%d sandbox=%d\n",
           model, g_max_turns, g_token_budget, g_spawn_depth, g_plan_mode, g_use_sandbox);

    int turn;
    int final_ok = 0;
    char *final_text = NULL;
    for (turn = 0; turn < g_max_turns; turn++) {
        if (should_stop_now()) {
            logerr("[stop] .agent/STOP detected, exiting\n");
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
        cJSON_AddItemToObject(body, "system", cJSON_Duplicate(system, 1));
        cJSON_AddItemToObject(body, "tools", cJSON_Duplicate(tools, 1));
        cJSON_AddItemToObject(body, "messages", cJSON_Duplicate(messages, 1));

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
            logerr("[api-error] %s\n", e);
            free(e);
            cJSON_Delete(rj);
            break;
        }

        cJSON *usage = cJSON_GetObjectItem(rj, "usage");
        if (usage) {
            g_total_in_tokens += (long)cJSON_GetNumberValue(cJSON_GetObjectItem(usage, "input_tokens"));
            g_total_out_tokens += (long)cJSON_GetNumberValue(cJSON_GetObjectItem(usage, "output_tokens"));
            cJSON *crc = cJSON_GetObjectItem(usage, "cache_read_input_tokens");
            cJSON *ccc = cJSON_GetObjectItem(usage, "cache_creation_input_tokens");
            if (!g_quiet && (crc || ccc)) {
                logerr("[cache] read=%g create=%g\n",
                       cJSON_GetNumberValue(crc), cJSON_GetNumberValue(ccc));
            }
        }

        cJSON *content = cJSON_GetObjectItem(rj, "content");
        if (!content) { logerr("[err] no content\n"); cJSON_Delete(rj); break; }

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
                if (t) {
                    free(final_text);
                    final_text = strdup(t);
                    /* When streaming is on, text was already printed live — skip to avoid duplicate. */
                    if (!g_quiet && !g_stream_mode) printf("\n[assistant] %s\n", t);
                }
            } else if (strcmp(type, "tool_use") == 0) {
                has_tool = 1;
                const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(block, "name"));
                const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(block, "id"));
                cJSON *input = cJSON_GetObjectItem(block, "input");
                char *inp_str = cJSON_PrintUnformatted(input);
                logerr("[tool] %s %.200s\n", name, inp_str ? inp_str : "");
                char *out = execute_tool(name, input);
                audit(name, inp_str, out);
                free(inp_str);
                size_t outlen = strlen(out);
                if (outlen > MAX_TOOL_OUT_BYTES) out[MAX_TOOL_OUT_BYTES] = 0;
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

    /* subagent mode: print only final assistant text */
    if (g_quiet && final_text) {
        printf("%s\n", final_text);
    }
    free(final_text);

    cJSON_Delete(messages);
    cJSON_Delete(tools);
    cJSON_Delete(system);
    curl_global_cleanup();
    return final_ok ? 0 : 2;
}
