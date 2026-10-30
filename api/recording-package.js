'use strict';

const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const {once} = require('events');

const MAGIC = Buffer.from('VECTORWARP-RECORDING-PACKAGE-V1\n');
const RECORDING_EXTENSIONS = new Set(['.blah2iq', '.mchq']);
const SIDECAR_SUFFIXES = ['.admission.json', '.timeline.json', '.timeline.jsonl.gz',
  '.provenance.json', '.replay.json', '.json'];
const MAX_HEADER_BYTES = 4096;
const MAX_ENTRIES = 16;

const frame = header => {
  const value = Buffer.from(JSON.stringify(header));
  if (value.length > MAX_HEADER_BYTES) throw new Error('Recording package entry header is too large.');
  const prefix = Buffer.alloc(4); prefix.writeUInt32BE(value.length);
  return Buffer.concat([prefix, value]);
};

async function descriptor(recording) {
  const primary = path.resolve(recording);
  const base = path.basename(primary);
  if (!RECORDING_EXTENSIONS.has(path.extname(base).toLowerCase()))
    throw Object.assign(new Error('Recording format cannot be packaged.'), {status: 400});
  const candidates = [primary, ...SIDECAR_SUFFIXES.map(suffix => `${primary}${suffix}`)];
  const entries = [];
  for (const filename of candidates) {
    let stat;
    try { stat = await fs.promises.lstat(filename); } catch (_) { continue; }
    if (!stat.isFile() || stat.isSymbolicLink()) continue;
    const name = path.basename(filename);
    entries.push({filename, name, size: stat.size, header: frame({name, size: stat.size})});
  }
  if (!entries.length || entries[0].filename !== primary)
    throw Object.assign(new Error('Recording not found.'), {status: 404});
  const size = MAGIC.length + entries.reduce((sum, entry) =>
    sum + entry.header.length + entry.size, 0) + 4;
  return {schema: 'vectorwarp-recording-package-v1', primary: base, entries, size,
    downloadName: `${base}.vwr`};
}

async function writeBuffer(writable, value) {
  if (!writable.write(value)) await once(writable, 'drain');
}

async function streamPackage(value, writable) {
  await writeBuffer(writable, MAGIC);
  for (const entry of value.entries) {
    await writeBuffer(writable, entry.header);
    const input = fs.createReadStream(entry.filename);
    for await (const chunk of input) await writeBuffer(writable, chunk);
  }
  await writeBuffer(writable, Buffer.alloc(4));
  writable.end();
}

async function readExact(handle, length, position, packageSize) {
  if (position + length > packageSize)
    throw Object.assign(new Error('Recording package is truncated.'), {status: 400});
  const value = Buffer.alloc(length);
  let offset = 0;
  while (offset < length) {
    const result = await handle.read(value, offset, length - offset, position + offset);
    if (!result.bytesRead)
      throw Object.assign(new Error('Recording package is truncated.'), {status: 400});
    offset += result.bytesRead;
  }
  return value;
}

function validEntries(entries) {
  if (!entries.length || entries.length > MAX_ENTRIES) return null;
  const primary = entries.find(entry => RECORDING_EXTENSIONS.has(path.extname(entry.name).toLowerCase()));
  if (!primary) return null;
  const allowed = new Set([primary.name, ...SIDECAR_SUFFIXES.map(suffix => `${primary.name}${suffix}`)]);
  if (entries.some(entry => !allowed.has(entry.name))) return null;
  return primary;
}

async function extractPackage(packageFile, destination) {
  const packageStat = await fs.promises.stat(packageFile);
  const handle = await fs.promises.open(packageFile, 'r');
  const entries = [];
  let position = 0;
  try {
    const magic = await readExact(handle, MAGIC.length, position, packageStat.size); position += MAGIC.length;
    if (!magic.equals(MAGIC))
      throw Object.assign(new Error('This is not a VectorWarp recording package.'), {status: 400});
    while (entries.length <= MAX_ENTRIES) {
      const prefix = await readExact(handle, 4, position, packageStat.size); position += 4;
      const headerLength = prefix.readUInt32BE(0);
      if (!headerLength) break;
      if (headerLength > MAX_HEADER_BYTES)
        throw Object.assign(new Error('Recording package entry header is invalid.'), {status: 400});
      const encoded = await readExact(handle, headerLength, position, packageStat.size); position += headerLength;
      let header;
      try { header = JSON.parse(encoded.toString('utf8')); }
      catch (_) { throw Object.assign(new Error('Recording package entry header is invalid.'), {status: 400}); }
      if (!header || typeof header.name !== 'string' || header.name !== path.basename(header.name) ||
          !Number.isSafeInteger(header.size) || header.size < 0 || header.name.startsWith('.'))
        throw Object.assign(new Error('Recording package entry is unsafe.'), {status: 400});
      if (entries.some(entry => entry.name === header.name))
        throw Object.assign(new Error('Recording package contains duplicate files.'), {status: 400});
      if (position + header.size > packageStat.size)
        throw Object.assign(new Error('Recording package is truncated.'), {status: 400});
      entries.push({name: header.name, size: header.size, position});
      position += header.size;
    }
    if (position !== packageStat.size)
      throw Object.assign(new Error('Recording package has unexpected trailing data.'), {status: 400});
    const primary = validEntries(entries);
    if (!primary)
      throw Object.assign(new Error('Recording package contents are unsupported.'), {status: 400});
    await fs.promises.mkdir(destination, {recursive: true, mode: 0o770});
    const extension = path.extname(primary.name).toLowerCase();
    const originalStem = path.basename(primary.name, extension)
      .replace(/[^A-Za-z0-9._-]+/g, '-').slice(0, 120) || 'recording';
    const sourcePrefix = primary.name;
    const suffixes = entries.map(entry => entry.name.slice(sourcePrefix.length));
    let stem = originalStem, counter = 0;
    while (suffixes.some(suffix => fs.existsSync(path.join(destination, `${stem}${extension}${suffix}`))))
      stem = `${originalStem}-${++counter}`;
    const temporaryDirectory = path.join(destination, `.package-import-${crypto.randomUUID()}`);
    await fs.promises.mkdir(temporaryDirectory, {mode: 0o700});
    const created = [];
    const published = [];
    try {
      for (const entry of entries) {
        const suffix = entry.name.slice(sourcePrefix.length);
        const finalName = `${stem}${extension}${suffix}`;
        const temporary = path.join(temporaryDirectory, finalName);
        const output = await fs.promises.open(temporary, 'wx', 0o660);
        try {
          let remaining = entry.size, sourcePosition = entry.position;
          const buffer = Buffer.alloc(Math.min(1024 * 1024, Math.max(1, remaining)));
          while (remaining) {
            const length = Math.min(buffer.length, remaining);
            const result = await handle.read(buffer, 0, length, sourcePosition);
            if (!result.bytesRead) throw Object.assign(new Error('Recording package is truncated.'), {status: 400});
            await output.write(buffer, 0, result.bytesRead);
            sourcePosition += result.bytesRead; remaining -= result.bytesRead;
          }
        } finally { await output.close(); }
        created.push({temporary, final: path.join(destination, finalName), name: finalName,
          size: entry.size, primary: entry === primary});
      }
      // Hard-link publication is atomic and refuses to overwrite a file that
      // appeared after the name check. All files live on the same recording
      // filesystem, so this adds no copy or additional recording-sized I/O.
      for (const entry of created) {
        await fs.promises.link(entry.temporary, entry.final);
        published.push(entry.final);
      }
      for (const entry of created) await fs.promises.unlink(entry.temporary);
      await fs.promises.rmdir(temporaryDirectory);
      const imported = created.find(entry => entry.primary);
      return {filename: imported.name, size_bytes: imported.size,
        package_bytes: packageStat.size, files: created.map(entry => entry.name)};
    } catch (error) {
      await fs.promises.rm(temporaryDirectory, {recursive: true, force: true}).catch(() => {});
      for (const filename of published) await fs.promises.rm(filename, {force: true}).catch(() => {});
      throw error;
    }
  } finally { await handle.close(); }
}

module.exports = {MAGIC, SIDECAR_SUFFIXES, descriptor, streamPackage, extractPackage};
