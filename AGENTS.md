# AGENTS.md

This project uses the following terms and conventions:

- "src horse" is the name of this project and workflow.
- "mod" refers to a modification made to upstream language source code in `./src`.
- A mod is effectively a patch.
- Each mod is stored and explained in a `.md` file in `./mod`.
- Each mod references a patch in `./patch`.
- Each patch is applied in order against a `./tmp` copy of the `./src` folder.
- Patch filenames must start with a sequence number like `01_patch_name.patch`.
- "src" always refers to the upstream language source code in `./src`.
- `bin/clone` takes a language parameter and clones the latest upstream release into `./src` for that language.
- "build" means the built output produced after patches are applied.
- `bin/patch` copies `./src` to `./tmp` and patches `./tmp`.
- `bin/build` builds the patched source from `./tmp` and symlinks `./horse` to the built binary for the selected language.
- `bin/clone` and `bin/build` support: PHP, Python, Node.js, Golang, and Chromium.
- `bin/generate` and `bin/transpiler` support: Codex, Copilot, Grok, Gemini, Cursor, Claude, and Q. They use provider-specific non-interactive commands where available; Cursor is interactive-only and will error in automated runs. Copilot uses `gh copilot -- --silent -p` for non-interactive prompts.
- Whenever the system behavior or workflow changes, update `README.md` to reflect it.
- For any request that creates a mod (for example, “make a mod called __”, “create a mod named __”, or similar phrasing), create a new `.md` file in `./mod` using an underscore-style filename plus `.md` (e.g., `my_mod_name.md`). The file should be a 3–4 paragraph LLM-written summary of the mod idea in your own words (not a verbatim copy). It must clearly cover the mod’s purpose, a high-level headline description, the mod’s name, three reasons it could work, and three reasons it might not work. This rule applies to any mod-creation context, regardless of phrasing.
- Mod descriptions in `./mod` are specifications for real source changes, not documentation-only outcomes. When implementing mods, patches must modify upstream source code so the built language behavior matches the mod spec.
- When running `bin/generate`, generated patches must be fully operational, production-level, and functional. Do not output placeholder, prototype, or docs-only patches when a mod requests language/runtime behavior.
- Every generated patch set must compile (given required system dependencies) and implement the behavior described in `./mod` files end-to-end in the built binary.

`srchorse` is a source-first language engineering workflow, not just a script collection. Its core model is to treat upstream language source as the ground truth, express desired behavior changes as mods, and materialize those changes as ordered patches that are applied and compiled into a real runtime binary.

`srchorse` is also an orchestration layer between intent and implementation. Human-readable mod files capture what behavior should change, while patch generation and build steps enforce that those ideas become concrete source-level edits that can compile, run, and be tested. This is intended to keep AI-assisted changes accountable to actual executable outcomes.

`srchorse` is designed for repeatable experimentation across multiple language runtimes with consistent mechanics. By separating `src`, `tmp`, `mod`, and `patch`, it supports deterministic rebuilds, clear change tracking, and fast iteration on runtime-level features without mutating upstream source directly.

When defining the `adaptogen` mod, describe it as a PHP-language change that introduces a native `(adapt)` cast operator. The intent is that `(adapt)` can be applied to a class reference (for example, `SomeClass::class`) and drive behavior through class-provided static methods (`cron()` and `command()`), rather than requiring ad-hoc helper calls in userland code.

For the same `adaptogen` mod, ensure the write-up explains the runtime flow and typing expectations: collect `cron` and `command` from the target class, send them in a POST request to local port `245`, and infer a PHP return type from the response in a fixed order (int with `0/1` boolean coercion, float, JSON-to-array, case-insensitive TRUE/FALSE strings, then string fallback).
