'use strict';
const fs = require('fs');
const crypto = require('crypto');

function readBounded(filename, maximum = 64 * 1024 * 1024) {
  const fd = fs.openSync(filename, fs.constants.O_RDONLY | fs.constants.O_NOFOLLOW);
  try {
    const stat = fs.fstatSync(fd);
    if (!stat.isFile() || stat.size > maximum) throw new Error('Detection artifact exceeds the read limit or is not a regular file');
    const bytes = Buffer.alloc(stat.size + 1);
    let read = 0, count;
    while (read < bytes.length && (count = fs.readSync(fd, bytes, read, bytes.length - read, null))) read += count;
    const after = fs.fstatSync(fd);
    if (read !== stat.size || after.size !== stat.size || after.mtimeMs !== stat.mtimeMs)
      throw new Error('Detection artifact changed while reading');
    return bytes.subarray(0, read);
  } finally { fs.closeSync(fd); }
}

function persistCandidate(filename, manifold) {
  const model = JSON.parse(JSON.stringify(manifold));
  // Evaluations are separate immutable ledger entries, never part of a model.
  delete model.calibration.evaluation;
  delete model.calibration.independent_validation_samples;
  delete model.calibration.independent_validation_passes;
  delete model.calibration.independent_miss_passes;
  const bytes = Buffer.from(JSON.stringify(model, null, 2) + '\n');
  // Publish only a complete file. Exclusive linking never overwrites an
  // existing version, including when two workers finish simultaneously.
  const temporary = `${filename}.${process.pid}.${crypto.randomBytes(8).toString('hex')}.tmp`;
  fs.writeFileSync(temporary, bytes, {mode: 0o600, flag: 'wx'});
  try {
    try { fs.linkSync(temporary, filename); }
    catch (error) {
      if (error.code !== 'EEXIST') throw error;
      if (!readBounded(filename).equals(bytes)) throw new Error('Immutable detection candidate content differs for this version');
    }
  } finally { fs.unlinkSync(temporary); }
  return crypto.createHash('sha256').update(bytes).digest('hex');
}
module.exports = {readBounded, persistCandidate};
