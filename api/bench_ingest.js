// Microbenchmark for the TCP frame ingest in server.js: the old rule (test the
// accumulated buffer's last character) against the current one (test the
// chunk's). Pure CPU, no sockets, so it isolates the string work.
//
// The old rule flattens the whole lazy rope on every chunk, so its cost grows
// with chunk count times map size; the current one should be flat in chunk
// count. Both must assemble the identical frame, which is checked here too.
//
// Run with: node bench_ingest.js [mapBytes]
// On a node, inside the production image (node 20, the node's own ARM core):
//   docker run --rm -v $PWD/bench_ingest.js:/b.js:ro --entrypoint node \
//     ghcr.io/offworldlabs/blah2-api:<tag> /b.js

const MAP_BYTES = Number(process.argv[2]) || 2210000;  // +-1000 Hz, cpi 0.5
const CHUNKS = [65536, 32768, 16384, 8192, 4096, 1024];
const REPEATS = 5;

function makeFrame(bytes) {
  const cell = '12.34,';
  const body = cell.repeat(Math.floor((bytes - 20) / cell.length));
  return '{"data":[' + body + '0]}';
}

function split(s, size) {
  const out = [];
  for (let i = 0; i < s.length; i += size) out.push(Buffer.from(s.slice(i, i + size)));
  return out;
}

function ingestOld(chunks) {
  let buf = '';
  let out = null;
  for (const msg of chunks) {
    buf = buf + msg.toString();
    if (buf.slice(-1) === '}') { out = buf; buf = ''; }
  }
  return out;
}

function ingestNew(chunks) {
  let buf = '';
  let out = null;
  for (const msg of chunks) {
    const s = msg.toString();
    buf += s;
    if (s.charCodeAt(s.length - 1) === 125) { out = buf; buf = ''; }
  }
  return out;
}

// The served response flattens the rope once (res.send encodes it); include
// that so the new rule is not credited with skipping work it only defers.
function timeIt(fn, chunks) {
  let best = Infinity;
  let frame;
  for (let r = 0; r < REPEATS; r++) {
    const t0 = process.hrtime.bigint();
    frame = fn(chunks);
    Buffer.byteLength(frame);
    Buffer.from(frame);
    best = Math.min(best, Number(process.hrtime.bigint() - t0) / 1e6);
  }
  return { ms: best, frame };
}

const frame = makeFrame(MAP_BYTES);
console.log(`node ${process.version} ${process.arch}, frame ${frame.length} bytes, best of ${REPEATS}`);
console.log('chunk  chunks   old ms   new ms   speedup  identical');
for (const size of CHUNKS) {
  const chunks = split(frame, size);
  const a = timeIt(ingestOld, chunks);
  const b = timeIt(ingestNew, chunks);
  const same = a.frame === frame && b.frame === frame;
  console.log(`${String(size).padStart(5)}  ${String(chunks.length).padStart(6)}  `
    + `${a.ms.toFixed(1).padStart(7)}  ${b.ms.toFixed(1).padStart(7)}  `
    + `${(a.ms / b.ms).toFixed(1).padStart(7)}x  ${same}`);
  if (!same) process.exitCode = 1;
}
