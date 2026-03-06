# src horse

The ecosystem antithesis workflow for creating patched language builds by applying a sequence of mods to upstream source code.

## purpose

- Track and explain modifications to upstream language sources in `./mod`.
- Generate ordered patches in `./patch` from mods, then apply them during build against `./tmp`.
- Build patched source and expose the resulting binary as `./horse`.

## terms

- `src horse`: the project and workflow name.
- `src`: upstream language source cloned into `./src`.
- `mod`: a source modification spec stored as a `.md` file in `./mod`.
- `patch`: unified diff in `./patch`, generated from mods and applied to `./tmp` in numeric order during build.
- `build`: compiled output from patched source.
- `adaptogen`: the first example mod implementation, that works and is tested

# example

The `adaptogen.md` mod was generated with Codex. It aims to create a new casting type that runs an `(adapt)` class or instantiated adaptogen object. The object is expected to expose `cron()` and `command()` methods, and it can also expose `key()` so PHP can compute a distinct Redis key before sending the request to port `245`.

The resulting code syntax for the example looks like this:

```php
<?php

class AdaptogenClass {

   private int $param;

   public function __construct(int $param) {
      $this->param = $param;
   }

   public function cron(): string {

      // every "$this->param" minutes
      return sprintf('*/%d * * * *', $this->param);
   }

   public function command(): string {

      // params passed into __construct available here
      return '/scripts/adaptogen.sh';
   }

   public function key(): string {

      // cache key
      return (string) $this->param;
   }

}

// these are different adapted objects, so they produce different PHP-side cache keys
// arbitrary arguments passed into AdaptogenClass, could be anything
// in this case, the cron function uses it to determine interval length
$result1 = (adapt) new AdaptogenClass(1);
$result2 = (adapt) new AdaptogenClass(2);

echo $result1 . "\n";
echo $result2 . "\n";

// before script change:
// working
// working

// after script change after one minute:
// working2
// working

// after script change after two minutes:
// working2
// working2
```

It runs a Bash script at [`scripts/adaptogen.sh`](scripts/adaptogen.sh). However, it points to a Docker Compose Node.js project I generated that sets up a cron job for the script to run, trusts the Redis key computed by PHP, and returns the cached or live command output from Redis every time it is requested. It's a caching service, essentially. But an extremely robust one in theory.

# full example

This is a full example flow for the `adaptogen` mod in this repo.

1. Clone upstream PHP source first:
   `bin/clone php`
2. The mod file is at [`mod/adaptogen.md`](mod/adaptogen.md). This mod describes a PHP-language change that adds a native `(adapt)` cast operator. In this example, the mod file was written with Codex, but mods can also be created by typing plain-text requirements directly.
3. Generate implementation patches from the mod:
   `bin/patch codex`
   With `codex`, this copies `./src` into a temporary worktree, removes inherited upstream git metadata, initializes a fresh throwaway git repo, runs Codex directly against that temp tree without nested sandboxing, pins the generation model to `gpt-5.4` with `model_reasoning_effort="high"`, converts those committed file changes into ordered patch files, validates that each generated patch applies cleanly to `./src` with `git apply --recount`, and only then replaces the contents of `./patch`. Other providers still generate raw patch text, which is also validated before being kept.
4. Build patched PHP:
   `bin/build php`
   This copies `./src` to `./tmp`, applies every patch in sequence, recreates the language-specific build directory so stale dependency files do not survive across regenerated `./tmp` trees, compiles patched source from `./tmp`, and creates the `./horse` symlink to the built PHP binary. The current adaptogen runtime patch also stays on exported Zend APIs in `zend_builtin_functions.c`, avoiding private or unavailable symbols during link.
5. Build and run the local adaptogen runtime service in [`./adaptogen/`](adaptogen/):
   `docker compose -f adaptogen/docker-compose.yml up -d --build`
   This container setup was built with Codex, listens on TCP `245`, accepts the form-encoded POST body emitted by the PHP runtime, returns the cached or live command output as plain text, and caches command results in Redis using the key computed and sent by PHP.
6. Use [`scripts/adaptogen.sh`](scripts/adaptogen.sh) as the example Bash command. It exists to demonstrate a command whose output is cached and returned by the adaptogen service.
7. Use [`adapt.php`](adapt.php) as the class-string example and [`adapt_serialize.php`](adapt_serialize.php) as the instantiated-object example. The `(adapt)` cast runs an adaptogen target's `cron()` and `command()` methods, computes the final cache key in PHP, posts `cron`, `command`, and `key` to port `245`, and returns inferred output.
   For parameterized object examples like [`adapt_serialize.php`](adapt_serialize.php), declare real typed properties instead of relying on deprecated dynamic properties, and inspect results with `var_dump()` when your command may legitimately produce JSON that adapts into an array.
8. Test caching behavior:
   Run `./horse adapt.php` once.
   Edit `scripts/adaptogen.sh` so it echoes a different value and save.
   Run `./horse adapt.php` again immediately and note it still prints the first value.
   Wait until the clock rolls into the next minute, run `./horse adapt.php` again, and the new value will be printed.
9. Types are inferred using PHP on the plain-text response body in this order: integer first with `0` and `1` coerced to booleans, then float, then decoded JSON arrays, then case-insensitive `TRUE` or `FALSE`, then string fallback.

## adaptogen behavior

- `(adapt)` supports both `SomeClass::class` and `new SomeClass(...)`.
- Instantiated adaptogen objects can preserve explicit constructor arguments and also receive autowired typed services.
- If a class exposes `key()`, PHP uses it as the object-specific cache discriminator before hashing/finalizing the Redis key sent to port `245`.
- `(adapt) new AdaptogenClass(1)` and `(adapt) new AdaptogenClass(2)` must not share a cached Redis value.
- The Node service in [`adaptogen/server.js`](adaptogen/server.js) accepts JSON and PHP form-encoded POST bodies, but returns the cached or live command output as plain text so PHP can run the mod’s fixed inference order on the response body.

# how to use

- `bin/clone <php|python|nodejs|golang|chromium>`: clone latest upstream source into `./src`.
- `bin/patch <codex|copilot|grok|gemini|cursor|claude|q>`: read all mods and generate ordered, functional patches in `./patch`.
- `bin/transpiler <codex|copilot|grok|gemini|cursor|claude|q>`: generate a Node.js transpiler in `./transpiler` and run `npm install`.
- `bin/build <php|python|nodejs|golang|chromium> [flags...]`: copy `./src` to `./tmp`, apply patches in order with `git apply --recount`, reset the language build directory, build patched source, and repoint `./horse`.

## supported llm providers

- Codex: `codex exec --cd <repo> -m gpt-5.4 -c 'model_reasoning_effort="high"' --dangerously-bypass-approvals-and-sandbox`
- Gemini: `gemini -p`
- Claude: `claude -p`
- Grok: `grok-one-shot --prompt`
- Q: `q chat --no-interactive --trust-all-tools`
- Copilot: `gh copilot -- --silent -p`
- Cursor: interactive only; automated runs are not supported.

## supported languages and build behavior

- PHP: runs `buildconf`/`configure`, then `make` and `make install` in `./build/php`.
- Python: runs `configure`, `make`, `make install` in `./build/python`.
- Node.js: runs `configure`, `make`, `make install` in `./build/nodejs`.
- Golang: runs `make.bash` in `./tmp/src` and copies binaries into `./build/golang/install/bin`.
- Chromium: runs `gn gen` then `ninja` in `./build/chromium/out`.

## folder layout

- `adaptogen/`: example src horse adaptogen mod supplementary implementation
- `scripts/`: adaptogen scripts collection
- `bin/`: helper commands
- `mod/`: mod files, explanatory documents explaining wanted changes
- `patch/`: generated patches from mod files
- `src/`: upstream source folder that gets cloned from running `bin/clone <language>`
- `tmp/`: patched temporary source tree
- `build/`: build outputs per language
- `transpiler/`: generated Node.js transpiler (experimental)

## current workflow

The current repo flow is now a two-step loop:

1. Run `bin/patch <provider>` to generate or regenerate the patch set from the mod files.
2. Run `bin/build <language>` to create `./tmp`, apply the generated patches, and compile the patched runtime.

There is no separate patch-application script anymore. Patch generation lives in [`bin/patch`](bin/patch), and patch application now happens inside [`bin/build`](bin/build).
