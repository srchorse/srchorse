# Adaptogen Cron Runner

Docker Compose stack with:
- Redis server
- Node.js HTTP server on external port `245`

## Start

```bash
docker compose up --build
```

## Request format

Send a `POST` request to `http://localhost:245/` with JSON body:

```json
{
  "cron": "*/30 * * * * *",
  "command": "echo",
  "keys": ["hello", "world"]
}
```

Fields:
- `cron`: cron expression used to schedule repeated execution
- `command`: shell command to execute
- `keys`: optional command arguments (string or array)

## Behavior

- Redis key is `sha256(command)`.
- If no cron job exists for that command hash, one is registered.
- If a cached value exists in Redis for that key, it is returned.
- If no cached value exists, the command runs immediately with `keys`, result is returned and cached.
