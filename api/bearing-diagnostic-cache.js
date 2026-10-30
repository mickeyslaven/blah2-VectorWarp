'use strict';

const fs = require('fs');
const os = require('os');
const path = require('path');
const {fork: forkProcess} = require('child_process');
const {siteKey} = require('./evidence-statistics');

class BearingDiagnosticCache {
  constructor(config, {fork = forkProcess, fileSystem = fs, now = Date.now,
    setPriority = os.setPriority,
    initialDelayMs = 60000, intervalMs = 60 * 60 * 1000,
    timeoutMs = 10 * 60 * 1000} = {}) {
    this.config = config;
    this.fork = fork;
    this.fs = fileSystem;
    this.now = now;
    this.setPriority = setPriority;
    this.initialDelayMs = initialDelayMs;
    this.intervalMs = intervalMs;
    this.timeoutMs = timeoutMs;
    this.samplePath = config.process?.bearing?.manifold_recorder?.path;
    this.referencePath = this.samplePath ? `${this.samplePath}.reference.jsonl` : null;
    this.filename = this.samplePath ? `${this.samplePath}.diagnostic-cache.json` : null;
    this.key = siteKey(config);
    this.timer = null;
    this.worker = null;
    this.workerTimer = null;
    this.signature = null;
    this.value = {available: false,
      reason: this.samplePath ? 'Bearing diagnostic is queued for background analysis.' :
        'No manifold recorder path is configured.'};
    this.meta = {state: this.samplePath ? 'waiting' : 'disabled', updated_at: null,
      error: null, low_priority_worker: true};
    this.load();
  }

  load() {
    if (!this.filename) return;
    try {
      const saved = JSON.parse(this.fs.readFileSync(this.filename, 'utf8'));
      if (saved?.schema !== 'vectorwarp-bearing-diagnostic-cache-v1' ||
          saved.site_key !== this.key || !saved.value || typeof saved.value !== 'object') return;
      this.value = saved.value;
      this.signature = saved.signature || null;
      this.meta = {...this.meta, state: 'cached', updated_at: saved.updated_at || null};
    } catch (error) {
      if (error.code !== 'ENOENT') this.meta = {...this.meta, state: 'error', error: error.message};
    }
  }

  start() {
    if (!this.samplePath || this.timer) return;
    const schedule = delay => {
      this.timer = setTimeout(async () => {
        await this.refresh();
        schedule(this.intervalMs);
      }, delay);
      this.timer.unref?.();
    };
    schedule(this.initialDelayMs);
  }

  async sourceSignature() {
    const signatures = [];
    for (const filename of [this.samplePath, this.referencePath]) try {
      const stat = await this.fs.promises.stat(filename);
      signatures.push(`${stat.size}:${stat.mtimeMs}`);
    } catch (error) {
      if (error.code !== 'ENOENT') throw error;
      signatures.push('missing');
    }
    return signatures.join('|');
  }

  async refresh({force = false} = {}) {
    if (!this.samplePath || this.worker) return false;
    let signature;
    try { signature = await this.sourceSignature(); }
    catch (error) {
      this.meta = {...this.meta, state: 'error', error: error.message};
      return false;
    }
    if (!force && signature === this.signature) return false;
    this.meta = {...this.meta, state: 'refreshing', error: null};
    const worker = this.fork(path.join(__dirname, 'bearing-diagnostic-worker.js'), [],
      {stdio: ['ignore', 'ignore', 'ignore', 'ipc']});
    this.worker = worker;
    try { this.setPriority(worker.pid, os.constants.priority.PRIORITY_LOW); }
    catch (_) { /* Best effort on platforms that cannot lower child priority. */ }
    let settled = false;
    const finish = (error, value) => {
      if (settled || worker !== this.worker) return;
      settled = true;
      clearTimeout(this.workerTimer);
      this.workerTimer = null;
      this.worker = null;
      if (error) this.meta = {...this.meta, state: 'error', error: error.message};
      else {
        this.value = value;
        this.signature = signature;
        this.meta = {...this.meta, state: 'cached', updated_at: this.now(), error: null};
        this.persist();
      }
      worker.disconnect?.();
    };
    worker.once('message', message => message?.ok ? finish(null, message.value) :
      finish(new Error(message?.error || 'Bearing diagnostic worker failed')));
    worker.once('error', finish);
    worker.once('exit', code => {
      if (!settled) finish(new Error(`Bearing diagnostic worker exited ${code}`));
    });
    this.workerTimer = setTimeout(() => {
      worker.kill?.();
      finish(new Error('Bearing diagnostic worker timed out'));
    }, this.timeoutMs);
    this.workerTimer.unref?.();
    worker.send({config: this.config});
    return true;
  }

  persist() {
    try {
      this.fs.mkdirSync(path.dirname(this.filename), {recursive: true});
      const temporary = `${this.filename}.tmp-${process.pid}`;
      const record = {schema: 'vectorwarp-bearing-diagnostic-cache-v1',
        site_key: this.key, signature: this.signature,
        updated_at: this.meta.updated_at, value: this.value};
      this.fs.writeFileSync(temporary, `${JSON.stringify(record)}\n`, {mode: 0o640});
      this.fs.renameSync(temporary, this.filename);
    } catch (error) { this.meta = {...this.meta, state: 'error', error: error.message}; }
  }

  status() { return {...this.value, cache: {...this.meta}}; }

  close() {
    if (this.timer) clearTimeout(this.timer);
    if (this.workerTimer) clearTimeout(this.workerTimer);
    this.worker?.kill?.();
    this.timer = null;
    this.workerTimer = null;
    this.worker = null;
  }
}

function createBearingDiagnosticCache(config, dependencies) {
  return new BearingDiagnosticCache(config, dependencies);
}

module.exports = {BearingDiagnosticCache, createBearingDiagnosticCache};
