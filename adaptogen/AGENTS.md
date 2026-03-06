# AGENTS.md

## Project Overview

This repository contains a Docker Compose project with two services:

- `redis`: Redis 7 (`redis:7-alpine`)
- `app`: Node.js 20 HTTP API server that exposes port `245`

Primary goal: accept requests that define a `cron` schedule + `command` + `key` (+ optional `keys`), execute commands, and cache command results in Redis.

## Current File Layout

- `docker-compose.yml`: Defines `redis` and `app` services, maps `245:245`, and sets `REDIS_URL`.
- `Dockerfile`: Builds Node app image (`node:20-alpine`) and runs `npm start`.
- `package.json`: App metadata and dependencies (`express`, `cron`, `redis`).
- `server.js`: Main API and scheduling/caching logic.
- `README.md`: Basic run instructions and payload example.
- `.dockerignore`: Build context exclusions.

## Runtime Behavior (Implemented)

### Endpoint

- `POST /`
- Body (JSON or URL-encoded form data):
  - `cron` (string, required)
  - `command` (string, required)
  - `key` (string, required; Redis key provided by PHP)
  - `keys` (optional; string or array, used as command args)

### Logic

1. Read Redis key directly from the incoming `key` field.
2. Check in-memory cron registry (`Map`) for scheduled job by that key.
3. If no scheduled job exists:
   - Register/start cron job using provided `cron` expression.
   - Cron job executes command and stores result JSON in Redis at the key.
4. If Redis already has the key:
   - Return the cached command output as plain text.
5. If Redis key is missing:
   - Execute command immediately with `keys` as shell-escaped args.
   - Store result JSON in Redis.
   - Return the live command output as plain text.

### Extra Endpoint

- `GET /health`: pings Redis and returns health status.

## Command Execution Details

- Uses Node `child_process.exec` (promisified).
- `keys` are normalized to array form.
- Args are shell-escaped before concatenation.
- Result object cached/returned includes:
  - `ok`, `command`, `keys`, `stdout`, `stderr`, `exitCode`, `ranAt`.

## Operational Notes

- Scheduled jobs are stored in memory only; they are lost on container restart.
- Cache key is provided by PHP rather than derived inside Node.
- First request for a command both schedules cron and may execute immediately if cache miss.
- App listens on `0.0.0.0:245`.

## Run / Verify

Start:

```bash
docker compose up --build
```

Example request:

```bash
curl -s -X POST http://localhost:245/ \
  -H 'content-type: application/x-www-form-urlencoded' \
  --data-urlencode 'cron=*/15 * * * * *' \
  --data-urlencode 'command=echo' \
  --data-urlencode 'key=php-cache-key' \
  --data-urlencode 'keys[]=hello'
```

Observed behavior from verification:

- First request returned the command output body directly.
- Second identical request returned the cached command output body directly.

## Suggested Next Work (If Needed)

- Add tests for request validation, scheduler behavior, and cache hits/misses.
- Decide what exact PHP-side hashing/normalization contract should remain stable over time.
- Add auth/rate limiting before exposing beyond local/internal use.
