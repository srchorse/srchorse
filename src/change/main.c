#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <mysql/mysql.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
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

static char *
xstrndup(const char *text, size_t len)
{
    char *copy = xmalloc(len + 1);

    memcpy(copy, text, len);
    copy[len] = '\0';
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
buffer_appendf(Buffer *buf, const char *fmt, ...)
{
    va_list args;
    va_list copy;
    int needed;

    va_start(args, fmt);
    va_copy(copy, args);
    needed = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (needed < 0) {
        va_end(args);
        die("vsnprintf failed");
    }

    buffer_reserve(buf, (size_t) needed);
    if (vsnprintf(buf->data + buf->len, buf->cap - buf->len, fmt, args) != needed) {
        va_end(args);
        die("vsnprintf wrote unexpected length");
    }
    va_end(args);
    buf->len += (size_t) needed;
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
    unsigned long flags = CLIENT_MULTI_STATEMENTS;

    memset(session, 0, sizeof(*session));
    session->conn = mysql_init(NULL);
    if (!session->conn) {
        die("mysql_init failed");
    }
    if (!mysql_real_connect(session->conn, cfg->host, cfg->user, cfg->password, cfg->database, cfg->port, NULL, flags)) {
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

static int
mysql_session_exec_capture(MysqlSession *session, const char *sql, Buffer *output)
{
    if (mysql_real_query(session->conn, sql, (unsigned long) strlen(sql)) != 0) {
        buffer_init(output);
        buffer_appendf(output, "ERROR %u (%s): %s\n",
            mysql_errno(session->conn),
            mysql_sqlstate(session->conn),
            mysql_error(session->conn));
        return 1;
    }

    buffer_init(output);
    for (;;) {
        MYSQL_RES *result = mysql_store_result(session->conn);

        if (result) {
            MYSQL_ROW row;
            unsigned int num_fields = mysql_num_fields(result);

            while ((row = mysql_fetch_row(result)) != NULL) {
                unsigned long *lengths = mysql_fetch_lengths(result);
                unsigned int i;

                for (i = 0; i < num_fields; i++) {
                    if (i > 0) {
                        buffer_append_char(output, '\t');
                    }
                    if (!row[i]) {
                        buffer_append_str(output, "NULL");
                    } else {
                        buffer_append_mem(output, row[i], lengths[i]);
                    }
                }
                buffer_append_char(output, '\n');
            }
            mysql_free_result(result);
        } else if (mysql_errno(session->conn) != 0) {
            buffer_appendf(output, "ERROR %u (%s): %s\n",
                mysql_errno(session->conn),
                mysql_sqlstate(session->conn),
                mysql_error(session->conn));
            return 1;
        }

        {
            int next = mysql_next_result(session->conn);

            if (next < 0) {
                break;
            }
            if (next > 0) {
                buffer_appendf(output, "ERROR %u (%s): %s\n",
                    mysql_errno(session->conn),
                    mysql_sqlstate(session->conn),
                    mysql_error(session->conn));
                return 1;
            }
        }
    }

    return 0;
}

static char *
trimmed_last_line(const char *text)
{
    const char *end = text + strlen(text);
    const char *start;

    while (end > text && (end[-1] == '\n' || end[-1] == '\r' || isspace((unsigned char) end[-1]))) {
        end--;
    }
    start = end;
    while (start > text && start[-1] != '\n' && start[-1] != '\r') {
        start--;
    }
    while (start < end && isspace((unsigned char) *start)) {
        start++;
    }
    return xstrndup(start, (size_t) (end - start));
}

static char *
mysql_scalar(MysqlSession *session, const char *sql, bool allow_empty)
{
    Buffer output;
    char *line;

    if (mysql_session_exec_capture(session, sql, &output) != 0) {
        char *message = output.data ? output.data : xstrdup("unknown mysql error\n");

        die("mysql command failed:\n%s", message);
    }

    line = trimmed_last_line(output.data ? output.data : "");
    if (!allow_empty && line[0] == '\0') {
        free(line);
        buffer_free(&output);
        die("mysql command returned no result");
    }
    buffer_free(&output);
    return line;
}

static void
mysql_exec_or_die(MysqlSession *session, const char *sql, const char *context)
{
    Buffer output;

    if (mysql_session_exec_capture(session, sql, &output) != 0) {
        char *message = output.data ? output.data : xstrdup("unknown mysql error\n");

        die("%s:\n%s", context, message);
    }
    buffer_free(&output);
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
append_source_match_clause(Buffer *buf, const char *source)
{
    buffer_append_str(buf, "INSTR(COALESCE(t.lexeme_id, ll.code), ");
    append_sql_hex_literal(buf, source);
    buffer_append_str(buf, ") > 0");
}

static long long
count_matching_tokens(MysqlSession *session)
{
    char *count_text;
    long long count;

    count_text = mysql_scalar(session, "SELECT COUNT(*) FROM tmp_change;\n", false);
    count = strtoll(count_text, NULL, 10);
    free(count_text);
    return count;
}

static unsigned long long
get_auto_increment_increment(MysqlSession *session)
{
    char *value_text;
    unsigned long long value;

    value_text = mysql_scalar(session, "SELECT @@auto_increment_increment;\n", false);
    value = strtoull(value_text, NULL, 10);
    free(value_text);
    if (value == 0) {
        die("invalid @@auto_increment_increment value");
    }
    return value;
}

static void
prepare_change_table(MysqlSession *session, const char *source, const char *target)
{
    Buffer sql;

    buffer_init(&sql);
    buffer_append_str(&sql,
        "DROP TEMPORARY TABLE IF EXISTS tmp_change;\n"
        "CREATE TEMPORARY TABLE tmp_change ("
        "  token_id INT NOT NULL PRIMARY KEY,"
        "  source_file_id INT NOT NULL,"
        "  new_value LONGBLOB NOT NULL,"
        "  value_sha CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,"
        "  value_len INT NOT NULL,"
        "  lexeme_long_id INT DEFAULT NULL"
        ") ENGINE=InnoDB;\n"
        "INSERT INTO tmp_change (token_id, source_file_id, new_value, value_sha, value_len) "
        "SELECT t.id, "
        "       t.source_file_id, "
        "       REPLACE(COALESCE(t.lexeme_id, ll.code), ");
    append_sql_hex_literal(&sql, source);
    buffer_append_str(&sql, ", ");
    append_sql_hex_literal(&sql, target);
    buffer_append_str(&sql,
        "), "
        "       SHA2(REPLACE(COALESCE(t.lexeme_id, ll.code), ");
    append_sql_hex_literal(&sql, source);
    buffer_append_str(&sql, ", ");
    append_sql_hex_literal(&sql, target);
    buffer_append_str(&sql,
        "), 256), "
        "       OCTET_LENGTH(REPLACE(COALESCE(t.lexeme_id, ll.code), ");
    append_sql_hex_literal(&sql, source);
    buffer_append_str(&sql, ", ");
    append_sql_hex_literal(&sql, target);
    buffer_append_str(&sql,
        ")) "
        "FROM token t "
        "LEFT JOIN lexeme_long ll ON ll.id = t.lexeme_long_id "
        "WHERE ");
    append_source_match_clause(&sql, source);
    buffer_append_str(&sql, ";\n");
    mysql_exec_or_die(session, sql.data, "failed to prepare token replacement set");
    buffer_free(&sql);
}

static void
replace_tokens(MysqlSession *session, const char *source, const char *target)
{
    Buffer sql;
    char *long_count_text;
    unsigned long long long_count;

    buffer_init(&sql);
    (void) source;
    (void) target;

    buffer_append_str(&sql,
        "INSERT IGNORE INTO lexeme (id) "
        "SELECT DISTINCT CAST(new_value AS BINARY(255)) "
        "FROM tmp_change "
        "WHERE value_len <= 255;\n");
    mysql_exec_or_die(session, sql.data, "failed to stage short lexeme replacements");
    buffer_free(&sql);

    long_count_text = mysql_scalar(session,
        "SELECT COUNT(*) "
        "FROM ("
        "  SELECT value_sha, new_value "
        "  FROM tmp_change "
        "  WHERE value_len > 255 "
        "  GROUP BY value_sha, new_value"
        ") AS long_values;\n",
        false);
    long_count = strtoull(long_count_text, NULL, 10);
    free(long_count_text);

    if (long_count > 0) {
        unsigned long long auto_increment_increment = get_auto_increment_increment(session);
        unsigned long long first_long_id;
        unsigned long long inserted_rows;

        buffer_init(&sql);
        buffer_append_str(&sql,
            "DROP TEMPORARY TABLE IF EXISTS tmp_long_values;\n"
            "CREATE TEMPORARY TABLE tmp_long_values ("
            "  slot INT NOT NULL AUTO_INCREMENT PRIMARY KEY,"
            "  value_sha CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,"
            "  new_value LONGBLOB NOT NULL,"
            "  lexeme_long_id INT DEFAULT NULL"
            ") ENGINE=InnoDB;\n"
            "INSERT INTO tmp_long_values (value_sha, new_value) "
            "SELECT value_sha, new_value "
            "FROM tmp_change "
            "WHERE value_len > 255 "
            "GROUP BY value_sha, new_value "
            "ORDER BY MIN(token_id);\n");
        mysql_exec_or_die(session, sql.data, "failed to stage long lexeme replacements");
        buffer_free(&sql);

        mysql_exec_or_die(session,
            "INSERT INTO lexeme_long (code_sha256, code) "
            "SELECT value_sha, new_value "
            "FROM tmp_long_values "
            "ORDER BY slot;\n",
            "failed to append long lexeme replacements");
        first_long_id = mysql_insert_id(session->conn);
        inserted_rows = mysql_affected_rows(session->conn);
        if (inserted_rows != long_count) {
            die("unexpected long lexeme insert count: expected %llu, inserted %llu",
                long_count, inserted_rows);
        }
        if (first_long_id == 0) {
            die("mysql_insert_id returned no id for long lexeme replacement batch");
        }

        buffer_init(&sql);
        buffer_append_str(&sql, "UPDATE tmp_long_values SET lexeme_long_id = ");
        buffer_appendf(&sql, "%llu", first_long_id);
        buffer_append_str(&sql, " + ((slot - 1) * ");
        buffer_appendf(&sql, "%llu", auto_increment_increment);
        buffer_append_str(&sql, ");\n"
            "UPDATE tmp_change c "
            "JOIN tmp_long_values lv "
            "  ON lv.value_sha = c.value_sha "
            " AND lv.new_value = c.new_value "
            "SET c.lexeme_long_id = lv.lexeme_long_id "
            "WHERE c.value_len > 255;\n");
        mysql_exec_or_die(session, sql.data, "failed to map long lexeme replacements");
        buffer_free(&sql);
    }

    buffer_init(&sql);
    buffer_append_str(&sql,
        "UPDATE token t "
        "JOIN tmp_change c ON c.token_id = t.id "
        "SET t.lexeme_id = CASE "
        "        WHEN c.value_len <= 255 THEN CAST(c.new_value AS BINARY(255)) "
        "        ELSE NULL "
        "    END, "
        "    t.lexeme_long_id = CASE "
        "        WHEN c.value_len > 255 THEN c.lexeme_long_id "
        "        ELSE NULL "
        "    END;\n"
        "INSERT IGNORE INTO lexeme_long_source_file (lexeme_long_id, source_file_id) "
        "SELECT DISTINCT lexeme_long_id, source_file_id "
        "FROM tmp_change "
        "WHERE lexeme_long_id IS NOT NULL;\n");
    mysql_exec_or_die(session, sql.data, "failed to replace token rows");
    buffer_free(&sql);
}

static void
cleanup_stale_long_source_links(MysqlSession *session)
{
    static const char sql[] =
        "DELETE llsf "
        "FROM lexeme_long_source_file llsf "
        "LEFT JOIN token t "
        "  ON t.lexeme_long_id = llsf.lexeme_long_id "
        " AND t.source_file_id = llsf.source_file_id "
        "WHERE t.id IS NULL;\n";

    mysql_exec_or_die(session, sql, "failed to clean stale lexeme_long_source_file rows");
}

static void
print_usage(FILE *stream, const char *argv0)
{
    fprintf(stream, "Usage: %s SOURCE TARGET\n", argv0);
}

int
main(int argc, char **argv)
{
    char *repo_root = NULL;
    char *env_path = NULL;
    char *database_url = NULL;
    DbConfig cfg;
    MysqlSession session;
    const char *source;
    const char *target;
    long long match_count;
    unsigned long long updated_rows;

    if (argc == 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        print_usage(stdout, argv[0]);
        return EXIT_SUCCESS;
    }
    if (argc != 3) {
        print_usage(stderr, argv[0]);
        return EXIT_FAILURE;
    }

    source = argv[1];
    target = argv[2];
    if (strcmp(source, target) == 0) {
        printf("source and target are identical; nothing to change\n");
        return EXIT_SUCCESS;
    }

    repo_root = getcwd(NULL, 0);
    if (!repo_root) {
        die("getcwd failed: %s", strerror(errno));
    }
    env_path = xmalloc(strlen(repo_root) + strlen("/.env") + 1);
    sprintf(env_path, "%s/.env", repo_root);

    database_url = read_env_value(env_path, "DATABASE_URL");
    parse_database_url(database_url, &cfg);
    mysql_session_start(&session, &cfg);

    mysql_exec_or_die(&session, "START TRANSACTION;\n", "failed to begin transaction");
    prepare_change_table(&session, source, target);
    match_count = count_matching_tokens(&session);
    if (match_count <= 0) {
        printf("no matching tokens found for source: %s\n", source);
        mysql_exec_or_die(&session, "ROLLBACK;\n", "failed to roll back token replacement");
        mysql_session_close(&session);
        db_config_free(&cfg);
        free(database_url);
        free(env_path);
        free(repo_root);
        return EXIT_FAILURE;
    }

    updated_rows = (unsigned long long) match_count;
    replace_tokens(&session, source, target);
    cleanup_stale_long_source_links(&session);
    mysql_exec_or_die(&session, "COMMIT;\n", "failed to commit token replacement");

    printf("updated %llu token rows from %s to %s\n", updated_rows, source, target);

    mysql_session_close(&session);
    db_config_free(&cfg);
    free(database_url);
    free(env_path);
    free(repo_root);
    return EXIT_SUCCESS;
}
