#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <mysql/mysql.h>

#if defined(Y) || defined(N)
#error "stb_c_lexer requires Y and N to be undefined before configuration"
#endif

#define STB_C_LEXER_IMPLEMENTATION
#define STB_C_LEXER_DEFINITIONS
#define STB_C_LEX_C_DECIMAL_INTS    Y
#define STB_C_LEX_C_HEX_INTS        Y
#define STB_C_LEX_C_OCTAL_INTS      Y
#define STB_C_LEX_C_DECIMAL_FLOATS  Y
#define STB_C_LEX_C99_HEX_FLOATS    N
#define STB_C_LEX_C_IDENTIFIERS     Y
#define STB_C_LEX_C_DQ_STRINGS      Y
#define STB_C_LEX_C_SQ_STRINGS      N
#define STB_C_LEX_C_CHARS           Y
#define STB_C_LEX_C_COMMENTS        Y
#define STB_C_LEX_CPP_COMMENTS      Y
#define STB_C_LEX_C_COMPARISONS     Y
#define STB_C_LEX_C_LOGICAL         Y
#define STB_C_LEX_C_SHIFTS          Y
#define STB_C_LEX_C_INCREMENTS      Y
#define STB_C_LEX_C_ARROW           Y
#define STB_C_LEX_EQUAL_ARROW       N
#define STB_C_LEX_C_BITWISEEQ       Y
#define STB_C_LEX_C_ARITHEQ         Y
#define STB_C_LEX_PARSE_SUFFIXES    Y
#define STB_C_LEX_DECIMAL_SUFFIXES  "uUlL"
#define STB_C_LEX_HEX_SUFFIXES      "uUlL"
#define STB_C_LEX_OCTAL_SUFFIXES    "uUlL"
#define STB_C_LEX_FLOAT_SUFFIXES    "fFlL"
#define STB_C_LEX_0_IS_EOF          N
#define STB_C_LEX_INTEGERS_AS_DOUBLES N
#define STB_C_LEX_MULTILINE_DSTRINGS N
#define STB_C_LEX_MULTILINE_SSTRINGS N
#define STB_C_LEX_USE_STDLIB        Y
#define STB_C_LEX_DISCARD_PREPROCESSOR N
#define STB_C_LEX_DOLLAR_IDENTIFIER N
#define STB_C_LEX_FLOAT_NO_DECIMAL  Y
#define STB_C_LEX_DEFINE_ALL_TOKEN_NAMES N
#include "vendor/stb_c_lexer.h"

#define ARRAY_GROW_CAP(cap) ((cap) < 16 ? 16 : (cap) * 2)
#define AST_MYSQL_PORT_DEFAULT 3306U
#define STATUS_TOKS "toks"
#define DEFAULT_DB_SOURCE_ROOT "/nfs/php-src"
#define TOKEN_INSERT_CHUNK 1000
#define LEXEME_INSERT_CHUNK 1000
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
    char **items;
    size_t len;
    size_t cap;
} StringArray;

typedef struct {
    int row;
    int col;
    char *text;
    bool store_in_db;
    long long lexeme_long_id;
} TokenPiece;

typedef struct {
    TokenPiece *items;
    size_t len;
    size_t cap;
} TokenArray;

typedef struct {
    char **slots;
    size_t cap;
    size_t len;
} ShortLexemeCache;

typedef struct {
    char *host;
    char *user;
    char *password;
    char *database;
    unsigned port;
} DbConfig;

typedef struct {
    MYSQL *conn;
    bool local_infile_enabled;
    unsigned long auto_increment_increment;
    unsigned long max_allowed_packet;
} MysqlSession;

typedef struct {
    char *repo_root;
    char *php_src_root;
    char *lint_php_src_root;
    char *db_source_root;
    const char *filter;
    size_t limit;
    size_t jobs;
    bool skip_existing;
    bool no_lint;
} Options;

typedef struct {
    bool interactive;
    bool use_color;
    size_t total;
    size_t processed;
    size_t imported;
    size_t skipped_existing;
    size_t lint_skipped;
} ProgressState;

typedef struct {
    bool *line_splice;
    bool *line_has_backslash_piece;
    size_t line_count;
} LineInfo;

typedef struct {
    TokenArray tokens;
    LineInfo line_info;
    char *error;
    bool ready;
    bool failed;
} PreparedFile;

typedef struct {
    const Options *opts;
    char **files;
    size_t file_count;
    PreparedFile *prepared;
    size_t next_index;
    bool stop;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} LexWorkQueue;

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
xcalloc(size_t n, size_t size)
{
    void *ptr = calloc(n, size);
    if (!ptr) {
        die("calloc(%zu, %zu) failed", n, size);
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
    if (need <= buf->cap) {
        return;
    }

    size_t cap = buf->cap ? buf->cap : 64;
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
ui_clear_live_line(void)
{
    if (!g_live_line_active) {
        return;
    }
    fprintf(stderr, "\r\x1b[2K\r");
    fflush(stderr);
    g_live_line_active = false;
}

static void
buffer_appendf(Buffer *buf, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    va_list copy;
    va_copy(copy, ap);
    int needed = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (needed < 0) {
        va_end(ap);
        die("vsnprintf failed");
    }
    buffer_reserve(buf, (size_t) needed);
    vsnprintf(buf->data + buf->len, buf->cap - buf->len, fmt, ap);
    va_end(ap);
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

static char *
buffer_take(Buffer *buf)
{
    if (!buf->data) {
        return xstrdup("");
    }
    char *data = buf->data;
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
    return data;
}

static void
string_array_push_owned(StringArray *arr, char *value)
{
    if (arr->len == arr->cap) {
        arr->cap = ARRAY_GROW_CAP(arr->cap);
        arr->items = xrealloc(arr->items, arr->cap * sizeof(*arr->items));
    }
    arr->items[arr->len++] = value;
}

static void
string_array_push_copy(StringArray *arr, const char *value)
{
    string_array_push_owned(arr, xstrdup(value));
}

static bool
string_array_contains(const StringArray *arr, const char *value)
{
    size_t i;
    for (i = 0; i < arr->len; i++) {
        if (strcmp(arr->items[i], value) == 0) {
            return true;
        }
    }
    return false;
}

static void
string_array_push_unique_copy(StringArray *arr, const char *value)
{
    if (!string_array_contains(arr, value)) {
        string_array_push_copy(arr, value);
    }
}

static void
string_array_free(StringArray *arr)
{
    size_t i;
    for (i = 0; i < arr->len; i++) {
        free(arr->items[i]);
    }
    free(arr->items);
    arr->items = NULL;
    arr->len = 0;
    arr->cap = 0;
}

static void
token_array_push(TokenArray *arr, int row, int col, char *text, bool store_in_db)
{
    if (arr->len == arr->cap) {
        arr->cap = ARRAY_GROW_CAP(arr->cap);
        arr->items = xrealloc(arr->items, arr->cap * sizeof(*arr->items));
    }

    arr->items[arr->len].row = row;
    arr->items[arr->len].col = col;
    arr->items[arr->len].text = text;
    arr->items[arr->len].store_in_db = store_in_db;
    arr->items[arr->len].lexeme_long_id = 0;
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

static uint64_t
hash_bytes_fnv1a(const char *text)
{
    const unsigned char *p = (const unsigned char *) text;
    uint64_t hash = 1469598103934665603ULL;

    while (*p) {
        hash ^= (uint64_t) *p++;
        hash *= 1099511628211ULL;
    }

    return hash;
}

static void
short_cache_rehash(ShortLexemeCache *cache, size_t new_cap)
{
    char **old_slots = cache->slots;
    size_t old_cap = cache->cap;
    size_t i;

    cache->slots = xcalloc(new_cap, sizeof(*cache->slots));
    cache->cap = new_cap;
    cache->len = 0;

    for (i = 0; i < old_cap; i++) {
        char *text = old_slots[i];
        if (text) {
            size_t idx = (size_t) (hash_bytes_fnv1a(text) & (uint64_t) (cache->cap - 1));
            while (cache->slots[idx]) {
                idx = (idx + 1) & (cache->cap - 1);
            }
            cache->slots[idx] = text;
            cache->len++;
        }
    }

    free(old_slots);
}

static bool
short_cache_contains(const ShortLexemeCache *cache, const char *text)
{
    size_t idx;

    if (cache->cap == 0) {
        return false;
    }

    idx = (size_t) (hash_bytes_fnv1a(text) & (uint64_t) (cache->cap - 1));
    while (cache->slots[idx]) {
        if (strcmp(cache->slots[idx], text) == 0) {
            return true;
        }
        idx = (idx + 1) & (cache->cap - 1);
    }

    return false;
}

static void
short_cache_add_copy(ShortLexemeCache *cache, const char *text)
{
    size_t idx;

    if (cache->cap == 0) {
        short_cache_rehash(cache, 1024);
    } else if ((cache->len + 1) * 10 >= cache->cap * 7) {
        short_cache_rehash(cache, cache->cap * 2);
    }

    idx = (size_t) (hash_bytes_fnv1a(text) & (uint64_t) (cache->cap - 1));
    while (cache->slots[idx]) {
        if (strcmp(cache->slots[idx], text) == 0) {
            return;
        }
        idx = (idx + 1) & (cache->cap - 1);
    }

    cache->slots[idx] = xstrdup(text);
    cache->len++;
}

static void
short_cache_add_batch(ShortLexemeCache *cache, char **items, size_t count)
{
    size_t i;

    for (i = 0; i < count; i++) {
        short_cache_add_copy(cache, items[i]);
    }
}

static void
short_cache_free(ShortLexemeCache *cache)
{
    size_t i;

    for (i = 0; i < cache->cap; i++) {
        free(cache->slots[i]);
    }
    free(cache->slots);
    cache->slots = NULL;
    cache->cap = 0;
    cache->len = 0;
}

static void
line_info_free(LineInfo *info)
{
    free(info->line_splice);
    free(info->line_has_backslash_piece);
    info->line_splice = NULL;
    info->line_has_backslash_piece = NULL;
    info->line_count = 0;
}

static int
cmp_string_ptr(const void *lhs, const void *rhs)
{
    const char *const *a = lhs;
    const char *const *b = rhs;
    return strcmp(*a, *b);
}

static int
cmp_long_long(const void *lhs, const void *rhs)
{
    const long long *a = lhs;
    const long long *b = rhs;
    if (*a < *b) {
        return -1;
    }
    if (*a > *b) {
        return 1;
    }
    return 0;
}

static bool
has_suffix(const char *path, const char *suffix)
{
    size_t path_len = strlen(path);
    size_t suffix_len = strlen(suffix);
    if (path_len < suffix_len) {
        return false;
    }
    return memcmp(path + path_len - suffix_len, suffix, suffix_len) == 0;
}

static bool
is_source_path(const char *path)
{
    return has_suffix(path, ".c") || has_suffix(path, ".h");
}

static bool
is_hidden_name(const char *name)
{
    return name[0] == '.';
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
    if (right[0] == '/') {
        right++;
    }
    buffer_append_str(&buf, right);
    return buffer_take(&buf);
}

static char *
path_dirname(const char *path)
{
    const char *slash = strrchr(path, '/');
    if (!slash) {
        return xstrdup(".");
    }
    if (slash == path) {
        return xstrdup("/");
    }
    return xstrndup(path, (size_t) (slash - path));
}

static bool
resolve_include_in_dirs(const char *header, const StringArray *include_dirs)
{
    size_t i;

    for (i = 0; i < include_dirs->len; i++) {
        char *candidate = path_join(include_dirs->items[i], header);
        bool exists = file_exists(candidate);
        free(candidate);
        if (exists) {
            return true;
        }
    }

    return false;
}

static char *
find_first_missing_local_include(const char *source_path, const StringArray *include_dirs)
{
    FILE *fp = fopen(source_path, "r");
    char line[8192];

    if (!fp) {
        die("failed to open %s: %s", source_path, strerror(errno));
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        char *p = trim_in_place(line);
        char *end;

        if (*p != '#') {
            continue;
        }
        p++;
        while (*p && isspace((unsigned char) *p)) {
            p++;
        }
        if (strncmp(p, "include", 7) != 0) {
            continue;
        }
        p += 7;
        while (*p && isspace((unsigned char) *p)) {
            p++;
        }
        if (*p != '"') {
            continue;
        }
        p++;
        end = strchr(p, '"');
        if (!end) {
            continue;
        }
        *end = '\0';
        if (!resolve_include_in_dirs(p, include_dirs)) {
            fclose(fp);
            return xstrdup(p);
        }
    }

    fclose(fp);
    return NULL;
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

    {
        size_t tail_len = max_len - 3;
        char *out = xmalloc(max_len + 1);
        memcpy(out, "...", 3);
        memcpy(out + 3, text + (len - tail_len), tail_len);
        out[max_len] = '\0';
        return out;
    }
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
    size_t i;
    size_t cols;
    size_t bar_width;
    size_t path_budget;
    size_t completed = 0;
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
        "[import] [] %6.1f%% %zu/%zu %-5s ok:%zu skip:%zu lint-skip:%zu ",
        percent,
        progress->processed,
        progress->total,
        phase ? phase : "",
        progress->imported,
        progress->skipped_existing,
        progress->lint_skipped);
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
        "[import] [%s] %6.1f%% %zu/%zu %-5s ok:%zu skip:%zu lint-skip:%zu ",
        bar,
        percent,
        progress->processed,
        progress->total,
        phase ? phase : "",
        progress->imported,
        progress->skipped_existing,
        progress->lint_skipped);
    if (fixed_with_bar < 0) {
        fixed_with_bar = 0;
    }
    path_budget = cols > (size_t) fixed_with_bar ? cols - (size_t) fixed_with_bar : 0U;
    display_path = tail_ellipsized_copy(path ? path : "", path_budget);

    fprintf(stderr,
        "\r\x1b[2K\r%s[import]%s %s[%s]%s %6.1f%% %zu/%zu %s%-5s%s ok:%zu skip:%zu lint-skip:%zu %s%s%s",
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
        progress->imported,
        progress->skipped_existing,
        progress->lint_skipped,
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
        "%s[import]%s %s%s%s processed:%zu imported:%zu skip:%zu lint-skip:%zu\n",
        label_color,
        reset,
        status_color,
        status ? status : "done",
        reset,
        progress->processed,
        progress->imported,
        progress->skipped_existing,
        progress->lint_skipped);
    fflush(stderr);
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

static char *
read_file(const char *path, size_t *size_out)
{
    FILE *fp = fopen(path, "rb");
    long len;
    char *data;
    size_t got;

    if (!fp) {
        die("failed to open %s: %s", path, strerror(errno));
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        die("failed to seek %s: %s", path, strerror(errno));
    }
    len = ftell(fp);
    if (len < 0) {
        fclose(fp);
        die("failed to tell %s: %s", path, strerror(errno));
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        die("failed to rewind %s: %s", path, strerror(errno));
    }

    data = xmalloc((size_t) len + 1);
    got = fread(data, 1, (size_t) len, fp);
    if (got != (size_t) len) {
        fclose(fp);
        free(data);
        die("failed to read %s", path);
    }
    fclose(fp);

    data[len] = '\0';
    if (size_out) {
        *size_out = (size_t) len;
    }
    return data;
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

static void
append_sql_string(Buffer *buf, const char *value)
{
    const unsigned char *p = (const unsigned char *) value;
    buffer_append_char(buf, '\'');
    while (*p) {
        switch (*p) {
        case '\0':
            buffer_append_str(buf, "\\0");
            break;
        case '\n':
            buffer_append_str(buf, "\\n");
            break;
        case '\r':
            buffer_append_str(buf, "\\r");
            break;
        case '\t':
            buffer_append_str(buf, "\\t");
            break;
        case '\\':
            buffer_append_str(buf, "\\\\");
            break;
        case '\'':
            buffer_append_str(buf, "\\'");
            break;
        case '\032':
            buffer_append_str(buf, "\\Z");
            break;
        default:
            buffer_append_char(buf, (char) *p);
            break;
        }
        p++;
    }
    buffer_append_char(buf, '\'');
}

static void
append_position(const char *from, const char *to, int *row, int *col)
{
    const char *p;
    for (p = from; p < to; p++) {
        if (*p == '\r') {
            continue;
        }
        if (*p == '\n') {
            (*row)++;
            *col = 1;
        } else {
            (*col)++;
        }
    }
}

static LineInfo
compute_line_info(const char *content, size_t size)
{
    LineInfo info;
    size_t i;
    size_t line_count = 0;
    size_t line_no = 1;
    size_t line_start = 0;

    memset(&info, 0, sizeof(info));

    for (i = 0; i < size; i++) {
        if (content[i] == '\n') {
            line_count++;
        }
    }
    if (size == 0 || content[size - 1] != '\n') {
        line_count++;
    }
    if (line_count == 0) {
        line_count = 1;
    }

    info.line_count = line_count;
    info.line_splice = xcalloc(line_count + 2, sizeof(*info.line_splice));
    info.line_has_backslash_piece = xcalloc(line_count + 2, sizeof(*info.line_has_backslash_piece));

    for (i = 0; i <= size; i++) {
        if (i == size || content[i] == '\n') {
            size_t line_end = i;
            if (line_end > line_start && content[line_end - 1] == '\r') {
                line_end--;
            }
            if (line_end > line_start && content[line_end - 1] == '\\') {
                info.line_splice[line_no] = true;
            }
            line_no++;
            line_start = i + 1;
        }
    }

    return info;
}

static void
lex_source_file(const char *path, const char *content, size_t size, TokenArray *tokens, LineInfo *line_info)
{
    stb_lexer lexer;
    char *string_store = xmalloc(size + 32);
    const char *scan = content;
    int row = 1;
    int col = 1;

    *line_info = compute_line_info(content, size);
    memset(tokens, 0, sizeof(*tokens));

    stb_c_lexer_init(&lexer, content, content + size, string_store, (int) (size + 32));

    while (stb_c_lexer_get_token(&lexer)) {
        char *start = lexer.where_firstchar;
        char *after = lexer.where_lastchar + 1;
        int gap_row;
        int gap_col;
        int token_row;
        int token_col;
        size_t len;
        char *text;

        gap_row = row;
        gap_col = col;
        if (start > scan) {
            token_array_push(tokens, gap_row, gap_col, xstrndup(scan, (size_t) (start - scan)), true);
            append_position(scan, start, &row, &col);
        }
        token_row = row;
        token_col = col;

        if (lexer.token == CLEX_parse_error) {
            free(string_store);
            token_array_free(tokens);
            line_info_free(line_info);
            die("lexer parse error in %s at %d:%d", path, token_row, token_col);
        }

        len = (size_t) (after - start);
        text = xstrndup(start, len);
        token_array_push(tokens, token_row, token_col, text, true);
        append_position(start, after, &row, &col);
        scan = after;
    }

    if (scan < content + size) {
        token_array_push(tokens, row, col, xstrndup(scan, (size_t) ((content + size) - scan)), true);
    }

    free(string_store);
}

static char *
normalized_db_path(const char *actual_path, const char *php_src_root, const char *db_source_root)
{
    size_t root_len = strlen(php_src_root);
    const char *relative;

    if (strncmp(actual_path, php_src_root, root_len) != 0) {
        die("path %s is not inside %s", actual_path, php_src_root);
    }

    relative = actual_path + root_len;
    while (*relative == '/') {
        relative++;
    }

    return path_join(db_source_root, relative);
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

static void
preflight_php_src_configured(const Options *opts)
{
    char *main_php_config = path_join(opts->lint_php_src_root, "main/php_config.h");
    char *zend_config = path_join(opts->lint_php_src_root, "Zend/zend_config.h");
    Buffer missing;

    buffer_init(&missing);

    if (!file_exists(main_php_config)) {
        buffer_append_str(&missing, " - missing ");
        buffer_append_str(&missing, main_php_config);
        buffer_append_char(&missing, '\n');
    }
    if (!file_exists(zend_config)) {
        buffer_append_str(&missing, " - missing ");
        buffer_append_str(&missing, zend_config);
        buffer_append_char(&missing, '\n');
    }

    free(main_php_config);
    free(zend_config);

    if (missing.len > 0) {
        die("php-src is not configured; run ./configure inside %s before importing.\n%s"
            "lint validation depends on the generated configure headers and should stay outside the import step.",
            opts->lint_php_src_root,
            missing.data ? missing.data : "");
    }

    buffer_free(&missing);
}

static void
collect_source_files_recursive(const char *dir, StringArray *files)
{
    DIR *dp = opendir(dir);
    struct dirent *entry;

    if (!dp) {
        die("failed to open directory %s: %s", dir, strerror(errno));
    }

    while ((entry = readdir(dp)) != NULL) {
        char *full;
        struct stat st;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (is_hidden_name(entry->d_name)) {
            continue;
        }

        full = path_join(dir, entry->d_name);
        if (stat(full, &st) != 0) {
            free(full);
            closedir(dp);
            die("stat(%s) failed: %s", dir, strerror(errno));
        }

        if (S_ISDIR(st.st_mode)) {
            collect_source_files_recursive(full, files);
            free(full);
            continue;
        }

        if (S_ISREG(st.st_mode) && is_source_path(full)) {
            string_array_push_owned(files, full);
        } else {
            free(full);
        }
    }

    closedir(dp);
}

static int
cmp_owned_string(const void *lhs, const void *rhs)
{
    const char *const *a = lhs;
    const char *const *b = rhs;
    return strcmp(*a, *b);
}

static void
build_lint_include_dirs(const Options *opts, const char *actual_path, StringArray *dirs)
{
    char *dir = path_dirname(actual_path);
    char *root_main = path_join(opts->lint_php_src_root, "main");
    char *root_zend = path_join(opts->lint_php_src_root, "Zend");
    char *root_tsrm = path_join(opts->lint_php_src_root, "TSRM");

    memset(dirs, 0, sizeof(*dirs));

    while (dir && dir[0] != '\0') {
        string_array_push_unique_copy(dirs, dir);
        if (strcmp(dir, opts->lint_php_src_root) == 0 || strcmp(dir, "/") == 0 || strcmp(dir, ".") == 0) {
            break;
        }

        char *parent = path_dirname(dir);
        if (strcmp(parent, dir) == 0) {
            free(parent);
            break;
        }
        free(dir);
        dir = parent;
    }

    free(dir);
    string_array_push_unique_copy(dirs, root_main);
    string_array_push_unique_copy(dirs, root_zend);
    string_array_push_unique_copy(dirs, root_tsrm);
    free(root_main);
    free(root_zend);
    free(root_tsrm);
}

static char *
canonicalize_path_or_keep(const char *path)
{
    char resolved[PATH_MAX];

    if (realpath(path, resolved)) {
        return xstrdup(resolved);
    }
    if (errno != ENOENT) {
        die("realpath(%s) failed: %s", path, strerror(errno));
    }
    return xstrdup(path);
}

static char *
map_path_between_roots(const char *actual_path, const char *from_root, const char *to_root)
{
    size_t root_len = strlen(from_root);
    const char *relative;

    if (strncmp(actual_path, from_root, root_len) != 0) {
        die("path %s is not inside %s", actual_path, from_root);
    }

    relative = actual_path + root_len;
    while (*relative == '/') {
        relative++;
    }

    return path_join(to_root, relative);
}

static void
load_configured_c_sources(const char *makefile_objects_path, StringArray *sources)
{
    FILE *fp = fopen(makefile_objects_path, "rb");
    char line[8192];

    if (!fp) {
        die("failed to open %s: %s", makefile_objects_path, strerror(errno));
    }

    memset(sources, 0, sizeof(*sources));

    while (fgets(line, sizeof(line), fp) != NULL) {
        char *colon;
        char *path;

        if (line[0] == '\t' || strncmp(line, "-include ", 9) == 0) {
            continue;
        }

        colon = strstr(line, ": ");
        if (!colon) {
            continue;
        }

        path = trim_in_place(colon + 2);
        if (path[0] != '/' || !has_suffix(path, ".c")) {
            continue;
        }

        if (!string_array_contains(sources, path)) {
            string_array_push_copy(sources, path);
        }
    }

    fclose(fp);
}

static bool
lint_should_skip_for_host(const char *actual_path, const StringArray *configured_c_sources)
{
    if (!has_suffix(actual_path, ".c")) {
        return true;
    }
    return !string_array_contains(configured_c_sources, actual_path);
}

static char *
make_temp_sibling_path(const char *actual_path)
{
    char *dir = path_dirname(actual_path);
    char *template = path_join(dir, ".import-lint-XXXXXX");
    int fd = mkstemp(template);
    if (fd < 0) {
        free(dir);
        free(template);
        die("mkstemp failed near %s: %s", actual_path, strerror(errno));
    }
    close(fd);
    free(dir);
    return template;
}

static char *
make_temp_path_in_dir(const char *dir, const char *pattern)
{
    char *template = path_join(dir, pattern);
    int fd = mkstemp(template);

    if (fd < 0) {
        free(template);
        die("mkstemp failed in %s: %s", dir, strerror(errno));
    }
    close(fd);
    return template;
}

static void
write_text_file(const char *path, const char *data, size_t len)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        die("failed to open %s for write: %s", path, strerror(errno));
    }
    if (len > 0 && fwrite(data, 1, len, fp) != len) {
        fclose(fp);
        die("failed to write %s", path);
    }
    fclose(fp);
}

static void
append_load_data_field(Buffer *buf, const char *value)
{
    const unsigned char *p = (const unsigned char *) value;

    while (*p) {
        switch (*p) {
        case '\0':
            buffer_append_str(buf, "\\0");
            break;
        case '\b':
            buffer_append_str(buf, "\\b");
            break;
        case '\n':
            buffer_append_str(buf, "\\n");
            break;
        case '\r':
            buffer_append_str(buf, "\\r");
            break;
        case '\t':
            buffer_append_str(buf, "\\t");
            break;
        case '\\':
            buffer_append_str(buf, "\\\\");
            break;
        case '\032':
            buffer_append_str(buf, "\\Z");
            break;
        default:
            buffer_append_char(buf, (char) *p);
            break;
        }
        p++;
    }
}

static int
run_command_capture(char *const argv[], const char *stdin_path, const char *extra_env_name, const char *extra_env_value, Buffer *output)
{
    int pipefd[2];
    pid_t pid;
    int status;
    ssize_t nread;
    char chunk[8192];

    if (pipe(pipefd) != 0) {
        die("pipe failed: %s", strerror(errno));
    }

    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        die("fork failed: %s", strerror(errno));
    }

    if (pid == 0) {
        int in_fd = -1;
        close(pipefd[0]);
        if (stdin_path) {
            in_fd = open(stdin_path, O_RDONLY);
            if (in_fd < 0) {
                perror("open stdin_path");
                _exit(127);
            }
            if (dup2(in_fd, STDIN_FILENO) < 0) {
                perror("dup2 stdin");
                _exit(127);
            }
            close(in_fd);
        }
        if (dup2(pipefd[1], STDOUT_FILENO) < 0 || dup2(pipefd[1], STDERR_FILENO) < 0) {
            perror("dup2 stdout/stderr");
            _exit(127);
        }
        close(pipefd[1]);
        if (extra_env_name && extra_env_value) {
            if (setenv(extra_env_name, extra_env_value, 1) != 0) {
                perror("setenv");
                _exit(127);
            }
        }
        execvp(argv[0], argv);
        perror(argv[0]);
        _exit(127);
    }

    close(pipefd[1]);
    buffer_init(output);
    while ((nread = read(pipefd[0], chunk, sizeof(chunk))) > 0) {
        buffer_append_mem(output, chunk, (size_t) nread);
    }
    close(pipefd[0]);

    if (waitpid(pid, &status, 0) < 0) {
        die("waitpid failed: %s", strerror(errno));
    }

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return status;
}

static void
mysql_session_start(MysqlSession *session, const DbConfig *cfg, const char *database_name)
{
    unsigned int local_infile = 1;
    unsigned long flags = CLIENT_MULTI_STATEMENTS | CLIENT_LOCAL_FILES;

    memset(session, 0, sizeof(*session));
    session->conn = mysql_init(NULL);
    if (!session->conn) {
        die("mysql_init failed");
    }
    if (mysql_options(session->conn, MYSQL_OPT_LOCAL_INFILE, &local_infile) != 0) {
        die("mysql_options(MYSQL_OPT_LOCAL_INFILE) failed: %s", mysql_error(session->conn));
    }
    if (!mysql_real_connect(
            session->conn,
            cfg->host,
            cfg->user,
            cfg->password,
            (database_name && database_name[0] != '\0') ? database_name : NULL,
            cfg->port,
            NULL,
            flags)) {
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
    if (!session->conn) {
        die("mysql session is not open");
    }

    buffer_init(output);
    if (mysql_real_query(session->conn, sql, (unsigned long) strlen(sql)) != 0) {
        buffer_appendf(output, "ERROR %u (%s): %s\n",
            mysql_errno(session->conn),
            mysql_sqlstate(session->conn),
            mysql_error(session->conn));
        return 1;
    }

    for (;;) {
        MYSQL_RES *result = mysql_store_result(session->conn);

        if (result) {
            unsigned int num_fields = mysql_num_fields(result);
            MYSQL_ROW row;

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
    int exit_code = mysql_session_exec_capture(session, sql, &output);
    char *line;

    if (exit_code != 0) {
        char *message = buffer_take(&output);
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
    int exit_code = mysql_session_exec_capture(session, sql, &output);

    if (exit_code != 0) {
        char *message = buffer_take(&output);
        die("%s:\n%s", context, message);
    }

    buffer_free(&output);
}

static bool
mysql_local_infile_enabled(MysqlSession *session)
{
    Buffer sql;
    char *value;
    bool enabled;

    buffer_init(&sql);
    buffer_append_str(&sql, "SELECT @@local_infile;\n");
    value = mysql_scalar(session, sql.data, false);
    buffer_free(&sql);
    enabled = strcmp(value, "1") == 0 || strcasecmp(value, "ON") == 0;
    free(value);
    return enabled;
}

static unsigned long
mysql_auto_increment_increment(MysqlSession *session)
{
    Buffer sql;
    char *value;
    unsigned long increment;

    buffer_init(&sql);
    buffer_append_str(&sql, "SELECT @@auto_increment_increment;\n");
    value = mysql_scalar(session, sql.data, false);
    buffer_free(&sql);
    increment = (unsigned long) strtoul(value, NULL, 10);
    free(value);
    return increment > 0 ? increment : 1;
}

static unsigned long
mysql_max_allowed_packet(MysqlSession *session)
{
    Buffer sql;
    char *value;
    unsigned long packet_size;

    buffer_init(&sql);
    buffer_append_str(&sql, "SELECT @@max_allowed_packet;\n");
    value = mysql_scalar(session, sql.data, false);
    buffer_free(&sql);
    packet_size = (unsigned long) strtoul(value, NULL, 10);
    free(value);
    return packet_size > 0 ? packet_size : (1024UL * 1024UL);
}

static bool
db_path_exists(MysqlSession *session, const char *db_path)
{
    Buffer sql;
    char *count_text;
    long long count;

    buffer_init(&sql);
    buffer_append_str(&sql, "SELECT COUNT(*) FROM source_file WHERE path = ");
    append_sql_string(&sql, db_path);
    buffer_append_str(&sql, ";\n");
    count_text = mysql_scalar(session, sql.data, false);
    count = strtoll(count_text, NULL, 10);
    free(count_text);
    buffer_free(&sql);
    return count > 0;
}

static void
reset_database_from_schema(MysqlSession *session, const DbConfig *cfg, const char *schema_path)
{
    size_t schema_size = 0;
    char *schema_text;
    Buffer sql;
    Buffer output;
    Buffer create_stmt;
    Buffer use_stmt;
    int exit_code;

    if (!file_exists(schema_path)) {
        die("missing schema file: %s", schema_path);
    }

    schema_text = read_file(schema_path, &schema_size);
    (void) schema_size;

    buffer_init(&create_stmt);
    buffer_append_str(&create_stmt, "CREATE DATABASE IF NOT EXISTS `");
    buffer_append_str(&create_stmt, cfg->database);
    buffer_append_str(&create_stmt, "`");

    buffer_init(&use_stmt);
    buffer_append_str(&use_stmt, "USE `");
    buffer_append_str(&use_stmt, cfg->database);
    buffer_append_str(&use_stmt, "`;");

    if (!strstr(schema_text, create_stmt.data) || !strstr(schema_text, use_stmt.data)) {
        free(schema_text);
        buffer_free(&create_stmt);
        buffer_free(&use_stmt);
        die("schema file %s does not target database `%s`; update ast.sql or DATABASE_URL so they match before importing",
            schema_path,
            cfg->database);
    }

    buffer_init(&sql);
    buffer_append_str(&sql, "DROP DATABASE IF EXISTS `");
    buffer_append_str(&sql, cfg->database);
    buffer_append_str(&sql, "`;\n");
    buffer_append_str(&sql, schema_text);

    free(schema_text);
    buffer_free(&create_stmt);
    buffer_free(&use_stmt);

    exit_code = mysql_session_exec_capture(session, sql.data, &output);
    buffer_free(&sql);
    if (exit_code != 0) {
        char *message = buffer_take(&output);
        die("failed to reset database from %s:\n%s", schema_path, message);
    }
    buffer_free(&output);
}


static void
assign_long_lexeme_ids(MysqlSession *session, TokenArray *tokens)
{
    size_t i = 0;
    unsigned long increment = session->auto_increment_increment > 0 ? session->auto_increment_increment : 1;

    while (i < tokens->len) {
        Buffer sql;
        size_t chunk_start;
        size_t chunk_count = 0;
        long long first_id;
        size_t j;

        while (i < tokens->len && (!tokens->items[i].store_in_db || strlen(tokens->items[i].text) <= 255)) {
            i++;
        }
        if (i >= tokens->len) {
            break;
        }

        chunk_start = i;
        buffer_init(&sql);
        buffer_append_str(&sql, "INSERT INTO lexeme_long (code_sha256, code) VALUES ");
        while (i < tokens->len && chunk_count < LEXEME_INSERT_CHUNK) {
            TokenPiece *token = &tokens->items[i];
            if (!token->store_in_db || strlen(token->text) <= 255) {
                i++;
                continue;
            }
            if (chunk_count > 0) {
                buffer_append_str(&sql, ", ");
            }
            buffer_append_str(&sql, "(SHA2(");
            append_sql_string(&sql, token->text);
            buffer_append_str(&sql, ", 256), ");
            append_sql_string(&sql, token->text);
            buffer_append_char(&sql, ')');
            chunk_count++;
            i++;
        }
        buffer_append_str(&sql, ";\n");
        mysql_exec_or_die(session, sql.data, "failed to insert lexeme_long rows");
        buffer_free(&sql);

        first_id = (long long) mysql_insert_id(session->conn);
        if (first_id <= 0) {
            die("mysql_insert_id returned no id for lexeme_long insert");
        }

        for (j = 0; j < chunk_count; j++) {
            while (chunk_start < tokens->len
                && (!tokens->items[chunk_start].store_in_db || strlen(tokens->items[chunk_start].text) <= 255)) {
                chunk_start++;
            }
            if (chunk_start >= tokens->len) {
                die("internal error while assigning lexeme_long ids");
            }
            tokens->items[chunk_start].lexeme_long_id = first_id + (long long) (j * increment);
            chunk_start++;
        }
    }
}

static size_t
collect_unique_short_lexemes(const TokenArray *tokens, const ShortLexemeCache *cache, char ***out)
{
    size_t i;
    size_t count = 0;
    char **items;

    for (i = 0; i < tokens->len; i++) {
        if (tokens->items[i].store_in_db
            && strlen(tokens->items[i].text) <= 255
            && !short_cache_contains(cache, tokens->items[i].text)) {
            count++;
        }
    }

    items = xcalloc(count ? count : 1, sizeof(*items));
    count = 0;
    for (i = 0; i < tokens->len; i++) {
        if (tokens->items[i].store_in_db
            && strlen(tokens->items[i].text) <= 255
            && !short_cache_contains(cache, tokens->items[i].text)) {
            items[count++] = tokens->items[i].text;
        }
    }

    if (count > 1) {
        size_t write_idx = 1;
        qsort(items, count, sizeof(*items), cmp_string_ptr);
        for (i = 1; i < count; i++) {
            if (strcmp(items[i], items[write_idx - 1]) != 0) {
                items[write_idx++] = items[i];
            }
        }
        count = write_idx;
    }

    *out = items;
    return count;
}

static size_t
collect_unique_long_lexeme_ids(TokenArray *tokens, long long **out)
{
    size_t i;
    size_t count = 0;
    long long *items;

    for (i = 0; i < tokens->len; i++) {
        if (tokens->items[i].store_in_db && tokens->items[i].lexeme_long_id != 0) {
            count++;
        }
    }

    items = xcalloc(count ? count : 1, sizeof(*items));
    count = 0;
    for (i = 0; i < tokens->len; i++) {
        if (tokens->items[i].store_in_db && tokens->items[i].lexeme_long_id != 0) {
            items[count++] = tokens->items[i].lexeme_long_id;
        }
    }

    if (count > 1) {
        size_t write_idx = 1;
        qsort(items, count, sizeof(*items), cmp_long_long);
        for (i = 1; i < count; i++) {
            if (items[i] != items[write_idx - 1]) {
                items[write_idx++] = items[i];
            }
        }
        count = write_idx;
    }

    *out = items;
    return count;
}

static void
write_token_load_file(const char *path, const TokenArray *tokens, long long source_file_id)
{
    FILE *fp = fopen(path, "wb");
    size_t i;

    if (!fp) {
        die("failed to open %s for write: %s", path, strerror(errno));
    }

    for (i = 0; i < tokens->len; i++) {
        const TokenPiece *token = &tokens->items[i];
        Buffer line;

        if (!token->store_in_db) {
            continue;
        }

        buffer_init(&line);
        if (token->lexeme_long_id == 0) {
            append_load_data_field(&line, token->text);
        }
        buffer_append_char(&line, '\t');
        if (token->lexeme_long_id != 0) {
            buffer_appendf(&line, "%lld", token->lexeme_long_id);
        }
        buffer_appendf(&line, "\t%lld\t%d\t%d\n", source_file_id, token->row, token->col);

        if (fwrite(line.data, 1, line.len, fp) != line.len) {
            buffer_free(&line);
            fclose(fp);
            die("failed to write %s", path);
        }
        buffer_free(&line);
    }

    fclose(fp);
}

static void
insert_token_batches(MysqlSession *session, const TokenArray *tokens)
{
    static const char prefix[] = "INSERT INTO token (lexeme_id, lexeme_long_id, source_file_id, row_num, col_num) VALUES ";
    size_t i = 0;
    size_t packet_limit = (size_t) session->max_allowed_packet;

    if (packet_limit > 16384U) {
        packet_limit -= 8192U;
    }
    if (packet_limit == 0) {
        packet_limit = 1024U * 1024U;
    }

    while (i < tokens->len) {
        Buffer stmt;
        size_t inserted = 0;

        buffer_init(&stmt);
        buffer_append_str(&stmt, prefix);

        while (i < tokens->len) {
            const TokenPiece *token = &tokens->items[i];
            Buffer row;
            size_t extra_sep = inserted > 0 ? 2U : 0U;

            if (!token->store_in_db) {
                i++;
                continue;
            }

            buffer_init(&row);
            buffer_append_char(&row, '(');
            if (token->lexeme_long_id != 0) {
                buffer_append_str(&row, "NULL, ");
                buffer_appendf(&row, "%lld", token->lexeme_long_id);
            } else {
                append_sql_string(&row, token->text);
                buffer_append_str(&row, ", NULL");
            }
            buffer_appendf(&row, ", @source_file_id, %d, %d)", token->row, token->col);

            if (inserted > 0
                && (inserted >= TOKEN_INSERT_CHUNK || stmt.len + extra_sep + row.len + 2U > packet_limit)) {
                buffer_free(&row);
                break;
            }

            if (inserted > 0) {
                buffer_append_str(&stmt, ", ");
            }
            buffer_append_mem(&stmt, row.data, row.len);
            buffer_free(&row);
            inserted++;
            i++;
        }

        if (inserted == 0) {
            buffer_free(&stmt);
            die("single token row exceeded max_allowed_packet safety limit");
        }

        buffer_append_str(&stmt, ";\n");
        mysql_exec_or_die(session, stmt.data, "failed to insert token batch");
        buffer_free(&stmt);
    }
}

static long long
import_tokenized_file(
    MysqlSession *session,
    ShortLexemeCache *short_cache,
    const char *db_path,
    const TokenArray *tokens,
    bool replace_existing)
{
    Buffer sql;
    char **short_lexemes = NULL;
    long long *long_ids = NULL;
    char *token_load_path = NULL;
    size_t short_count;
    size_t long_count;
    size_t i;
    char *scalar;
    long long source_file_id;

    short_count = collect_unique_short_lexemes(tokens, short_cache, &short_lexemes);
    long_count = collect_unique_long_lexeme_ids((TokenArray *) tokens, &long_ids);

    buffer_init(&sql);
    buffer_append_str(&sql, "START TRANSACTION;\n");

    if (replace_existing) {
        buffer_append_str(&sql, "DELETE FROM token WHERE source_file_id IN (SELECT id FROM source_file WHERE path = ");
        append_sql_string(&sql, db_path);
        buffer_append_str(&sql, ");\n");
        buffer_append_str(&sql, "DELETE FROM source_file WHERE path = ");
        append_sql_string(&sql, db_path);
        buffer_append_str(&sql, ";\n");
    }

    for (i = 0; i < short_count; i += LEXEME_INSERT_CHUNK) {
        size_t j;
        size_t chunk_end = i + LEXEME_INSERT_CHUNK;
        if (chunk_end > short_count) {
            chunk_end = short_count;
        }
        buffer_append_str(&sql, "INSERT IGNORE INTO lexeme (id) VALUES ");
        for (j = i; j < chunk_end; j++) {
            if (j != i) {
                buffer_append_str(&sql, ", ");
            }
            buffer_append_char(&sql, '(');
            append_sql_string(&sql, short_lexemes[j]);
            buffer_append_char(&sql, ')');
        }
        buffer_append_str(&sql, ";\n");
    }

    buffer_append_str(&sql, "INSERT INTO source_file (status, path) VALUES (");
    append_sql_string(&sql, STATUS_TOKS);
    buffer_append_str(&sql, ", ");
    append_sql_string(&sql, db_path);
    buffer_append_str(&sql, ");\nSET @source_file_id = LAST_INSERT_ID();\n");

    if (long_count > 0) {
        buffer_append_str(&sql, "INSERT INTO lexeme_long_source_file (lexeme_long_id, source_file_id) VALUES ");
        for (i = 0; i < long_count; i++) {
            if (i > 0) {
                buffer_append_str(&sql, ", ");
            }
            buffer_appendf(&sql, "(%lld, @source_file_id)", long_ids[i]);
        }
        buffer_append_str(&sql, ";\n");
    }

    buffer_append_str(&sql, "SELECT @source_file_id;\n");
    scalar = mysql_scalar(session, sql.data, false);
    source_file_id = strtoll(scalar, NULL, 10);
    free(scalar);
    buffer_free(&sql);

    if (session->local_infile_enabled) {
        token_load_path = make_temp_path_in_dir("/tmp", ".ast-token-load-XXXXXX");
        write_token_load_file(token_load_path, tokens, source_file_id);
        Buffer load_sql;

        buffer_init(&load_sql);
        buffer_append_str(&load_sql,
            "LOAD DATA LOCAL INFILE ");
        append_sql_string(&load_sql, token_load_path);
        buffer_append_str(&load_sql,
            " INTO TABLE token "
            "FIELDS TERMINATED BY '\\t' ESCAPED BY '\\\\' "
            "LINES TERMINATED BY '\\n' "
            "(@lexeme_id, @lexeme_long_id, source_file_id, row_num, col_num) "
            "SET lexeme_id = NULLIF(@lexeme_id, ''), "
            "lexeme_long_id = NULLIF(@lexeme_long_id, '');\n");
        buffer_append_str(&load_sql, "COMMIT;\n");
        mysql_exec_or_die(session, load_sql.data, "failed to bulk load tokens");
        buffer_free(&load_sql);
    } else {
        insert_token_batches(session, tokens);
        mysql_exec_or_die(session, "COMMIT;\n", "failed to commit token insert batches");
    }

    if (token_load_path) {
        unlink(token_load_path);
        free(token_load_path);
    }
    short_cache_add_batch(short_cache, short_lexemes, short_count);
    free(short_lexemes);
    free(long_ids);
    return source_file_id;
}

static char *
reconstruct_source(const TokenArray *tokens, const LineInfo *line_info)
{
    Buffer output;
    size_t i;

    (void) line_info;
    buffer_init(&output);
    for (i = 0; i < tokens->len; i++) {
        buffer_append_str(&output, tokens->items[i].text);
    }
    return buffer_take(&output);
}

static char **
build_gcc_argv(const StringArray *include_dirs, const char *compile_path, size_t *argc_out)
{
    size_t i;
    size_t argc = 8 + include_dirs->len;
    char **argv = xcalloc(argc, sizeof(*argv));

    argv[0] = "gcc";
    argv[1] = "-fsyntax-only";
    argv[2] = "-std=gnu11";
    argv[3] = "-w";
    argv[4] = "-x";
    argv[5] = "c";

    for (i = 0; i < include_dirs->len; i++) {
        Buffer flag;
        buffer_init(&flag);
        buffer_append_str(&flag, "-I");
        buffer_append_str(&flag, include_dirs->items[i]);
        argv[6 + i] = buffer_take(&flag);
    }

    argv[6 + include_dirs->len] = (char *) compile_path;
    argv[7 + include_dirs->len] = NULL;
    if (argc_out) {
        *argc_out = 7 + include_dirs->len;
    }
    return argv;
}

static void
free_gcc_argv(char **argv, const StringArray *include_dirs)
{
    size_t i;
    for (i = 0; i < include_dirs->len; i++) {
        free(argv[6 + i]);
    }
    free(argv);
}

static bool
lint_reconstructed_source(const char *actual_path, const char *reconstructed, const StringArray *include_dirs, char **failure_output, char **reconstructed_path_out)
{
    char *reconstructed_path = NULL;
    char *wrapper_path = NULL;
    bool is_header = has_suffix(actual_path, ".h");
    char *compile_path;
    char **argv;
    Buffer output;
    int exit_code;
    size_t argc_unused;

    if (is_header) {
        if (failure_output) {
            *failure_output = NULL;
        }
        if (reconstructed_path_out) {
            *reconstructed_path_out = NULL;
        }
        return true;
    }

    reconstructed_path = make_temp_sibling_path(actual_path);
    write_text_file(reconstructed_path, reconstructed, strlen(reconstructed));

    compile_path = reconstructed_path;

    argv = build_gcc_argv(include_dirs, compile_path, &argc_unused);
    exit_code = run_command_capture(argv, NULL, NULL, NULL, &output);
    free_gcc_argv(argv, include_dirs);

    if (exit_code == 0) {
        unlink(reconstructed_path);
        if (wrapper_path) {
            unlink(wrapper_path);
        }
        free(wrapper_path);
        free(reconstructed_path);
        buffer_free(&output);
        if (failure_output) {
            *failure_output = NULL;
        }
        if (reconstructed_path_out) {
            *reconstructed_path_out = NULL;
        }
        return true;
    }

    if (failure_output) {
        *failure_output = buffer_take(&output);
    } else {
        buffer_free(&output);
    }
    if (reconstructed_path_out) {
        *reconstructed_path_out = reconstructed_path;
    } else {
        free(reconstructed_path);
    }

    if (wrapper_path) {
        unlink(wrapper_path);
        free(wrapper_path);
    }
    return false;
}

static void
print_usage(FILE *stream, const char *argv0)
{
    fprintf(stream,
        "Usage: %s [--repo-root PATH] [--db-source-root PATH] [--filter TEXT] [--limit N] [--jobs N] [--skip-existing] [--no-lint]\n"
        "  --jobs defaults to the detected CPU core count.\n",
        argv0);
}

static size_t
parse_size_arg(const char *value, const char *flag)
{
    char *end = NULL;
    unsigned long long parsed = strtoull(value, &end, 10);
    if (!value[0] || (end && *end != '\0')) {
        die("invalid numeric value for %s: %s", flag, value);
    }
    return (size_t) parsed;
}

static size_t
default_job_count(void)
{
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (cpus < 1) {
        return 1;
    }
    return (size_t) cpus;
}

static Options
parse_options(int argc, char **argv)
{
    Options opts;
    int i;
    char cwd[PATH_MAX];

    memset(&opts, 0, sizeof(opts));
    if (!getcwd(cwd, sizeof(cwd))) {
        die("getcwd failed: %s", strerror(errno));
    }
    opts.repo_root = xstrdup(cwd);
    opts.db_source_root = xstrdup(DEFAULT_DB_SOURCE_ROOT);
    opts.filter = NULL;
    opts.limit = 0;
    opts.jobs = default_job_count();
    opts.skip_existing = false;
    opts.no_lint = false;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--repo-root") == 0) {
            if (++i >= argc) {
                die("--repo-root requires a value");
            }
            free(opts.repo_root);
            opts.repo_root = canonicalize_path(argv[i]);
        } else if (strcmp(argv[i], "--db-source-root") == 0) {
            if (++i >= argc) {
                die("--db-source-root requires a value");
            }
            free(opts.db_source_root);
            opts.db_source_root = xstrdup(argv[i]);
        } else if (strcmp(argv[i], "--filter") == 0) {
            if (++i >= argc) {
                die("--filter requires a value");
            }
            opts.filter = argv[i];
        } else if (strcmp(argv[i], "--limit") == 0) {
            if (++i >= argc) {
                die("--limit requires a value");
            }
            opts.limit = parse_size_arg(argv[i], "--limit");
        } else if (strcmp(argv[i], "--jobs") == 0) {
            if (++i >= argc) {
                die("--jobs requires a value");
            }
            opts.jobs = parse_size_arg(argv[i], "--jobs");
            if (opts.jobs == 0) {
                die("--jobs must be at least 1");
            }
        } else if (strcmp(argv[i], "--skip-existing") == 0) {
            opts.skip_existing = true;
        } else if (strcmp(argv[i], "--no-lint") == 0) {
            opts.no_lint = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(stdout, argv[0]);
            exit(EXIT_SUCCESS);
        } else {
            print_usage(stderr, argv[0]);
            die("unknown option: %s", argv[i]);
        }
    }

    opts.php_src_root = path_join(opts.repo_root, "php-src-forensic");
    {
        char *canonical = canonicalize_path(opts.php_src_root);
        free(opts.php_src_root);
        opts.php_src_root = canonical;
    }
    opts.lint_php_src_root = path_join(opts.repo_root, "php-src");
    {
        char *canonical = canonicalize_path_or_keep(opts.lint_php_src_root);
        free(opts.lint_php_src_root);
        opts.lint_php_src_root = canonical;
    }
    return opts;
}

static void
free_options(Options *opts)
{
    free(opts->repo_root);
    free(opts->php_src_root);
    free(opts->lint_php_src_root);
    free(opts->db_source_root);
    opts->repo_root = NULL;
    opts->php_src_root = NULL;
    opts->lint_php_src_root = NULL;
    opts->db_source_root = NULL;
    opts->filter = NULL;
    opts->limit = 0;
    opts->jobs = 0;
    opts->skip_existing = false;
    opts->no_lint = false;
}

static void
prepared_file_free(PreparedFile *prepared)
{
    token_array_free(&prepared->tokens);
    line_info_free(&prepared->line_info);
    free(prepared->error);
    prepared->error = NULL;
    prepared->ready = false;
    prepared->failed = false;
}

static void *
lex_worker_main(void *arg)
{
    LexWorkQueue *queue = arg;

    for (;;) {
        size_t index;
        char *content;
        size_t size = 0;
        TokenArray tokens = {0};
        LineInfo line_info = {0};

        pthread_mutex_lock(&queue->mutex);
        while (!queue->stop && queue->next_index >= queue->file_count) {
            pthread_mutex_unlock(&queue->mutex);
            return NULL;
        }
        if (queue->stop) {
            pthread_mutex_unlock(&queue->mutex);
            return NULL;
        }
        index = queue->next_index++;
        pthread_mutex_unlock(&queue->mutex);

        content = read_file(queue->files[index], &size);
        lex_source_file(queue->files[index], content, size, &tokens, &line_info);
        free(content);

        pthread_mutex_lock(&queue->mutex);
        queue->prepared[index].tokens = tokens;
        queue->prepared[index].line_info = line_info;
        queue->prepared[index].ready = true;
        pthread_cond_broadcast(&queue->cond);
        pthread_mutex_unlock(&queue->mutex);
    }
}

static void
lex_work_queue_init(LexWorkQueue *queue, const Options *opts, char **files, size_t file_count, PreparedFile *prepared)
{
    memset(queue, 0, sizeof(*queue));
    queue->opts = opts;
    queue->files = files;
    queue->file_count = file_count;
    queue->prepared = prepared;
    if (pthread_mutex_init(&queue->mutex, NULL) != 0) {
        die("pthread_mutex_init failed");
    }
    if (pthread_cond_init(&queue->cond, NULL) != 0) {
        pthread_mutex_destroy(&queue->mutex);
        die("pthread_cond_init failed");
    }
}

static void
lex_work_queue_destroy(LexWorkQueue *queue)
{
    pthread_cond_destroy(&queue->cond);
    pthread_mutex_destroy(&queue->mutex);
}

int
main(int argc, char **argv)
{
    Options opts = parse_options(argc, argv);
    DbConfig cfg;
    MysqlSession session;
    ProgressState progress;
    StringArray source_files = {0};
    StringArray target_files = {0};
    StringArray configured_c_sources = {0};
    ShortLexemeCache short_cache = {0};
    PreparedFile *prepared = NULL;
    pthread_t *workers = NULL;
    LexWorkQueue lex_queue;
    char *env_path = NULL;
    char *schema_path = NULL;
    char *makefile_objects_path = NULL;
    char *database_url = NULL;
    int exit_code = EXIT_SUCCESS;
    size_t imported = 0;
    size_t target_total = 0;
    size_t i;
    size_t worker_count = 0;

    env_path = path_join(opts.repo_root, ".env");
    schema_path = path_join(opts.repo_root, "ast.sql");
    makefile_objects_path = path_join(opts.lint_php_src_root, "Makefile.objects");
    database_url = read_env_value(env_path, "DATABASE_URL");
    parse_database_url(database_url, &cfg);
    mysql_session_start(&session, &cfg, NULL);

    if (!opts.no_lint) {
        preflight_php_src_configured(&opts);
    }
    session.local_infile_enabled = mysql_local_infile_enabled(&session);
    session.auto_increment_increment = mysql_auto_increment_increment(&session);
    session.max_allowed_packet = mysql_max_allowed_packet(&session);
    reset_database_from_schema(&session, &cfg, schema_path);
    if (!opts.no_lint) {
        load_configured_c_sources(makefile_objects_path, &configured_c_sources);
    }

    collect_source_files_recursive(opts.php_src_root, &source_files);
    if (source_files.len == 0) {
        die("no .c or .h files found under %s", opts.php_src_root);
    }
    qsort(source_files.items, source_files.len, sizeof(*source_files.items), cmp_owned_string);

    for (i = 0; i < source_files.len; i++) {
        if (opts.filter && strstr(source_files.items[i], opts.filter) == NULL) {
            continue;
        }
        string_array_push_copy(&target_files, source_files.items[i]);
        if (opts.limit != 0 && target_files.len >= opts.limit) {
            break;
        }
    }

    target_total = target_files.len;
    if (target_total == 0) {
        die("no matching .c or .h files found under %s", opts.php_src_root);
    }
    progress_init(&progress, target_total);
    prepared = xcalloc(target_total, sizeof(*prepared));
    worker_count = opts.jobs;
    if (worker_count > target_total) {
        worker_count = target_total;
    }
    if (worker_count == 0) {
        worker_count = 1;
    }
    workers = xcalloc(worker_count, sizeof(*workers));
    lex_work_queue_init(&lex_queue, &opts, target_files.items, target_total, prepared);
    for (i = 0; i < worker_count; i++) {
        if (pthread_create(&workers[i], NULL, lex_worker_main, &lex_queue) != 0) {
            die("pthread_create failed");
        }
    }

    for (i = 0; i < target_total; i++) {
        char *actual_path = target_files.items[i];
        const char *display_path = path_relative_or_self(actual_path, opts.php_src_root);
        char *lint_actual_path = NULL;
        char *db_path = NULL;
        char *reconstructed = NULL;
        char *lint_error = NULL;
        char *lint_path = NULL;
        char *missing_local_include = NULL;
        StringArray lint_include_dirs = {0};
        long long source_file_id = 0;
        TokenArray *tokens;
        LineInfo *line_info;
        bool lint_skipped = false;

        progress_render(&progress, "lex", display_path);
        pthread_mutex_lock(&lex_queue.mutex);
        while (!prepared[i].ready) {
            pthread_cond_wait(&lex_queue.cond, &lex_queue.mutex);
        }
        pthread_mutex_unlock(&lex_queue.mutex);

        if (prepared[i].failed) {
            ui_clear_live_line();
            fprintf(stderr, "lex failed for %s\n%s\n", actual_path, prepared[i].error ? prepared[i].error : "unknown error");
            exit_code = EXIT_FAILURE;
            break;
        }

        tokens = &prepared[i].tokens;
        line_info = &prepared[i].line_info;
        db_path = normalized_db_path(actual_path, opts.php_src_root, opts.db_source_root);
        lint_actual_path = map_path_between_roots(actual_path, opts.php_src_root, opts.lint_php_src_root);
        if (opts.skip_existing && db_path_exists(&session, db_path)) {
            progress.processed++;
            progress.skipped_existing++;
            progress_render(&progress, "skip", display_path);
            free(lint_actual_path);
            free(db_path);
            db_path = NULL;
            prepared_file_free(&prepared[i]);
            continue;
        }

        assign_long_lexeme_ids(&session, tokens);
        progress_render(&progress, "db", display_path);
        source_file_id = import_tokenized_file(&session, &short_cache, db_path, tokens, !opts.skip_existing);

        reconstructed = reconstruct_source(tokens, line_info);
        if (opts.no_lint) {
            if (has_suffix(actual_path, ".c")) {
                lint_skipped = true;
            }
        } else if (!lint_should_skip_for_host(lint_actual_path, &configured_c_sources)) {
            build_lint_include_dirs(&opts, lint_actual_path, &lint_include_dirs);
            missing_local_include = find_first_missing_local_include(lint_actual_path, &lint_include_dirs);
            if (missing_local_include) {
                lint_skipped = true;
            } else {
                progress_render(&progress, "lint", display_path);
                if (!lint_reconstructed_source(lint_actual_path, reconstructed, &lint_include_dirs, &lint_error, &lint_path)) {
                    ui_clear_live_line();
                    fprintf(stderr, "lint failed for %s\n", db_path);
                    fprintf(stderr, "source_file.id=%lld\n", source_file_id);
                    if (lint_path) {
                        fprintf(stderr, "reconstructed_path=%s\n", lint_path);
                    }
                    if (lint_error) {
                        fprintf(stderr, "%s", lint_error);
                        if (lint_error[0] && lint_error[strlen(lint_error) - 1] != '\n') {
                            fputc('\n', stderr);
                        }
                    }
                    exit_code = EXIT_FAILURE;
                }
            }
        } else if (has_suffix(actual_path, ".c")) {
            lint_skipped = true;
        }

        if (exit_code == EXIT_SUCCESS) {
            progress.processed++;
            progress.imported++;
            if (lint_skipped) {
                progress.lint_skipped++;
            }
            progress_render(&progress, "ok", display_path);
            imported++;
        }

        free(db_path);
        free(lint_actual_path);
        free(reconstructed);
        free(lint_error);
        free(lint_path);
        free(missing_local_include);
        string_array_free(&lint_include_dirs);
        prepared_file_free(&prepared[i]);

        if (exit_code != EXIT_SUCCESS) {
            break;
        }
    }

    pthread_mutex_lock(&lex_queue.mutex);
    lex_queue.stop = true;
    pthread_cond_broadcast(&lex_queue.cond);
    pthread_mutex_unlock(&lex_queue.mutex);
    for (i = 0; i < worker_count; i++) {
        pthread_join(workers[i], NULL);
    }
    lex_work_queue_destroy(&lex_queue);

    if (exit_code == EXIT_SUCCESS && opts.limit != 0 && imported >= opts.limit && progress.processed < progress.total) {
        progress.total = progress.processed;
    }
    if (exit_code == EXIT_SUCCESS) {
        progress_finish(&progress, "done");
    }

    free(database_url);
    free(env_path);
    free(schema_path);
    free(makefile_objects_path);
    string_array_free(&source_files);
    string_array_free(&target_files);
    string_array_free(&configured_c_sources);
    short_cache_free(&short_cache);
    if (prepared) {
        for (i = 0; i < target_total; i++) {
            prepared_file_free(&prepared[i]);
        }
    }
    free(prepared);
    free(workers);
    mysql_session_close(&session);
    db_config_free(&cfg);
    free_options(&opts);
    return exit_code;
}
