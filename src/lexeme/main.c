#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <mysql/mysql.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define AST_MYSQL_PORT_DEFAULT 3306U

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} Buffer;

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
    long long token_count;
    long long source_file_count;
} LexemeStats;

static void
die(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
    exit(EXIT_FAILURE);
}

static void *
xmalloc(size_t size)
{
    void *ptr = malloc(size);

    if (!ptr) {
        die("malloc failed");
    }
    return ptr;
}

static void *
xrealloc(void *ptr, size_t size)
{
    void *next = realloc(ptr, size);

    if (!next) {
        die("realloc failed");
    }
    return next;
}

static char *
xstrdup(const char *text)
{
    size_t len = strlen(text);
    char *copy = xmalloc(len + 1);

    memcpy(copy, text, len + 1);
    return copy;
}

static void
buffer_init(Buffer *buf)
{
    memset(buf, 0, sizeof(*buf));
}

static void
buffer_reserve(Buffer *buf, size_t needed)
{
    size_t required = buf->len + needed + 1;

    if (required <= buf->cap) {
        return;
    }

    if (buf->cap == 0) {
        buf->cap = 64;
    }
    while (buf->cap < required) {
        buf->cap *= 2;
    }
    buf->data = xrealloc(buf->data, buf->cap);
}

static void
buffer_append_mem(Buffer *buf, const void *data, size_t len)
{
    buffer_reserve(buf, len);
    memcpy(buf->data + buf->len, data, len);
    buf->len += len;
    buf->data[buf->len] = '\0';
}

static void
buffer_append_char(Buffer *buf, char ch)
{
    buffer_reserve(buf, 1);
    buf->data[buf->len++] = ch;
    buf->data[buf->len] = '\0';
}

static void
buffer_append_str(Buffer *buf, const char *text)
{
    buffer_append_mem(buf, text, strlen(text));
}

static void
buffer_free(Buffer *buf)
{
    free(buf->data);
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

static void
trim_in_place(char *text)
{
    char *start = text;
    char *end;

    while (*start && isspace((unsigned char) *start)) {
        start++;
    }
    if (start != text) {
        memmove(text, start, strlen(start) + 1);
    }

    end = text + strlen(text);
    while (end > text && isspace((unsigned char) end[-1])) {
        *--end = '\0';
    }
}

static int
hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
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
            int hi = hex_value(src[i + 1]);
            int lo = hex_value(src[i + 2]);

            if (hi >= 0 && lo >= 0) {
                buffer_append_char(&buf, (char) ((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        buffer_append_char(&buf, src[i]);
    }
    return buf.data ? buf.data : xstrdup("");
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
    return NULL;
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

static void
mysql_exec_or_die(MysqlSession *session, const char *sql, const char *context)
{
    if (mysql_real_query(session->conn, sql, (unsigned long) strlen(sql)) != 0) {
        die("%s: %s", context, mysql_error(session->conn));
    }
}

static void
append_sql_hex_literal(Buffer *buf, const char *value)
{
    static const char digits[] = "0123456789abcdef";
    const unsigned char *p = (const unsigned char *) value;

    buffer_append_str(buf, "x'");
    while (*p) {
        buffer_append_char(buf, digits[*p >> 4]);
        buffer_append_char(buf, digits[*p & 0x0f]);
        p++;
    }
    buffer_append_char(buf, '\'');
}

static void
append_lexeme_match_clause(Buffer *buf, const char *lexeme)
{
    buffer_append_str(buf, "(t.lexeme_id = ");
    append_sql_hex_literal(buf, lexeme);
    buffer_append_str(buf, " OR ll.code = ");
    append_sql_hex_literal(buf, lexeme);
    buffer_append_char(buf, ')');
}

static LexemeStats
query_lexeme_stats(MysqlSession *session, const char *lexeme)
{
    Buffer sql;
    MYSQL_RES *result;
    MYSQL_ROW row;
    LexemeStats stats;

    stats.token_count = 0;
    stats.source_file_count = 0;

    buffer_init(&sql);
    buffer_append_str(&sql,
        "SELECT COUNT(*), COUNT(DISTINCT t.source_file_id) "
        "FROM token t "
        "LEFT JOIN lexeme_long ll ON ll.id = t.lexeme_long_id "
        "WHERE ");
    append_lexeme_match_clause(&sql, lexeme);
    buffer_append_str(&sql, ";\n");

    mysql_exec_or_die(session, sql.data, "failed to query lexeme stats");
    result = mysql_store_result(session->conn);
    if (!result) {
        buffer_free(&sql);
        die("failed to read lexeme stats: %s", mysql_error(session->conn));
    }

    row = mysql_fetch_row(result);
    if (row) {
        if (row[0]) {
            stats.token_count = strtoll(row[0], NULL, 10);
        }
        if (row[1]) {
            stats.source_file_count = strtoll(row[1], NULL, 10);
        }
    }

    mysql_free_result(result);
    buffer_free(&sql);
    return stats;
}

static void
print_usage(FILE *stream, const char *argv0)
{
    fprintf(stream, "Usage: %s LEXEME\n", argv0);
}

int
main(int argc, char **argv)
{
    char *repo_root = NULL;
    char *env_path = NULL;
    char *database_url = NULL;
    DbConfig cfg;
    MysqlSession session;
    LexemeStats stats;
    const char *lexeme;

    if (argc == 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        print_usage(stdout, argv[0]);
        return EXIT_SUCCESS;
    }
    if (argc != 2) {
        print_usage(stderr, argv[0]);
        return EXIT_FAILURE;
    }

    lexeme = argv[1];

    repo_root = getcwd(NULL, 0);
    if (!repo_root) {
        die("getcwd failed: %s", strerror(errno));
    }
    env_path = xmalloc(strlen(repo_root) + strlen("/.env") + 1);
    sprintf(env_path, "%s/.env", repo_root);

    database_url = read_env_value(env_path, "DATABASE_URL");
    parse_database_url(database_url, &cfg);
    mysql_session_start(&session, &cfg);

    stats = query_lexeme_stats(&session, lexeme);

    printf("lexeme: %s\n", lexeme);
    printf("token_count: %lld\n", stats.token_count);
    printf("source_file_count: %lld\n", stats.source_file_count);

    mysql_session_close(&session);
    db_config_free(&cfg);
    free(database_url);
    free(env_path);
    free(repo_root);
    return EXIT_SUCCESS;
}
