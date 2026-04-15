/* bot.c — agent-bot: Telegram + LINE + Web → agent.v10 (pure C)
 * Deps: libmicrohttpd, libcurl, libsqlite3, libssl, cJSON (bundled)
 */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>

#include <microhttpd.h>
#include <curl/curl.h>
#include <sqlite3.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>

#include "cJSON.h"

/* ── Config ──────────────────────────────────────────────────────────── */
static const char *TELEGRAM_TOKEN;
static const char *LINE_CHANNEL_SECRET;
static const char *LINE_CHANNEL_ACCESS_TOKEN;
static const char *ANTHROPIC_API_KEY;
static const char *AGENT_PATH  = "/app/agent.v10";
static const char *AGENT_DIR   = "/app";
static const char *AGENT_API_BASE = NULL;
static const char *WEB_TOKEN;
static const char *AUTHORIZED_USERS;   /* comma-separated or "*" */
static const char *PON_BASE_URL = "https://pon-sign.fly.dev";
static int PORT = 8080;
static const char *DB_PATH;

#define MAX_TASK_LEN      2000
#define RATE_LIMIT_MAX    10
#define RATE_LIMIT_WIN    60
#define MAX_CONTEXT_TURNS 10
#define MAX_USERS         256
#define CHUNK_SIZE        4000

/* ── SQLite ──────────────────────────────────────────────────────────── */
static sqlite3        *g_db;
static pthread_mutex_t g_db_mu = PTHREAD_MUTEX_INITIALIZER;

static void db_init(void) {
    if (sqlite3_open(DB_PATH, &g_db) != SQLITE_OK) {
        fprintf(stderr, "[db] open: %s\n", sqlite3_errmsg(g_db)); exit(1);
    }
    sqlite3_exec(g_db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
    sqlite3_exec(g_db,
        "CREATE TABLE IF NOT EXISTS context("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  user_id TEXT NOT NULL, role TEXT NOT NULL, text TEXT NOT NULL,"
        "  created_at INTEGER NOT NULL DEFAULT(strftime('%s','now')))",
        NULL, NULL, NULL);
    sqlite3_exec(g_db,
        "CREATE INDEX IF NOT EXISTS idx_ctx ON context(user_id,id)",
        NULL, NULL, NULL);
    /* Pasha-style expense log */
    sqlite3_exec(g_db,
        "CREATE TABLE IF NOT EXISTS expenses("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  user_id TEXT NOT NULL,"
        "  date TEXT,"
        "  amount REAL,"
        "  currency TEXT DEFAULT 'JPY',"
        "  category TEXT,"
        "  vendor TEXT,"
        "  memo TEXT,"
        "  ocr_text TEXT,"
        "  created_at INTEGER NOT NULL DEFAULT(strftime('%s','now')))",
        NULL, NULL, NULL);
}

/* Returns malloc'd "[Previous conversation:\n…]\n" or NULL */
static char *db_load_ctx(const char *uid) {
    pthread_mutex_lock(&g_db_mu);
    sqlite3_stmt *s;
    sqlite3_prepare_v2(g_db,
        "SELECT role,text FROM context WHERE user_id=?"
        " ORDER BY id DESC LIMIT ?", -1, &s, NULL);
    sqlite3_bind_text(s, 1, uid, -1, SQLITE_STATIC);
    sqlite3_bind_int (s, 2, MAX_CONTEXT_TURNS * 2);

    struct { char role[8]; char *text; } rows[MAX_CONTEXT_TURNS * 2];
    int n = 0;
    while (sqlite3_step(s) == SQLITE_ROW && n < MAX_CONTEXT_TURNS * 2) {
        strncpy(rows[n].role, (const char*)sqlite3_column_text(s,0), 7);
        rows[n].role[7] = '\0';
        rows[n].text = strdup((const char*)sqlite3_column_text(s,1));
        n++;
    }
    sqlite3_finalize(s);
    pthread_mutex_unlock(&g_db_mu);

    if (n == 0) return NULL;

    size_t cap = 64;
    for (int i = 0; i < n; i++) cap += strlen(rows[i].text) + 32;
    char *out = malloc(cap);
    strcpy(out, "[Previous conversation:\n");
    for (int i = n - 1; i >= 0; i--) {
        strcat(out, strcmp(rows[i].role,"user") == 0 ? "User: " : "Agent: ");
        char tmp[501]; strncpy(tmp, rows[i].text, 500); tmp[500] = '\0';
        strcat(out, tmp); strcat(out, "\n");
        free(rows[i].text);
    }
    strcat(out, "]\n");
    return out;
}

static void db_save_ctx(const char *uid, const char *task, const char *res) {
    pthread_mutex_lock(&g_db_mu);
    sqlite3_stmt *s;
    sqlite3_prepare_v2(g_db,
        "INSERT INTO context(user_id,role,text) VALUES(?,?,?)", -1, &s, NULL);
    char t[1001]; strncpy(t, task, 1000); t[1000] = '\0';
    char r[2001]; strncpy(r, res,  2000); r[2000] = '\0';
    sqlite3_bind_text(s,1,uid,-1,SQLITE_STATIC);
    sqlite3_bind_text(s,2,"user",-1,SQLITE_STATIC);
    sqlite3_bind_text(s,3,t,-1,SQLITE_STATIC);
    sqlite3_step(s); sqlite3_reset(s);
    sqlite3_bind_text(s,2,"agent",-1,SQLITE_STATIC);
    sqlite3_bind_text(s,3,r,-1,SQLITE_STATIC);
    sqlite3_step(s); sqlite3_finalize(s);
    pthread_mutex_unlock(&g_db_mu);
}

static void db_clear_ctx(const char *uid) {
    pthread_mutex_lock(&g_db_mu);
    sqlite3_stmt *s;
    sqlite3_prepare_v2(g_db,"DELETE FROM context WHERE user_id=?",-1,&s,NULL);
    sqlite3_bind_text(s,1,uid,-1,SQLITE_STATIC);
    sqlite3_step(s); sqlite3_finalize(s);
    pthread_mutex_unlock(&g_db_mu);
}

static int db_ctx_count(const char *uid) {
    pthread_mutex_lock(&g_db_mu);
    sqlite3_stmt *s;
    sqlite3_prepare_v2(g_db,
        "SELECT COUNT(*)/2 FROM context WHERE user_id=?",-1,&s,NULL);
    sqlite3_bind_text(s,1,uid,-1,SQLITE_STATIC);
    int c = (sqlite3_step(s) == SQLITE_ROW) ? sqlite3_column_int(s,0) : 0;
    sqlite3_finalize(s);
    pthread_mutex_unlock(&g_db_mu);
    return c;
}

/* ── ANSI strip ──────────────────────────────────────────────────────── */
static void strip_ansi(const char *in, char *out, size_t cap) {
    size_t j = 0;
    for (size_t i = 0; in[i] && j < cap - 1; ) {
        if (in[i] == '\x1b') {
            i++;
            if (in[i] == '[') { i++; while (in[i] && (in[i]<'@'||in[i]>'~')) i++; if(in[i]) i++; }
        } else { out[j++] = in[i++]; }
    }
    out[j] = '\0';
}

/* ── Per-user state ──────────────────────────────────────────────────── */
struct UserState {
    char            uid[64];
    bool            in_use;
    pthread_mutex_t proc_lock;
    pid_t           proc_pid;
    pthread_mutex_t rate_lock;
    time_t          rate_times[RATE_LIMIT_MAX];
    int             rate_n;
    /* web SSE pipe */
    pthread_mutex_t stream_lock;
    int             pipe_r, pipe_w;
    bool            stream_active;
};

static struct UserState  g_us[MAX_USERS];
static pthread_mutex_t   g_us_lock = PTHREAD_MUTEX_INITIALIZER;

static struct UserState *get_state(const char *uid) {
    pthread_mutex_lock(&g_us_lock);
    for (int i = 0; i < MAX_USERS; i++)
        if (g_us[i].in_use && strcmp(g_us[i].uid, uid) == 0) {
            pthread_mutex_unlock(&g_us_lock); return &g_us[i]; }
    for (int i = 0; i < MAX_USERS; i++) {
        if (!g_us[i].in_use) {
            memset(&g_us[i], 0, sizeof g_us[i]);
            strncpy(g_us[i].uid, uid, 63);
            g_us[i].in_use = true; g_us[i].proc_pid = -1;
            g_us[i].pipe_r = -1;  g_us[i].pipe_w   = -1;
            pthread_mutex_init(&g_us[i].proc_lock,   NULL);
            pthread_mutex_init(&g_us[i].rate_lock,   NULL);
            pthread_mutex_init(&g_us[i].stream_lock, NULL);
            pthread_mutex_unlock(&g_us_lock); return &g_us[i];
        }
    }
    pthread_mutex_unlock(&g_us_lock); return NULL;
}

static bool is_authorized(const char *uid) {
    if (!AUTHORIZED_USERS || strcmp(AUTHORIZED_USERS,"*")==0) return true;
    char buf[1024]; strncpy(buf, AUTHORIZED_USERS, 1023); buf[1023]='\0';
    for (char *t = strtok(buf,","); t; t = strtok(NULL,","))
        if (strcmp(t,uid)==0) return true;
    return false;
}

static bool check_rate(struct UserState *st) {
    pthread_mutex_lock(&st->rate_lock);
    time_t now = time(NULL); int k = 0;
    for (int i = 0; i < st->rate_n; i++)
        if (now - st->rate_times[i] <= RATE_LIMIT_WIN)
            st->rate_times[k++] = st->rate_times[i];
    st->rate_n = k;
    if (st->rate_n >= RATE_LIMIT_MAX) { pthread_mutex_unlock(&st->rate_lock); return false; }
    st->rate_times[st->rate_n++] = now;
    pthread_mutex_unlock(&st->rate_lock); return true;
}

static bool is_busy(struct UserState *st) {
    pthread_mutex_lock(&st->proc_lock);
    bool b = false;
    if (st->proc_pid > 0) {
        if (waitpid(st->proc_pid, NULL, WNOHANG) == 0) b = true;
        else st->proc_pid = -1;
    }
    pthread_mutex_unlock(&st->proc_lock); return b;
}

static bool cancel_agent(const char *uid) {
    struct UserState *st = get_state(uid);
    if (!st) return false;
    pthread_mutex_lock(&st->proc_lock);
    if (st->proc_pid > 0) { kill(st->proc_pid, SIGKILL); st->proc_pid = -1;
        pthread_mutex_unlock(&st->proc_lock); return true; }
    pthread_mutex_unlock(&st->proc_lock); return false;
}

/* ── libcurl helpers ─────────────────────────────────────────────────── */
struct Buf { char *d; size_t n; };

static size_t write_cb(void *p, size_t sz, size_t nm, void *ud) {
    struct Buf *b = ud; size_t add = sz*nm;
    b->d = realloc(b->d, b->n + add + 1);
    memcpy(b->d + b->n, p, add); b->n += add; b->d[b->n] = '\0';
    return add;
}

static char *http_get(const char *url) {
    CURL *c = curl_easy_init(); if (!c) return NULL;
    struct Buf b = {malloc(1), 0};
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 35L);
    CURLcode r = curl_easy_perform(c); curl_easy_cleanup(c);
    if (r != CURLE_OK) { free(b.d); return NULL; }
    return b.d;
}

static char *http_post_json(const char *url, const char *json,
                             const char *auth_hdr) {
    CURL *c = curl_easy_init(); if (!c) return NULL;
    struct Buf b = {malloc(1), 0};
    struct curl_slist *hdr = curl_slist_append(NULL,"Content-Type: application/json");
    if (auth_hdr) hdr = curl_slist_append(hdr, auth_hdr);
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, json);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
    CURLcode r = curl_easy_perform(c);
    curl_slist_free_all(hdr); curl_easy_cleanup(c);
    if (r != CURLE_OK) { free(b.d); return NULL; }
    return b.d;
}

/* ── System context (self-awareness) ────────────────────────────────── */
static char *make_sys_ctx(const char *uid, const char *channel) {
    time_t now = time(NULL);
    char tbuf[64];
    struct tm *tm = gmtime(&now);
    strftime(tbuf, sizeof tbuf, "%Y-%m-%d %H:%M:%S UTC", tm);

    /* hostname from env or fallback */
    const char *hostname = getenv("FLY_APP_NAME");
    if (!hostname || !*hostname) hostname = "agent-bot";
    const char *region = getenv("FLY_REGION");
    if (!region || !*region) region = "local";

    char *sys;
    asprintf(&sys,
        "[System context — read this before answering]\n"
        "You are agent-bot, an autonomous AI agent.\n"
        "Runtime  : agent.v10 (C) + HTTP server (C/libmicrohttpd)\n"
        "Deployed : Fly.io app=%s region=%s\n"
        "Storage  : SQLite on /data volume\n"
        "Interfaces: Telegram, LINE webhook, Web chat (SSE streaming)\n"
        "Channel  : %s\n"
        "User ID  : %s\n"
        "Time     : %s\n"
        "Capabilities: file read/write, code execution, web search,\n"
        "              API calls, multi-step reasoning, image analysis\n"
        "\n"
        "── Pon API (電子契約・署名) ── base: %s\n"
        "POST /api/contracts          {title,content,creator_name,creator_email,client_name,client_email}\n"
        "                             → {id, token, sign_url}  # share sign_url with client\n"
        "GET  /api/contracts/{id}     → contract details\n"
        "GET  /api/contracts/{id}/pdf → PDF binary\n"
        "GET  /api/templates          → [{id,name,description,content}, ...] NDA/業務委託/保守など\n"
        "POST /api/sign/{token}       {signer:\"creator\"|\"client\", signature:base64PNG, agreement_text}\n"
        "GET  /api/contracts/{id}/verify → audit trail\n"
        "Usage: curl -X POST %s/api/contracts -H 'Content-Type: application/json' -d '{...}'\n"
        "\n"
        "── Pasha経費ログ (local SQLite) ──\n"
        "DBパス: %s  テーブル: expenses\n"
        "カラム: date, amount, currency, category, vendor, memo, ocr_text\n"
        "例: INSERT INTO expenses(user_id,date,amount,category,vendor) VALUES('%s','2026-01-01',1000,'交通費','JR東日本');\n"
        "[/System context]\n\n",
        hostname, region, channel, uid, tbuf,
        PON_BASE_URL, PON_BASE_URL, DB_PATH, uid);
    return sys;
}

/* ── OCR: call Claude Vision API to extract text from image ─────────── */
/* Returns malloc'd text, or NULL on failure. Caller frees. */
static char *ocr_image(const char *file_path) {
    if (!ANTHROPIC_API_KEY || !*ANTHROPIC_API_KEY) return NULL;

    /* Detect MIME type from extension */
    const char *ext = strrchr(file_path, '.');
    const char *mime = "image/jpeg";
    if (ext) {
        if (!strcasecmp(ext,".png"))  mime = "image/png";
        else if (!strcasecmp(ext,".gif"))  mime = "image/gif";
        else if (!strcasecmp(ext,".webp")) mime = "image/webp";
        else if (!strcasecmp(ext,".pdf")) return NULL; /* skip PDF */
    }

    /* Read file */
    FILE *f = fopen(file_path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long fsz = ftell(f); rewind(f);
    if (fsz <= 0 || fsz > 10 * 1024 * 1024) { fclose(f); return NULL; } /* >10MB skip */
    uint8_t *raw = malloc(fsz);
    if (fread(raw, 1, fsz, f) != (size_t)fsz) { fclose(f); free(raw); return NULL; }
    fclose(f);

    /* base64 encode */
    BIO *b64 = BIO_new(BIO_f_base64());
    BIO *mem = BIO_new(BIO_s_mem());
    BIO_push(b64, mem);
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(b64, raw, fsz); BIO_flush(b64);
    BUF_MEM *bm; BIO_get_mem_ptr(b64, &bm);
    char *b64data = malloc(bm->length + 1);
    memcpy(b64data, bm->data, bm->length); b64data[bm->length] = '\0';
    BIO_free_all(b64); free(raw);

    /* Build Claude API request */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", "claude-haiku-4-5-20251001");
    cJSON_AddNumberToObject(root, "max_tokens", 1024);
    cJSON *msgs = cJSON_CreateArray();
    cJSON *msg  = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "user");
    cJSON *content = cJSON_CreateArray();
    /* image block */
    cJSON *img_blk = cJSON_CreateObject();
    cJSON_AddStringToObject(img_blk, "type", "image");
    cJSON *src = cJSON_CreateObject();
    cJSON_AddStringToObject(src, "type", "base64");
    cJSON_AddStringToObject(src, "media_type", mime);
    cJSON_AddStringToObject(src, "data", b64data);
    cJSON_AddItemToObject(img_blk, "source", src);
    cJSON_AddItemToArray(content, img_blk);
    /* text block */
    cJSON *txt_blk = cJSON_CreateObject();
    cJSON_AddStringToObject(txt_blk, "type", "text");
    cJSON_AddStringToObject(txt_blk, "text",
        "この画像に含まれるテキストをすべて抽出してください。"
        "レシートや領収書の場合は日付・金額・店舗名・品目を構造化して出力してください。"
        "テキストのみ返答し、説明は不要です。");
    cJSON_AddItemToArray(content, txt_blk);
    cJSON_AddItemToObject(msg, "content", content);
    cJSON_AddItemToArray(msgs, msg);
    cJSON_AddItemToObject(root, "messages", msgs);
    char *req_body = cJSON_PrintUnformatted(root); cJSON_Delete(root);
    free(b64data);

    /* POST to Anthropic */
    CURL *c = curl_easy_init();
    if (!c) { free(req_body); return NULL; }
    struct Buf resp = {malloc(1), 0};
    char auth_hdr[256]; snprintf(auth_hdr, sizeof auth_hdr, "x-api-key: %s", ANTHROPIC_API_KEY);
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    hdrs = curl_slist_append(hdrs, auth_hdr);
    hdrs = curl_slist_append(hdrs, "anthropic-version: 2023-06-01");
    curl_easy_setopt(c, CURLOPT_URL, "https://api.anthropic.com/v1/messages");
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, req_body);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    CURLcode rc = curl_easy_perform(c);
    curl_slist_free_all(hdrs); curl_easy_cleanup(c); free(req_body);

    if (rc != CURLE_OK) { free(resp.d); return NULL; }

    /* Parse response: content[0].text */
    cJSON *rj = cJSON_Parse(resp.d); free(resp.d);
    char *result = NULL;
    if (rj) {
        cJSON *cnt = cJSON_GetObjectItem(rj, "content");
        if (cJSON_IsArray(cnt) && cJSON_GetArraySize(cnt) > 0) {
            cJSON *first = cJSON_GetArrayItem(cnt, 0);
            const char *txt = cJSON_GetStringValue(cJSON_GetObjectItem(first, "text"));
            if (txt) result = strdup(txt);
        }
        cJSON_Delete(rj);
    }
    return result;
}

/* ── Build task with context prefix ─────────────────────────────────── */
static char *build_task_ch(const char *uid, const char *task, const char *channel) {
    char *sys = make_sys_ctx(uid, channel);
    char *ctx = db_load_ctx(uid);

    size_t l = strlen(sys) + (ctx ? strlen(ctx) : 0) + strlen(task) + 4;
    char *out = malloc(l);
    snprintf(out, l, "%s%s%s", sys, ctx ? ctx : "", task);
    free(sys);
    if (ctx) free(ctx);
    return out;
}

static char *build_task(const char *uid, const char *task) {
    return build_task_ch(uid, task, "unknown");
}

/* ── Spawn child env ─────────────────────────────────────────────────── */
static char **make_envp(char **api_kv_out) {
    extern char **environ;
    int ec = 0; while (environ[ec]) ec++;
    char **ep = malloc((ec + 2) * sizeof(char*));
    int ei = 0;
    for (int i = 0; i < ec; i++) {
        if (strncmp(environ[i],"ANTHROPIC_API_KEY=",18)==0) continue;
        ep[ei++] = environ[i];
    }
    *api_kv_out = NULL;
    if (ANTHROPIC_API_KEY && *ANTHROPIC_API_KEY) {
        char *kv; asprintf(&kv,"ANTHROPIC_API_KEY=%s",ANTHROPIC_API_KEY);
        ep[ei++] = *api_kv_out = kv;
    }
    ep[ei] = NULL; return ep;
}

/* ── Agent: blocking (Telegram / LINE) ──────────────────────────────── */
static char *run_agent(const char *uid, const char *task, const char *channel,
                       const char *file_path) {
    struct UserState *st = get_state(uid);
    if (!st) return strdup("❌ Too many users");

    /* OCR preprocessing (Pasha-style) */
    char *ocr = file_path ? ocr_image(file_path) : NULL;
    char *augmented_task = NULL;
    if (ocr) {
        asprintf(&augmented_task, "%s\n\n[OCR抽出テキスト from %s]\n%s\n[/OCR]",
                 task, file_path, ocr);
        free(ocr);
    }
    const char *final_task = augmented_task ? augmented_task : task;

    char *full = build_task_ch(uid, final_task, channel);
    if (augmented_task) free(augmented_task);
    const char *av[6] = {0};
    int ai = 0;
    av[ai++] = AGENT_PATH;
    av[ai++] = "--quiet";
    if (AGENT_API_BASE && *AGENT_API_BASE) {
        av[ai++] = "--api-base"; av[ai++] = AGENT_API_BASE;
    }
    av[ai++] = full;
    char *api_kv; char **ep = make_envp(&api_kv);

    int pout[2]; pipe(pout);
    pid_t pid = fork();
    if (pid == 0) {
        close(pout[0]);
        dup2(pout[1], STDOUT_FILENO); dup2(pout[1], STDERR_FILENO);
        close(pout[1]);
        if (AGENT_DIR) chdir(AGENT_DIR);
        execve(AGENT_PATH, (char*const*)av, ep);
        _exit(1);
    }
    close(pout[1]);
    pthread_mutex_lock(&st->proc_lock); st->proc_pid = pid; pthread_mutex_unlock(&st->proc_lock);

    char raw[65536] = ""; size_t rl = 0; char tmp[4096]; ssize_t n;
    while ((n = read(pout[0], tmp, sizeof tmp)) > 0) {
        if (rl + (size_t)n < sizeof raw - 1) { memcpy(raw+rl, tmp, n); rl += n; }
    }
    raw[rl] = '\0'; close(pout[0]);
    waitpid(pid, NULL, 0);
    pthread_mutex_lock(&st->proc_lock); st->proc_pid = -1; pthread_mutex_unlock(&st->proc_lock);

    char clean[65536]; strip_ansi(raw, clean, sizeof clean);
    db_save_ctx(uid, task, clean);

    free(full); if (api_kv) free(api_kv); free(ep);
    return strdup(*clean ? clean : "(no output)");
}

/* ── Agent: streaming (Web SSE) ─────────────────────────────────────── */
struct StreamArgs { char uid[64]; char *task; char *file_path; int pipe_w; };

static void *stream_thread(void *arg) {
    struct StreamArgs *sa = arg;
    struct UserState *st = get_state(sa->uid);

    /* OCR preprocessing (Pasha-style) */
    char *ocr = sa->file_path ? ocr_image(sa->file_path) : NULL;
    char *augmented = NULL;
    if (ocr) {
        asprintf(&augmented, "%s\n\n[OCR抽出テキスト from %s]\n%s\n[/OCR]",
                 sa->task, sa->file_path, ocr);
        free(ocr);
    }
    const char *ftask = augmented ? augmented : sa->task;
    char *full = build_task_ch(sa->uid, ftask, "Web");
    if (augmented) free(augmented);
    const char *av[5] = {0};
    int ai2 = 0;
    av[ai2++] = AGENT_PATH;
    if (AGENT_API_BASE && *AGENT_API_BASE) {
        av[ai2++] = "--api-base"; av[ai2++] = AGENT_API_BASE;
    }
    av[ai2++] = full;
    char *api_kv; char **ep = make_envp(&api_kv);

    int pout[2]; pipe(pout);
    pid_t pid = fork();
    if (pid == 0) {
        close(pout[0]);
        dup2(pout[1], STDOUT_FILENO); dup2(pout[1], STDERR_FILENO);
        close(pout[1]);
        if (AGENT_DIR) chdir(AGENT_DIR);
        execve(AGENT_PATH, (char*const*)av, ep);
        _exit(1);
    }
    close(pout[1]);
    if (st) { pthread_mutex_lock(&st->proc_lock); st->proc_pid = pid; pthread_mutex_unlock(&st->proc_lock); }

    char accum[65536] = ""; size_t alen = 0;
    char line[4096]; size_t li = 0; char byte;

    while (read(pout[0], &byte, 1) > 0) {
        line[li++] = byte;
        if (byte == '\n' || li >= sizeof line - 2) {
            line[li] = '\0';
            char clean[4096]; strip_ansi(line, clean, sizeof clean);
            if (alen + strlen(clean) < sizeof accum - 1) { strcat(accum, clean); alen += strlen(clean); }

            /* strip trailing newline for display */
            char disp[4096]; strncpy(disp, clean, sizeof disp - 1); disp[sizeof disp-1]='\0';
            size_t dl = strlen(disp);
            while (dl > 0 && (disp[dl-1]=='\n'||disp[dl-1]=='\r')) disp[--dl]='\0';

            cJSON *ev = cJSON_CreateObject();
            cJSON_AddStringToObject(ev, "type", "line");
            cJSON_AddStringToObject(ev, "text", disp);
            char *js = cJSON_PrintUnformatted(ev); cJSON_Delete(ev);
            char sse[8192]; snprintf(sse, sizeof sse, "data: %s\n\n", js); free(js);
            if (write(sa->pipe_w, sse, strlen(sse)) < 0) break;
            li = 0;
        }
    }
    close(pout[0]);
    waitpid(pid, NULL, 0);
    if (st) { pthread_mutex_lock(&st->proc_lock); st->proc_pid = -1; pthread_mutex_unlock(&st->proc_lock); }

    db_save_ctx(sa->uid, sa->task, accum);

    const char *done = "data: {\"type\":\"done\"}\n\nretry: 0\n";
    write(sa->pipe_w, done, strlen(done));
    close(sa->pipe_w);

    if (st) { pthread_mutex_lock(&st->stream_lock); st->stream_active = false; st->pipe_w = -1; pthread_mutex_unlock(&st->stream_lock); }

    free(full); free(sa->task); if (sa->file_path) free(sa->file_path);
    if (api_kv) free(api_kv); free(ep); free(sa);
    return NULL;
}

/* ── HMAC-SHA256 → base64 ────────────────────────────────────────────── */
static char *hmac_b64(const char *key, const uint8_t *data, size_t dlen) {
    uint8_t hash[32]; unsigned int hl = 32;
    HMAC(EVP_sha256(), key, (int)strlen(key), data, dlen, hash, &hl);
    BIO *b64 = BIO_new(BIO_f_base64());
    BIO *mem = BIO_new(BIO_s_mem());
    BIO_push(b64, mem);
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(b64, hash, hl); BIO_flush(b64);
    BUF_MEM *bm; BIO_get_mem_ptr(b64, &bm);
    char *out = malloc(bm->length + 1);
    memcpy(out, bm->data, bm->length); out[bm->length] = '\0';
    BIO_free_all(b64); return out;
}

/* ── Telegram ────────────────────────────────────────────────────────── */
static void tg_send(long long chat_id, const char *text) {
    if (!TELEGRAM_TOKEN || !*TELEGRAM_TOKEN) return;
    size_t len = strlen(text);
    for (size_t off = 0; off < (len ? len : 1); off += CHUNK_SIZE) {
        char chunk[CHUNK_SIZE + 1];
        size_t n = len - off; if (n > CHUNK_SIZE) n = CHUNK_SIZE;
        memcpy(chunk, text + off, n); chunk[n] = '\0';
        cJSON *b = cJSON_CreateObject();
        cJSON_AddNumberToObject(b, "chat_id", (double)chat_id);
        cJSON_AddStringToObject(b, "text", chunk);
        char *js = cJSON_PrintUnformatted(b); cJSON_Delete(b);
        char url[256];
        snprintf(url, sizeof url, "https://api.telegram.org/bot%s/sendMessage", TELEGRAM_TOKEN);
        char *r = http_post_json(url, js, NULL); free(js); if (r) free(r);
    }
}

struct TgArgs { long long chat_id; char uid[64]; char task[MAX_TASK_LEN+1]; char *file_path; };

static void *tg_task_thread(void *arg) {
    struct TgArgs *a = arg;
    char *res = run_agent(a->uid, a->task, "Telegram", a->file_path);
    tg_send(a->chat_id, res ? res : "❌ No result");
    if (res) free(res); if (a->file_path) free(a->file_path); free(a); return NULL;
}

static void tg_handle(cJSON *upd) {
    cJSON *msg = cJSON_GetObjectItem(upd, "message");
    if (!msg) msg = cJSON_GetObjectItem(upd, "edited_message");
    if (!msg) return;
    cJSON *chat = cJSON_GetObjectItem(msg, "chat");
    cJSON *from = cJSON_GetObjectItem(msg, "from");
    if (!chat || !from) return;
    long long chat_id = (long long)cJSON_GetNumberValue(cJSON_GetObjectItem(chat,"id"));
    long long from_id = (long long)cJSON_GetNumberValue(cJSON_GetObjectItem(from,"id"));
    char uid[32]; snprintf(uid, sizeof uid, "%lld", from_id);
    if (!is_authorized(uid)) { tg_send(chat_id, "⛔ Unauthorized"); return; }

    const char *text = "";
    cJSON *tj = cJSON_GetObjectItem(msg, "text");
    if (tj) text = cJSON_GetStringValue(tj);
    if (!text || !*text) { cJSON *cj = cJSON_GetObjectItem(msg,"caption"); if(cj) text=cJSON_GetStringValue(cj); }
    if (!text) text = "";

    if (!strcmp(text,"/start")||!strcmp(text,"/help")) {
        char h[512]; snprintf(h, sizeof h,
            "agent.v10 bot\n\nタスクを送信してください。\n\n"
            "コマンド:\n  /help — このメッセージ\n  /status — 実行状態\n"
            "  /cancel — キャンセル\n  /clear — コンテキストリセット\n\n"
            "あなたのID: %s", uid);
        tg_send(chat_id, h); return;
    }
    if (!strcmp(text,"/status")) {
        struct UserState *st = get_state(uid); bool busy = st && is_busy(st);
        char m[128]; snprintf(m, sizeof m, busy ? "⚙️ 実行中（/cancel でキャンセル）"
            : "✅ アイドル（コンテキスト: %d ターン）", db_ctx_count(uid));
        tg_send(chat_id, m); return;
    }
    if (!strcmp(text,"/cancel")) { tg_send(chat_id, cancel_agent(uid) ? "🛑 キャンセルしました。" : "（実行中のタスクはありません）"); return; }
    if (!strcmp(text,"/clear"))  { db_clear_ctx(uid); tg_send(chat_id, "🗑 コンテキストをクリアしました。"); return; }
    if (!*text) return;

    struct UserState *st = get_state(uid);
    if (!st) { tg_send(chat_id, "❌ Internal error"); return; }
    if (!check_rate(st)) { tg_send(chat_id, "⚠️ レートリミット（60s後に再試行）"); return; }
    if (is_busy(st))      { tg_send(chat_id, "⚙️ 実行中。/cancel でキャンセル後に再送してください。"); return; }
    if (strlen(text) > MAX_TASK_LEN) { tg_send(chat_id, "⚠️ 入力が長すぎます"); return; }

    char preview[128]; snprintf(preview, sizeof preview, "⏳ 実行中...\n%.80s%s", text, strlen(text)>80?"…":"");
    tg_send(chat_id, preview);

    struct TgArgs *a = malloc(sizeof *a);
    a->chat_id = chat_id; strncpy(a->uid, uid, 63); strncpy(a->task, text, MAX_TASK_LEN);
    pthread_t t; pthread_create(&t, NULL, tg_task_thread, a); pthread_detach(t);
}

static void *tg_poll(void *arg) {
    (void)arg;
    if (!TELEGRAM_TOKEN || !*TELEGRAM_TOKEN) { printf("[tg] disabled\n"); return NULL; }
    printf("[tg] long-polling...\n");
    long long offset = 0;
    while (1) {
        char url[512];
        snprintf(url, sizeof url,
            "https://api.telegram.org/bot%s/getUpdates?offset=%lld&timeout=30",
            TELEGRAM_TOKEN, offset);
        char *resp = http_get(url);
        if (!resp) { sleep(5); continue; }
        cJSON *root = cJSON_Parse(resp); free(resp);
        if (!root) { sleep(1); continue; }
        cJSON *result = cJSON_GetObjectItem(root, "result");
        if (cJSON_IsArray(result)) {
            cJSON *upd;
            cJSON_ArrayForEach(upd, result) {
                cJSON *ij = cJSON_GetObjectItem(upd, "update_id");
                if (ij) { long long id = (long long)cJSON_GetNumberValue(ij); if (id >= offset) offset = id + 1; }
                tg_handle(upd);
            }
        }
        cJSON_Delete(root);
    }
    return NULL;
}

/* ── LINE ────────────────────────────────────────────────────────────── */
static void line_push(const char *to, const char *text, bool reply, const char *token);

static void line_push(const char *to, const char *text, bool reply, const char *token) {
    if (!LINE_CHANNEL_ACCESS_TOKEN) return;
    size_t len = strlen(text);
    cJSON *msgs = cJSON_CreateArray();
    for (int i = 0; i < 5 && (size_t)(i*2000) < (len?len:1); i++) {
        size_t off = i * 2000;
        size_t n = len - off; if (n > 2000) n = 2000;
        char chunk[2001]; memcpy(chunk, text+off, n); chunk[n]='\0';
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "type", "text");
        cJSON_AddStringToObject(m, "text", chunk);
        cJSON_AddItemToArray(msgs, m);
    }
    cJSON *body = cJSON_CreateObject();
    if (reply) cJSON_AddStringToObject(body, "replyToken", token);
    else       cJSON_AddStringToObject(body, "to", to);
    cJSON_AddItemToObject(body, "messages", msgs);
    char *js = cJSON_PrintUnformatted(body); cJSON_Delete(body);
    char auth[256]; snprintf(auth, sizeof auth, "Authorization: Bearer %s", LINE_CHANNEL_ACCESS_TOKEN);
    const char *url = reply ? "https://api.line.me/v2/bot/message/reply"
                            : "https://api.line.me/v2/bot/message/push";
    char *r = http_post_json(url, js, auth); free(js); if (r) free(r);
}

struct LineArgs { char uid[256]; char task[MAX_TASK_LEN+1]; char *file_path; };
static void *line_task_thread(void *arg) {
    struct LineArgs *a = arg;
    char *res = run_agent(a->uid, a->task, "LINE", a->file_path);
    line_push(a->uid, res ? res : "❌ No result", false, NULL);
    if (res) free(res); if (a->file_path) free(a->file_path); free(a); return NULL;
}

/* ── HTTP server ─────────────────────────────────────────────────────── */

/* Dashboard HTML (same design as bot.py) */
static const char *DASHBOARD =
"<!DOCTYPE html>\n<html lang=\"ja\">\n<head>\n"
"<meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
"<title>agent-bot</title>\n<style>\n"
":root{--bg:#090c10;--surface:#0d1117;--card:#161b22;--border:#21262d;"
"--text:#c9d1d9;--dim:#8b949e;--dim2:#484f58;"
"--blue:#58a6ff;--green:#3fb950;--red:#f85149;--yellow:#e3b341;}\n"
"*{box-sizing:border-box;margin:0;padding:0;}html,body{height:100%;}\n"
"body{background:var(--bg);color:var(--text);font-family:-apple-system,BlinkMacSystemFont,\"Segoe UI\",sans-serif;"
"display:flex;flex-direction:column;height:100vh;overflow:hidden;}\n"
"header{display:flex;align-items:center;gap:1rem;padding:.75rem 1.25rem;"
"border-bottom:1px solid var(--border);background:var(--surface);flex-shrink:0;}\n"
"header h1{font-size:1rem;font-weight:600;letter-spacing:-.01em;}header h1 span{color:var(--blue);}\n"
".dot{display:inline-block;width:7px;height:7px;border-radius:50%;}\n"
".dot.g{background:var(--green);box-shadow:0 0 6px var(--green);}.dot.r{background:var(--red);}\n"
"nav{display:flex;border-bottom:1px solid var(--border);background:var(--surface);flex-shrink:0;}\n"
"nav button{background:none;border:none;border-bottom:2px solid transparent;"
"color:var(--dim);font-size:.85rem;padding:.6rem 1.2rem;cursor:pointer;transition:color .15s,border-color .15s;}\n"
"nav button.active{color:var(--text);border-bottom-color:var(--blue);}\n"
"nav button:hover:not(.active){color:var(--text);}\n"
".panel{flex:1;overflow:hidden;display:none;flex-direction:column;}.panel.active{display:flex;}\n"
"#chat-output{flex:1;overflow-y:auto;padding:1rem 1.25rem;"
"font-family:'SF Mono','Fira Code',Monaco,Consolas,monospace;"
"font-size:.82rem;line-height:1.6;background:var(--bg);white-space:pre-wrap;word-break:break-all;"
"scrollbar-width:thin;scrollbar-color:var(--border) transparent;}\n"
"#chat-output::-webkit-scrollbar{width:4px;}"
"#chat-output::-webkit-scrollbar-thumb{background:var(--border);border-radius:2px;}\n"
".msg-user{color:var(--blue);margin-top:.75rem;}.msg-user::before{content:'❯ ';}\n"
".msg-agent{color:var(--text);}.msg-system{color:var(--dim);font-style:italic;}.msg-error{color:var(--red);}\n"
"#chat-bar{display:flex;align-items:center;gap:.6rem;padding:.75rem 1rem;"
"border-top:1px solid var(--border);background:var(--surface);flex-shrink:0;}\n"
"#chat-input{flex:1;background:var(--card);border:1px solid var(--border);border-radius:8px;"
"color:var(--text);padding:.55rem .9rem;font-size:.9rem;font-family:inherit;outline:none;transition:border-color .15s;}\n"
"#chat-input:focus{border-color:var(--blue);}#chat-input::placeholder{color:var(--dim2);}\n"
"#chat-send{background:var(--blue);color:#000;border:none;border-radius:8px;"
"padding:.55rem 1.1rem;font-size:.85rem;font-weight:600;cursor:pointer;white-space:nowrap;}\n"
"#chat-send:disabled{opacity:.4;cursor:not-allowed;}\n"
"#chat-task-status{font-size:.75rem;color:var(--dim);min-width:60px;text-align:right;}\n"
"#token-row{display:flex;align-items:center;gap:.5rem;padding:.4rem 1rem;"
"border-top:1px solid var(--border);background:var(--card);font-size:.75rem;color:var(--dim2);flex-shrink:0;}\n"
"#chat-token{background:var(--surface);border:1px solid var(--border);border-radius:5px;"
"color:var(--dim);padding:.2rem .5rem;font-size:.75rem;width:200px;outline:none;}\n"
"#status-panel{overflow-y:auto;padding:1.25rem;scrollbar-width:thin;scrollbar-color:var(--border) transparent;}\n"
".s-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:.75rem;margin-bottom:1.25rem;}\n"
".s-card{background:var(--card);border:1px solid var(--border);border-radius:8px;padding:.9rem 1rem;}\n"
".s-label{font-size:.68rem;text-transform:uppercase;letter-spacing:.07em;color:var(--dim);margin-bottom:.3rem;}\n"
".s-value{font-size:1.7rem;font-weight:700;line-height:1;}.s-sub{font-size:.73rem;color:var(--dim);margin-top:.2rem;}\n"
".s-section{font-size:.72rem;font-weight:600;text-transform:uppercase;letter-spacing:.07em;color:var(--dim);margin:1rem 0 .5rem;}\n"
".s-row{display:flex;align-items:center;gap:.6rem;padding:.5rem 0;border-bottom:1px solid var(--border);font-size:.85rem;}\n"
".s-row:last-child{border-bottom:none;}.s-row .s-name{flex:1;}\n"
".badge{font-size:.67rem;padding:.15rem .45rem;border-radius:8px;font-weight:600;}\n"
".badge.on{background:rgba(63,185,80,.12);color:var(--green);border:1px solid rgba(63,185,80,.25);}\n"
".badge.off{background:rgba(248,81,73,.08);color:var(--red);border:1px solid rgba(248,81,73,.2);}\n"
".badge.busy{background:rgba(227,179,65,.12);color:var(--yellow);border:1px solid rgba(227,179,65,.25);}\n"
".s-table{width:100%;border-collapse:collapse;font-size:.82rem;}\n"
".s-table th{text-align:left;padding:.4rem .6rem;color:var(--dim);font-weight:500;border-bottom:1px solid var(--border);}\n"
".s-table td{padding:.4rem .6rem;border-bottom:1px solid var(--card);font-family:monospace;}\n"
".s-table tr:last-child td{border-bottom:none;}\n"
".s-refresh{font-size:.7rem;color:var(--dim2);text-align:right;margin-top:1rem;}\n"
"@keyframes pulse{0%,100%{opacity:1}50%{opacity:.35}}.pulse{animation:pulse 2s infinite;}\n"
"</style></head><body>\n"
"<header>\n"
"  <span class=\"dot g\" id=\"hdr-dot\"></span>\n"
"  <h1>agent<span>-bot</span></h1>\n"
"  <span id=\"hdr-status\" style=\"color:var(--dim);font-size:.78rem\">読み込み中...</span>\n"
"  <div style=\"flex:1\"></div>\n"
"  <span id=\"hdr-tasks\" style=\"font-size:.75rem;color:var(--dim)\"></span>\n"
"</header>\n"
"<nav>\n"
"  <button class=\"active\" onclick=\"switchTab('chat',this)\">Chat</button>\n"
"  <button onclick=\"switchTab('status',this)\">Status</button>\n"
"</nav>\n"
"<div class=\"panel active\" id=\"tab-chat\">\n"
"  <div id=\"chat-output\"></div>\n"
"  <div id=\"chat-bar\">\n"
"    <input id=\"chat-input\" type=\"text\" placeholder=\"タスクを入力… (Enter で送信)\" autocomplete=\"off\">\n"
"    <span id=\"chat-task-status\"></span>\n"
"    <button id=\"chat-send\">送信</button>\n"
"  </div>\n"
"  <div id=\"token-row\">\n"
"    <span>Token</span>\n"
"    <input id=\"chat-token\" type=\"password\" placeholder=\"WEB_TOKEN を入力\">\n"
"    <button onclick=\"clearChat()\" style=\"margin-left:auto;background:none;border:1px solid var(--border);color:var(--dim);border-radius:5px;padding:.15rem .5rem;font-size:.72rem;cursor:pointer\">クリア</button>\n"
"  </div>\n"
"</div>\n"
"<div class=\"panel\" id=\"tab-status\">\n"
"  <div id=\"status-panel\">\n"
"    <div class=\"s-grid\">\n"
"      <div class=\"s-card\"><div class=\"s-label\">Status</div><div class=\"s-value\" id=\"s-status\" style=\"font-size:1.1rem;margin-top:.2rem\">—</div><div class=\"s-sub\" id=\"s-time\">—</div></div>\n"
"      <div class=\"s-card\"><div class=\"s-label\">Active tasks</div><div class=\"s-value\" id=\"s-tasks\">—</div><div class=\"s-sub\">実行中</div></div>\n"
"      <div class=\"s-card\"><div class=\"s-label\">Users</div><div class=\"s-value\" id=\"s-users\">—</div><div class=\"s-sub\">セッション保持</div></div>\n"
"      <div class=\"s-card\"><div class=\"s-label\">Rate limit</div><div class=\"s-value\" style=\"font-size:1.3rem\">10</div><div class=\"s-sub\">req / 60s</div></div>\n"
"    </div>\n"
"    <div class=\"s-section\">Services</div>\n"
"    <div class=\"s-card\" style=\"padding:.5rem .9rem\">\n"
"      <div class=\"s-row\"><span class=\"dot\" id=\"d-agent\"></span><span class=\"s-name\">agent.v10</span><span class=\"badge\" id=\"b-agent\">—</span></div>\n"
"      <div class=\"s-row\"><span class=\"dot\" id=\"d-tg\"></span><span class=\"s-name\">Telegram</span><span class=\"badge\" id=\"b-tg\">—</span></div>\n"
"      <div class=\"s-row\"><span class=\"dot\" id=\"d-line\"></span><span class=\"s-name\">LINE webhook</span><span class=\"badge\" id=\"b-line\">—</span></div>\n"
"      <div class=\"s-row\"><span class=\"dot g\"></span><span class=\"s-name\">SQLite</span><span class=\"badge on\" id=\"b-db\" style=\"font-family:monospace;font-size:.65rem\">—</span></div>\n"
"    </div>\n"
"    <div class=\"s-section\" style=\"margin-top:1.1rem\">Active sessions</div>\n"
"    <div class=\"s-card\" style=\"padding:.4rem .6rem\">\n"
"      <table class=\"s-table\">\n"
"        <thead><tr><th>User</th><th>Context</th><th>State</th></tr></thead>\n"
"        <tbody id=\"users-tbody\"><tr><td colspan=\"3\" style=\"color:var(--dim);padding:.5rem .6rem\">アイドル</td></tr></tbody>\n"
"      </table>\n"
"    </div>\n"
"    <div class=\"s-refresh\"><span class=\"pulse\">●</span> <span id=\"nxt\">10</span>s 後に更新</div>\n"
"  </div>\n"
"</div>\n"
"<script>\n"
"function switchTab(name,btn){\n"
"  document.querySelectorAll('.panel').forEach(p=>p.classList.remove('active'));\n"
"  document.querySelectorAll('nav button').forEach(b=>b.classList.remove('active'));\n"
"  document.getElementById('tab-'+name).classList.add('active');\n"
"  btn.classList.add('active');\n"
"  if(name==='chat')document.getElementById('chat-input').focus();\n"
"}\n"
"const tok=document.getElementById('chat-token');\n"
"tok.value=localStorage.getItem('wt')||'';\n"
"tok.oninput=()=>localStorage.setItem('wt',tok.value);\n"
"document.getElementById('chat-input').addEventListener('keydown',e=>{\n"
"  if(e.key==='Enter'&&!e.shiftKey){e.preventDefault();sendChat();}\n"
"});\n"
"document.getElementById('chat-send').addEventListener('click',sendChat);\n"
"function clearChat(){document.getElementById('chat-output').innerHTML='';}\n"
"function badge(el,dot,on){\n"
"  if(el){el.textContent=on?'ON':'OFF';el.className='badge '+(on?'on':'off');}\n"
"  if(dot)dot.className='dot '+(on?'g':'r');\n"
"}\n"
"async function refresh(){\n"
"  try{\n"
"    const[h,u]=await Promise.all([\n"
"      fetch('/health').then(r=>r.json()),\n"
"      fetch('/api/users').then(r=>r.json()).catch(()=>({users:[]})),\n"
"    ]);\n"
"    const ok=h.status==='ok';\n"
"    document.getElementById('hdr-dot').className='dot '+(ok?'g':'r');\n"
"    document.getElementById('hdr-status').textContent=ok?location.hostname:'error';\n"
"    document.getElementById('hdr-tasks').textContent=h.active_tasks>0?`${h.active_tasks} task running`:'';\n"
"    document.getElementById('s-status').textContent=ok?'✓ OK':'✗ ERR';\n"
"    document.getElementById('s-status').style.color=ok?'var(--green)':'var(--red)';\n"
"    document.getElementById('s-time').textContent=new Date().toLocaleTimeString('ja-JP');\n"
"    document.getElementById('s-tasks').textContent=h.active_tasks??0;\n"
"    document.getElementById('s-users').textContent=h.tracked_users??0;\n"
"    badge(document.getElementById('b-agent'),document.getElementById('d-agent'),h.agent);\n"
"    badge(document.getElementById('b-tg'),document.getElementById('d-tg'),h.telegram);\n"
"    badge(document.getElementById('b-line'),document.getElementById('d-line'),h.line);\n"
"    const bdb=document.getElementById('b-db');\n"
"    if(bdb)bdb.textContent=(h.db||'').replace('/data/','vol:').replace('/tmp/','tmp:');\n"
"    const users=u.users||[];\n"
"    document.getElementById('users-tbody').innerHTML=users.length===0\n"
"      ?'<tr><td colspan=\"3\" style=\"color:var(--dim);padding:.5rem .6rem\">アイドル</td></tr>'\n"
"      :users.map(u=>`<tr><td>${u.id}</td><td>${u.context_turns}</td><td>${u.busy?'<span class=\"badge busy\">実行中</span>':'—'}</td></tr>`).join('');\n"
"  }catch(_){document.getElementById('hdr-dot').className='dot r';}\n"
"}\n"
"let taskRunning=false;\n"
"function appendMsg(text,cls){\n"
"  const out=document.getElementById('chat-output');\n"
"  const el=document.createElement('div');el.className='msg-'+cls;el.textContent=text;\n"
"  out.appendChild(el);out.scrollTop=out.scrollHeight;\n"
"}\n"
"function setStatus(text,fade){\n"
"  const el=document.getElementById('chat-task-status');el.textContent=text;\n"
"  if(fade)setTimeout(()=>{el.textContent='';},3000);\n"
"}\n"
"function finish(ok){\n"
"  taskRunning=false;document.getElementById('chat-send').disabled=false;\n"
"  setStatus(ok?'✅ 完了':'❌ エラー',true);refresh();\n"
"}\n"
"async function sendChat(){\n"
"  if(taskRunning)return;\n"
"  const btn=document.getElementById('chat-send');if(btn.disabled)return;\n"
"  const inp=document.getElementById('chat-input');\n"
"  const task=inp.value.trim();const token=tok.value.trim();\n"
"  if(!task)return;\n"
"  if(!token){appendMsg('⚠ Token を入力してください','error');return;}\n"
"  taskRunning=true;btn.disabled=true;inp.value='';\n"
"  appendMsg(task,'user');setStatus('⏳ 実行中…',false);\n"
"  try{\n"
"    const r=await fetch('/api/chat',{\n"
"      method:'POST',\n"
"      headers:{'Content-Type':'application/json','X-Web-Token':token},\n"
"      body:JSON.stringify({task}),\n"
"    });\n"
"    if(!r.ok){appendMsg('❌ '+(await r.text()),'error');finish(false);return;}\n"
"  }catch(e){appendMsg('❌ 接続エラー: '+e,'error');finish(false);return;}\n"
"  try{\n"
"    const resp=await fetch('/api/chat/stream?token='+encodeURIComponent(token));\n"
"    if(!resp.ok){appendMsg('❌ Stream error','error');finish(false);return;}\n"
"    const reader=resp.body.getReader();const dec=new TextDecoder();let buf='';\n"
"    while(true){\n"
"      const{done,value}=await reader.read();if(done)break;\n"
"      buf+=dec.decode(value,{stream:true});\n"
"      const parts=buf.split('\\n\\n');buf=parts.pop();\n"
"      for(const part of parts){\n"
"        for(const line of part.split('\\n')){\n"
"          if(!line.startsWith('data: '))continue;\n"
"          try{\n"
"            const d=JSON.parse(line.slice(6));\n"
"            if(d.type==='line')appendMsg(d.text.replace(/\\n$/,''),'agent');\n"
"            else if(d.type==='done'){appendMsg('─ 完了 ─','system');reader.cancel();finish(true);return;}\n"
"            else if(d.type==='error'){appendMsg('❌ '+d.text,'error');reader.cancel();finish(false);return;}\n"
"            else if(d.type==='idle'){reader.cancel();finish(true);return;}\n"
"          }catch(_){}\n"
"        }\n"
"      }\n"
"    }\n"
"    finish(true);\n"
"  }catch(e){appendMsg('❌ Stream エラー: '+e,'error');finish(false);}\n"
"}\n"
"let countdown=10;\n"
"setInterval(()=>{\n"
"  if(--countdown<=0){countdown=10;refresh();}\n"
"  const el=document.getElementById('nxt');if(el)el.textContent=countdown;\n"
"},1000);\n"
"refresh();\n"
"</script></body></html>\n";

/* forward declaration */
static bool line_verify_sig(const char *sig, const uint8_t *body, size_t blen);

/* ── MHD request handler ─────────────────────────────────────────────── */
struct ConnData {
    char   *body;
    size_t  body_len;
    size_t  body_cap;
    char    token[256];   /* X-Web-Token header */
    char    line_sig[256];
};

static ssize_t sse_read_cb(void *cls, uint64_t pos, char *buf, size_t max) {
    (void)pos;
    int *fd = cls;
    ssize_t n = read(*fd, buf, max);
    if (n == 0) return MHD_CONTENT_READER_END_OF_STREAM;
    if (n < 0) {
        if (errno == EAGAIN || errno == EINTR) return 0;
        return MHD_CONTENT_READER_END_WITH_ERROR;
    }
    return n;
}
static void sse_free_cb(void *cls) { int *fd = cls; if (*fd >= 0) close(*fd); free(fd); }

/* URL-decode helper */
static char *url_decode(const char *src) {
    char *out = malloc(strlen(src) + 1); size_t j = 0;
    for (size_t i = 0; src[i]; i++) {
        if (src[i] == '%' && src[i+1] && src[i+2]) {
            char hex[3] = {src[i+1], src[i+2], 0};
            out[j++] = (char)strtol(hex, NULL, 16); i += 2;
        } else if (src[i] == '+') { out[j++] = ' ';
        } else { out[j++] = src[i]; }
    }
    out[j] = '\0'; return out;
}

static bool check_token(const char *tok) {
    if (!WEB_TOKEN || !*WEB_TOKEN || !tok || !*tok) return false;
    return strlen(tok) == strlen(WEB_TOKEN) &&
           memcmp(tok, WEB_TOKEN, strlen(WEB_TOKEN)) == 0; /* constant-time-ish */
}

static struct MHD_Response *json_resp(const char *body, unsigned int *status) {
    *status = MHD_HTTP_OK;
    return MHD_create_response_from_buffer(strlen(body), (void*)body, MHD_RESPMEM_MUST_COPY);
}

/* Count active tasks and tracked users */
static void stats(int *tasks, int *users) {
    *tasks = 0; *users = 0;
    pthread_mutex_lock(&g_us_lock);
    for (int i = 0; i < MAX_USERS; i++) {
        if (!g_us[i].in_use) continue;
        (*users)++;
        pthread_mutex_lock(&g_us[i].proc_lock);
        if (g_us[i].proc_pid > 0 && waitpid(g_us[i].proc_pid, NULL, WNOHANG) == 0) (*tasks)++;
        pthread_mutex_unlock(&g_us[i].proc_lock);
    }
    pthread_mutex_unlock(&g_us_lock);
}

static enum MHD_Result handle_request(
    void *cls, struct MHD_Connection *con,
    const char *url, const char *method,
    const char *version, const char *upload, size_t *upload_sz,
    void **con_cls)
{
    (void)cls; (void)version;

    /* First call: allocate per-connection data */
    if (*con_cls == NULL) {
        struct ConnData *cd = calloc(1, sizeof *cd);
        /* grab headers */
        const char *tok = MHD_lookup_connection_value(con, MHD_HEADER_KIND, "X-Web-Token");
        if (tok) strncpy(cd->token, tok, 255);
        const char *sig = MHD_lookup_connection_value(con, MHD_HEADER_KIND, "X-Line-Signature");
        if (sig) strncpy(cd->line_sig, sig, 255);
        *con_cls = cd;
        return MHD_YES;
    }
    struct ConnData *cd = *con_cls;

    /* Accumulate POST body */
    if (*upload_sz > 0) {
        if (cd->body_len + *upload_sz + 1 > cd->body_cap) {
            cd->body_cap = cd->body_len + *upload_sz + 1024;
            cd->body = realloc(cd->body, cd->body_cap);
        }
        memcpy(cd->body + cd->body_len, upload, *upload_sz);
        cd->body_len += *upload_sz;
        *upload_sz = 0;
        return MHD_YES;
    }
    if (cd->body) cd->body[cd->body_len] = '\0';

    unsigned int status = MHD_HTTP_OK;
    struct MHD_Response *resp = NULL;

    /* ── GET / ── */
    if (!strcmp(method,"GET") && !strcmp(url,"/")) {
        resp = MHD_create_response_from_buffer(strlen(DASHBOARD), (void*)DASHBOARD, MHD_RESPMEM_PERSISTENT);
        MHD_add_response_header(resp, "Content-Type", "text/html; charset=utf-8");

    /* ── GET /health ── */
    } else if (!strcmp(method,"GET") && !strcmp(url,"/health")) {
        int tasks, users; stats(&tasks, &users);
        struct stat st; bool agent_ok = stat(AGENT_PATH, &st) == 0;
        char buf[512];
        snprintf(buf, sizeof buf,
            "{\"status\":\"ok\",\"telegram\":%s,\"line\":%s,\"agent\":%s,"
            "\"active_tasks\":%d,\"tracked_users\":%d,\"db\":\"%s\",\"web_chat\":%s}",
            (TELEGRAM_TOKEN && *TELEGRAM_TOKEN)             ? "true":"false",
            (LINE_CHANNEL_ACCESS_TOKEN && *LINE_CHANNEL_ACCESS_TOKEN) ? "true":"false",
            agent_ok ? "true":"false", tasks, users, DB_PATH,
            (WEB_TOKEN && *WEB_TOKEN)                       ? "true":"false");
        resp = json_resp(buf, &status);
        MHD_add_response_header(resp, "Content-Type", "application/json");

    /* ── GET /api/users ── */
    } else if (!strcmp(method,"GET") && !strcmp(url,"/api/users")) {
        char buf[16384]; int blen = 0;
        blen += snprintf(buf+blen, sizeof buf-blen, "{\"users\":[");
        bool first = true;
        pthread_mutex_lock(&g_us_lock);
        for (int i = 0; i < MAX_USERS; i++) {
            if (!g_us[i].in_use) continue;
            pthread_mutex_lock(&g_us[i].proc_lock);
            bool busy = g_us[i].proc_pid > 0 && waitpid(g_us[i].proc_pid, NULL, WNOHANG) == 0;
            pthread_mutex_unlock(&g_us[i].proc_lock);
            int turns = db_ctx_count(g_us[i].uid);
            blen += snprintf(buf+blen, sizeof buf-blen,
                "%s{\"id\":\"%s\",\"context_turns\":%d,\"busy\":%s}",
                first?"":",", g_us[i].uid, turns, busy?"true":"false");
            first = false;
        }
        pthread_mutex_unlock(&g_us_lock);
        snprintf(buf+blen, sizeof buf-blen, "]}");
        resp = json_resp(buf, &status);
        MHD_add_response_header(resp, "Content-Type", "application/json");

    /* ── POST /api/chat ── */
    } else if (!strcmp(method,"POST") && !strcmp(url,"/api/chat")) {
        if (!check_token(cd->token)) {
            status = MHD_HTTP_UNAUTHORIZED;
            resp = MHD_create_response_from_buffer(12, "Unauthorized", MHD_RESPMEM_PERSISTENT);
        } else {
            cJSON *j = cd->body ? cJSON_Parse(cd->body) : NULL;
            const char *task = j ? cJSON_GetStringValue(cJSON_GetObjectItem(j,"task")) : NULL;
            if (!task || !*task) {
                status = MHD_HTTP_BAD_REQUEST;
                resp = MHD_create_response_from_buffer(10,"Empty task",MHD_RESPMEM_PERSISTENT);
            } else if (strlen(task) > MAX_TASK_LEN) {
                status = MHD_HTTP_BAD_REQUEST;
                resp = MHD_create_response_from_buffer(8,"Too long",MHD_RESPMEM_PERSISTENT);
            } else {
                struct UserState *st = get_state("web");
                if (st && is_busy(st)) {
                    status = MHD_HTTP_CONFLICT;
                    resp = MHD_create_response_from_buffer(19,"Task already running",MHD_RESPMEM_PERSISTENT);
                } else if (!st) {
                    status = MHD_HTTP_INTERNAL_SERVER_ERROR;
                    resp = MHD_create_response_from_buffer(5,"Error",MHD_RESPMEM_PERSISTENT);
                } else {
                    /* Create pipe, start stream thread */
                    int pfd[2]; pipe(pfd);
                    pthread_mutex_lock(&st->stream_lock);
                    if (st->pipe_r >= 0) close(st->pipe_r);
                    if (st->pipe_w >= 0) close(st->pipe_w);
                    st->pipe_r = pfd[0]; st->pipe_w = pfd[1];
                    st->stream_active = true;
                    pthread_mutex_unlock(&st->stream_lock);

                    struct StreamArgs *sa = malloc(sizeof *sa);
                    strncpy(sa->uid, "web", 63);
                    sa->task = strdup(task);
                    sa->pipe_w = pfd[1];   /* thread owns write end */

                    pthread_t t; pthread_create(&t, NULL, stream_thread, sa); pthread_detach(t);
                    resp = json_resp("{\"status\":\"started\"}", &status);
                    MHD_add_response_header(resp, "Content-Type", "application/json");
                }
            }
            if (j) cJSON_Delete(j);
        }

    /* ── GET /api/chat/stream ── */
    } else if (!strcmp(method,"GET") && !strcmp(url,"/api/chat/stream")) {
        /* token from query string */
        const char *qt = MHD_lookup_connection_value(con, MHD_GET_ARGUMENT_KIND, "token");
        char *decoded = qt ? url_decode(qt) : NULL;
        if (!check_token(decoded)) {
            status = MHD_HTTP_UNAUTHORIZED;
            resp = MHD_create_response_from_buffer(12,"Unauthorized",MHD_RESPMEM_PERSISTENT);
        } else {
            struct UserState *st = get_state("web");
            pthread_mutex_lock(&st->stream_lock);
            int pipe_r = st->pipe_r;
            st->pipe_r = -1;   /* transfer ownership to callback */
            pthread_mutex_unlock(&st->stream_lock);

            if (pipe_r < 0) {
                /* No active task — send idle event */
                const char *idle = "retry: 0\ndata: {\"type\":\"idle\"}\n\n";
                resp = MHD_create_response_from_buffer(strlen(idle),(void*)idle,MHD_RESPMEM_PERSISTENT);
            } else {
                int *fd = malloc(sizeof(int)); *fd = pipe_r;
                resp = MHD_create_response_from_callback(
                    MHD_SIZE_UNKNOWN, 4096, sse_read_cb, fd, sse_free_cb);
            }
            MHD_add_response_header(resp, "Content-Type", "text/event-stream");
            MHD_add_response_header(resp, "Cache-Control", "no-cache");
            MHD_add_response_header(resp, "X-Accel-Buffering", "no");
        }
        if (decoded) free(decoded);

    /* ── POST /webhook/line ── */
    } else if (!strcmp(method,"POST") && !strcmp(url,"/webhook/line")) {
        bool ok = false;
        if (cd->body && cd->body_len <= 65536) {
            ok = line_verify_sig(cd->line_sig, (uint8_t*)cd->body, cd->body_len);
        }
        if (!ok) {
            status = MHD_HTTP_FORBIDDEN;
            resp = MHD_create_response_from_buffer(17,"Invalid signature",MHD_RESPMEM_PERSISTENT);
        } else {
            cJSON *root = cd->body ? cJSON_Parse(cd->body) : NULL;
            cJSON *events = root ? cJSON_GetObjectItem(root,"events") : NULL;
            if (cJSON_IsArray(events)) {
                cJSON *ev;
                cJSON_ArrayForEach(ev, events) {
                    if (strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(ev,"type")),"message")!=0) continue;
                    const char *reply_token = cJSON_GetStringValue(cJSON_GetObjectItem(ev,"replyToken"));
                    cJSON *src = cJSON_GetObjectItem(ev,"source");
                    const char *uid = src ? cJSON_GetStringValue(cJSON_GetObjectItem(src,"userId")) : NULL;
                    cJSON *msg = cJSON_GetObjectItem(ev,"message");
                    if (!uid || !msg) continue;
                    if (!is_authorized(uid)) { line_push(uid,"⛔ Unauthorized",true,reply_token); continue; }
                    const char *mtype = cJSON_GetStringValue(cJSON_GetObjectItem(msg,"type"));
                    const char *text  = "";
                    if (mtype && !strcmp(mtype,"text"))
                        text = cJSON_GetStringValue(cJSON_GetObjectItem(msg,"text"));

                    if (!strcmp(text,"/help")||!strcmp(text,"/start")) {
                        line_push(uid,"agent.v10 bot\nコマンド: /status /cancel /clear",true,reply_token); continue;
                    }
                    if (!strcmp(text,"/status")) {
                        struct UserState *st = get_state(uid); bool busy = st && is_busy(st);
                        char m[128]; snprintf(m,sizeof m, busy?"⚙️ 実行中":"✅ アイドル（%d ターン）",db_ctx_count(uid));
                        line_push(uid,m,true,reply_token); continue;
                    }
                    if (!strcmp(text,"/cancel")) { line_push(uid, cancel_agent(uid)?"🛑 キャンセルしました。":"（実行中のタスクはありません）",true,reply_token); continue; }
                    if (!strcmp(text,"/clear"))  { db_clear_ctx(uid); line_push(uid,"🗑 クリアしました。",true,reply_token); continue; }
                    if (!text || !*text) continue;

                    struct UserState *st = get_state(uid);
                    if (!check_rate(st)) { line_push(uid,"⚠️ レートリミット",true,reply_token); continue; }
                    if (is_busy(st))     { line_push(uid,"⚙️ 実行中",true,reply_token); continue; }
                    line_push(uid,"⏳ 実行中...",true,reply_token);
                    struct LineArgs *la = malloc(sizeof *la);
                    strncpy(la->uid, uid, 255); strncpy(la->task, text, MAX_TASK_LEN);
                    pthread_t t; pthread_create(&t,NULL,line_task_thread,la); pthread_detach(t);
                }
            }
            if (root) cJSON_Delete(root);
            resp = json_resp("{\"status\":\"ok\"}", &status);
            MHD_add_response_header(resp, "Content-Type", "application/json");
        }

    /* ── 404 ── */
    } else {
        status = MHD_HTTP_NOT_FOUND;
        resp = MHD_create_response_from_buffer(9,"Not found",MHD_RESPMEM_PERSISTENT);
    }

    if (!resp) {
        resp = MHD_create_response_from_buffer(5,"Error",MHD_RESPMEM_PERSISTENT);
        status = MHD_HTTP_INTERNAL_SERVER_ERROR;
    }

    enum MHD_Result ret = MHD_queue_response(con, status, resp);
    MHD_destroy_response(resp);
    return ret;
}

static bool line_verify_sig(const char *sig, const uint8_t *body, size_t blen) {
    if (!LINE_CHANNEL_SECRET || !*LINE_CHANNEL_SECRET) return false;
    char *expected = hmac_b64(LINE_CHANNEL_SECRET, body, blen);
    bool ok = (strcmp(expected, sig ? sig : "") == 0);
    free(expected); return ok;
}

static void conn_cleanup(void *cls, struct MHD_Connection *con,
                          void **con_cls, enum MHD_RequestTerminationCode tc) {
    (void)cls; (void)con; (void)tc;
    struct ConnData *cd = *con_cls;
    if (cd) { if (cd->body) free(cd->body); free(cd); *con_cls = NULL; }
}

/* ── main ────────────────────────────────────────────────────────────── */
int main(void) {
    /* env */
    TELEGRAM_TOKEN            = getenv("TELEGRAM_BOT_TOKEN");
    LINE_CHANNEL_SECRET       = getenv("LINE_CHANNEL_SECRET");
    LINE_CHANNEL_ACCESS_TOKEN = getenv("LINE_CHANNEL_ACCESS_TOKEN");
    ANTHROPIC_API_KEY         = getenv("ANTHROPIC_API_KEY");
    AGENT_PATH                = getenv("AGENT_PATH")  ?: "/app/agent.v10";
    AGENT_DIR                 = getenv("AGENT_DIR")   ?: "/app";
    AGENT_API_BASE            = getenv("AGENT_API_BASE");
    WEB_TOKEN                 = getenv("WEB_TOKEN");
    AUTHORIZED_USERS          = getenv("AUTHORIZED_USERS") ?: "*";
    const char *port_s        = getenv("PORT"); if (port_s) PORT = atoi(port_s);

    struct stat st;
    DB_PATH = (stat("/data", &st) == 0 && S_ISDIR(st.st_mode))
              ? "/data/agent-bot.db" : "/tmp/agent-bot.db";

    signal(SIGPIPE, SIG_IGN);
    curl_global_init(CURL_GLOBAL_ALL);
    db_init();

    printf("[bot] DB:       %s\n", DB_PATH);
    printf("[bot] Agent:    %s (%s)\n", AGENT_PATH,
           stat(AGENT_PATH,&st)==0 ? "found" : "MISSING");
    printf("[bot] Telegram: %s\n", (TELEGRAM_TOKEN && *TELEGRAM_TOKEN) ? "on" : "off");
    printf("[bot] LINE:     %s\n", (LINE_CHANNEL_ACCESS_TOKEN && *LINE_CHANNEL_ACCESS_TOKEN) ? "on" : "off");
    printf("[bot] Web chat: %s\n", (WEB_TOKEN && *WEB_TOKEN) ? "on" : "off");
    printf("[bot] Port:     %d\n", PORT);

    /* Telegram polling thread */
    if (TELEGRAM_TOKEN && *TELEGRAM_TOKEN) {
        pthread_t t; pthread_create(&t, NULL, tg_poll, NULL); pthread_detach(t);
    }

    /* HTTP server — thread-per-connection for blocking SSE reads */
    struct MHD_Daemon *d = MHD_start_daemon(
        MHD_USE_THREAD_PER_CONNECTION | MHD_USE_INTERNAL_POLLING_THREAD,
        PORT, NULL, NULL,
        handle_request, NULL,
        MHD_OPTION_NOTIFY_COMPLETED, conn_cleanup, NULL,
        MHD_OPTION_END);
    if (!d) { fprintf(stderr, "[bot] MHD_start_daemon failed\n"); return 1; }

    printf("[bot] Listening on :%d\n", PORT);
    for (;;) sleep(60);   /* main thread just waits */
    MHD_stop_daemon(d);
    return 0;
}
