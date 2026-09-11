'use strict';
const net = require('net');
const SOCKET = '/run/vectorwarp-receiver/management.sock';
const MAX_BYTES = 65536;

function createReceiverHelperClient(options = {}) {
  const connect = options.connect || net.createConnection;
  return function request(payload) {
    return new Promise((resolve, reject) => {
      let data = '', bytes = 0, finished = false;
      const client = connect({path: SOCKET});
      const done = (error, value) => {
        if (finished) return;
        finished = true;
        clearTimeout(timer);
        client.destroy();
        if (error) reject(error); else resolve(value);
      };
      const fail = (code, message) => {
        const error = new Error(message); error.code = code; error.status = 503;
        done(error);
      };
      const timer = setTimeout(() => fail('MANAGEMENT_TIMEOUT', payload.verb === 'execute' ?
        'Receiver action outcome is unknown. Inspect the local service before retrying.' :
        'Receiver management did not respond before its deadline.'), payload.verb === 'execute' ? 200000 : 20000);
      client.setEncoding('utf8');
      client.on('connect', () => client.write(`${JSON.stringify(payload)}\n`));
      client.on('error', () => fail('MANAGEMENT_UNAVAILABLE',
        'The local receiver management helper is unavailable. Receiver discovery and remote endpoints remain usable.'));
      client.on('end', () => { if (!finished) fail('MANAGEMENT_RESPONSE_INCOMPLETE', 'Receiver management response was incomplete.'); });
      client.on('data', chunk => {
        bytes += Buffer.byteLength(chunk);
        if (bytes > MAX_BYTES) return fail('MANAGEMENT_RESPONSE_INVALID', 'Receiver management response exceeded its size limit.');
        data += chunk;
        const newline = data.indexOf('\n');
        if (newline < 0) return;
        let result;
        try { result = JSON.parse(data.slice(0, newline)); }
        catch (_) { return fail('MANAGEMENT_RESPONSE_INVALID', 'Receiver management returned invalid JSON.'); }
        if (!result || typeof result !== 'object' || Array.isArray(result) || typeof result.ok !== 'boolean')
          return fail('MANAGEMENT_RESPONSE_INVALID', 'Receiver management returned an invalid receipt.');
        done(null, result);
      });
    });
  };
}
module.exports = {createReceiverHelperClient};
