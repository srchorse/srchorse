import crypto from "node:crypto";
import { exec } from "node:child_process";
import { promisify } from "node:util";
import express from "express";
import { CronJob } from "cron";
import { createClient } from "redis";

const execAsync = promisify(exec);
const app = express();
const port = Number(process.env.PORT || 245);
const redisUrl = process.env.REDIS_URL || "redis://redis:6379";
const scheduledJobs = new Map();

app.use(express.json());

const redis = createClient({ url: redisUrl });
redis.on("error", (err) => {
  console.error("Redis error:", err);
});

function hashCommand(command) {
  return crypto.createHash("sha256").update(command).digest("hex");
}

function normalizeKeys(keys) {
  if (Array.isArray(keys)) {
    return keys.map((value) => String(value));
  }
  if (keys === undefined || keys === null) {
    return [];
  }
  return [String(keys)];
}

function shellEscapeArg(value) {
  return `'${String(value).replace(/'/g, `'\\''`)}'`;
}

function buildCommand(command, keys) {
  if (keys.length === 0) {
    return command;
  }

  const escapedArgs = keys.map(shellEscapeArg).join(" ");
  return `${command} ${escapedArgs}`;
}

async function runCommand(command, keys) {
  const fullCommand = buildCommand(command, keys);

  try {
    const { stdout, stderr } = await execAsync(fullCommand, { maxBuffer: 1024 * 1024 });
    return {
      ok: true,
      command,
      keys,
      stdout: stdout.trimEnd(),
      stderr: stderr.trimEnd(),
      exitCode: 0,
      ranAt: new Date().toISOString()
    };
  } catch (error) {
    return {
      ok: false,
      command,
      keys,
      stdout: (error.stdout || "").trimEnd(),
      stderr: (error.stderr || error.message || "").trimEnd(),
      exitCode: typeof error.code === "number" ? error.code : 1,
      ranAt: new Date().toISOString()
    };
  }
}

async function runAndCache(command, keys, redisKey) {
  const result = await runCommand(command, keys);
  await redis.set(redisKey, JSON.stringify(result));
  return result;
}

app.post("/", async (req, res) => {
  const { cron: cronExpression, command, keys } = req.body || {};

  if (!command || typeof command !== "string") {
    res.status(400).json({ error: "`command` is required and must be a string." });
    return;
  }

  if (!cronExpression || typeof cronExpression !== "string") {
    res.status(400).json({ error: "`cron` is required and must be a string." });
    return;
  }

  const commandKeys = normalizeKeys(keys);
  const redisKey = hashCommand(command);

  if (!scheduledJobs.has(redisKey)) {
    try {
      const job = new CronJob(cronExpression, async () => {
        const scheduledResult = await runCommand(command, commandKeys);
        await redis.set(redisKey, JSON.stringify(scheduledResult));
      });
      job.start();
      scheduledJobs.set(redisKey, job);
    } catch (error) {
      res.status(400).json({ error: "Invalid cron expression.", detail: String(error.message || error) });
      return;
    }
  }

  const existing = await redis.get(redisKey);
  if (existing) {
    res.json({
      source: "redis",
      key: redisKey,
      result: JSON.parse(existing)
    });
    return;
  }

  const liveResult = await runAndCache(command, commandKeys, redisKey);
  res.json({
    source: "live",
    key: redisKey,
    result: liveResult
  });
});

app.get("/health", async (_, res) => {
  try {
    await redis.ping();
    res.json({ ok: true });
  } catch {
    res.status(503).json({ ok: false });
  }
});

async function start() {
  await redis.connect();
  app.listen(port, "0.0.0.0", () => {
    console.log(`Server listening on port ${port}`);
  });
}

async function shutdown(signal) {
  console.log(`Received ${signal}, shutting down...`);
  for (const job of scheduledJobs.values()) {
    job.stop();
  }
  try {
    await redis.quit();
  } catch {
    await redis.disconnect();
  }
  process.exit(0);
}

process.on("SIGINT", () => {
  shutdown("SIGINT");
});
process.on("SIGTERM", () => {
  shutdown("SIGTERM");
});

start().catch((error) => {
  console.error("Failed to start server:", error);
  process.exit(1);
});
