'use strict';

// Read-only macOS observations. Never open a receiver or invoke an installer.
const fs = require('fs').promises;
const path = require('path');
const {execFile} = require('child_process');

function usbFromSystemProfiler(text) {
  if (typeof text !== 'string' || Buffer.byteLength(text) > 262144)
    throw new Error('USB inventory exceeds its size limit.');
  const report = JSON.parse(text);
  if (!report || !Array.isArray(report.SPUSBDataType)) throw new Error('Invalid macOS USB inventory.');
  const devices = [];
  let count = 0;
  function walk(items, depth = 0) {
    if (depth > 16 || !Array.isArray(items)) throw new Error('Invalid macOS USB topology.');
    for (const item of items) {
      if (++count > 512 || !item || typeof item !== 'object' || Array.isArray(item))
        throw new Error('USB inventory exceeds its device limit.');
      const textField = value => typeof value === 'string' && value.length <= 160 &&
        !/[\u0000-\u001f\u007f]/.test(value) ? value : null;
      // Bus/controller records have no product ID and are not receiver devices.
      if (item.product_id && textField(item._name)) {
        devices.push({product: textField(item._name), manufacturer: textField(item.manufacturer),
          serial: textField(item.serial_num)});
        if (devices.length > 64) throw new Error('USB inventory exceeds its device limit.');
      }
      if (item._items !== undefined) walk(item._items, depth + 1);
    }
  }
  walk(report.SPUSBDataType);
  return devices;
}

function runProfiler(_file, args, {signal, timeoutMs = 1500} = {}) {
  return new Promise((resolve, reject) => execFile('/usr/sbin/system_profiler', args,
    {signal, timeout: Math.min(timeoutMs, 3000), maxBuffer: 262144, encoding: 'utf8',
      env: {PATH: '/usr/bin:/bin:/usr/sbin:/sbin', LANG: 'C', LC_ALL: 'C'}},
    (error, output) => error ? reject(new Error('The read-only macOS USB check failed.')) : resolve(output)));
}

function createMacReceiverProbes(options = {}) {
  const run = options.run || runProfiler;
  const env = options.env || process.env;
  const readHeader = options.readHeader || (async file => {
    let handle;
    try {
      handle = await fs.open(file, 'r');
      const data = Buffer.alloc(65537);
      const {bytesRead} = await handle.read(data, 0, data.length, 0);
      return bytesRead <= 65536 ? data.subarray(0, bytesRead).toString('utf8') : '';
    } catch (_) { return ''; }
    finally { if (handle) await handle.close(); }
  });
  const readable = options.readable || (async file => {
    try { await fs.access(file, require('fs').constants.R_OK); return true; }
    catch (_) { return false; }
  });
  return {
    async usbInventory(_request, context) {
      return usbFromSystemProfiler(await run('/usr/sbin/system_profiler', ['SPUSBDataType', '-json'], context));
    },
    async dependencyInventory() {
      const result = Object.fromEntries(['Kraken', 'RspDuo', 'Usrp', 'HackRF'].map(type => [type, {state: 'unknown'}]));
      for (const prefix of ['/opt/homebrew', '/usr/local']) {
        if (await readable(path.join(prefix, 'lib/libhackrf.dylib'))) result.HackRF = {state: 'installed'};
      }
      // UHD minimum-version evidence comes from the actual native adapter's
      // successful load. A filename alone cannot establish UHD >= 4.1.
      const headers = [env.BLAH2_SDRPLAY_INCLUDE_DIR && path.join(env.BLAH2_SDRPLAY_INCLUDE_DIR, 'sdrplay_api.h'),
        '/usr/local/include/sdrplay_api.h', '/opt/homebrew/include/sdrplay_api.h'];
      const libraries = [env.BLAH2_SDRPLAY_LIBRARY, '/usr/local/lib/libsdrplay_api.so.3.15', '/usr/local/lib/libsdrplay_api.dylib',
        '/usr/local/lib/libsdrplay_api.3.15.dylib', '/opt/homebrew/lib/libsdrplay_api.dylib'];
      const anyReadable = async files => {
        for (const file of files) if (file && path.isAbsolute(file) && await readable(file)) return true;
        return false;
      };
      if (await anyReadable(libraries)) {
        for (const header of headers) {
          if (!header || !path.isAbsolute(header) || !await readable(header)) continue;
          const text = await readHeader(header);
          const versions = typeof text === 'string' && text.length <= 65536 ?
            [...text.matchAll(/^[ \t]*#[ \t]*define[ \t]+SDRPLAY_API_VERSION\b([^\r\n]*)/gm)] : [];
          const expression = versions.length === 1 ? versions[0][1].split('//')[0].split('/*')[0].replace(/\s+/g, '') : '';
          if (['(float)(3.15)', '(3.15)', '3.15', '3.15f', '3.15F'].includes(expression)) {
            result.RspDuo = {state: 'installed', version: '3.15'};
            break;
          }
        }
      }
      // Matching headers and a local library establish SDK presence only.
      // Native adapter loading separately checks architecture/runtime linkage;
      // neither observation proves the daemon or RF works.
      return result;
    }
  };
}
module.exports = {createMacReceiverProbes, usbFromSystemProfiler};
