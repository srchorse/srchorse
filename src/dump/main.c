#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <mysql/mysql.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define ARRAY_GROW_CAP(cap) ((cap) < 16 ? 16 : (cap) * 2)
#define AST_MYSQL_PORT_DEFAULT 3306U
#define DEFAULT_DB_SOURCE_ROOT "/nfs/php-src"
#define PROGRESS_BAR_WIDTH 28

static bool g_live_line_active = false;
static const char *CLR_RESET = "\x1b[0m";
static const char *CLR_BLUE = "\x1b[94m";
static const char *CLR_GREEN = "\x1b[32m";
static const char *CLR_YELLOW = "\x1b[33m";
static const char *CLR_CYAN = "\x1b[36m";

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} Buffer;

typedef struct {
    int row;
    int col;
    char *text;
} TokenPiece;

typedef struct {
    TokenPiece *items;
    size_t len;
    size_t cap;
} TokenArray;

typedef struct {
    long long id;
    char *db_path;
    char *actual_path;
} SourceFileEntry;

typedef struct {
    SourceFileEntry *items;
    size_t len;
    size_t cap;
} SourceFileArray;

typedef struct {
    char *host;
    char *user;
    char *password;
    char *database;
    unsigned port;
} DbConfig;

typedef struct {
    MYSQL *conn;
} MysqlSession;

typedef struct {
    bool interactive;
    bool use_color;
    size_t total;
    size_t processed;
    size_t written;
} ProgressState;

static void ui_clear_live_line(void);
static bool file_exists(const char *path);
static char *trim_in_place(char *s);
static void die(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));

static void
die(const char *fmt, ...)
{
    va_list ap;

    ui_clear_live_line();
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(EXIT_FAILURE);
}

static void *
xmalloc(size_t size)
{
    void *ptr = malloc(size);

    if (!ptr) {
        die("malloc(%zu) failed", size);
    }
    return ptr;
}

static void *
xrealloc(void *ptr, size_t size)
{
    void *next = realloc(ptr, size);

    if (!next) {
        die("realloc(%zu) failed", size);
    }
    return next;
}

static char *
xstrdup(const char *s)
{
    size_t len = strlen(s);
    char *out = xmalloc(len + 1);

    memcpy(out, s, len + 1);
    return out;
}

static char *
xstrndup(const char *s, size_t len)
{
    char *out = xmalloc(len + 1);

    memcpy(out, s, len);
    out[len] = '\0';
    return out;
}

static void
buffer_init(Buffer *buf)
{
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

static void
buffer_reserve(Buffer *buf, size_t extra)
{
    size_t need = buf->len + extra + 1;
    size_t cap;

    if (need <= buf->cap) {
        return;
    }

    cap = buf->cap ? buf->cap : 64;
    while (cap < need) {
        cap *= 2;
    }
    buf->data = xrealloc(buf->data, cap);
    buf->cap = cap;
}

static void
buffer_append_mem(Buffer *buf, const void *src, size_t len)
{
    buffer_reserve(buf, len);
    memcpy(buf->data + buf->len, src, len);
    buf->len += len;
    buf->data[buf->len] = '\0';
}

static void
buffer_append_str(Buffer *buf, const char *s)
{
    buffer_append_mem(buf, s, strlen(s));
}

static void
buffer_append_char(Buffer *buf, char c)
{
    buffer_reserve(buf, 1);
    buf->data[buf->len++] = c;
    buf->data[buf->len] = '\0';
}

static void
buffer_free(Buffer *buf)
{
    free(buf->data);
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

static char *
buffer_take(Buffer *buf)
{
    char *data;

    if (!buf->data) {
        return xstrdup("");
    }

    data = buf->data;
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
    return data;
}

static void
token_array_push(TokenArray *arr, int row, int col, char *text)
{
    if (arr->len == arr->cap) {
        arr->cap = ARRAY_GROW_CAP(arr->cap);
        arr->items = xrealloc(arr->items, arr->cap * sizeof(*arr->items));
    }

    arr->items[arr->len].row = row;
    arr->items[arr->len].col = col;
    arr->items[arr->len].text = text;
    arr->len++;
}

static void
token_array_free(TokenArray *arr)
{
    size_t i;

    for (i = 0; i < arr->len; i++) {
        free(arr->items[i].text);
    }
    free(arr->items);
    arr->items = NULL;
    arr->len = 0;
    arr->cap = 0;
}

static void
source_file_array_push(
    SourceFileArray *arr,
    long long id,
    char *db_path,
    char *actual_path)
{
    if (arr->len == arr->cap) {
        arr->cap = ARRAY_GROW_CAP(arr->cap);
        arr->items = xrealloc(arr->items, arr->cap * sizeof(*arr->items));
    }

    arr->items[arr->len].id = id;
    arr->items[arr->len].db_path = db_path;
    arr->items[arr->len].actual_path = actual_path;
    arr->len++;
}

static void
source_file_array_free(SourceFileArray *arr)
{
    size_t i;

    for (i = 0; i < arr->len; i++) {
        free(arr->items[i].db_path);
        free(arr->items[i].actual_path);
    }
    free(arr->items);
    arr->items = NULL;
    arr->len = 0;
    arr->cap = 0;
}

static void
ui_clear_live_line(void)
{
    if (!g_live_line_active) {
        return;
    }
    fprintf(stderr, "\r\x1b[2K\r");
    fflush(stderr);
    g_live_line_active = false;
}

static char *
trim_in_place(char *s)
{
    char *start = s;
    char *end;

    while (*start && isspace((unsigned char) *start)) {
        start++;
    }
    if (*start == '\0') {
        *s = '\0';
        return s;
    }

    end = start + strlen(start) - 1;
    while (end > start && isspace((unsigned char) *end)) {
        *end-- = '\0';
    }

    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }
    return s;
}

static char
hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return (char) (c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return (char) (10 + c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return (char) (10 + c - 'A');
    }
    return -1;
}

static char *
url_decode(const char *src)
{
    Buffer buf;
    size_t i;

    buffer_init(&buf);
    for (i = 0; src[i] != '\0'; i++) {
        if (src[i] == '%' && src[i + 1] != '\0' && src[i + 2] != '\0') {
            char hi = hex_value(src[i + 1]);
            char lo = hex_value(src[i + 2]);
            if (hi >= 0 && lo >= 0) {
                buffer_append_char(&buf, (char) ((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        buffer_append_char(&buf, src[i]);
    }
    return buffer_take(&buf);
}

static char *
strip_optional_quotes(char *value)
{
    size_t len = strlen(value);

    if (len >= 2 && ((value[0] == '"' && value[len - 1] == '"') || (value[0] == '\'' && value[len - 1] == '\''))) {
        value[len - 1] = '\0';
        return value + 1;
    }
    return value;
}

static char *
read_env_value(const char *env_path, const char *key)
{
    FILE *fp = fopen(env_path, "r");
    char line[8192];

    if (!fp) {
        die("failed to open %s: %s", env_path, strerror(errno));
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        char *eq;
        char *value;

        trim_in_place(line);
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }
        eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        trim_in_place(line);
        if (strcmp(line, key) != 0) {
            continue;
        }
        value = eq + 1;
        trim_in_place(value);
        value = strip_optional_quotes(value);
        fclose(fp);
        return xstrdup(value);
    }

    fclose(fp);
    die("missing %s in %s", key, env_path);
}

static void
parse_database_url(const char *database_url, DbConfig *cfg)
{
    const char *prefix = "mysql://";
    char *work;
    char *slash;
    char *at;
    char *hostport;
    char *credentials;
    char *colon;
    char *query;

    memset(cfg, 0, sizeof(*cfg));
    cfg->port = AST_MYSQL_PORT_DEFAULT;

    if (strncmp(database_url, prefix, strlen(prefix)) != 0) {
        die("unsupported DATABASE_URL, expected mysql://...");
    }

    work = xstrdup(database_url + strlen(prefix));
    query = strchr(work, '?');
    if (query) {
        *query = '\0';
    }

    slash = strchr(work, '/');
    if (!slash || slash[1] == '\0') {
        free(work);
        die("DATABASE_URL is missing the database name");
    }
    *slash = '\0';
    cfg->database = url_decode(slash + 1);

    at = strrchr(work, '@');
    if (!at) {
        free(work);
        die("DATABASE_URL is missing credentials or host");
    }
    *at = '\0';
    credentials = work;
    hostport = at + 1;

    colon = strchr(credentials, ':');
    if (colon) {
        *colon = '\0';
        cfg->user = url_decode(credentials);
        cfg->password = url_decode(colon + 1);
    } else {
        cfg->user = url_decode(credentials);
        cfg->password = xstrdup("");
    }

    if (hostport[0] == '[') {
        char *end = strchr(hostport, ']');

        if (!end) {
            free(work);
            die("invalid DATABASE_URL IPv6 host");
        }
        *end = '\0';
        cfg->host = xstrdup(hostport + 1);
        if (end[1] == ':') {
            cfg->port = (unsigned) strtoul(end + 2, NULL, 10);
        }
    } else {
        char *last_colon = strrchr(hostport, ':');

        if (last_colon && strchr(last_colon + 1, ':') == NULL) {
            *last_colon = '\0';
            cfg->host = xstrdup(hostport);
            cfg->port = (unsigned) strtoul(last_colon + 1, NULL, 10);
        } else {
            cfg->host = xstrdup(hostport);
        }
    }

    if (cfg->host[0] == '\0' || cfg->user[0] == '\0' || cfg->database[0] == '\0') {
        free(work);
        die("DATABASE_URL is missing required connection parts");
    }

    free(work);
}

static void
db_config_free(DbConfig *cfg)
{
    free(cfg->host);
    free(cfg->user);
    free(cfg->password);
    free(cfg->database);
    cfg->host = NULL;
    cfg->user = NULL;
    cfg->password = NULL;
    cfg->database = NULL;
    cfg->port = 0;
}

static char *
path_join(const char *left, const char *right)
{
    Buffer buf;

    buffer_init(&buf);
    buffer_append_str(&buf, left);
    if (buf.len == 0 || buf.data[buf.len - 1] != '/') {
        buffer_append_char(&buf, '/');
    }
    while (right[0] == '/') {
        right++;
    }
    buffer_append_str(&buf, right);
    return buffer_take(&buf);
}

static char *
canonicalize_path(const char *path)
{
    char resolved[PATH_MAX];

    if (!realpath(path, resolved)) {
        die("realpath(%s) failed: %s", path, strerror(errno));
    }
    return xstrdup(resolved);
}

static bool
file_exists(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0;
}

static const char *
path_relative_or_self(const char *path, const char *root)
{
    size_t root_len = strlen(root);
    const char *relative;

    if (strncmp(path, root, root_len) != 0) {
        return path;
    }

    relative = path + root_len;
    while (*relative == '/') {
        relative++;
    }
    return *relative ? relative : path;
}

static size_t
terminal_columns(void)
{
    struct winsize ws;
    const char *env_columns;

    if (ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        return (size_t) ws.ws_col;
    }

    env_columns = getenv("COLUMNS");
    if (env_columns && env_columns[0] != '\0') {
        char *end = NULL;
        unsigned long cols = strtoul(env_columns, &end, 10);
        if (end && *end == '\0' && cols > 0) {
            return (size_t) cols;
        }
    }

    return 120;
}

static char *
tail_ellipsized_copy(const char *text, size_t max_len)
{
    size_t len;
    size_t tail_len;
    char *out;

    if (!text) {
        return xstrdup("");
    }

    len = strlen(text);
    if (len <= max_len) {
        return xstrdup(text);
    }
    if (max_len <= 3) {
        return xstrndup(text + (len - max_len), max_len);
    }

    tail_len = max_len - 3;
    out = xmalloc(max_len + 1);
    memcpy(out, "...", 3);
    memcpy(out + 3, text + (len - tail_len), tail_len);
    out[max_len] = '\0';
    return out;
}

static void
progress_init(ProgressState *progress, size_t total)
{
    memset(progress, 0, sizeof(*progress));
    progress->interactive = isatty(STDERR_FILENO) != 0;
    progress->use_color = progress->interactive;
    progress->total = total;
}

static void
progress_render(const ProgressState *progress, const char *phase, const char *path)
{
    char *bar;
    char *display_path;
    size_t cols;
    size_t bar_width;
    size_t path_budget;
    size_t completed = 0;
    size_t i;
    double percent = 0.0;
    const char *phase_color = progress->use_color ? CLR_YELLOW : "";
    const char *label_color = progress->use_color ? CLR_BLUE : "";
    const char *bar_color = progress->use_color ? CLR_GREEN : "";
    const char *path_color = progress->use_color ? CLR_CYAN : "";
    const char *reset = progress->use_color ? CLR_RESET : "";
    int fixed_without_bar_and_path;
    int fixed_with_bar;

    if (!progress->interactive) {
        return;
    }

    if (progress->total > 0) {
        completed = (progress->processed * PROGRESS_BAR_WIDTH) / progress->total;
        if (completed > PROGRESS_BAR_WIDTH) {
            completed = PROGRESS_BAR_WIDTH;
        }
        percent = (100.0 * (double) progress->processed) / (double) progress->total;
        if (percent > 100.0) {
            percent = 100.0;
        }
    }

    cols = terminal_columns();
    fixed_without_bar_and_path = snprintf(NULL, 0,
        "[dump] [] %6.1f%% %zu/%zu %-5s ok:%zu ",
        percent,
        progress->processed,
        progress->total,
        phase ? phase : "",
        progress->written);
    if (fixed_without_bar_and_path < 0) {
        fixed_without_bar_and_path = 0;
    }

    if (cols > (size_t) fixed_without_bar_and_path + 12U) {
        bar_width = cols - (size_t) fixed_without_bar_and_path - 12U;
        if (bar_width > PROGRESS_BAR_WIDTH) {
            bar_width = PROGRESS_BAR_WIDTH;
        }
        if (bar_width < 8U) {
            bar_width = 8U;
        }
    } else {
        bar_width = 8U;
    }

    bar = xmalloc(bar_width + 1);
    completed = progress->total > 0 ? (progress->processed * bar_width) / progress->total : 0;
    if (completed > bar_width) {
        completed = bar_width;
    }

    for (i = 0; i < bar_width; i++) {
        if (i < completed) {
            bar[i] = '=';
        } else if (i == completed && progress->processed < progress->total) {
            bar[i] = '>';
        } else {
            bar[i] = ' ';
        }
    }
    bar[bar_width] = '\0';

    fixed_with_bar = snprintf(NULL, 0,
        "[dump] [%s] %6.1f%% %zu/%zu %-5s ok:%zu ",
        bar,
        percent,
        progress->processed,
        progress->total,
        phase ? phase : "",
        progress->written);
    if (fixed_with_bar < 0) {
        fixed_with_bar = 0;
    }
    path_budget = cols > (size_t) fixed_with_bar ? cols - (size_t) fixed_with_bar : 0U;
    display_path = tail_ellipsized_copy(path ? path : "", path_budget);

    fprintf(stderr,
        "\r\x1b[2K\r%s[dump]%s %s[%s]%s %6.1f%% %zu/%zu %s%-5s%s ok:%zu %s%s%s",
        label_color,
        reset,
        bar_color,
        bar,
        reset,
        percent,
        progress->processed,
        progress->total,
        phase_color,
        phase ? phase : "",
        reset,
        progress->written,
        path_color,
        display_path,
        reset);
    fflush(stderr);
    g_live_line_active = true;
    free(display_path);
    free(bar);
}

static void
progress_finish(ProgressState *progress, const char *status)
{
    const char *label_color = progress->use_color ? CLR_BLUE : "";
    const char *status_color = progress->use_color ? CLR_GREEN : "";
    const char *reset = progress->use_color ? CLR_RESET : "";

    ui_clear_live_line();
    fprintf(stderr,
        "%s[dump]%s %s%s%s processed:%zu written:%zu\n",
        label_color,
        reset,
        status_color,
        status ? status : "done",
        reset,
        progress->processed,
        progress->written);
    fflush(stderr);
}

static char *
reconstruct_source(const TokenArray *tokens)
{
    Buffer output;
    size_t i;

    buffer_init(&output);
    for (i = 0; i < tokens->len; i++) {
        buffer_append_str(&output, tokens->items[i].text);
    }
    return buffer_take(&output);
}

static void
write_text_file(const char *path, const char *data, size_t len)
{
    FILE *fp = fopen(path, "wb");
    size_t wrote;

    if (!fp) {
        die("failed to open %s for writing: %s", path, strerror(errno));
    }

    wrote = fwrite(data, 1, len, fp);
    if (wrote != len) {
        fclose(fp);
        die("failed to write %s", path);
    }
    if (fclose(fp) != 0) {
        die("failed to close %s: %s", path, strerror(errno));
    }
}

static void
mysql_session_start(MysqlSession *session, const DbConfig *cfg)
{
    memset(session, 0, sizeof(*session));
    session->conn = mysql_init(NULL);
    if (!session->conn) {
        die("mysql_init failed");
    }
    if (!mysql_real_connect(session->conn, cfg->host, cfg->user, cfg->password, cfg->database, cfg->port, NULL, 0)) {
        die("mysql_real_connect failed: %s", mysql_error(session->conn));
    }
}

static void
mysql_session_close(MysqlSession *session)
{
    if (session->conn) {
        mysql_close(session->conn);
        session->conn = NULL;
    }
}

static bool
path_has_unsafe_components(const char *relative)
{
    const char *segment = relative;
    const char *p = relative;

    while (1) {
        if (*p == '/' || *p == '\0') {
            size_t len = (size_t) (p - segment);

            if (len == 0) {
                if (*p == '\0') {
                    return false;
                }
                segment = p + 1;
                p++;
                continue;
            }
            if ((len == 1 && segment[0] == '.') || (len == 2 && segment[0] == '.' && segment[1] == '.')) {
                return true;
            }
            if (*p == '\0') {
                return false;
            }
            segment = p + 1;
        }
        p++;
    }
}

static char *
actual_path_from_db_path(const char *db_path, const char *php_src_root, const char *db_source_root)
{
    size_t root_len = strlen(db_source_root);
    const char *relative;

    if (strncmp(db_path, db_source_root, root_len) != 0) {
        die("stored path %s is not inside %s", db_path, db_source_root);
    }

    relative = db_path + root_len;
    while (*relative == '/') {
        relative++;
    }
    if (*relative == '\0') {
        die("stored path %s does not resolve to a file under %s", db_path, db_source_root);
    }
    if (path_has_unsafe_components(relative)) {
        die("stored path %s escapes %s", db_path, db_source_root);
    }

    return path_join(php_src_root, relative);
}

static void
load_source_files(MysqlSession *session, const char *php_src_root, const char *db_source_root, SourceFileArray *files)
{
    static const char sql[] =
        "SELECT id, path "
        "FROM source_file "
        "ORDER BY path;\n";
    MYSQL_RES *result;
    MYSQL_ROW row;

    if (mysql_real_query(session->conn, sql, (unsigned long) strlen(sql)) != 0) {
        die("failed to query source_file list: %s", mysql_error(session->conn));
    }

    result = mysql_store_result(session->conn);
    if (!result) {
        die("failed to read source_file list: %s", mysql_error(session->conn));
    }

    while ((row = mysql_fetch_row(result)) != NULL) {
        unsigned long *lengths = mysql_fetch_lengths(result);
        long long id;
        char *db_path;
        char *actual_path;

        if (!row[0] || !row[1]) {
            mysql_free_result(result);
            die("source_file row is missing id or path");
        }

        id = strtoll(row[0], NULL, 10);
        db_path = xstrndup(row[1], lengths[1]);
        actual_path = actual_path_from_db_path(db_path, php_src_root, db_source_root);
        source_file_array_push(files, id, db_path, actual_path);
    }

    mysql_free_result(result);
}

static void
load_tokens_for_source_file(MysqlSession *session, long long source_file_id, TokenArray *tokens)
{
    Buffer sql;
    MYSQL_RES *result;
    MYSQL_ROW row;

    buffer_init(&sql);
    buffer_append_str(&sql,
        "SELECT t.row_num, t.col_num, t.lexeme_id, ll.code "
        "FROM token t "
        "LEFT JOIN lexeme_long ll ON ll.id = t.lexeme_long_id "
        "WHERE t.source_file_id = ");
    {
        char id_buf[64];
        snprintf(id_buf, sizeof(id_buf), "%lld", source_file_id);
        buffer_append_str(&sql, id_buf);
    }
    buffer_append_str(&sql, " ORDER BY t.id;\n");

    if (mysql_real_query(session->conn, sql.data, (unsigned long) sql.len) != 0) {
        buffer_free(&sql);
        die("failed to query tokens for source_file.id=%lld: %s", source_file_id, mysql_error(session->conn));
    }
    buffer_free(&sql);

    result = mysql_store_result(session->conn);
    if (!result) {
        die("failed to read tokens for source_file.id=%lld: %s", source_file_id, mysql_error(session->conn));
    }

    while ((row = mysql_fetch_row(result)) != NULL) {
        unsigned long *lengths = mysql_fetch_lengths(result);
        int token_row;
        int token_col;
        char *text;

        if (!row[0] || !row[1]) {
            mysql_free_result(result);
            die("token row is missing row_num or col_num for source_file.id=%lld", source_file_id);
        }

        token_row = (int) strtol(row[0], NULL, 10);
        token_col = (int) strtol(row[1], NULL, 10);
        if (row[2]) {
            text = xstrndup(row[2], lengths[2]);
        } else if (row[3]) {
            text = xstrndup(row[3], lengths[3]);
        } else {
            mysql_free_result(result);
            die("token row has neither lexeme_id nor lexeme_long.code for source_file.id=%lld", source_file_id);
        }
        token_array_push(tokens, token_row, token_col, text);
    }

    mysql_free_result(result);
}

static void
print_usage(FILE *stream, const char *argv0)
{
    fprintf(stream, "Usage: %s\n", argv0);
}

int
main(int argc, char **argv)
{
    char *repo_root = NULL;
    char *php_src_root = NULL;
    char *env_path = NULL;
    char *database_url = NULL;
    DbConfig cfg;
    MysqlSession session;
    SourceFileArray files = {0};
    ProgressState progress;
    size_t i;

    if (argc == 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        print_usage(stdout, argv[0]);
        return EXIT_SUCCESS;
    }
    if (argc != 1) {
        print_usage(stderr, argv[0]);
        return EXIT_FAILURE;
    }

    repo_root = getcwd(NULL, 0);
    if (!repo_root) {
        die("getcwd failed: %s", strerror(errno));
    }

    php_src_root = path_join(repo_root, "php-src");
    {
        char *canonical = canonicalize_path(php_src_root);
        free(php_src_root);
        php_src_root = canonical;
    }

    env_path = path_join(repo_root, ".env");
    database_url = read_env_value(env_path, "DATABASE_URL");
    parse_database_url(database_url, &cfg);
    mysql_session_start(&session, &cfg);

    load_source_files(&session, php_src_root, DEFAULT_DB_SOURCE_ROOT, &files);
    if (files.len == 0) {
        die("no source_file rows found in database");
    }

    progress_init(&progress, files.len);
    for (i = 0; i < files.len; i++) {
        TokenArray tokens = {0};
        char *reconstructed = NULL;
        const char *display_path = path_relative_or_self(files.items[i].actual_path, php_src_root);

        progress_render(&progress, "query", display_path);
        if (!file_exists(files.items[i].actual_path)) {
            die("target file does not exist for dump: %s", files.items[i].actual_path);
        }
        load_tokens_for_source_file(&session, files.items[i].id, &tokens);

        progress_render(&progress, "write", display_path);
        reconstructed = reconstruct_source(&tokens);
        write_text_file(files.items[i].actual_path, reconstructed, strlen(reconstructed));

        progress.processed++;
        progress.written++;
        progress_render(&progress, "ok", display_path);

        free(reconstructed);
        token_array_free(&tokens);
    }

    progress_finish(&progress, "done");
    source_file_array_free(&files);
    mysql_session_close(&session);
    db_config_free(&cfg);
    free(database_url);
    free(env_path);
    free(php_src_root);
    free(repo_root);
    return EXIT_SUCCESS;
}
