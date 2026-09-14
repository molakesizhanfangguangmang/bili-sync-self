/*
 * steward.c —— bili-sync 管家（注入型小件）
 *
 * 三个视图：
 *   A 视频清单（bili-sync「视频」栏那一份数据）：读 data.sqlite 的 video / page，逐条按
 *     page.path 探测本地文件，给出「存在 / 部分 / 不存在 / 未下载 / 无分页」角标。
 *   B 本地文件：直接扫 /downloads 目录树，列出每个视频文件夹的体积与文件数，可多选删除。
 *   C 下载列表：库里每一条的「要不要下」和「下完没有」两个开关，分三档（待下载 / 已下载 /
 *     不下）批量改：加入下载队列、移出下载队列、重置并重下、删除记录。
 *
 * 还管两件库里的事（都写在 data.sqlite 里，bili-sync 每轮从库里读，最多一轮生效）：
 *   · 新加的源默认带一条过滤规则「只下此刻之后发布的」（SQLite 触发器，见 --init）；
 *   · 下载卷可用空间低于阈值时拉闸：把当时开着的源全部停用，并推一条通知；腾出空间后一键恢复。
 *
 * 删除文件只动文件系统、不动 DB；改「要不要下」和「源开关」只动 DB、不动文件。
 *
 * 依赖：SQLite amalgamation（静态编进来）、glibc（静态链接）。无运行时依赖。
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/vfs.h>
#include <time.h>
#include <unistd.h>

#include "sqlite3.h"
#include "web_assets.h"

/* ------------------------------------------------------------------ */
/* 配置                                                                */
/* ------------------------------------------------------------------ */

static int  g_port            = 12346;
static char g_bind[64]        = "0.0.0.0";
static char g_db[PATH_MAX]    = "/app/.config/bili-sync/data.sqlite";
static char g_root[PATH_MAX]  = "/downloads";
static char g_root_real[PATH_MAX] = "";
static char g_token[256]      = "";   /* 非空则所有 API 要带 token */
static int  g_verbose         = 0;

/* 磁盘闸门 */
static int  g_floor_pct       = 10;    /* 可用空间低于这个百分比就拉闸；<=0 表示不管 */
static char g_push_url[256]   = "";    /* 拉闸/恢复的通知往哪推（HTTP POST），空串=不推 */
static int  g_push_every      = 900;   /* 拉闸后每隔多少秒再推一次 */
static int  g_check_every     = 60;    /* 多久看一次盘 */
static char g_state_dir[PATH_MAX] = "";/* 闸门快照放这儿，默认跟 data.sqlite 同目录 */

/* ------------------------------------------------------------------ */
/* 缓冲与 JSON 转义                                                     */
/* ------------------------------------------------------------------ */

typedef struct { char *p; size_t len, cap; } buf_t;

static void buf_init(buf_t *b)
{
    b->cap = 8192;
    b->len = 0;
    b->p = malloc(b->cap);
    if (!b->p) { perror("malloc"); exit(1); }
    b->p[0] = 0;
}

static void buf_free(buf_t *b)
{
    free(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
}

static void buf_need(buf_t *b, size_t extra)
{
    if (b->len + extra + 1 <= b->cap) return;
    while (b->len + extra + 1 > b->cap) b->cap *= 2;
    b->p = realloc(b->p, b->cap);
    if (!b->p) { perror("realloc"); exit(1); }
}

static void buf_puts(buf_t *b, const char *s)
{
    size_t n = strlen(s);
    buf_need(b, n);
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

static void buf_putc(buf_t *b, char c)
{
    buf_need(b, 1);
    b->p[b->len++] = c;
    b->p[b->len] = 0;
}

static void buf_printf(buf_t *b, const char *fmt, ...)
{
    va_list ap;
    char tmp[1024];
    int n;

    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof tmp) {
        buf_puts(b, tmp);
        return;
    }
    {
        char *big = malloc((size_t)n + 1);
        if (!big) return;
        va_start(ap, fmt);
        vsnprintf(big, (size_t)n + 1, fmt, ap);
        va_end(ap);
        buf_puts(b, big);
        free(big);
    }
}

static void json_str(buf_t *b, const char *s)
{
    const unsigned char *p;

    if (!s) { buf_puts(b, "null"); return; }
    buf_putc(b, '"');
    for (p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        switch (c) {
        case '"':  buf_puts(b, "\\\""); break;
        case '\\': buf_puts(b, "\\\\"); break;
        case '\n': buf_puts(b, "\\n");  break;
        case '\r': buf_puts(b, "\\r");  break;
        case '\t': buf_puts(b, "\\t");  break;
        case '\b': buf_puts(b, "\\b");  break;
        case '\f': buf_puts(b, "\\f");  break;
        default:
            if (c < 0x20) buf_printf(b, "\\u%04x", c);
            else buf_putc(b, (char)c);
        }
    }
    buf_putc(b, '"');
}

/* 从 {"key":"value"} 里取一个字符串；找不到返回 0 */
static int json_str_value(const char *body, const char *key, char *out, size_t outlen)
{
    char pat[128];
    const char *p, *q;
    size_t n = 0;

    out[0] = 0;
    if (!body) return 0;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    p = strstr(body, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == ':') p++;
    if (*p != '"') return 0;
    p++;
    q = strchr(p, '"');
    if (!q) return 0;
    while (p < q && n + 1 < outlen) out[n++] = *p++;
    out[n] = 0;
    return 1;
}

/* 从 {"key":[1,2,3]} 里取一串整数；调用方 free，个数写在 count */
static long long *json_ints(const char *body, const char *key, int *count)
{
    char pat[128];
    const char *p, *q;
    long long *arr = NULL;
    int n = 0, cap = 0;

    *count = 0;
    if (!body) return NULL;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    p = strstr(body, pat);
    if (!p) return NULL;
    p = strchr(p, '[');
    if (!p) return NULL;
    q = strchr(p, ']');
    if (!q) return NULL;
    p++;
    while (p < q) {
        char *end;
        long long v;

        while (p < q && !isdigit((unsigned char)*p) && *p != '-') p++;
        if (p >= q) break;
        v = strtoll(p, &end, 10);
        if (end == p) { p++; continue; }
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            arr = realloc(arr, (size_t)cap * sizeof *arr);
            if (!arr) { *count = 0; return NULL; }
        }
        arr[n++] = v;
        p = end;
    }
    *count = n;
    return arr;
}

/* ------------------------------------------------------------------ */
/* 文件系统小工具                                                       */
/* ------------------------------------------------------------------ */

static int stat_reg(const char *path, long long *size_out)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    if (!S_ISREG(st.st_mode)) return 0;
    if (size_out) *size_out = (long long)st.st_size;
    return 1;
}

static int stat_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode) ? 1 : 0;
}

/* 递归累加目录下普通文件的大小与个数（不跟随符号链接） */
static void tree_size(const char *path, long long *size, int *files)
{
    DIR *d;
    struct dirent *e;

    d = opendir(path);
    if (!d) return;
    while ((e = readdir(d)) != NULL) {
        char child[PATH_MAX * 2];
        struct stat st;

        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(child, sizeof child, "%s/%s", path, e->d_name);
        if (lstat(child, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            tree_size(child, size, files);
        } else if (S_ISREG(st.st_mode)) {
            *size += (long long)st.st_size;
            (*files)++;
        }
    }
    closedir(d);
}

static int rm_rf(const char *path)
{
    struct stat st;
    DIR *d;
    struct dirent *e;
    int rc = 0;

    if (lstat(path, &st) != 0) return -1;

    if (S_ISDIR(st.st_mode)) {
        d = opendir(path);
        if (!d) return -1;
        while ((e = readdir(d)) != NULL) {
            char child[PATH_MAX * 2];
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            snprintf(child, sizeof child, "%s/%s", path, e->d_name);
            if (rm_rf(child) != 0) rc = -1;
        }
        closedir(d);
        if (rc == 0 && rmdir(path) != 0) rc = -1;
        return rc;
    }
    return unlink(path) == 0 ? 0 : -1;
}

/* 目录项名排序 */
static int cmp_name(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* ------------------------------------------------------------------ */
/* DB：只读连接                                                         */
/* ------------------------------------------------------------------ */

static sqlite3 *db_open(void)
{
    sqlite3 *db = NULL;

    if (sqlite3_open_v2(g_db, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK)
        return db;

    if (db) { sqlite3_close(db); db = NULL; }

    /* 只读打不开（WAL 需要 shm 写权限之类）时，退到读写连接但立刻 query_only */
    if (sqlite3_open_v2(g_db, &db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return NULL;
    }
    sqlite3_exec(db, "PRAGMA query_only=ON", NULL, NULL, NULL);
    return db;
}

/* 源表（收藏夹/合集/稍后再看/投稿）的行，用于给记录打分组标签 */
typedef struct {
    long long id;
    char name[512];
    char path[PATH_MAX];
} src_t;

static int load_src(sqlite3 *db, const char *table, const char *namecol, src_t **out)
{
    sqlite3_stmt *st = NULL;
    char sql[256];
    src_t *arr = NULL;
    int n = 0, cap = 0;

    *out = NULL;
    if (namecol)
        snprintf(sql, sizeof sql, "SELECT id, %s, path FROM %s ORDER BY id", namecol, table);
    else
        snprintf(sql, sizeof sql, "SELECT id, '', path FROM %s ORDER BY id", table);

    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return 0;

    while (sqlite3_step(st) == SQLITE_ROW) {
        if (n == cap) {
            cap = cap ? cap * 2 : 8;
            arr = realloc(arr, (size_t)cap * sizeof *arr);
            if (!arr) { sqlite3_finalize(st); return 0; }
        }
        memset(&arr[n], 0, sizeof arr[n]);
        arr[n].id = sqlite3_column_int64(st, 0);
        snprintf(arr[n].name, sizeof arr[n].name, "%s",
                 (const char *)sqlite3_column_text(st, 1) ? (const char *)sqlite3_column_text(st, 1) : "");
        snprintf(arr[n].path, sizeof arr[n].path, "%s",
                 (const char *)sqlite3_column_text(st, 2) ? (const char *)sqlite3_column_text(st, 2) : "");
        n++;
    }
    sqlite3_finalize(st);
    *out = arr;
    return n;
}

static const src_t *src_find(const src_t *arr, int n, long long id)
{
    int i;
    if (!arr) return NULL;
    for (i = 0; i < n; i++)
        if (arr[i].id == id) return &arr[i];
    return NULL;
}

/* ------------------------------------------------------------------ */
/* DB：可写连接                                                         */
/* ------------------------------------------------------------------ */

static sqlite3 *db_open_rw(void)
{
    sqlite3 *db = NULL;

    if (sqlite3_open_v2(g_db, &db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return NULL;
    }
    sqlite3_busy_timeout(db, 5000);
    /* bili-sync 那边开着 WAL，两个进程同时写靠 busy_timeout 顶住；不动 journal 模式 */
    return db;
}

/* ------------------------------------------------------------------ */
/* 源表与默认规则                                                       */
/* ------------------------------------------------------------------ */

/* 四张源表：收藏夹/合集看「收藏时间」，稍后再看/投稿看「发布时间」 */
static const char *SRC_TABLES[]     = { "favorite", "collection", "watch_later", "submission" };
static const char *SRC_TIME_FIELD[] = { "favTime",  "pubTime",    "favTime",     "pubTime"    };
#define NSRC_TABLES 4

static int src_table_index(const char *name)
{
    int i;
    for (i = 0; i < NSRC_TABLES; i++)
        if (!strcmp(name, SRC_TABLES[i])) return i;
    return -1;
}

/* 默认规则的字面量：时间字段 > 此刻。
 * 「此刻」交给 SQLite 现算 —— strftime 的 localtime 用的就是进程时区，
 * 跟 bili-sync 写 pubtime / favtime 用的是同一套时区，两边能直接比。 */
static void rule_literal(char *out, size_t outlen, const char *field)
{
    snprintf(out, outlen,
             "'[[{\"field\":\"%s\",\"rule\":{\"operator\":\"greaterThan\",\"value\":\"'"
             " || strftime('%%Y-%%m-%%dT%%H:%%M:%%S','now','localtime') || '\"}}]]'",
             field);
}

/* 四个 AFTER INSERT 触发器：源入库那一刻把默认规则写上。
 * 触发器跑在 bili-sync 自己的写事务里，所以不存在「源已经入库、规则还没写」的窗口。 */
static int ensure_triggers(char *report, size_t reportlen)
{
    sqlite3 *db = db_open_rw();
    int i, ok = 0;
    size_t off = 0;

    if (report && reportlen) report[0] = 0;
    if (!db) {
        if (report && reportlen) snprintf(report, reportlen, "打不开 %.200s（没有写权限？）", g_db);
        return 0;
    }
    for (i = 0; i < NSRC_TABLES; i++) {
        char lit[512], sql[1024], *msg = NULL;

        rule_literal(lit, sizeof lit, SRC_TIME_FIELD[i]);
        snprintf(sql, sizeof sql,
                 "CREATE TRIGGER IF NOT EXISTS steward_default_rule_%s AFTER INSERT ON %s "
                 "WHEN NEW.rule IS NULL OR NEW.rule = '' "
                 "BEGIN UPDATE %s SET rule = %s WHERE id = NEW.id; END;",
                 SRC_TABLES[i], SRC_TABLES[i], SRC_TABLES[i], lit);
        if (sqlite3_exec(db, sql, NULL, NULL, &msg) == SQLITE_OK) {
            ok++;
        } else if (report && off + 1 < reportlen) {
            off += (size_t)snprintf(report + off, reportlen - off, "%s%s: %s",
                                    off ? "；" : "", SRC_TABLES[i], msg ? msg : "?");
        }
        sqlite3_free(msg);
    }
    sqlite3_close(db);
    return ok;
}

/* 给 rule 为空的源补上默认规则。已经有规则（哪怕手写清空成一格）的一律不碰。 */
static int backfill_rules(char *err, size_t errlen)
{
    sqlite3 *db = db_open_rw();
    int i, total = 0;
    size_t off = 0;

    if (err && errlen) err[0] = 0;
    if (!db) {
        if (err && errlen) snprintf(err, errlen, "打不开 %.200s（没有写权限？）", g_db);
        return -1;
    }
    for (i = 0; i < NSRC_TABLES; i++) {
        char lit[512], sql[1024], *msg = NULL;
        sqlite3_stmt *st = NULL;
        int before = 0, after = 0;

        snprintf(sql, sizeof sql, "SELECT count(*) FROM %s WHERE rule IS NULL OR rule = ''",
                 SRC_TABLES[i]);
        if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
            if (sqlite3_step(st) == SQLITE_ROW) before = sqlite3_column_int(st, 0);
            sqlite3_finalize(st);
        }
        rule_literal(lit, sizeof lit, SRC_TIME_FIELD[i]);
        snprintf(sql, sizeof sql, "UPDATE %s SET rule = %s WHERE rule IS NULL OR rule = ''",
                 SRC_TABLES[i], lit);
        if (sqlite3_exec(db, sql, NULL, NULL, &msg) != SQLITE_OK) {
            if (err && errlen && off + 1 < errlen)
                off += (size_t)snprintf(err + off, errlen - off, "%s%s: %s",
                                        off ? "；" : "", SRC_TABLES[i], msg ? msg : "?");
            sqlite3_free(msg);
            continue;
        }
        sqlite3_free(msg);
        snprintf(sql, sizeof sql, "SELECT count(*) FROM %s WHERE rule IS NULL OR rule = ''",
                 SRC_TABLES[i]);
        if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
            if (sqlite3_step(st) == SQLITE_ROW) after = sqlite3_column_int(st, 0);
            sqlite3_finalize(st);
        }
        if (before > after) total += before - after;
    }
    sqlite3_close(db);
    return total;
}

/* 把「要下、但一页都没落过盘」的条目改成不下（一次性的清理动作，命令行 --pending-off） */
static int mark_pending_off(void)
{
    sqlite3 *db = db_open_rw();
    int n = -1;

    if (!db) return -1;
    if (sqlite3_exec(db,
            "UPDATE video SET should_download = 0 WHERE should_download = 1 AND NOT EXISTS ("
            " SELECT 1 FROM page WHERE page.video_id = video.id"
            "   AND page.path IS NOT NULL AND page.path <> '')",
            NULL, NULL, NULL) == SQLITE_OK)
        n = sqlite3_changes(db);
    sqlite3_close(db);
    return n;
}

/* ------------------------------------------------------------------ */
/* 磁盘闸门                                                            */
/* ------------------------------------------------------------------ */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int  g_braked          = 0;     /* 现在是拉闸状态（源被停用、快照在） */
static long long g_disk_total = 0, g_disk_free = 0;
static int  g_disk_pct        = 100;
static long long g_last_push  = 0;
static int  g_push_count      = 0;
static char g_brake_note[256] = "";
static char g_brake_path[PATH_MAX] = "";

/* 落盘监视：bili-sync 自己的通知只认子任务状态位，不认盘上有没有文件，
 * 所以「入库」那句不能当「下完了」。这里改成每分钟核一次盘：
 *   某条从「盘上没有」变成「分页文件全在盘上」-> 推「下载完成」
 *   反过来从「全在」变成「记录还在但文件一个都不剩」-> 推「文件不在了」
 * 记忆写在 g_watch_path 那份文件里。 */
static char g_watch_path[PATH_MAX] = "";
static int  g_watch_on        = 1;
static int  g_watch_seen      = 0;      /* 记忆里「在盘上」的条数 */
static int  g_watch_pushed    = 0;      /* 累计推过几条「下载完成」 */
static int  g_watch_lost      = 0;      /* 累计推过几条「文件不在了」 */
static long long g_watch_last = 0;

/* 网速：容器自己的 netns 里读 /proc/net/dev（下载流量就是这里的收包）。
 * 单独一个 2 秒的采样线程，页面也是 2 秒来问一次 /api/net。 */
static long long g_net_rx = 0, g_net_tx = 0;          /* 累计字节 */
static long long g_net_rx_rate = 0, g_net_tx_rate = 0;/* 字节/秒 */
static long long g_net_ts = 0;                        /* 上次采样时刻 */
static char g_net_iface[64] = "";

/* 首页板块配置，落在 data.sqlite 同目录的 steward-prefs.json 里（bind mount，重建容器不丢） */
static char g_prefs_path[PATH_MAX] = "";

static void human_size(long long n, char *out, size_t outlen)
{
    static const char *u[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    double v = (double)n;
    int i = 0;

    while (v >= 1024.0 && i < 4) { v /= 1024.0; i++; }
    if (i == 0 || v >= 100.0) snprintf(out, outlen, "%.0f %s", v, u[i]);
    else snprintf(out, outlen, "%.1f %s", v, u[i]);
}

/* 看这个目录所在文件系统的用量。容器里 /downloads 就是宿主那个下载卷。 */
static void disk_usage(const char *path, long long *total, long long *freeb, int *pct)
{
    struct statfs st;

    *total = *freeb = 0;
    *pct = 100;
    if (statfs(path, &st) != 0) return;
    {
        long long unit = st.f_frsize ? st.f_frsize : st.f_bsize;
        *total = (long long)st.f_blocks * unit;
        *freeb = (long long)st.f_bavail * unit;
        if (*total > 0) *pct = (int)(*freeb * 100 / *total);
    }
}

/* 把主机名翻成 IPv4：先看点分十进制，再翻 /etc/hosts。
 * 静态链接不借 NSS，所以域名走 hosts 文件；容器的 host.docker.internal 就在那里，
 * 主机换局域网 IP 也不用改配置。 */
static int resolve_ipv4(const char *host, struct in_addr *out)
{
    FILE *fp;
    char line[512];

    if (inet_pton(AF_INET, host, out) == 1) return 1;

    fp = fopen("/etc/hosts", "r");
    if (!fp) return 0;
    while (fgets(line, sizeof line, fp)) {
        char *cmt = strchr(line, '#');
        const char *p, *end, *q;
        char ip[64];
        size_t len;
        int hit = 0;

        if (cmt) *cmt = 0;
        p = line;
        while (*p == ' ' || *p == '\t') p++;
        end = p;
        while (*end && *end != ' ' && *end != '\t' && *end != '\n') end++;
        len = (size_t)(end - p);
        if (len == 0 || len >= sizeof ip) continue;
        memcpy(ip, p, len);
        ip[len] = 0;

        q = end;
        while (*q && !hit) {
            const char *w;
            while (*q == ' ' || *q == '\t' || *q == '\n') q++;
            w = q;
            while (*w && *w != ' ' && *w != '\t' && *w != '\n') w++;
            if (w > q && (size_t)(w - q) == strlen(host) && strncmp(q, host, (size_t)(w - q)) == 0) hit = 1;
            q = w;
        }
        if (hit && inet_pton(AF_INET, ip, out) == 1) {
            fclose(fp);
            return 1;
        }
    }
    fclose(fp);
    return 0;
}

/* 一条极简 HTTP POST，只打明文 http。返回 1 = 对端答了 200。 */
static int http_post_json(const char *url, const char *json)
{
    char host[256], path[512];
    const char *p, *slash;
    int port = 80, fd, ok = 0;
    struct sockaddr_in addr;
    struct timeval tv = { 5, 0 };
    buf_t req;

    if (!url || strncmp(url, "http://", 7) != 0) return 0;
    p = url + 7;
    slash = strchr(p, '/');
    {
        const char *hostend = slash ? slash : p + strlen(p);
        size_t hl = (size_t)(hostend - p);
        if (hl == 0 || hl >= sizeof host) return 0;
        memcpy(host, p, hl);
        host[hl] = 0;
        snprintf(path, sizeof path, "%s", slash ? slash : "/");
    }
    {
        char *colon = strchr(host, ':');
        if (colon) { *colon = 0; port = atoi(colon + 1); }
    }

    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    /* 域名先翻 /etc/hosts（静态链接不借 NSS），点分十进制直接过。 */
    if (!resolve_ipv4(host, &addr.sin_addr)) {
        fprintf(stderr, "[steward] 推送地址解析不了：%s（--push-url 得写名字或点分十进制 IP）\n", host);
        return 0;
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0) {
        buf_init(&req);
        buf_printf(&req, "POST %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n"
                         "Content-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",
                   path, host, strlen(json), json);
        if (write(fd, req.p, req.len) == (ssize_t)req.len) {
            char resp[256];
            ssize_t n = read(fd, resp, sizeof resp - 1);
            if (n > 0) {
                resp[n] = 0;
                ok = strstr(resp, " 200 ") != NULL;
            }
        }
        buf_free(&req);
    }
    close(fd);
    return ok;
}

static int push_text(const char *text)
{
    buf_t body;
    int rc;
    char msg[512];

    if (!g_push_url[0]) return 0;
    buf_init(&body);
    buf_puts(&body, "{\"text\":");
    json_str(&body, text);
    buf_putc(&body, '}');
    rc = http_post_json(g_push_url, body.p);
    buf_free(&body);

    snprintf(msg, sizeof msg, "[steward] 推送%s：%.120s\n", rc ? "成功" : "失败", text);
    if (write(2, msg, strlen(msg)) < 0) {}
    return rc;
}

/* ---------------- 落盘监视 ---------------- */

#define WATCH_MARK "# steward on-disk v1"

typedef struct {
    long long id;
    char name[512];
    int pages;
    int withpath;
    int present;
    int ondisk;     /* 分页文件全在盘上 */
    int stale;      /* 记过落盘路径，但一个文件都不在了 */
} wrec_t;

static int wrec_cmp_id(const void *a, const void *b)
{
    long long x = ((const wrec_t *)a)->id, y = ((const wrec_t *)b)->id;
    return x < y ? -1 : x > y ? 1 : 0;
}

/* 把名字里可能出现的换行/制表压成空格（状态文件一行一条） */
static void one_line(char *s)
{
    for (; *s; s++) if (*s == '\n' || *s == '\r' || *s == '\t') *s = ' ';
}

/* 扫一遍库 + 盘。成功返回按 id 升序的数组，失败返回 NULL。 */
static wrec_t *watch_scan(int *n)
{
    sqlite3 *db = db_open();
    sqlite3_stmt *sv = NULL, *sp = NULL;
    wrec_t *arr = NULL;
    int cnt = 0, cap = 0, i = 0;

    *n = 0;
    if (!db) return NULL;

    if (sqlite3_prepare_v2(db, "SELECT id, name FROM video ORDER BY id", -1, &sv, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return NULL;
    }
    while (sqlite3_step(sv) == SQLITE_ROW) {
        const char *nm = (const char *)sqlite3_column_text(sv, 1);
        if (cnt == cap) {
            cap = cap ? cap * 2 : 1024;
            arr = realloc(arr, (size_t)cap * sizeof *arr);
            if (!arr) { sqlite3_finalize(sv); sqlite3_close(db); return NULL; }
        }
        memset(&arr[cnt], 0, sizeof arr[cnt]);
        arr[cnt].id = sqlite3_column_int64(sv, 0);
        snprintf(arr[cnt].name, sizeof arr[cnt].name, "%s", nm ? nm : "");
        one_line(arr[cnt].name);
        cnt++;
    }
    sqlite3_finalize(sv);

    if (sqlite3_prepare_v2(db, "SELECT video_id, path FROM page ORDER BY video_id, id",
                           -1, &sp, NULL) != SQLITE_OK) {
        free(arr);
        sqlite3_close(db);
        return NULL;
    }
    while (sqlite3_step(sp) == SQLITE_ROW) {
        long long vid = sqlite3_column_int64(sp, 0);
        const char *p = (const char *)sqlite3_column_text(sp, 1);
        long long sz = 0;

        while (i < cnt && arr[i].id < vid) i++;
        if (i >= cnt || arr[i].id != vid) continue;
        arr[i].pages++;
        if (p && *p) {
            arr[i].withpath++;
            if (stat_reg(p, &sz)) arr[i].present++;
        }
    }
    sqlite3_finalize(sp);
    sqlite3_close(db);

    for (i = 0; i < cnt; i++) {
        arr[i].ondisk = (arr[i].pages > 0 && arr[i].present == arr[i].pages);
        arr[i].stale  = (arr[i].withpath > 0 && arr[i].present == 0);
    }
    qsort(arr, (size_t)cnt, sizeof *arr, wrec_cmp_id);
    *n = cnt;
    return arr;
}

/* 读记忆文件。返回 1 = 有基线（文件里有标记行），0 = 头一次。 */
static int watch_load(wrec_t **out, int *n)
{
    FILE *f;
    char line[1024];
    wrec_t *arr = NULL;
    int cnt = 0, cap = 0, marked = 0;

    *out = NULL;
    *n = 0;
    if (!g_watch_path[0]) return 0;
    f = fopen(g_watch_path, "r");
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        char *tab;
        long long id;

        if (!strncmp(line, WATCH_MARK, strlen(WATCH_MARK))) { marked = 1; continue; }
        tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        id = atoll(line);
        if (id <= 0) continue;
        if (cnt == cap) {
            cap = cap ? cap * 2 : 1024;
            arr = realloc(arr, (size_t)cap * sizeof *arr);
            if (!arr) { fclose(f); return 0; }
        }
        memset(&arr[cnt], 0, sizeof arr[cnt]);
        arr[cnt].id = id;
        snprintf(arr[cnt].name, sizeof arr[cnt].name, "%s", tab + 1);
        {
            size_t L = strlen(arr[cnt].name);
            while (L && (arr[cnt].name[L - 1] == '\n' || arr[cnt].name[L - 1] == '\r'))
                arr[cnt].name[--L] = 0;
        }
        cnt++;
    }
    fclose(f);
    if (!marked) { free(arr); return 0; }
    qsort(arr, (size_t)cnt, sizeof *arr, wrec_cmp_id);
    *out = arr;
    *n = cnt;
    return 1;
}

static void watch_save(const wrec_t *arr, int n)
{
    FILE *f;
    int i;

    if (!g_watch_path[0]) return;
    f = fopen(g_watch_path, "w");
    if (!f) return;
    fprintf(f, WATCH_MARK "\n");
    for (i = 0; i < n; i++) {
        if (!arr[i].ondisk) continue;
        fprintf(f, "%lld\t%s\n", arr[i].id, arr[i].name);
    }
    fclose(f);
}

/* 把 l 里若干条拼成一段「1. xxx」；最多列 max 条 */
static void list_lines(buf_t *b, const wrec_t *const *items, int n, int max)
{
    int i;

    if (n > max) {
        buf_printf(b, "（%d 条，这里只列前 %d 条）\n", n, max);
        n = max;
    }
    for (i = 0; i < n; i++) buf_printf(b, "%d. %s\n", i + 1, items[i]->name);
}

static void watch_once(void)
{
    wrec_t *now = NULL, *old = NULL;
    int nnow = 0, nold = 0, have_base, i, j;
    const wrec_t **added = NULL, **lost = NULL;
    int nadded = 0, nlost = 0;

    now = watch_scan(&nnow);
    if (!now) return;
    have_base = watch_load(&old, &nold);
    added = malloc((size_t)(nnow + 1) * sizeof *added);
    lost  = malloc((size_t)(nold + 1) * sizeof *lost);
    if (!added || !lost) {
        free(added); free(lost); free(old); free(now);
        return;
    }

    if (have_base) {
        /* added：这轮在盘上、上轮记忆里没有 */
        i = j = 0;
        while (i < nnow && j < nold) {          /* 两边都按 id 升序 */
            if (now[i].id == old[j].id) { i++; j++; }
            else if (now[i].id < old[j].id) { if (now[i].ondisk) added[nadded++] = &now[i]; i++; }
            else j++;
        }
        for (; i < nnow; i++) if (now[i].ondisk) added[nadded++] = &now[i];

        /* lost：上轮记忆里有、这轮不在盘上。记录被删掉的（用户在列表里点删除）不算，
         * 只提示「记录还在、落盘路径也还写着、文件却一个都不剩」的那类。 */
        i = j = 0;
        while (i < nold && j < nnow) {
            if (old[i].id == now[j].id) {
                if (!now[j].ondisk && now[j].stale) lost[nlost++] = &old[i];
                i++;
                j++;
            } else if (old[i].id < now[j].id) {
                i++;
            } else {
                j++;
            }
        }
    }

    if (have_base && nadded) {
        buf_t t;
        buf_init(&t);
        buf_printf(&t, "bili-sync 管家：下载完成 %d 条\n", nadded);
        list_lines(&t, added, nadded, 10);
        {
            char *s = t.p ? t.p : (char *)"";
            if (push_text(s)) {
                pthread_mutex_lock(&g_lock);
                g_watch_pushed += nadded;
                g_watch_last = time(NULL);
                pthread_mutex_unlock(&g_lock);
            }
        }
        buf_free(&t);
    }
    if (have_base && nlost) {
        buf_t t;
        buf_init(&t);
        buf_printf(&t, "bili-sync 管家：盘上有 %d 条的文件不在了\n", nlost);
        list_lines(&t, lost, nlost, 10);
        buf_puts(&t, "记录还在。要清掉这些条的完成标记，到管家页面点「清理僵尸完成」。");
        {
            char *s = t.p ? t.p : (char *)"";
            if (push_text(s)) {
                pthread_mutex_lock(&g_lock);
                g_watch_lost += nlost;
                pthread_mutex_unlock(&g_lock);
            }
        }
        buf_free(&t);
    }

    if (!have_base) {
        int on = 0;
        for (i = 0; i < nnow; i++) if (now[i].ondisk) on++;
        fprintf(stderr, "[steward] 落盘监视：第一次跑，记下 %d 条已在盘上的，不推\n", on);
    }
    watch_save(now, nnow);
    pthread_mutex_lock(&g_lock);
    {
        int on = 0;
        for (i = 0; i < nnow; i++) if (now[i].ondisk) on++;
        g_watch_seen = on;
    }
    pthread_mutex_unlock(&g_lock);

    free(added);
    free(lost);
    free(old);
    free(now);
}

/* POST /api/watch  {"action":"baseline"} —— 把此刻「已在盘上」的这批记成基线，不推 */
static int api_watch(const char *body, buf_t *out, char *err, size_t errlen)
{
    char action[32];
    wrec_t *now;
    int n = 0, i, on = 0;

    if (!json_str_value(body, "action", action, sizeof action) || strcmp(action, "baseline") != 0) {
        snprintf(err, errlen, "不认识的 action（只认 baseline）");
        return 0;
    }
    now = watch_scan(&n);
    if (!now) {
        snprintf(err, errlen, "扫库失败");
        return 0;
    }
    for (i = 0; i < n; i++) if (now[i].ondisk) on++;
    watch_save(now, n);
    free(now);
    pthread_mutex_lock(&g_lock);
    g_watch_seen = on;
    pthread_mutex_unlock(&g_lock);
    buf_printf(out, "{\"ok\":true,\"action\":\"baseline\",\"onDisk\":%d,\"file\":", on);
    json_str(out, g_watch_path);
    buf_putc(out, '}');
    return 1;
}

/* 拉闸：把当时开着的源记进快照，然后全部停用 */
static int brake_on(char *note, size_t notelen)
{
    sqlite3 *db = db_open_rw();
    FILE *f;
    int i, n = 0, snap_ok;

    if (note && notelen) note[0] = 0;
    if (!db) {
        if (note && notelen) snprintf(note, notelen, "打不开库，拉闸没做成");
        return -1;
    }
    f = g_brake_path[0] ? fopen(g_brake_path, "w") : NULL;
    snap_ok = (f != NULL);
    for (i = 0; i < NSRC_TABLES; i++) {
        sqlite3_stmt *st = NULL;
        char sql[256];

        snprintf(sql, sizeof sql, "SELECT id FROM %s WHERE enabled = 1 ORDER BY id", SRC_TABLES[i]);
        if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
            while (sqlite3_step(st) == SQLITE_ROW) {
                long long id = sqlite3_column_int64(st, 0);
                if (f) fprintf(f, "%s %lld\n", SRC_TABLES[i], id);
                n++;
            }
            sqlite3_finalize(st);
        }
        snprintf(sql, sizeof sql, "UPDATE %s SET enabled = 0 WHERE enabled = 1", SRC_TABLES[i]);
        sqlite3_exec(db, sql, NULL, NULL, NULL);
    }
    if (f) fclose(f);
    sqlite3_close(db);
    if (note && notelen)
        snprintf(note, notelen, "已停用 %d 个源%s", n,
                 snap_ok ? "（快照已记，腾出空间后可一键恢复）" : "（快照没写成，恢复要手动开）");
    return n;
}

/* 解除：照快照把当时那些源开回来。快照里没有的一律不动。 */
static int brake_off(char *note, size_t notelen)
{
    sqlite3 *db = db_open_rw();
    FILE *f;
    char line[256];
    int n = 0;

    if (note && notelen) note[0] = 0;
    if (!db) {
        if (note && notelen) snprintf(note, notelen, "打不开库，恢复没做成");
        return -1;
    }
    f = g_brake_path[0] ? fopen(g_brake_path, "r") : NULL;
    if (!f) {
        if (note && notelen) snprintf(note, notelen, "没有快照，恢复不了（源得自己去 bili-sync 里开）");
        sqlite3_close(db);
        return -1;
    }
    while (fgets(line, sizeof line, f)) {
        char table[64], sql[256];
        long long id;

        if (sscanf(line, "%63s %lld", table, &id) != 2) continue;
        if (src_table_index(table) < 0) continue;
        snprintf(sql, sizeof sql, "UPDATE %s SET enabled = 1 WHERE id = %lld", table, id);
        sqlite3_exec(db, sql, NULL, NULL, NULL);
        n++;
    }
    fclose(f);
    remove(g_brake_path);
    sqlite3_close(db);
    if (note && notelen) snprintf(note, notelen, "已按快照恢复 %d 个源", n);
    return n;
}

/* 推送正文：说清楚盘多满、拉了什么闸、怎么解 */
static void brake_message(char *out, size_t outlen)
{
    char hum[32];

    human_size(g_disk_free, hum, sizeof hum);
    snprintf(out, outlen,
             "bili-sync 管家：下载卷只剩 %s（%d%%），低于 %d%% 阈值，已停用全部源。"
             "腾出空间后到管家页面点「恢复源」即可。",
             hum, g_disk_pct, g_floor_pct);
}

static void *disk_thread(void *arg)
{
    (void)arg;
    for (;;) {
        long long total, freeb;
        int pct;

        disk_usage(g_root, &total, &freeb, &pct);
        pthread_mutex_lock(&g_lock);
        g_disk_total = total;
        g_disk_free = freeb;
        g_disk_pct = pct;
        pthread_mutex_unlock(&g_lock);

        if (g_floor_pct > 0 && pct < g_floor_pct) {
            int do_brake = 0, do_push = 0;
            char note[256] = "", text[512];

            pthread_mutex_lock(&g_lock);
            if (!g_braked) {
                g_braked = 1;
                do_brake = 1;
                do_push = 1;
            } else if (time(NULL) - g_last_push >= g_push_every) {
                do_push = 1;
            }
            if (do_push) g_last_push = time(NULL);
            pthread_mutex_unlock(&g_lock);

            if (do_brake) {
                brake_on(note, sizeof note);
                pthread_mutex_lock(&g_lock);
                snprintf(g_brake_note, sizeof g_brake_note, "%s", note);
                pthread_mutex_unlock(&g_lock);
                fprintf(stderr, "[steward] 低于 %d%%，拉闸：%s\n", g_floor_pct, note);
            }
            if (do_push) {
                brake_message(text, sizeof text);
                if (do_brake && note[0]) snprintf(text + strlen(text),
                                                  sizeof text - strlen(text), "（%s）", note);
                if (push_text(text)) {
                    pthread_mutex_lock(&g_lock);
                    g_push_count++;
                    pthread_mutex_unlock(&g_lock);
                }
            }
        }
        if (g_watch_on) watch_once();
        sleep((unsigned)g_check_every);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* 网速                                                                */
/* ------------------------------------------------------------------ */

/* 读 /proc/net/dev，把非环回、非虚拟口加起来。容器里通常只有一个 eth0。 */
static int net_read(long long *rx, long long *tx, char *iface, size_t ifacelen)
{
    FILE *f;
    char line[512];
    long long r = 0, t = 0;

    f = fopen("/proc/net/dev", "r");
    if (!f) return 0;
    if (iface && ifacelen) iface[0] = 0;
    while (fgets(line, sizeof line, f)) {
        char *colon = strchr(line, ':'), *name = line, *p;
        long long v[16];
        int i;

        while (*name == ' ' || *name == '\t') name++;
        if (!colon || colon < name) continue;
        *colon = 0;
        for (p = name + strlen(name); p > name && (p[-1] == ' ' || p[-1] == '\t'); p--) p[-1] = 0;
        if (!*name) continue;
        if (!strcmp(name, "lo") || !strncmp(name, "veth", 4) || !strncmp(name, "docker", 6) ||
            !strncmp(name, "br-", 3) || !strncmp(name, "virbr", 5)) continue;

        p = colon + 1;
        for (i = 0; i < 16; i++) {
            char *end;
            v[i] = strtoll(p, &end, 10);
            if (end == p) break;
            p = end;
        }
        if (i < 9) continue;
        r += v[0];   /* 收 */
        t += v[8];   /* 发 */
        if (iface && ifacelen && !iface[0]) snprintf(iface, ifacelen, "%s", name);
    }
    fclose(f);
    *rx = r;
    *tx = t;
    return 1;
}

static void *net_thread(void *arg)
{
    long long prx = -1, ptx = 0, pts = 0;

    (void)arg;
    for (;;) {
        long long rx, tx, now = (long long)time(NULL);
        char iface[64];
        int got = net_read(&rx, &tx, iface, sizeof iface);

        if (got) {
            long long dls = 0;
            pthread_mutex_lock(&g_lock);
            if (prx >= 0 && now > pts && rx >= prx) {
                dls = now - pts;
                g_net_rx_rate = (rx - prx) / dls;
                g_net_tx_rate = (tx - ptx) / dls;
            }
            g_net_rx = rx; g_net_tx = tx; g_net_ts = now;
            if (iface[0]) snprintf(g_net_iface, sizeof g_net_iface, "%s", iface);
            pthread_mutex_unlock(&g_lock);
            prx = rx; ptx = tx; pts = now;
        }
        sleep(2);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* 首页板块配置（steward-prefs.json）                                   */
/* ------------------------------------------------------------------ */

static const char *BLOCK_IDS[] = { "disk", "net", "videos", "queue", "gate", NULL };

static int block_known(const char *id)
{
    int i;
    for (i = 0; BLOCK_IDS[i]; i++) if (!strcmp(BLOCK_IDS[i], id)) return 1;
    return 0;
}

static int api_net(buf_t *out, char *err, size_t errlen)
{
    long long now;
    int age = -1;

    (void)err; (void)errlen;
    now = (long long)time(NULL);
    buf_puts(out, "{\"ok\":true,\"file\":\"/proc/net/dev\"");
    pthread_mutex_lock(&g_lock);
    if (g_net_ts) age = (int)(now - g_net_ts);
    buf_puts(out, ",\"iface\":");
    json_str(out, g_net_iface);
    buf_printf(out, ",\"rxRate\":%lld,\"txRate\":%lld,\"rx\":%lld,\"tx\":%lld"
                    ",\"ts\":%lld,\"age\":%d",
               g_net_rx_rate, g_net_tx_rate, g_net_rx, g_net_tx, g_net_ts, age);
    pthread_mutex_unlock(&g_lock);
    buf_printf(out, ",\"prefsFile\":");
    json_str(out, g_prefs_path);
    buf_putc(out, '}');
    return 1;
}

static int api_prefs_get(buf_t *out, char *err, size_t errlen)
{
    FILE *f;
    char raw[2048];
    size_t n;

    (void)err; (void)errlen;
    n = 0;
    f = g_prefs_path[0] ? fopen(g_prefs_path, "r") : NULL;
    if (f) {
        n = fread(raw, 1, sizeof raw - 1, f);
        fclose(f);
        raw[n] = 0;
    }
    /* 文件不在或者读出来不像个 JSON 对象 -> prefs 给 null，页面用自带默认 */
    while (n > 0 && (raw[n-1] == '\n' || raw[n-1] == '\r' || raw[n-1] == ' ')) raw[--n] = 0;
    {
        char *p = raw;
        while (*p == ' ' || *p == '\t') p++;
        buf_puts(out, "{\"ok\":true,\"file\":");
        json_str(out, g_prefs_path);
        if (*p == '{' && n > 0 && strstr(p, "\"blocks\"")) {
            buf_puts(out, ",\"prefs\":");
            buf_puts(out, p);
        } else {
            buf_puts(out, ",\"prefs\":null");
        }
        buf_putc(out, '}');
    }
    return 1;
}

/* POST /api/prefs  {"blocks":["disk","net","videos","queue","gate"]}
 * 只认白名单里的块名，去重，写回一份规范化的文件（不存页面上送来的原文）。 */
static int api_prefs_post(const char *body, buf_t *out, char *err, size_t errlen)
{
    const char *p;
    buf_t canon;
    FILE *f;
    int n = 0;

    if (!body) { snprintf(err, errlen, "空请求"); return 0; }
    p = strstr(body, "\"blocks\"");
    if (!p) { snprintf(err, errlen, "没给 blocks"); return 0; }
    p = strchr(p, '[');
    if (!p) { snprintf(err, errlen, "blocks 不是数组"); return 0; }

    buf_init(&canon);
    buf_puts(&canon, "{\"blocks\":[");
    for (p++; *p && *p != ']'; ) {
        char id[32];
        size_t k = 0;

        if (*p != '"') { p++; continue; }
        p++;
        while (*p && *p != '"' && k < sizeof id - 1) {
            if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) {
                buf_free(&canon);
                snprintf(err, errlen, "块名里有不认的字符");
                return 0;
            }
            id[k++] = *p++;
        }
        if (*p != '"') { buf_free(&canon); snprintf(err, errlen, "块名没闭合"); return 0; }
        p++;
        id[k] = 0;
        if (!block_known(id)) { buf_free(&canon); snprintf(err, errlen, "不认识的块：%.20s", id); return 0; }
        {
            char pat[40];
            snprintf(pat, sizeof pat, "\"%s\"", id);
            if (strstr(canon.p, pat)) continue;   /* 去重 */
        }
        if (n) buf_putc(&canon, ',');
        buf_printf(&canon, "\"%s\"", id);
        n++;
    }
    buf_puts(&canon, "]}");

    if (!g_prefs_path[0]) {
        buf_free(&canon);
        snprintf(err, errlen, "没定下 prefs 文件位置");
        return 0;
    }
    f = fopen(g_prefs_path, "w");
    if (!f) {
        buf_free(&canon);
        snprintf(err, errlen, "写不了 %.200s（%s）", g_prefs_path, strerror(errno));
        return 0;
    }
    fprintf(f, "%s\n", canon.p);
    if (fflush(f) != 0) { fclose(f); buf_free(&canon);
        snprintf(err, errlen, "写 %.200s 失败", g_prefs_path); return 0; }
    fclose(f);

    buf_printf(out, "{\"ok\":true,\"blocks\":%d,\"prefs\":", n);
    buf_puts(out, canon.p);
    buf_puts(out, ",\"file\":");
    json_str(out, g_prefs_path);
    buf_putc(out, '}');
    buf_free(&canon);
    return 1;
}

/* ------------------------------------------------------------------ */
/* A 视图：记录 + 本地状态                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    long long id;
    char name[512];
    char upper[512];
    char bvid[64];
    char folder[PATH_MAX];
    long long fav, col, wl, sub;
    char group[600];
    char group_key[64];
    char group_path[PATH_MAX];
    int pages;       /* page 表里的行数 */
    int withpath;    /* 其中已经有落盘路径的页数（未下载的页 path 为空） */
    int present;     /* 路径指向的文件确实在的页数 */
    int want;        /* should_download：bili-sync 认为这条要不要下 */
    long long size;
} rec_t;

static void fill_group(rec_t *r,
                       const src_t *fav, int nfav,
                       const src_t *col, int ncol,
                       const src_t *wl,  int nwl,
                       const src_t *sub, int nsub)
{
    const src_t *s;

    if (r->fav && (s = src_find(fav, nfav, r->fav)) != NULL) {
        snprintf(r->group, sizeof r->group, "收藏夹 · %s", s->name);
        snprintf(r->group_key, sizeof r->group_key, "fav:%lld", r->fav);
        snprintf(r->group_path, sizeof r->group_path, "%s", s->path);
    } else if (r->col && (s = src_find(col, ncol, r->col)) != NULL) {
        snprintf(r->group, sizeof r->group, "合集 · %s", s->name);
        snprintf(r->group_key, sizeof r->group_key, "col:%lld", r->col);
        snprintf(r->group_path, sizeof r->group_path, "%s", s->path);
    } else if (r->wl && (s = src_find(wl, nwl, r->wl)) != NULL) {
        snprintf(r->group, sizeof r->group, "稍后再看");
        snprintf(r->group_key, sizeof r->group_key, "wl:%lld", r->wl);
        snprintf(r->group_path, sizeof r->group_path, "%s", s->path);
    } else if (r->sub && (s = src_find(sub, nsub, r->sub)) != NULL) {
        snprintf(r->group, sizeof r->group, "投稿 · %s", s->name);
        snprintf(r->group_key, sizeof r->group_key, "sub:%lld", r->sub);
        snprintf(r->group_path, sizeof r->group_path, "%s", s->path);
    } else {
        snprintf(r->group, sizeof r->group, "未归源");
        snprintf(r->group_key, sizeof r->group_key, "other");
        r->group_path[0] = 0;
    }
}

static const char *status_of(const rec_t *r)
{
    if (r->pages <= 0) return "nopages";          /* 没有分页行 */
    if (r->withpath == 0) return "undownloaded";  /* 有分页行但都没落盘路径 = 还没下过 */
    if (r->present == 0) return "missing";        /* 下过，文件没了 */
    if (r->present == r->pages) return "exists";  /* 每一页都在 */
    return "partial";                             /* 有的在、有的不在（或缺分页） */
}

/* 下载列表的三档，只看 bili-sync 自己的两个开关 */
static const char *bucket_of(const rec_t *r)
{
    if (!r->want) return "off";                            /* 不下 */
    if (r->pages > 0 && r->withpath == r->pages) return "done"; /* 要下、且每一页都有落盘路径 */
    return "pending";                                      /* 要下、还没下完 */
}

static int api_records(buf_t *out, char *err, size_t errlen)
{
    sqlite3 *db;
    sqlite3_stmt *st = NULL, *sp = NULL;
    src_t *fav = NULL, *col = NULL, *wl = NULL, *sub = NULL;
    int nfav, ncol, nwl, nsub;
    rec_t *recs = NULL;
    int nrec = 0, cap = 0, i, cur = 0;
    int c_exists = 0, c_partial = 0, c_missing = 0, c_nopages = 0, c_undl = 0;
    int c_pending = 0, c_done = 0, c_off = 0;

    db = db_open();
    if (!db) {
        snprintf(err, errlen, "打不开 data.sqlite：%.200s（%.120s）", g_db, strerror(errno));
        return 0;
    }

    nfav = load_src(db, "favorite",    "name",              &fav);
    ncol = load_src(db, "collection",  "name",              &col);
    nwl  = load_src(db, "watch_later", NULL,                &wl);
    nsub = load_src(db, "submission",  "upper_name",        &sub);

    if (sqlite3_prepare_v2(db,
            "SELECT id, name, upper_name, bvid, path,"
            " favorite_id, collection_id, watch_later_id, submission_id, should_download"
            " FROM video ORDER BY id", -1, &st, NULL) != SQLITE_OK) {
        snprintf(err, errlen, "video 表查询失败：%s", sqlite3_errmsg(db));
        goto done;
    }

    while (sqlite3_step(st) == SQLITE_ROW) {
        rec_t *r;
        const char *txt;

        if (nrec == cap) {
            cap = cap ? cap * 2 : 256;
            recs = realloc(recs, (size_t)cap * sizeof *recs);
            if (!recs) { snprintf(err, errlen, "内存不足"); goto done; }
        }
        r = &recs[nrec];
        memset(r, 0, sizeof *r);
        r->id = sqlite3_column_int64(st, 0);
        txt = (const char *)sqlite3_column_text(st, 1);
        snprintf(r->name, sizeof r->name, "%s", txt ? txt : "");
        txt = (const char *)sqlite3_column_text(st, 2);
        snprintf(r->upper, sizeof r->upper, "%s", txt ? txt : "");
        txt = (const char *)sqlite3_column_text(st, 3);
        snprintf(r->bvid, sizeof r->bvid, "%s", txt ? txt : "");
        txt = (const char *)sqlite3_column_text(st, 4);
        snprintf(r->folder, sizeof r->folder, "%s", txt ? txt : "");
        r->fav = sqlite3_column_int64(st, 5);
        r->col = sqlite3_column_int64(st, 6);
        r->wl  = sqlite3_column_int64(st, 7);
        r->sub = sqlite3_column_int64(st, 8);
        r->want = sqlite3_column_int(st, 9) ? 1 : 0;
        fill_group(r, fav, nfav, col, ncol, wl, nwl, sub, nsub);
        nrec++;
    }
    sqlite3_finalize(st); st = NULL;

    /* page 表与 video 表都按 id 升序，一遍扫过对起来 */
    if (sqlite3_prepare_v2(db, "SELECT video_id, path FROM page ORDER BY video_id, id",
                           -1, &sp, NULL) != SQLITE_OK) {
        snprintf(err, errlen, "page 表查询失败：%s", sqlite3_errmsg(db));
        goto done;
    }
    while (sqlite3_step(sp) == SQLITE_ROW) {
        long long vid = sqlite3_column_int64(sp, 0);
        const char *p = (const char *)sqlite3_column_text(sp, 1);
        long long sz = 0;

        while (cur < nrec && recs[cur].id < vid) cur++;
        if (cur >= nrec || recs[cur].id != vid) continue;
        recs[cur].pages++;
        if (p && *p) {
            recs[cur].withpath++;
        }
        if (p && stat_reg(p, &sz)) {
            recs[cur].present++;
            recs[cur].size += sz;
        }
    }
    sqlite3_finalize(sp); sp = NULL;

    for (i = 0; i < nrec; i++) {
        const char *s = status_of(&recs[i]);
        if (!strcmp(s, "exists")) c_exists++;
        else if (!strcmp(s, "partial")) c_partial++;
        else if (!strcmp(s, "missing")) c_missing++;
        else if (!strcmp(s, "undownloaded")) c_undl++;
        else c_nopages++;

        if (!strcmp(bucket_of(&recs[i]), "pending")) c_pending++;
        else if (!strcmp(bucket_of(&recs[i]), "done")) c_done++;
        else c_off++;
    }

    buf_puts(out, "{\"ok\":true,\"db\":");
    json_str(out, g_db);
    buf_puts(out, ",\"root\":");
    json_str(out, g_root);
    buf_printf(out, ",\"count\":%d,\"summary\":{\"exists\":%d,\"partial\":%d,\"missing\":%d,"
                    "\"undownloaded\":%d,\"nopages\":%d},"
                    "\"queue\":{\"pending\":%d,\"done\":%d,\"off\":%d}",
               nrec, c_exists, c_partial, c_missing, c_undl, c_nopages,
               c_pending, c_done, c_off);
    buf_puts(out, ",\"records\":[");

    for (i = 0; i < nrec; i++) {
        rec_t *r = &recs[i];
        if (i) buf_putc(out, ',');
        buf_printf(out, "{\"id\":%lld,\"name\":", r->id); json_str(out, r->name);
        buf_puts(out, ",\"upper\":"); json_str(out, r->upper);
        buf_puts(out, ",\"bvid\":");  json_str(out, r->bvid);
        buf_puts(out, ",\"folder\":"); json_str(out, r->folder);
        buf_printf(out, ",\"folderExists\":%s", stat_dir(r->folder) ? "true" : "false");
        buf_puts(out, ",\"group\":");     json_str(out, r->group);
        buf_puts(out, ",\"groupKey\":");  json_str(out, r->group_key);
        buf_puts(out, ",\"groupPath\":"); json_str(out, r->group_path);
        buf_printf(out, ",\"pages\":%d,\"withPath\":%d,\"present\":%d,\"size\":%lld"
                        ",\"want\":%s,\"bucket\":",
                   r->pages, r->withpath, r->present, r->size, r->want ? "true" : "false");
        json_str(out, bucket_of(r));
        buf_puts(out, ",\"status\":");
        json_str(out, status_of(r));
        buf_putc(out, '}');
    }
    buf_puts(out, "]}");

done:
    if (st) sqlite3_finalize(st);
    if (sp) sqlite3_finalize(sp);
    free(recs);
    free(fav); free(col); free(wl); free(sub);
    sqlite3_close(db);
    return err[0] == 0;
}

/* ------------------------------------------------------------------ */
/* B 视图：/downloads 目录树                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    char rel[PATH_MAX];
    char abs[PATH_MAX];
    long long size;
    int files;
    long long mtime;
} item_t;

static item_t *g_items = NULL;
static int g_nitems = 0, g_icap = 0;

static void item_add(const char *rel, const char *abs, long long size, int files, long long mtime)
{
    item_t *it;

    if (g_nitems == g_icap) {
        g_icap = g_icap ? g_icap * 2 : 64;
        g_items = realloc(g_items, (size_t)g_icap * sizeof *g_items);
        if (!g_items) { perror("realloc"); exit(1); }
    }
    it = &g_items[g_nitems++];
    snprintf(it->rel, sizeof it->rel, "%s", rel);
    snprintf(it->abs, sizeof it->abs, "%s", abs);
    it->size = size;
    it->files = files;
    it->mtime = mtime;
}

/* 扫一层：本层有普通文件就当「一个视频文件夹」收下（整棵子树计入体积，不再往里钻）；
 * 本层没有普通文件就继续往子目录走。 */
static void scan_dir(const char *rel, const char *abs)
{
    DIR *d;
    struct dirent *e;
    char **subs = NULL;
    int nsubs = 0, cap = 0, i;
    int has_file = 0;
    long long mtime = 0;

    d = opendir(abs);
    if (!d) return;

    while ((e = readdir(d)) != NULL) {
        struct stat st;
        char child[PATH_MAX * 2];

        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(child, sizeof child, "%s/%s", abs, e->d_name);
        if (lstat(child, &st) != 0) continue;

        if (S_ISREG(st.st_mode)) {
            has_file = 1;
            if (st.st_mtime > mtime) mtime = st.st_mtime;
        } else if (S_ISDIR(st.st_mode)) {
            if (nsubs == cap) {
                cap = cap ? cap * 2 : 16;
                subs = realloc(subs, (size_t)cap * sizeof *subs);
                if (!subs) { closedir(d); return; }
            }
            subs[nsubs++] = strdup(e->d_name);
        }
    }
    closedir(d);

    if (has_file && rel[0]) {
        long long size = 0;
        int files = 0;
        tree_size(abs, &size, &files);
        item_add(rel, abs, size, files, mtime);
        for (i = 0; i < nsubs; i++) free(subs[i]);
        free(subs);
        return;
    }

    if (nsubs > 1) qsort(subs, (size_t)nsubs, sizeof *subs, cmp_name);
    for (i = 0; i < nsubs; i++) {
        char crel[PATH_MAX], cabs[PATH_MAX * 2];
        if (rel[0]) snprintf(crel, sizeof crel, "%s/%s", rel, subs[i]);
        else        snprintf(crel, sizeof crel, "%s", subs[i]);
        snprintf(cabs, sizeof cabs, "%s/%s", abs, subs[i]);
        scan_dir(crel, cabs);
        free(subs[i]);
    }
    free(subs);
}

/* 取父目录相对路径：收藏夹/测试/某个视频 -> 收藏夹/测试 */
static void dir_of(const char *rel, char *out, size_t outlen)
{
    const char *s = strrchr(rel, '/');
    size_t n;

    out[0] = 0;
    if (!s) return;
    n = (size_t)(s - rel);
    if (n >= outlen) n = outlen - 1;
    memcpy(out, rel, n);
    out[n] = 0;
}

static int api_files(buf_t *out, char *err, size_t errlen)
{
    int i, g;
    long long total = 0;
    char cur_group[PATH_MAX];

    g_nitems = 0;
    if (!stat_dir(g_root)) {
        snprintf(err, errlen, "找不到下载目录：%.300s", g_root);
        return 0;
    }

    scan_dir("", g_root);

    for (i = 0; i < g_nitems; i++) total += g_items[i].size;

    buf_puts(out, "{\"ok\":true,\"root\":");
    json_str(out, g_root);
    buf_printf(out, ",\"count\":%d,\"totalSize\":%lld,\"groups\":[", g_nitems, total);

    cur_group[0] = 0;
    g = 0;
    for (i = 0; i < g_nitems; i++) {
        char grp[PATH_MAX];

        dir_of(g_items[i].rel, grp, sizeof grp);
        if (strcmp(grp, cur_group) != 0) {
            if (g) buf_puts(out, "]}");
            snprintf(cur_group, sizeof cur_group, "%s", grp);
            if (g) buf_putc(out, ',');
            buf_puts(out, "{\"group\":");
            json_str(out, cur_group);
            buf_puts(out, ",\"items\":[");
            g++;
        } else {
            buf_putc(out, ',');
        }
        buf_puts(out, "{\"rel\":");  json_str(out, g_items[i].rel);
        buf_puts(out, ",\"abs\":");  json_str(out, g_items[i].abs);
        buf_printf(out, ",\"size\":%lld,\"files\":%d,\"mtime\":%lld}",
                   g_items[i].size, g_items[i].files, g_items[i].mtime);
    }
    if (g) buf_puts(out, "]}");
    buf_puts(out, "]}");
    return 1;
}

/* ------------------------------------------------------------------ */
/* 删除                                                                */
/* ------------------------------------------------------------------ */

/* 源目录清单：favorite / collection / watch_later / submission 四张表各自的 path。
 * 删除守卫要拿它挡住「把某个源的整个下载目录端掉」这种手滑。 */
static char g_srcpath[128][PATH_MAX];
static int  g_nsrcpath = 0;

static void load_source_paths(void)
{
    static const char *tables[] = { "favorite", "collection", "watch_later", "submission" };
    sqlite3 *db = db_open();
    size_t t;

    g_nsrcpath = 0;
    if (!db) return;
    for (t = 0; t < sizeof tables / sizeof tables[0]; t++) {
        sqlite3_stmt *st = NULL;
        char sql[128];
        snprintf(sql, sizeof sql, "SELECT path FROM %s WHERE path IS NOT NULL AND path <> ''", tables[t]);
        if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) continue;
        while (sqlite3_step(st) == SQLITE_ROW && g_nsrcpath < 128) {
            const char *p = (const char *)sqlite3_column_text(st, 0);
            char real[PATH_MAX];
            if (!p) continue;
            if (!realpath(p, real)) snprintf(real, sizeof real, "%s", p);
            snprintf(g_srcpath[g_nsrcpath++], PATH_MAX, "%s", real);
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
}

/* 允许删的范围：根目录之下、比每个源目录都更深（源目录本身与它的上级都挡掉） */
static int delete_allowed(const char *path, char *why, size_t whylen)
{
    char real[PATH_MAX];
    const char *rel;
    int depth = 0, i;
    const char *p;

    if (!path || path[0] != '/') {
        snprintf(why, whylen, "不是绝对路径");
        return 0;
    }
    if (strstr(path, "..")) {
        snprintf(why, whylen, "路径里有 ..");
        return 0;
    }
    if (!realpath(path, real)) {
        snprintf(why, whylen, "路径不存在");
        return 0;
    }
    if (!strcmp(real, g_root_real)) {
        snprintf(why, whylen, "拒绝删下载根目录");
        return 0;
    }
    if (strncmp(real, g_root_real, strlen(g_root_real)) != 0 ||
        real[strlen(g_root_real)] != '/') {
        snprintf(why, whylen, "不在下载目录里");
        return 0;
    }
    rel = real + strlen(g_root_real) + 1;
    for (p = rel; *p; p++)
        if (*p == '/') depth++;
    if (depth < 1) {
        snprintf(why, whylen, "拒绝删源目录本身");
        return 0;
    }
    for (i = 0; i < g_nsrcpath; i++) {
        size_t k = strlen(real);
        if (!strncmp(g_srcpath[i], real, k) && (g_srcpath[i][k] == '/' || g_srcpath[i][k] == 0)) {
            snprintf(why, whylen, "拒绝删源目录或其上级");
            return 0;
        }
    }
    if (!stat_dir(real)) {
        snprintf(why, whylen, "不是目录");
        return 0;
    }
    return 1;
}

/* 极简 JSON 字符串数组抽取：{"paths":["a","b"]} */
static int json_strings(const char *body, const char *key, char ***out)
{
    const char *p, *q;
    char **arr = NULL;
    int n = 0, cap = 0;

    *out = NULL;
    p = strstr(body, key);
    if (!p) return 0;
    p = strchr(p, '[');
    if (!p) return 0;
    p++;

    while (*p) {
        char tmp[PATH_MAX * 2];
        size_t n2 = 0;

        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',') p++;
        if (*p == ']' || !*p) break;
        if (*p != '"') { p++; continue; }
        p++;
        q = p;
        while (*q && *q != '"') {
            if (*q == '\\' && q[1] == 'u' &&
                isxdigit((unsigned char)q[2]) && isxdigit((unsigned char)q[3]) &&
                isxdigit((unsigned char)q[4]) && isxdigit((unsigned char)q[5])) {
                unsigned long cp;
                char hex[5];
                memcpy(hex, q + 2, 4);
                hex[4] = 0;
                cp = strtoul(hex, NULL, 16);
                q += 6;
                /* 代理对 */
                if (cp >= 0xD800 && cp <= 0xDBFF &&
                    q[0] == '\\' && q[1] == 'u' &&
                    isxdigit((unsigned char)q[2]) && isxdigit((unsigned char)q[3]) &&
                    isxdigit((unsigned char)q[4]) && isxdigit((unsigned char)q[5])) {
                    unsigned long lo;
                    memcpy(hex, q + 2, 4);
                    hex[4] = 0;
                    lo = strtoul(hex, NULL, 16);
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        q += 6;
                    }
                }
                if (cp < 0x80) {
                    tmp[n2++] = (char)cp;
                } else if (cp < 0x800) {
                    tmp[n2++] = (char)(0xC0 | (cp >> 6));
                    tmp[n2++] = (char)(0x80 | (cp & 0x3F));
                } else if (cp < 0x10000) {
                    tmp[n2++] = (char)(0xE0 | (cp >> 12));
                    tmp[n2++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    tmp[n2++] = (char)(0x80 | (cp & 0x3F));
                } else {
                    tmp[n2++] = (char)(0xF0 | (cp >> 18));
                    tmp[n2++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                    tmp[n2++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    tmp[n2++] = (char)(0x80 | (cp & 0x3F));
                }
                continue;
            }
            if (*q == '\\') {
                q++;
                if (!*q) break;
                switch (*q) {
                case 'n': tmp[n2++] = '\n'; break;
                case 't': tmp[n2++] = '\t'; break;
                case 'r': tmp[n2++] = '\r'; break;
                case 'b': tmp[n2++] = '\b'; break;
                case 'f': tmp[n2++] = '\f'; break;
                case '/': tmp[n2++] = '/';  break;
                case '"': tmp[n2++] = '"';  break;
                case '\\': tmp[n2++] = '\\'; break;
                default: tmp[n2++] = *q; break;
                }
                q++;
                continue;
            }
            tmp[n2++] = *q++;
            if (n2 > sizeof tmp - 8) break;
        }
        tmp[n2] = 0;
        if (n == cap) {
            cap = cap ? cap * 2 : 8;
            arr = realloc(arr, (size_t)cap * sizeof *arr);
            if (!arr) return 0;
        }
        arr[n++] = strdup(tmp);
        if (*q == '"') q++;
        p = q;
    }
    *out = arr;
    return n;
}

static int api_delete(const char *body, buf_t *out, char *err, size_t errlen)
{
    char **paths = NULL;
    int n, i;
    int del = 0, fail = 0;
    buf_t ok, bad;

    n = json_strings(body, "\"paths\"", &paths);
    if (n <= 0) {
        snprintf(err, errlen, "没有要删的路径");
        return 0;
    }

    load_source_paths();

    buf_init(&ok);
    buf_init(&bad);

    for (i = 0; i < n; i++) {
        char why[256] = "";
        if (delete_allowed(paths[i], why, sizeof why)) {
            if (rm_rf(paths[i]) == 0) {
                if (del++) buf_putc(&ok, ',');
                json_str(&ok, paths[i]);
            } else {
                if (fail++) buf_putc(&bad, ',');
                buf_putc(&bad, '{');
                buf_puts(&bad, "\"path\":"); json_str(&bad, paths[i]);
                buf_puts(&bad, ",\"error\":"); json_str(&bad, strerror(errno));
                buf_putc(&bad, '}');
            }
        } else {
            if (fail++) buf_putc(&bad, ',');
            buf_putc(&bad, '{');
            buf_puts(&bad, "\"path\":"); json_str(&bad, paths[i]);
            buf_puts(&bad, ",\"error\":"); json_str(&bad, why);
            buf_putc(&bad, '}');
        }
        free(paths[i]);
    }
    free(paths);

    buf_printf(out, "{\"ok\":true,\"deletedCount\":%d,\"failedCount\":%d,\"deleted\":[", del, fail);
    buf_puts(out, ok.p);
    buf_puts(out, "],\"failed\":[");
    buf_puts(out, bad.p);
    buf_puts(out, "]}");

    buf_free(&ok);
    buf_free(&bad);
    return 1;
}

/* ------------------------------------------------------------------ */
/* C 视图：下载列表的动作                                               */
/* ------------------------------------------------------------------ */

/* POST /api/queue  {"action":"want|unwant|delete|redownload","ids":[1,2,3]} */
static int api_queue(const char *body, buf_t *out, char *err, size_t errlen)
{
    char action[32];
    long long *ids;
    int n = 0, i, rc = 0, affected = 0;
    sqlite3 *db;
    buf_t idlist, sql;

    if (!json_str_value(body, "action", action, sizeof action)) {
        snprintf(err, errlen, "没给 action");
        return 0;
    }
    ids = json_ints(body, "ids", &n);
    if (n <= 0) {
        free(ids);
        snprintf(err, errlen, "没给 ids");
        return 0;
    }

    buf_init(&idlist);
    for (i = 0; i < n; i++) {
        if (i) buf_putc(&idlist, ',');
        buf_printf(&idlist, "%lld", ids[i]);
    }
    free(ids);

    db = db_open_rw();
    if (!db) {
        buf_free(&idlist);
        snprintf(err, errlen, "打不开 %.200s（写权限？）", g_db);
        return 0;
    }

    buf_init(&sql);
    if (!strcmp(action, "want") || !strcmp(action, "unwant")) {
        buf_printf(&sql, "UPDATE video SET should_download = %d WHERE id IN (%s)",
                   !strcmp(action, "want") ? 1 : 0, idlist.p);
    } else if (!strcmp(action, "delete")) {
        buf_printf(&sql, "DELETE FROM page WHERE video_id IN (%s);"
                         "DELETE FROM video WHERE id IN (%s)", idlist.p, idlist.p);
    } else if (!strcmp(action, "redownload")) {
        /* 跟上游 clear-and-reset-status 一个路子：清掉分页行、状态归零，
         * 下一轮 bili-sync 重新抓一次分页再下。区别是盘上的旧文件不动。 */
        buf_printf(&sql, "DELETE FROM page WHERE video_id IN (%s);"
                         "UPDATE video SET should_download = 1, download_status = 0"
                         " WHERE id IN (%s)", idlist.p, idlist.p);
    } else {
        snprintf(err, errlen, "不认识的 action：%s", action);
        buf_free(&sql);
        buf_free(&idlist);
        sqlite3_close(db);
        return 0;
    }

    {
        char *msg = NULL;
        rc = sqlite3_exec(db, sql.p, NULL, NULL, &msg);
        if (rc == SQLITE_OK) affected = sqlite3_changes(db);
        else snprintf(err, errlen, "改库失败：%s", msg ? msg : "?");
        sqlite3_free(msg);
    }
    sqlite3_close(db);
    buf_free(&sql);
    buf_free(&idlist);

    if (rc != SQLITE_OK) return 0;
    buf_printf(out, "{\"ok\":true,\"action\":");
    json_str(out, action);
    buf_printf(out, ",\"requested\":%d,\"affected\":%d}", n, affected);
    return 1;
}

/* body 里 key 是不是 true（只认布尔字面量，够用） */
static int json_has_true(const char *body, const char *key)
{
    char pat[64];
    const char *p;

    if (!body) return 0;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    p = strstr(body, pat);
    if (!p) return 0;
    p = strchr(p + strlen(pat), ':');
    if (!p) return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    return strncmp(p, "true", 4) == 0;
}

/* POST /api/purge  {"action":"stale","dry":true|false}
 *
 * 「僵尸完成标记」：库里写着这条已完成（download_status <> 0）也记了落盘路径，
 * 但那些文件一个都不在盘上 —— 多半是下过、后来把文件删了。这类条目 bili-sync
 * 自己不会去核对，会一直算「已完成」。
 *
 * 这个动作把它们收回完成标记、清掉落盘路径，并标成 should_download = 0（不下），
 * 免得下一轮 bili-sync 又照下载状态把整条重下一遍。只动库，不碰任何文件。
 * 想重下就在「下载列表」里勾上「加入下载队列」，再点「重置并重下」。
 */
static int api_purge(const char *body, buf_t *out, char *err, size_t errlen)
{
    sqlite3 *db;
    sqlite3_stmt *sv = NULL, *sp = NULL;
    long long *cand = NULL;
    int *npages = NULL;
    char *haspath = NULL, *alive = NULL;
    int ncand = 0, cap = 0, i, nstale = 0, npages_total = 0, cur = 0;
    int dry = json_has_true(body, "dry");
    buf_t ids;

    {
        char action[32];
        if (json_str_value(body, "action", action, sizeof action) && strcmp(action, "stale") != 0) {
            snprintf(err, errlen, "不认识的 action：%s", action);
            return 0;
        }
    }

    db = db_open_rw();
    if (!db) {
        snprintf(err, errlen, "打不开 %.200s（写权限？）", g_db);
        return 0;
    }

    /* 先圈出所有「库里说完成」的条目 */
    if (sqlite3_prepare_v2(db, "SELECT id FROM video WHERE download_status <> 0 ORDER BY id",
                           -1, &sv, NULL) != SQLITE_OK) {
        snprintf(err, errlen, "video 表查询失败：%s", sqlite3_errmsg(db));
        sqlite3_close(db);
        return 0;
    }
    while (sqlite3_step(sv) == SQLITE_ROW) {
        if (ncand == cap) {
            cap = cap ? cap * 2 : 256;
            cand    = realloc(cand,    (size_t)cap * sizeof *cand);
            npages  = realloc(npages,  (size_t)cap * sizeof *npages);
            haspath = realloc(haspath, (size_t)cap);
            alive   = realloc(alive,   (size_t)cap);
            if (!cand || !npages || !haspath || !alive) {
                snprintf(err, errlen, "内存不足");
                sqlite3_finalize(sv);
                free(cand); free(npages); free(haspath); free(alive);
                sqlite3_close(db);
                return 0;
            }
        }
        cand[ncand] = sqlite3_column_int64(sv, 0);
        npages[ncand] = 0;
        haspath[ncand] = 0;
        alive[ncand] = 0;
        ncand++;
    }
    sqlite3_finalize(sv); sv = NULL;

    /* 两个表都按 video_id 升序，一遍扫过对起来 */
    if (sqlite3_prepare_v2(db, "SELECT video_id, path FROM page ORDER BY video_id, id",
                           -1, &sp, NULL) != SQLITE_OK) {
        snprintf(err, errlen, "page 表查询失败：%s", sqlite3_errmsg(db));
        goto done;
    }
    while (sqlite3_step(sp) == SQLITE_ROW) {
        long long vid = sqlite3_column_int64(sp, 0);
        const char *p = (const char *)sqlite3_column_text(sp, 1);
        long long sz = 0;

        while (cur < ncand && cand[cur] < vid) cur++;
        if (cur >= ncand || cand[cur] != vid) continue;
        npages[cur]++;
        if (p && *p) {
            haspath[cur] = 1;
            if (stat_reg(p, &sz)) alive[cur] = 1;
        }
    }
    sqlite3_finalize(sp); sp = NULL;

    /* 僵尸 = 记过落盘路径（说明下过），但一个文件都不在了。
     * 没记过路径的（从没下过）不算，免得碰上正在下的条目。 */
    buf_init(&ids);
    for (i = 0; i < ncand; i++) {
        if (haspath[i] && !alive[i]) {
            if (nstale) buf_putc(&ids, ',');
            buf_printf(&ids, "%lld", cand[i]);
            nstale++;
            npages_total += npages[i];
        }
    }

    if (nstale && !dry) {
        buf_t sql;
        char *msg = NULL;
        int rc;

        buf_init(&sql);
        buf_printf(&sql,
                   "UPDATE page SET path = '', download_status = 0 WHERE video_id IN (%s);"
                   "UPDATE video SET path = '', download_status = 0, should_download = 0"
                   " WHERE id IN (%s)", ids.p, ids.p);
        rc = sqlite3_exec(db, sql.p, NULL, NULL, &msg);
        buf_free(&sql);
        if (rc != SQLITE_OK) {
            snprintf(err, errlen, "改库失败：%s", msg ? msg : "?");
            sqlite3_free(msg);
            buf_free(&ids);
            goto done;
        }
        sqlite3_free(msg);
    }

    buf_printf(out, "{\"ok\":true,\"dry\":%s,\"stale\":%d,\"pages\":%d,\"note\":",
               dry ? "true" : "false", nstale, npages_total);
    json_str(out, dry ? "只数不改。" :
              "收回完成标记、清掉落盘路径，并标成「不下」；盘上的文件没动。");
    buf_putc(out, '}');
    buf_free(&ids);

done:
    free(cand); free(npages); free(haspath); free(alive);
    sqlite3_close(db);
    return err[0] ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* 磁盘与闸门                                                          */
/* ------------------------------------------------------------------ */

static int api_disk(buf_t *out, char *err, size_t errlen)
{
    long long total = 0, freeb = 0;
    int pct = 100;
    char hum[32];

    (void)err;
    (void)errlen;
    disk_usage(g_root, &total, &freeb, &pct);
    human_size(freeb, hum, sizeof hum);

    buf_puts(out, "{\"ok\":true,\"root\":");
    json_str(out, g_root);
    buf_printf(out, ",\"total\":%lld,\"free\":%lld,\"pct\":%d,\"floor\":%d,\"human\":",
               total, freeb, pct, g_floor_pct);
    json_str(out, hum);
    pthread_mutex_lock(&g_lock);
    buf_printf(out, ",\"braked\":%s,\"pushed\":%d,\"lastPush\":%lld,\"checkEvery\":%d,\"note\":",
               g_braked ? "true" : "false", g_push_count, g_last_push, g_check_every);
    json_str(out, g_brake_note);
    pthread_mutex_unlock(&g_lock);
    buf_puts(out, ",\"pushUrl\":");
    json_str(out, g_push_url);
    buf_puts(out, ",\"snapshot\":");
    json_str(out, g_brake_path);
    pthread_mutex_lock(&g_lock);
    buf_printf(out, ",\"watch\":{\"on\":%s,\"onDisk\":%d,\"pushed\":%d,\"lost\":%d,\"lastPush\":%lld}",
               g_watch_on ? "true" : "false", g_watch_seen, g_watch_pushed, g_watch_lost, g_watch_last);
    pthread_mutex_unlock(&g_lock);
    buf_puts(out, ",\"watchFile\":");
    json_str(out, g_watch_path);
    buf_putc(out, '}');
    return 1;
}

/* POST /api/brake  {"action":"release"|"on"} —— release 照快照恢复源；on 是手动拉一次 */
static int api_brake(const char *body, buf_t *out, char *err, size_t errlen)
{
    char action[32], note[256] = "";
    int n;

    if (!json_str_value(body, "action", action, sizeof action)) {
        snprintf(err, errlen, "没给 action");
        return 0;
    }
    if (!strcmp(action, "release")) {
        n = brake_off(note, sizeof note);
        pthread_mutex_lock(&g_lock);
        g_braked = 0;
        snprintf(g_brake_note, sizeof g_brake_note, "%s", n >= 0 ? note : "");
        pthread_mutex_unlock(&g_lock);
    } else if (!strcmp(action, "on")) {
        n = brake_on(note, sizeof note);
        pthread_mutex_lock(&g_lock);
        g_braked = 1;
        snprintf(g_brake_note, sizeof g_brake_note, "%s", note);
        pthread_mutex_unlock(&g_lock);
    } else {
        snprintf(err, errlen, "不认识的 action：%s", action);
        return 0;
    }
    if (n >= 0 && g_push_url[0]) {
        char text[512];
        snprintf(text, sizeof text, "bili-sync 管家：%s", note);
        push_text(text);
    }
    buf_printf(out, "{\"ok\":true,\"action\":");
    json_str(out, action);
    buf_printf(out, ",\"affected\":%d,\"note\":", n);
    json_str(out, note);
    buf_putc(out, '}');
    return 1;
}

/* ------------------------------------------------------------------ */
/* HTTP                                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    char method[8];
    char path[1024];
    char query[4096];
    char token[256];
    char *body;
    size_t body_len;
} req_t;

static void http_send(int fd, int code, const char *ctype, const char *body, size_t len)
{
    char head[512];
    int n;
    const char *reason = code == 200 ? "OK" : code == 400 ? "Bad Request"
                       : code == 403 ? "Forbidden" : code == 404 ? "Not Found"
                       : code == 405 ? "Method Not Allowed" : "Error";

    n = snprintf(head, sizeof head,
                 "HTTP/1.1 %d %s\r\n"
                 "Content-Type: %s\r\n"
                 "Content-Length: %zu\r\n"
                 "Cache-Control: no-store\r\n"
                 "Connection: close\r\n"
                 "\r\n",
                 code, reason, ctype, len);
    if (n > 0 && write(fd, head, (size_t)n) < 0) return;
    if (len) {
        size_t off = 0;
        while (off < len) {
            ssize_t w = write(fd, body + off, len - off);
            if (w <= 0) return;
            off += (size_t)w;
        }
    }
}

static void http_json(int fd, buf_t *b)
{
    http_send(fd, 200, "application/json; charset=utf-8", b->p, b->len);
}

static void http_err_json(int fd, int code, const char *msg)
{
    buf_t b;
    buf_init(&b);
    buf_puts(&b, "{\"ok\":false,\"error\":");
    json_str(&b, msg);
    buf_putc(&b, '}');
    http_send(fd, code, "application/json; charset=utf-8", b.p, b.len);
    buf_free(&b);
}

static void url_decode(const char *src, char *dst, size_t dstlen)
{
    size_t o = 0;
    while (*src && o + 1 < dstlen) {
        if (*src == '%' && src[1] && src[2]) {
            char h[3] = { src[1], src[2], 0 };
            dst[o++] = (char)strtol(h, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            dst[o++] = ' ';
            src++;
        } else {
            dst[o++] = *src++;
        }
    }
    dst[o] = 0;
}

static int query_param(const char *query, const char *key, char *out, size_t outlen)
{
    const char *p = query;
    size_t klen = strlen(key);

    out[0] = 0;
    while (p && *p) {
        if (!strncmp(p, key, klen) && p[klen] == '=') {
            const char *v = p + klen + 1;
            const char *end = strchr(v, '&');
            size_t n = end ? (size_t)(end - v) : strlen(v);
            char tmp[4096];
            if (n >= sizeof tmp) n = sizeof tmp - 1;
            memcpy(tmp, v, n);
            tmp[n] = 0;
            url_decode(tmp, out, outlen);
            return 1;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return 0;
}

static int read_request(int fd, req_t *r)
{
    char buf[65536];
    size_t used = 0;
    ssize_t n;
    char *hdr_end;
    long long clen = 0;
    char *p;

    memset(r, 0, sizeof *r);

    while (used < sizeof buf - 1) {
        n = read(fd, buf + used, sizeof buf - 1 - used);
        if (n <= 0) return 0;
        used += (size_t)n;
        buf[used] = 0;
        if ((hdr_end = strstr(buf, "\r\n\r\n")) != NULL) break;
    }
    if (!hdr_end) return 0;

    /* 请求行 */
    {
        char line[2048];
        char *q;
        size_t n2 = (size_t)(strchr(buf, '\r') - buf);
        if (n2 >= sizeof line) n2 = sizeof line - 1;
        memcpy(line, buf, n2);
        line[n2] = 0;
        q = strchr(line, ' ');
        if (!q) return 0;
        *q = 0;
        snprintf(r->method, sizeof r->method, "%.7s", line);
        q++;
        {
            char *sp = strchr(q, ' ');
            char url[2048];
            if (sp) *sp = 0;
            snprintf(url, sizeof url, "%s", q);
            {
                char *qmark = strchr(url, '?');
                if (qmark) {
                    *qmark = 0;
                    snprintf(r->query, sizeof r->query, "%s", qmark + 1);
                }
                url_decode(url, r->path, sizeof r->path);
            }
        }
    }

    /* Content-Length */
    p = buf;
    while (p && *p) {
        char *eol = strstr(p, "\r\n");
        if (!eol) break;
        if (!strncasecmp(p, "Content-Length:", 15))
            clen = atoll(p + 15);
        if (!strncasecmp(p, "X-Token:", 8)) {
            char *v = p + 8;
            while (*v == ' ') v++;
            snprintf(r->token, sizeof r->token, "%.*s", (int)(eol - v), v);
        }
        p = eol + 2;
        if (p == buf) break;
    }
    if (!r->token[0])
        query_param(r->query, "token", r->token, sizeof r->token);

    /* body */
    if (clen > 0) {
        size_t hdr_len = (size_t)(hdr_end + 4 - buf);
        size_t have = used - hdr_len;
        if (clen > 4 * 1024 * 1024) return 0;
        r->body = malloc((size_t)clen + 1);
        if (!r->body) return 0;
        if (have > (size_t)clen) have = (size_t)clen;
        memcpy(r->body, hdr_end + 4, have);
        while (have < (size_t)clen) {
            n = read(fd, r->body + have, (size_t)clen - have);
            if (n <= 0) break;
            have += (size_t)n;
        }
        r->body[have] = 0;
        r->body_len = have;
    }
    return 1;
}

static int token_ok(const req_t *r)
{
    if (!g_token[0]) return 1;
    if (!r->token[0]) return 0;
    return strcmp(r->token, g_token) == 0;
}

static void handle_conn(int fd)
{
    req_t r;
    buf_t out;
    char err[512] = "";

    if (!read_request(fd, &r)) {
        http_send(fd, 400, "text/plain; charset=utf-8", "bad request\n", 12);
        return;
    }

    if (!strcmp(r.method, "GET") && (!strcmp(r.path, "/") || !strcmp(r.path, "/index.html"))) {
        http_send(fd, 200, "text/html; charset=utf-8", INDEX_HTML, sizeof INDEX_HTML - 1);
        goto out;
    }
    if (!strcmp(r.method, "GET") && !strcmp(r.path, "/healthz")) {
        http_send(fd, 200, "text/plain; charset=utf-8", "ok\n", 3);
        goto out;
    }

    if (!strcmp(r.path, "/api/records") || !strcmp(r.path, "/api/files") ||
        !strcmp(r.path, "/api/delete") || !strcmp(r.path, "/api/queue") ||
        !strcmp(r.path, "/api/purge") || !strcmp(r.path, "/api/watch") ||
        !strcmp(r.path, "/api/disk") || !strcmp(r.path, "/api/brake") ||
        !strcmp(r.path, "/api/net") || !strcmp(r.path, "/api/prefs")) {
        if (!token_ok(&r)) {
            http_err_json(fd, 403, "token 不对（URL 或请求头要带 token）");
            goto out;
        }
    }

    buf_init(&out);
    if (!strcmp(r.method, "GET") && !strcmp(r.path, "/api/records")) {
        if (api_records(&out, err, sizeof err)) http_json(fd, &out);
        else http_err_json(fd, 500, err);
    } else if (!strcmp(r.method, "GET") && !strcmp(r.path, "/api/files")) {
        if (api_files(&out, err, sizeof err)) http_json(fd, &out);
        else http_err_json(fd, 500, err);
    } else if (!strcmp(r.method, "GET") && !strcmp(r.path, "/api/disk")) {
        if (api_disk(&out, err, sizeof err)) http_json(fd, &out);
        else http_err_json(fd, 500, err);
    } else if (!strcmp(r.method, "POST") && !strcmp(r.path, "/api/delete")) {
        if (api_delete(r.body ? r.body : "", &out, err, sizeof err)) http_json(fd, &out);
        else http_err_json(fd, 400, err);
    } else if (!strcmp(r.method, "POST") && !strcmp(r.path, "/api/queue")) {
        if (api_queue(r.body ? r.body : "", &out, err, sizeof err)) http_json(fd, &out);
        else http_err_json(fd, 400, err);
    } else if (!strcmp(r.method, "POST") && !strcmp(r.path, "/api/purge")) {
        if (api_purge(r.body ? r.body : "", &out, err, sizeof err)) http_json(fd, &out);
        else http_err_json(fd, 400, err);
    } else if (!strcmp(r.method, "POST") && !strcmp(r.path, "/api/watch")) {
        if (api_watch(r.body ? r.body : "", &out, err, sizeof err)) http_json(fd, &out);
        else http_err_json(fd, 400, err);
    } else if (!strcmp(r.method, "POST") && !strcmp(r.path, "/api/brake")) {
        if (api_brake(r.body ? r.body : "", &out, err, sizeof err)) http_json(fd, &out);
        else http_err_json(fd, 400, err);
    } else if (!strcmp(r.method, "GET") && !strcmp(r.path, "/api/net")) {
        if (api_net(&out, err, sizeof err)) http_json(fd, &out);
        else http_err_json(fd, 500, err);
    } else if (!strcmp(r.method, "GET") && !strcmp(r.path, "/api/prefs")) {
        if (api_prefs_get(&out, err, sizeof err)) http_json(fd, &out);
        else http_err_json(fd, 500, err);
    } else if (!strcmp(r.method, "POST") && !strcmp(r.path, "/api/prefs")) {
        if (api_prefs_post(r.body ? r.body : "", &out, err, sizeof err)) http_json(fd, &out);
        else http_err_json(fd, 400, err);
    } else {
        http_send(fd, 404, "text/plain; charset=utf-8", "not found\n", 10);
    }
    buf_free(&out);

out:
    free(r.body);
    if (g_verbose) {
        char line[2048];
        int n = snprintf(line, sizeof line, "%s %s -> done\n", r.method, r.path);
        if (n > 0) { if (write(1, line, (size_t)n) < 0) {} }
    }
}

static void *conn_thread(void *arg)
{
    int fd = (int)(intptr_t)arg;
    handle_conn(fd);
    close(fd);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* 自检（离线用，不上机）                                               */
/* ------------------------------------------------------------------ */

static int selftest(void)
{
    buf_t out;
    char err[512] = "";
    int ok;

    printf("db   = %s\n", g_db);
    printf("root = %s\n", g_root);
    printf("real root = %s\n", g_root_real);

    buf_init(&out);
    ok = api_records(&out, err, sizeof err);
    if (!ok) {
        printf("records: FAIL %s\n", err);
    } else {
        const char *s = strstr(out.p, "\"summary\":");
        const char *e = s ? strchr(s, '}') : NULL;
        printf("records: OK, %zd bytes, ", out.len);
        if (s && e) printf("%.*s", (int)(e - s + 1), s);
        printf("\n");
    }
    buf_free(&out);

    buf_init(&out);
    ok = api_files(&out, err, sizeof err);
    if (!ok) {
        printf("files: FAIL %s\n", err);
    } else {
        const char *s = strstr(out.p, "\"count\":");
        const char *e = s ? strstr(s, ",\"groups\"") : NULL;
        printf("files: OK, %zd bytes, ", out.len);
        if (s && e) printf("%.*s", (int)(e - s), s);
        printf("\n");
    }
    buf_free(&out);

    buf_init(&out);
    ok = api_disk(&out, err, sizeof err);
    if (!ok) {
        printf("disk: FAIL %s\n", err);
    } else {
        printf("disk: OK, %.200s\n", out.p);
    }
    buf_free(&out);

    /* 触发器在不在（--init 之前也应该已经就位） */
    {
        sqlite3 *db = db_open();
        sqlite3_stmt *st = NULL;
        int n = 0;
        if (db) {
            if (sqlite3_prepare_v2(db, "SELECT count(*) FROM sqlite_master WHERE type='trigger'"
                                       " AND name LIKE 'steward_default_rule_%'",
                                   -1, &st, NULL) == SQLITE_OK &&
                sqlite3_step(st) == SQLITE_ROW)
                n = sqlite3_column_int(st, 0);
            if (st) sqlite3_finalize(st);
            sqlite3_close(db);
        }
        printf("triggers: %d 个默认规则触发器\n", n);
    }
    return 0;
}

/* ------------------------------------------------------------------ */

static void usage(const char *argv0)
{
    printf("用法: %s [--port N] [--bind ADDR] [--db FILE] [--root DIR] [--token S]\n"
           "          [--floor PCT] [--push-url URL] [--push-every SEC] [--check-every SEC]\n"
           "          [--state-dir DIR] [--init] [--pending-off] [--selftest] [--push-now] [-v]\n"
           "          [--watch | --no-watch]\n"
           "\n"
           "  --init        建四个默认规则触发器（幂等）并把 rule 为空的源补上\n"
           "  --pending-off 把「要下、但一页都没落过盘」的条目改成不下\n"
           "  --floor PCT   可用空间低于这个百分比就停用所有源（0 = 不管，默认 10）\n"
           "  --push-url    拉闸/恢复时推一条到这个地址（HTTP POST，默认打 push-relay）\n"
           "  --push-now    只推一条测试消息然后退出（验证 push-url 通不通）\n"
           "  --no-watch    不核盘、不推「下载完成 / 文件不在了」（默认核）\n",
           argv0);
}

int main(int argc, char **argv)
{
    int i;
    int fd, on = 1;
    int do_selftest = 0, do_init = 0, do_pending_off = 0, do_push_now = 0;
    struct sockaddr_in addr;
    const char *env;
    pthread_t th;

    if ((env = getenv("STEWARD_PORT")) && *env) g_port = atoi(env);
    if ((env = getenv("STEWARD_BIND")) && *env) snprintf(g_bind, sizeof g_bind, "%s", env);
    if ((env = getenv("STEWARD_DB")) && *env)   snprintf(g_db, sizeof g_db, "%s", env);
    if ((env = getenv("STEWARD_ROOT")) && *env) snprintf(g_root, sizeof g_root, "%s", env);
    if ((env = getenv("STEWARD_TOKEN")) && *env) snprintf(g_token, sizeof g_token, "%s", env);
    if ((env = getenv("STEWARD_FLOOR")) && *env) g_floor_pct = atoi(env);
    if ((env = getenv("STEWARD_PUSH_URL")) && *env) snprintf(g_push_url, sizeof g_push_url, "%s", env);
    if ((env = getenv("STEWARD_PUSH_EVERY")) && *env) g_push_every = atoi(env);
    if ((env = getenv("STEWARD_CHECK_EVERY")) && *env) g_check_every = atoi(env);
    if ((env = getenv("STEWARD_WATCH")) && *env) g_watch_on = atoi(env) != 0;
    if ((env = getenv("STEWARD_STATE_DIR")) && *env) snprintf(g_state_dir, sizeof g_state_dir, "%s", env);

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc)       g_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bind") && i + 1 < argc)  snprintf(g_bind, sizeof g_bind, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--db") && i + 1 < argc)    snprintf(g_db, sizeof g_db, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--root") && i + 1 < argc)  snprintf(g_root, sizeof g_root, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--token") && i + 1 < argc) snprintf(g_token, sizeof g_token, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--floor") && i + 1 < argc) g_floor_pct = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--push-url") && i + 1 < argc) snprintf(g_push_url, sizeof g_push_url, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--push-every") && i + 1 < argc) g_push_every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--check-every") && i + 1 < argc) g_check_every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--state-dir") && i + 1 < argc) snprintf(g_state_dir, sizeof g_state_dir, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--watch"))                 g_watch_on = 1;
        else if (!strcmp(argv[i], "--no-watch"))              g_watch_on = 0;
        else if (!strcmp(argv[i], "--init"))                  do_init = 1;
        else if (!strcmp(argv[i], "--pending-off"))           do_pending_off = 1;
        else if (!strcmp(argv[i], "--selftest"))              do_selftest = 1;
        else if (!strcmp(argv[i], "--push-now"))              do_push_now = 1;
        else if (!strcmp(argv[i], "-v"))                      g_verbose = 1;
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(argv[0]); return 0; }
    }

    if (g_check_every < 5) g_check_every = 5;
    if (g_push_every < g_check_every) g_push_every = g_check_every;

    /* 闸门快照跟 data.sqlite 放一起（那条 bind mount 重建容器也不丢） */
    {
        char dir[PATH_MAX];
        snprintf(dir, sizeof dir, "%s", g_state_dir[0] ? g_state_dir : g_db);
        if (!g_state_dir[0]) {
            char *slash = strrchr(dir, '/');
            if (slash) *slash = 0;
            else snprintf(dir, sizeof dir, ".");
        }
        snprintf(g_brake_path, sizeof g_brake_path, "%.3920s/steward-brake-sources.txt", dir);
        snprintf(g_watch_path, sizeof g_watch_path, "%.3920s/steward-ondisk.txt", dir);
        snprintf(g_prefs_path, sizeof g_prefs_path, "%.3920s/steward-prefs.json", dir);
    }
    if (access(g_brake_path, F_OK) == 0) {
        g_braked = 1;
        snprintf(g_brake_note, sizeof g_brake_note, "上次拉过闸，快照还在（%.120s）", g_brake_path);
    }

    if (do_init) {
        char report[512];
        int nt = ensure_triggers(report, sizeof report);
        int nb = backfill_rules(report, sizeof report);
        printf("触发器 %d/%d 就位%s%s\n", nt, NSRC_TABLES,
               report[0] ? "，报错：" : "", report);
        printf("补默认规则的源：%d 个\n", nb);
        return nb < 0 ? 1 : 0;
    }
    if (do_pending_off) {
        int n = mark_pending_off();
        if (n < 0) { fprintf(stderr, "改库失败（%s 能写吗）\n", g_db); return 1; }
        printf("改成不下的条目：%d 条\n", n);
        return 0;
    }
    /* 自检：只推一条就走，用来验证 push-url 通不通 */
    if (do_push_now) {
        int ok = push_text("steward 自检：推送链路已通");
        printf("推送%s -> %s\n", ok ? "成功" : "失败", g_push_url[0] ? g_push_url : "(没配 push-url)");
        return ok ? 0 : 1;
    }

    if (!realpath(g_root, g_root_real)) {
        snprintf(g_root_real, sizeof g_root_real, "%s", g_root);
        fprintf(stderr, "[warn] 下载目录不存在: %s\n", g_root);
    }

    if (do_selftest) return selftest();

    signal(SIGPIPE, SIG_IGN);

    /* 触发器是闸门之外的护栏，每次起来都确认一遍（幂等，建过就不动） */
    {
        char report[512];
        int nt = ensure_triggers(report, sizeof report);
        if (nt < NSRC_TABLES)
            fprintf(stderr, "[warn] 默认规则触发器只建了 %d/%d 个：%s\n",
                    nt, NSRC_TABLES, report);
    }

    if (pthread_create(&th, NULL, disk_thread, NULL) == 0) pthread_detach(th);
    else fprintf(stderr, "[warn] 磁盘闸门线程没起来\n");

    if (pthread_create(&th, NULL, net_thread, NULL) == 0) pthread_detach(th);
    else fprintf(stderr, "[warn] 网速采样线程没起来\n");

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);

    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)g_port);
    if (inet_pton(AF_INET, g_bind, &addr.sin_addr) != 1) addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0) { perror("bind"); return 1; }
    if (listen(fd, 32) != 0) { perror("listen"); return 1; }

    fprintf(stderr, "[steward] listening on %s:%d  db=%s root=%s floor=%d%% push=%s%s\n",
            g_bind, g_port, g_db, g_root, g_floor_pct,
            g_push_url[0] ? g_push_url : "(关)", g_token[0] ? " token=on" : "");
    fflush(stderr);

    for (;;) {
        struct sockaddr_in cli;
        socklen_t cl = sizeof cli;
        int cfd = accept(fd, (struct sockaddr *)&cli, &cl);
        pthread_t th;

        if (cfd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }
        if (pthread_create(&th, NULL, conn_thread, (void *)(intptr_t)cfd) != 0) {
            close(cfd);
            continue;
        }
        pthread_detach(th);
    }
    return 0;
}
