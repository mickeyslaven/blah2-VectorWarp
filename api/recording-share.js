'use strict';

const os = require('os');
const path = require('path');
const {pathToFileURL} = require('url');
const {execFile: defaultExecFile} = require('child_process');

function normalizeAddress(value) {
  const address = String(value || '').split('%', 1)[0];
  return address.startsWith('::ffff:') ? address.slice(7) : address;
}

function localAddresses(interfaces = os.networkInterfaces()) {
  const result = new Set(['127.0.0.1', '::1']);
  for (const values of Object.values(interfaces || {}))
    for (const value of values || []) if (value?.address) result.add(normalizeAddress(value.address));
  return result;
}

function isLocalDeviceRequest(req, interfaces) {
  const address = normalizeAddress(req?.socket?.remoteAddress);
  return Boolean(address && localAddresses(interfaces).has(address));
}

const run = (file, args, execFile = defaultExecFile) => new Promise((resolve, reject) =>
  execFile(file, args, {windowsHide: true, timeout: 10000}, error => error ? reject(error) : resolve()));

async function reveal(filename, {platform = process.platform, execFile = defaultExecFile} = {}) {
  if (platform === 'darwin') {
    await run('/usr/bin/open', ['-R', filename], execFile);
    return {selected: true, method: 'finder'};
  }
  if (platform === 'win32') {
    await run('explorer.exe', [`/select,${filename}`], execFile);
    return {selected: true, method: 'explorer'};
  }
  const uri = pathToFileURL(filename).href;
  try {
    await run('dbus-send', ['--session', '--dest=org.freedesktop.FileManager1', '--type=method_call',
      '/org/freedesktop/FileManager1', 'org.freedesktop.FileManager1.ShowItems',
      `array:string:${uri}`, 'string:'], execFile);
    return {selected: true, method: 'file-manager'};
  } catch (_) {
    await run('xdg-open', [path.dirname(filename)], execFile);
    return {selected: false, method: 'folder'};
  }
}

module.exports = {normalizeAddress, localAddresses, isLocalDeviceRequest, reveal};
