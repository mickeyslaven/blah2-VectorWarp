// Test for the TCP ingest of map, detection, timing and iqdata frames: what
// blah2 writes into each socket must come back out of the matching GET route
// unchanged, however the bytes were split on the way in.
//
// Boots server.js as a child process, writes frames into its TCP ports the way
// blah2 does (one persistent connection per socket, many writes per frame) and
// compares what the API serves against what was sent, byte for byte.
//
// It deliberately knows nothing about how server.js assembles frames, so the
// same file can be run against an old and a new server.js to show the two
// serve identical bytes. The map case is the one that matters: a full-size
// map, split into 1 KiB writes exactly as Socket::sendData does, several maps
// in a row on one connection.
//
// Run with: node test_ingest.js

const crypto = require('crypto');
const fs = require('fs');
const http = require('http');
const net = require('net');
const os = require('os');
const path = require('path');
const { spawn } = require('child_process');

// must be 3000: the stash modules self-poll the API on a hardcoded port 3000
const API_PORT = 3000;
const PORTS = {
  map: 33110,
  detection: 33111,
  track: 33112,
  timestamp: 33113,
  timing: 33114,
  iqdata: 33115,
};

let passed = 0;
let failed = 0;

function check(name, cond, detail) {
  if (cond) {
    passed++;
    console.log(`  PASS: ${name}`);
  } else {
    failed++;
    console.error(`  FAIL: ${name}${detail ? `  <- ${detail}` : ''}`);
  }
}

function md5(s) {
  return crypto.createHash('md5').update(s).digest('hex');
}

// Deterministic PRNG so a failure reproduces.
function rng(seed) {
  let s = seed >>> 0;
  return () => {
    s = (s * 1664525 + 1013904223) >>> 0;
    return s / 4294967296;
  };
}

// A map shaped like Map::to_json_km: one object, flat arrays and a 2D data
// array, so the only "}" is the last byte. 1001 x 411 is +-1000 Hz at cpi 0.5
// with delayMax 400, about 2.2 MB, the size the quadratic ingest cost hurt most.
function makeMap(timestamp, nRows, nCols, seed) {
  const r = rng(seed);
  const delay = Array.from({ length: nCols }, (_, i) => +(i * 0.149896).toFixed(5));
  const doppler = Array.from({ length: nRows }, (_, i) => +(-1000 + i * 1.0).toFixed(5));
  const data = [];
  for (let i = 0; i < nRows; i++) {
    const row = [];
    for (let j = 0; j < nCols; j++) row.push(+(r() * 30).toFixed(2));
    data.push(row);
  }
  return JSON.stringify({
    timestamp, minPower: 0, maxPower: 30, noisePower: 1.2,
    nRows, nCols, delay, doppler, data,
  });
}

// One persistent connection, like blah2's Socket. send() resolves once every
// write has been flushed to the kernel.
function connect(port) {
  return new Promise((resolve, reject) => {
    const socket = net.createConnection(port, '127.0.0.1', () => {
      socket.setNoDelay(true);
      resolve(socket);
    });
    socket.on('error', reject);
  });
}

function writeAll(socket, pieces, gapMs) {
  return pieces.reduce((p, piece) => p.then(() => new Promise((resolve) => {
    socket.write(piece, () => {
      if (gapMs > 0) setTimeout(resolve, gapMs);
      else resolve();
    });
  })), Promise.resolve());
}

// Socket::sendData splits into fixed 1 KiB writes.
function splitFixed(s, size) {
  const out = [];
  for (let i = 0; i < s.length; i += size) out.push(s.slice(i, i + size));
  return out;
}

// Irregular splits, including 1-byte pieces and a piece that is just the
// closing "}", to land frame boundaries on awkward chunk edges.
function splitRandom(s, seed) {
  const r = rng(seed);
  const out = [];
  let i = 0;
  while (i < s.length) {
    const roll = r();
    const n = roll < 0.2 ? 1 : roll < 0.5 ? 1 + Math.floor(r() * 64) : 1 + Math.floor(r() * 9000);
    out.push(s.slice(i, i + n));
    i += n;
  }
  if (out.length > 1 && out[out.length - 1].length > 1) {
    const last = out.pop();
    out.push(last.slice(0, -1), last.slice(-1));
  }
  return out;
}

function get(route) {
  return new Promise((resolve, reject) => {
    http.get({ host: '127.0.0.1', port: API_PORT, path: route }, (res) => {
      const chunks = [];
      res.on('data', (c) => chunks.push(c));
      res.on('end', () => resolve({
        status: res.statusCode,
        type: res.headers['content-type'],
        body: Buffer.concat(chunks).toString('utf8'),
      }));
    }).on('error', reject);
  });
}

function waitForServer(timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  return new Promise((resolve, reject) => {
    const poll = () => {
      http.get({ host: '127.0.0.1', port: API_PORT, path: '/api/map' }, (res) => {
        res.resume();
        resolve();
      }).on('error', () => {
        if (Date.now() > deadline) reject(new Error('server.js did not come up in time'));
        else setTimeout(poll, 200);
      });
    };
    poll();
  });
}

// Poll a route until it serves `expected` or the deadline passes, and return
// the last body seen.
async function waitForBody(route, expected, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  let last;
  while (Date.now() < deadline) {
    last = await get(route);
    if (last.body === expected) return last;
    await new Promise((r) => setTimeout(r, 50));
  }
  return last;
}

async function main() {
  const cfg = `
network:
  ip: '127.0.0.1'
  ports:
    api: ${API_PORT}
    map: ${PORTS.map}
    detection: ${PORTS.detection}
    track: ${PORTS.track}
    timestamp: ${PORTS.timestamp}
    timing: ${PORTS.timing}
    iqdata: ${PORTS.iqdata}
capture:
  fc: 98000000
  device:
    gainReduction: [40, 41]
    lnaState: 4
truth:
  adsb:
    enabled: false
    tar1090: ""
    adsb2dd: ""
location:
  rx: {latitude: 0, longitude: 0, altitude: 0}
  tx: {latitude: 0, longitude: 0, altitude: 0}
`;
  const cfgPath = path.join(os.tmpdir(), `test_ingest_config_${process.pid}.yml`);
  fs.writeFileSync(cfgPath, cfg);

  const server = spawn(process.execPath, ['server.js', cfgPath],
    { cwd: __dirname, stdio: ['ignore', 'pipe', 'pipe'] });
  server.stderr.on('data', (d) => process.stderr.write(`[server] ${d}`));

  try {
    await waitForServer(10000);

    // ---- map ----
    console.log('map:');
    const mapSock = await connect(PORTS.map);
    const maps = [
      { label: 'full-size map in 1 KiB writes', body: makeMap(1757400000000, 1001, 411, 1), split: (s) => splitFixed(s, 1024), gap: 0 },
      { label: 'second map on the same connection', body: makeMap(1757400000500, 1001, 411, 2), split: (s) => splitFixed(s, 1024), gap: 0 },
      { label: 'map in irregular writes, closing brace alone', body: makeMap(1757400001000, 601, 411, 3), split: (s) => splitRandom(s, 7), gap: 0 },
      { label: 'map in 1 KiB writes paced so each is its own read', body: makeMap(1757400001500, 301, 411, 4), split: (s) => splitFixed(s, 1024), gap: 1 },
    ];
    for (const m of maps) {
      await writeAll(mapSock, m.split(m.body), m.gap);
      const res = await waitForBody('/api/map', m.body, 10000);
      check(`${m.label}: served byte-identical (${m.body.length} bytes)`,
        res.body === m.body,
        `sent ${md5(m.body)} ${m.body.length}B, served ${md5(res.body)} ${res.body.length}B`);
    }
    // A frame is only published once complete: midway through a map the
    // previous one must still be served, never a prefix of the new one.
    const partial = makeMap(1757400002000, 301, 411, 5);
    const pieces = splitFixed(partial, 1024);
    await writeAll(mapSock, pieces.slice(0, pieces.length - 1), 0);
    await new Promise((r) => setTimeout(r, 200));
    const mid = await get('/api/map');
    check('an incomplete map is not served', mid.body === maps[maps.length - 1].body,
      `served ${mid.body.length}B`);
    await writeAll(mapSock, pieces.slice(-1), 0);
    const done = await waitForBody('/api/map', partial, 5000);
    check('it is served once its last byte arrives', done.body === partial);
    mapSock.destroy();

    // ---- detection ----
    // With ADS-B truth off, server.js re-serialises the parsed frame, so what
    // comes back is JSON.stringify(JSON.parse(frame)), not the raw bytes.
    console.log('detection:');
    const detSock = await connect(PORTS.detection);
    for (let k = 0; k < 3; k++) {
      const det = JSON.stringify({
        timestamp: 1757400000000 + k * 500,
        delay: [12.34 + k, 88.01], doppler: [-45.6, 12.0 + k], snr: [11.2, 4.8],
      });
      await writeAll(detSock, splitRandom(det, 100 + k), 0);
      const want = JSON.stringify(JSON.parse(det));
      const res = await waitForBody('/api/detection', want, 5000);
      check(`detection frame ${k + 1} served intact`, res.body === want, res.body.slice(0, 60));
    }
    detSock.destroy();

    // ---- timing, iqdata: served raw ----
    for (const [name, route, port] of [
      ['timing', '/api/timing', PORTS.timing],
      ['iqdata', '/api/iqdata', PORTS.iqdata],
    ]) {
      console.log(`${name}:`);
      const sock = await connect(port);
      for (let k = 0; k < 3; k++) {
        const body = JSON.stringify({ timestamp: 1757400000000 + k * 500, a: [1, 2, 3], b: 'x'.repeat(3000 + k) });
        await writeAll(sock, splitRandom(body, 200 + k), 0);
        const res = await waitForBody(route, body, 5000);
        check(`${name} frame ${k + 1} served byte-identical`, res.body === body,
          `served ${res.body.length}B`);
      }
      sock.destroy();
    }
  } finally {
    server.kill();
    fs.unlinkSync(cfgPath);
  }

  console.log(`\n${passed} passed, ${failed} failed`);
  process.exit(failed === 0 ? 0 : 1);
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
