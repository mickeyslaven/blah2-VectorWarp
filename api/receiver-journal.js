'use strict';

const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const LIMIT = 65536;

function createReceiverJournal(configFile, {io = fs} = {}) {
  const filename = `${path.resolve(configFile)}.receiver-state.json`;
  function load() {
    let descriptor;
    try {
      descriptor = io.openSync(filename, fs.constants.O_RDONLY |
        fs.constants.O_NOFOLLOW | fs.constants.O_NONBLOCK);
      const stat = io.fstatSync(descriptor);
      if (!stat.isFile() || stat.size > LIMIT) throw new Error('Invalid receiver journal');
      const raw = io.readFileSync(descriptor, 'utf8');
      if (Buffer.byteLength(raw) > LIMIT) throw new Error('Oversized receiver journal');
      const value = JSON.parse(raw);
      if (!value || value.schemaVersion !== 1 || typeof value.state !== 'string' ||
          typeof value.reconciliationRequired !== 'boolean') throw new Error('Invalid receiver journal');
      // A malformed state cannot turn an interrupted transaction into permission
      // to start. Only known settled states may carry a clear interlock.
      const settled = ['unknown', 'not-required', 'already-matched', 'synchronized', 'saved-pending'];
      return {...value, exists: true,
        reconciliationRequired: value.reconciliationRequired || !settled.includes(value.state)};
    } catch (error) {
      if (error.code === 'ENOENT') return {exists: false, state: 'unknown',
        receipt: null, reconciliationRequired: false, applicationVerified: false};
      return {exists: true, state: 'journal-unreadable', receipt: null,
        reconciliationRequired: true, applicationVerified: false};
    } finally { if (descriptor !== undefined) io.closeSync(descriptor); }
  }
  function write(value) {
    const raw = `${JSON.stringify({...value, schemaVersion: 1})}\n`;
    if (Buffer.byteLength(raw) > LIMIT) throw new Error('Receiver journal exceeds its size limit');
    const temporary = `${filename}.${crypto.randomBytes(12).toString('hex')}.tmp`;
    let descriptor;
    try {
      descriptor = io.openSync(temporary, 'wx', 0o640);
      // Packaged API and processor have separate primary groups. Preserve the
      // shared configuration group so the read-only start interlock can read it.
      let owner;
      try { owner = io.statSync(configFile); }
      catch (error) { if (error.code !== 'ENOENT') throw error; owner = io.statSync(path.dirname(filename)); }
      io.fchownSync(descriptor, -1, owner.gid);
      io.writeFileSync(descriptor, raw);
      io.fsyncSync(descriptor); io.closeSync(descriptor); descriptor = undefined;
      io.renameSync(temporary, filename);
      descriptor = io.openSync(path.dirname(filename), fs.constants.O_RDONLY);
      io.fsyncSync(descriptor);
    } catch (cause) {
      const error = new Error('Receiver transaction could not be durably recorded. No restart was requested.');
      error.code = 'RECEIVER_JOURNAL_PERSISTENCE_FAILED'; error.status = 503; error.cause = cause;
      throw error;
    } finally {
      if (descriptor !== undefined) io.closeSync(descriptor);
      try { io.unlinkSync(temporary); } catch (_) {}
    }
  }
  function recover() {
    const value = load();
    return {...value, applicationVerified: false,
      state: value.state === 'in-progress' ? 'interrupted' :
        ['synchronized', 'already-matched', 'not-required'].includes(value.state) ? 'unknown' : value.state};
  }
  return {load, write, recover, filename};
}

module.exports = {createReceiverJournal};
