'use strict';

const fs = require('fs');
const path = require('path');
const STATUS = '/run/vectorwarp-sdrplay/status.json';

// Root-owned restart receipt, not receiver readback or continuing service health.
function readSdrplayStartup(revision, {filename = STATUS, now = Date.now(), io = fs} = {}) {
  let fd;
  try {
    const directory = io.lstatSync(path.dirname(filename));
    if (!directory.isDirectory() || directory.uid !== 0 || directory.mode & 0o022) return null;
    fd = io.openSync(filename, fs.constants.O_RDONLY | fs.constants.O_NOFOLLOW | fs.constants.O_NONBLOCK);
    const stat = io.fstatSync(fd);
    if (!stat.isFile() || stat.uid !== 0 || stat.mode & 0o022 || stat.size > 4096) return null;
    const data = Buffer.alloc(4097);
    const size = io.readSync(fd, data, 0, data.length, 0);
    if (size > 4096) return null;
    const value = JSON.parse(data.subarray(0, size).toString('utf8'));
    if ((revision !== null && value.configRevision !== revision) || !/^[a-f0-9]{64}$/.test(value.configRevision) ||
        !Number.isFinite(value.updatedAt) || now < value.updatedAt || typeof value.inProgress !== 'boolean' ||
        !['starting', 'ready', 'failed', 'not-required', 'complete'].includes(value.state) ||
        (value.message !== undefined && (typeof value.message !== 'string' || value.message.length > 1800))) return null;
    // The installed oneshot is killed within 95 seconds. A lost finalizer must
    // become a visible error, not a permanent lock or a disappearing spinner.
    if (value.inProgress && now - value.updatedAt > 120000)
      return {...value, inProgress: false, state: 'failed', message:
        'The restart did not finish within its time limit. Check the VectorWarp service logs before retrying.'};
    return value;
  } catch (_) { return null; }
  finally { if (fd !== undefined) io.closeSync(fd); }
}

module.exports = {readSdrplayStartup};
