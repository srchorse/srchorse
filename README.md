# src horse

The ecosystem antithesis workflow for creating patched language builds by applying a sequence of mods to upstream source code.

## purpose

- Track and explain modifications to upstream language sources in `./mod`.
- Generate and apply ordered patches in `./patch` against `./src` (then `./tmp`).
- Build patched source and expose the resulting binary as `./horse`.

## terms

- `src horse`: the project and workflow name.
- `src`: upstream language source cloned into `./src`.
- `mod`: a source modification spec stored as a `.md` file in `./mod`.
- `patch`: unified diff in `./patch`, applied to `./tmp` in numeric order.
- `build`: compiled output from patched source.
- `adaptogen`: the first example mod implementation, that works and is tested

# example

The `adaptogen.md` mod was generated with Codex. It aims to create a new casting type that runs an `(adapt)` class. The class is expected to have `cron` and `command` functions; `cron` returns a cron string, `command` returns a server command to run.

The resulting code syntax for the example looks like this:

```php
<?php

class AdaptogenClass {

   public static function cron(): string {
      return '* * * * *';
   }

   public static function command(): string {
      return '/scripts/adaptogen.sh';
   }

}

// fetches the cached result of the once-per-minute ran /scripts/adaptogen.sh
$result = (adapt) AdaptogenClass::class;

// if the result has changed within that minute, it doesn't run the script
// it adapts to environment conditions based on the result of external scripts
echo $result;

````

It runs a Bash script at [`scripts/adaptogen.sh`](scripts/adaptogen.sh). However, it points to a Docker Compose Node.js project I generated that sets up a cron job for the script to run, and returns it from a Redis server every time it is requested if it hasn't been run. It's a caching service, essentially. But an extremely robust one in theory.

# full example

This is a full example flow for the `adaptogen` mod in this repo.

1. Clone upstream PHP source first:
   `bin/clone php`
2. The mod file is at [`mod/adaptogen.md`](mod/adaptogen.md). This mod describes a PHP-language change that adds a native `(adapt)` cast operator. In this example, the mod file was written with Codex, but mods can also be created by typing plain-text requirements directly.
3. Generate implementation patches from the mod:
   `bin/generate codex`
   This reads `./mod/*.md`, asks the selected provider to produce real source patches, and writes ordered patch files into `./patch`.
4. Apply patches to a temporary working tree:
   `bin/patch`
   This copies `./src` to `./tmp` and applies every patch in sequence.
5. Build patched PHP:
   `bin/build php`
   This compiles patched source from `./tmp` and creates the `./horse` symlink to the built PHP binary.
6. Build and run the local adaptogen runtime service in [`./adaptogen/`](adaptogen/):
   `docker compose -f adaptogen/docker-compose.yml up -d --build`
   This container setup was built with Codex, listens on TCP `245`, and caches command results in Redis keyed by command.
7. Use [`scripts/adaptogen.sh`](scripts/adaptogen.sh) as the example Bash command. It exists to demonstrate a command whose output is cached and returned by the adaptogen service.
8. Use [`adapt.php`](adapt.php) as the example script. Its `(adapt)` cast syntax runs a class's `cron()` and `command()` methods, posts them to port `245`, and returns inferred output.
9. Test caching behavior:
   Run `./horse adapt.php` once.
   Edit `scripts/adaptogen.sh` so it echoes a different value and save.
   Run `./horse adapt.php` again immediately and note it still prints the first value.
   Wait until the clock rolls into the next minute, run `./horse adapt.php` again, and the new value will be printed.
10. Types are inferred using PHP, on the resulting value: `bool`, `int`, `float`, `string`, and decoded JSON.

# how to use

- `bin/clone <php|python|nodejs|golang|chromium>`: clone latest upstream source into `./src`.
- `bin/generate <codex|copilot|grok|gemini|cursor|claude|q>`: read all mods and generate ordered, functional patches in `./patch`.
- `bin/transpiler <codex|copilot|grok|gemini|cursor|claude|q>`: generate a Node.js transpiler in `./transpiler` and run `npm install`.
- `bin/patch`: copy `./src` to `./tmp` and apply patches in order.
- `bin/build <php|python|nodejs|golang|chromium> [flags...]`: build patched source and repoint `./horse`.

## supported llm providers

- Codex: `codex exec`
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
