# AGENTS.md

## Project Overview

This repository contains a Docker Compose project with two services:

- `redis`: Redis 7 (`redis:7-alpine`)
- `app`: Node.js 20 HTTP API server that exposes port `245`

Primary goal: accept requests that define a `cron` schedule + `command` (+ optional `keys`), execute commands, and cache command results in Redis.

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
- Body (JSON):
  - `cron` (string, required)
  - `command` (string, required)
  - `keys` (optional; string or array, used as command args)

### Logic

1. Compute Redis key as `sha256(command)`.
2. Check in-memory cron registry (`Map`) for scheduled job by that key.
3. If no scheduled job exists:
   - Register/start cron job using provided `cron` expression.
   - Cron job executes command and stores result JSON in Redis at the key.
4. If Redis already has the key:
   - Return cached payload (`source: "redis"`).
5. If Redis key is missing:
   - Execute command immediately with `keys` as shell-escaped args.
   - Store result JSON in Redis.
   - Return fresh payload (`source: "live"`).

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
- Cache key is based only on `command` (not `keys` or `cron`) per original requirements.
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
  -H 'content-type: application/json' \
  -d '{"cron":"*/15 * * * * *","command":"echo","keys":["hello"]}'
```

Observed behavior from verification:

- First request returned `source: "live"`.
- Second identical request returned `source: "redis"`.

## Suggested Next Work (If Needed)

- Add tests for request validation, scheduler behavior, and cache hits/misses.
- Decide whether keying should include `keys` and/or `cron` for stronger isolation.
- Add auth/rate limiting before exposing beyond local/internal use.
