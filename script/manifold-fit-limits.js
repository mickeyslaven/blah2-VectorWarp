'use strict';

const fs = require('fs');

// Shared ceilings for explicitly invoked offline manifold fitting. No runtime
// scheduler, configuration writes, or model activation is provided here.
const LIMITS = Object.freeze({input_bytes: 64 * 1024 * 1024, input_rows: 20000,
  output_bytes: 2 * 1024 * 1024, worker_timeout_ms: 30000,
  worker_old_generation_mb: 256, worker_young_generation_mb: 16,
  config_bytes: 256 * 1024});
const PROMOTION_BLOCKED = 'Automatic activation is unavailable: independent negative/false-track controls, production replay, supported-route, and resource qualification are not implemented.';

function readBoundedFile(filename, maximumBytes, label) {
  const fd = fs.openSync(filename, 'r');
  try {
    const stat = fs.fstatSync(fd);
    if (!stat.isFile() || stat.size > maximumBytes)
      throw new Error(`${label} must be a regular file of at most ${maximumBytes} bytes`);
    const buffer = Buffer.alloc(stat.size + 1);
    let length = 0, count;
    while (length < buffer.length &&
        (count = fs.readSync(fd, buffer, length, buffer.length - length, null)) > 0)
      length += count;
    if (length > stat.size) throw new Error('Manifold experiment input grew during bounded read; retry later');
    return buffer.subarray(0, length);
  } finally { fs.closeSync(fd); }
}

function readBoundedJsonl(filename) {
    const bytes = readBoundedFile(filename, LIMITS.input_bytes, 'Manifold input (64 MiB)');
    const lines = bytes.toString('utf8').split(/\r?\n/).filter(Boolean);
    if (lines.length > LIMITS.input_rows)
      throw new Error('Manifold experiment input exceeds 20000 rows');
    return {bytes, rows: lines.map((line, index) => {
      try { return JSON.parse(line); }
      catch (error) { throw new Error(`Invalid manifold input line ${index + 1}: ${error.message}`); }
    })};
}

async function writeBoundedCandidate(filename, value) {
  const bytes = JSON.stringify(value, null, 2) + '\n';
  if (Buffer.byteLength(bytes) > LIMITS.output_bytes)
    throw new Error('Manifold experiment candidate exceeds 2 MiB');
  const temporary = `${filename}.experiment.tmp`;
  let owned = false;
  try {
    const handle = await fs.promises.open(temporary, 'wx', 0o600);
    owned = true;
    try { await handle.writeFile(bytes); } finally { await handle.close(); }
    // Linking is atomic and refuses to replace an existing result or symlink.
    await fs.promises.link(temporary, filename);
  } finally {
    // A worker timeout can leave one bounded temporary file. Never overwrite it
    // silently on a later run; a successful/failed write cleans up its own file.
    if (owned) try { await fs.promises.unlink(temporary); }
    catch (error) { if (error.code !== 'ENOENT') throw error; }
  }
}

function runBoundedWorker(Worker, filename, workerData, timeoutMs = LIMITS.worker_timeout_ms) {
  return new Promise((resolve, reject) => {
    const worker = new Worker(filename, {workerData, resourceLimits: {
      maxOldGenerationSizeMb: LIMITS.worker_old_generation_mb,
      maxYoungGenerationSizeMb: LIMITS.worker_young_generation_mb, stackSizeMb: 4}});
    let settled = false;
    const finish = (error, result) => {
      if (settled) return;
      settled = true; clearTimeout(timer);
      Promise.resolve(worker.terminate()).then(() => error ? reject(error) : resolve(result), reject);
    };
    const timer = setTimeout(() => finish(new Error('Manifold experiment worker exceeded its 30-second deadline')),
      Math.min(timeoutMs, LIMITS.worker_timeout_ms));
    worker.once('message', message => message?.ok ? finish(null, message.result) :
      finish(new Error(message?.error || 'Malformed manifold worker response')));
    worker.once('error', error => finish(error));
    worker.once('exit', code => finish(new Error(`Manifold experiment worker exited ${code} without a result`)));
  });
}

module.exports = {LIMITS, PROMOTION_BLOCKED, readBoundedFile, readBoundedJsonl, writeBoundedCandidate,
  runBoundedWorker};
