# src horse

src horse imports the `php/php-src` C and header source tree into MySQL as a tokenized corpus, then rebuilds and validates that corpus from the database.

## Prerequisites

Ensure you have MySQL available and the MySQL client development dependencies installed before building or running the importer.

On Debian or Ubuntu, the minimum useful packages are typically:

```bash
sudo apt-get install -y mysql-server mysql-client default-libmysqlclient-dev pkg-config
```

`default-libmysqlclient-dev` is required because `src/import` links against the native MySQL client library.

## Configure `.env`

Create `.env` in the repo root and set `DATABASE_URL`:

```env
DATABASE_URL=mysql://username:password@localhost:3306/ast
```

An example is included in [`.env.example`](./.env.example).

## Increase MySQL `max_allowed_packet`

The importer can insert very large token batches. Set the global packet limit high enough before running imports:

```sql
SET GLOBAL max_allowed_packet = 268435456;
```

That sets the value to `256M`.

To verify:

```sql
SHOW GLOBAL VARIABLES LIKE 'max_allowed_packet';
```

If you want it to persist across MySQL restarts, add this to your MySQL config:

```ini
[mysqld]
max_allowed_packet=256M
```

## Clone php-src

Pull `php/php-src` into `./php-src` and create a clean forensic copy at `./php-src-forensic`:

```bash
bin/source-clone
```

`bin/source-clone` uses the detected CPU core count for Git's `--jobs` setting.

This repo treats:

- `php-src-forensic` as the clean source-of-truth tree used for import
- `php-src` as the working tree used for dump, configure, and build steps

Both source trees are tracked as git submodules that point at `php/php-src`.

## Reset php-src from the forensic copy

Run:

```bash
bin/source-reset
```

`bin/source-reset` deletes `./php-src` and recreates it from `./php-src-forensic`.

Use this when `./php-src` has been modified by `bin/source-dump`, local edits, or failed build experiments and you need to restore the working tree from the clean forensic copy before building again.

## Import into MySQL

Run:

```bash
bin/db-import
```

By default, `bin/db-import` uses as many worker jobs as there are detected CPU cores. You can override that with `--jobs N`.

What `bin/db-import` does:

- Reads `DATABASE_URL` from `.env`
- Drops the target database and recreates it from [`ast.sql`](./ast.sql)
- Recursively scans `./php-src-forensic` for `.c` and `.h` files
- Lexes each file and imports the token stream into the project schema
- Stores every source piece needed for exact reconstruction, including whitespace, comments, preprocessor markers, and line splices
- Stores `source_file`, `lexeme`, `lexeme_long`, `lexeme_long_source_file`, and `token`
- Reconstructs each file from the imported tokens
- Syntax-checks reconstructed host `.c` files unless linting is disabled

By default, lint uses the configured `./php-src` tree for include paths and generated headers, but the imported source content comes from `./php-src-forensic`.

Why this exists:

- It turns `php-src` into a queryable database representation
- It preserves token order and source locations so files can be reconstructed later
- It gives you a reproducible imported corpus for later analysis, queries, and build work

If you want a database snapshot after import, run [`bin/db-snapshot`](./bin/db-snapshot) explicitly.

Useful option:

```bash
bin/db-import --no-lint
```

That still imports everything, but skips the syntax-lint phase if you want the fastest possible import pass.

Importer usage:

```text
bin/db-import [--repo-root PATH] [--db-source-root PATH] [--filter TEXT] [--limit N] [--jobs N] [--skip-existing] [--no-lint]
```

## Restore the database from the forensic snapshot

Run:

```bash
bin/db-restore
```

`bin/db-restore` drops the configured database, recreates it, and loads `./sql/forensic.sql`.

Use this when you want the fastest path back to the last full imported corpus without re-tokenizing `./php-src-forensic`.

## Export the forensic database snapshot

Run:

```bash
bin/db-snapshot
```

`bin/db-snapshot` exports the current database to `./sql/forensic.sql` with `mysqldump --hex-blob`, so binary lexeme data stays round-trippable.

## Rewrite imported tokens

Run:

```bash
bin/db-change SOURCE TARGET
```

`bin/db-change` reads `DATABASE_URL` from `.env`, connects to MySQL, finds token rows matching `SOURCE`, and rewrites them to `TARGET`.

It performs substring replacement inside stored token text across the imported corpus. That means a change like `coolness -> coolness` also updates tokens such as `zif_coolness`, `arginfo_coolness`, and `"coolness"` when they are present in imported files.

It handles both storage forms used by the corpus:

- short tokens stored in `lexeme` / `token.lexeme_id`
- long tokens stored in `lexeme_long` / `token.lexeme_long_id`

For each affected token, if the replaced value is short enough to fit in `lexeme`, it rewrites that row to `lexeme_id`. If the replaced value is longer than `255` bytes, it appends a new `lexeme_long` row and rewrites that token to the new `lexeme_long_id`.

Important limitation:

- `bin/db-change` only affects files that were imported into MySQL
- `bin/db-import` currently imports `.c` and `.h` files
- source-of-truth files outside that corpus, such as `*.stub.php`, are not changed by `bin/db-change`

The binary is built from [`src/change`](./src/change) and is exposed through [`db-change`](./bin/db-change).

## Recursive filesystem string replacement

Run:

```bash
bin/str FIND REPLACE
```

`bin/str` recursively renames paths and replaces UTF-8 text content under the current working directory.

This is useful as a companion to `bin/db-change` when you need to update authored files that are not currently imported into MySQL, such as `*.stub.php`.

Important behavior:

- it operates on the current working directory, not automatically on `./php-src` or `./php-src-forensic`
- it skips binary files and non-UTF-8 files
- it may rename filenames and directories that contain the search text

Pragmatic workflow for a broad rename:

```bash
bin/db-change OLD NEW
(cd php-src-forensic && ../bin/str OLD NEW)
```

## Query lexeme usage

Run:

```bash
bin/db-lexeme LEXEME
```

`bin/db-lexeme` reads `DATABASE_URL` from `.env`, queries the imported token corpus for the provided lexeme, and prints:

- the total number of matching `token` rows in the database
- the number of distinct `source_file` rows where that lexeme appears

It matches both storage forms used by the corpus:

- short tokens stored in `lexeme` / `token.lexeme_id`
- long tokens stored in `lexeme_long` / `token.lexeme_long_id`

The binary is built from [`src/lexeme`](./src/lexeme) and is exposed through [`db-lexeme`](./bin/db-lexeme).

## Dump database source back to php-src

Run:

```bash
bin/source-dump
```

`bin/source-dump` reads `DATABASE_URL` from `.env`, queries every `source_file` and its ordered `token` rows from MySQL, reconstructs each file, and overwrites the corresponding file under `./php-src`.

Behavior:

- it processes the full imported corpus from the database
- it maps stored database paths from `/nfs/php-src/...` back into the local `./php-src/...` tree
- it reconstructs files directly from the imported token-piece stream in MySQL
- exact reconstruction depends on using a database imported by the current importer, because older imports discarded some characters
- it fails immediately if a stored database path does not map cleanly under `./php-src`
- it shows a single-line colored progress bar while running

This is a destructive filesystem operation for the local `./php-src` checkout. `./php-src-forensic` should remain untouched so imports always have a clean source tree. If you need to restore the working tree after a dump, use `bin/source-reset`.

The binary is built from [`src/dump`](./src/dump) and is exposed through [`source-dump`](./bin/source-dump).

## Configure php-src

Run:

```bash
bin/php-configure
```

`bin/php-configure` is a repo-local wrapper around `php-src/./configure`.

What it does:

- Uses a hardcoded module list in [`bin/php-configure`](./bin/php-configure)
- Always enables ZTS with `--enable-zts`
- Exports `MAKEFLAGS` so downstream make-based steps use the detected CPU core count
- Checks required system dependencies before running `./configure`
- Runs against `./php-src`

If you want a different PHP build, edit [`bin/php-configure`](./bin/php-configure) directly. The script is intentionally just a Bash wrapper with a hardcoded module set and flag mapping, so it can be adapted to any configure string you want.

To inspect the current generated configure command without running it:

```bash
bin/php-configure --dry-run
```

The current default hardcoded configure flags are:

```bash
./configure \
  --enable-zts \
  --enable-bcmath \
  --with-bz2 \
  --enable-cli \
  --with-curl \
  --enable-fpm \
  --enable-gd \
  --enable-intl \
  --enable-mbstring \
  --enable-pdo \
  --enable-mysqlnd \
  --with-mysqli \
  --with-pdo-mysql \
  --with-pgsql \
  --with-pdo-pgsql \
  --with-readline \
  --enable-soap \
  --with-sqlite3 \
  --with-pdo-sqlite \
  --with-tidy \
  --enable-xml \
  --enable-dom \
  --enable-simplexml \
  --enable-xmlreader \
  --enable-xmlwriter \
  --with-xsl \
  --with-zip
```

## Full php-src rebuild

Run:

```bash
bin/php-build
```

`bin/php-build` is a full reset and then a full build inside `./php-src`. It performs:

1. `make distclean`
2. `./buildconf --force`
3. `bin/php-configure`
4. `make -j$(cpu core count)`

Use this when you want to rebuild the php-src checkout from a clean state using the configure policy defined in this repo.

## Build repo-local C tools

Run:

```bash
bin/tools-build
```

`bin/tools-build` builds everything under [`src`](./src).

## Building repo-local tooling

If you change anything under `./src/**/*`, rebuild the repo-local binaries with:

```bash
src/make
```

`src/make` uses the detected CPU core count for each sub-build. Right now that builds the repo-local tools in `src/import`, `src/change`, `src/lexeme`, and `src/dump`, and outputs them to `./bin/`.
