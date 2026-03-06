# Adaptogen Cron Runner

Docker Compose stack with:
- Redis server
- Node.js HTTP server on external port `245`

## Start

```bash
docker compose up --build
```

## Request format

Send a `POST` request to `http://localhost:245/` with either a JSON body or the `application/x-www-form-urlencoded` body that PHP sends with `http_build_query()`:

```json
{
  "cron": "*/30 * * * * *",
  "command": "echo",
  "key": "php-computed-cache-key",
  "keys": ["hello", "world"]
}
```

Fields:
- `cron`: cron expression used to schedule repeated execution
- `command`: shell command to execute
- `key`: Redis key already computed by PHP and sent to the service
- `keys`: optional command arguments (string or array)

## Behavior

- Redis key is taken directly from the request `key` field.
- If no cron job exists for that Redis key, one is registered.
- If a cached value exists in Redis for that key, the cached command output is returned as plain text.
- If no cached value exists, the command runs immediately with `keys`, its output is returned as plain text, and the full execution record is cached.
