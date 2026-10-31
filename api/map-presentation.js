'use strict';

const zlib = require('zlib');

let cachedSource = null;
let cachedKey = '';
let cachedResult = null;
let cachedEncodedSource = null;
let cachedEncodedResult = null;
const pendingEncodedResults = new Map();

function requestedLimit(value) {
  if (!/^\d+$/.test(String(value ?? ''))) return null;
  const parsed = Number(value);
  return Number.isSafeInteger(parsed) && parsed >= 32 && parsed <= 4096 ? parsed : null;
}

function presentationQuery(request = {}) {
  if (typeof request.originalUrl === 'string') {
    const search = new URL(request.originalUrl, 'http://vectorwarp.invalid').searchParams;
    return {max_rows: search.get('max_rows'), max_cols: search.get('max_cols')};
  }
  return request.query || {};
}

// Produce a display-only, peak-preserving view of a delay-Doppler map.  This
// never changes the processor map used by detection, recording, or evidence.
function presentMap(frame, query = {}) {
  const maxRows = requestedLimit(query.max_rows);
  const maxColumns = requestedLimit(query.max_cols);
  if (!maxRows || !maxColumns || !frame) return frame;
  let parsed = frame;
  if (typeof frame === 'string') {
    try { parsed = JSON.parse(frame); }
    catch (_) { return frame; }
  }
  if (!Array.isArray(parsed.data) || !Array.isArray(parsed.doppler) ||
      !Array.isArray(parsed.delay) || parsed.data.length !== parsed.doppler.length ||
      !parsed.data.length || !parsed.data.every(row =>
        Array.isArray(row) && row.length === parsed.delay.length)) return frame;

  const rowStep = Math.max(1, Math.ceil(parsed.doppler.length / maxRows));
  const columnStep = Math.max(1, Math.ceil(parsed.delay.length / maxColumns));
  if (rowStep === 1 && columnStep === 1) return frame;
  const key = `${maxRows}:${maxColumns}`;
  if (cachedSource === frame && cachedKey === key) return cachedResult;

  const data = [];
  const doppler = [];
  for (let row = 0; row < parsed.data.length; row += rowStep) {
    const rowEnd = Math.min(parsed.data.length, row + rowStep);
    doppler.push(parsed.doppler[Math.min(rowEnd - 1, row + Math.floor((rowEnd - row - 1) / 2))]);
    const output = [];
    for (let column = 0; column < parsed.delay.length; column += columnStep) {
      const columnEnd = Math.min(parsed.delay.length, column + columnStep);
      let peak = -Infinity;
      for (let sourceRow = row; sourceRow < rowEnd; sourceRow++)
        for (let sourceColumn = column; sourceColumn < columnEnd; sourceColumn++)
          peak = Math.max(peak, parsed.data[sourceRow][sourceColumn]);
      output.push(peak);
    }
    data.push(output);
  }
  const delay = [];
  for (let column = 0; column < parsed.delay.length; column += columnStep) {
    const columnEnd = Math.min(parsed.delay.length, column + columnStep);
    delay.push(parsed.delay[Math.min(columnEnd - 1,
      column + Math.floor((columnEnd - column - 1) / 2))]);
  }
  cachedSource = frame;
  cachedKey = key;
  cachedResult = {...parsed, data, delay, doppler, nRows: data.length, nCols: delay.length,
    presentation: {peak_preserving: true, source_rows: parsed.doppler.length,
      source_cols: parsed.delay.length, row_step: rowStep, column_step: columnStep}};
  return cachedResult;
}

function encodeMapResponse(frame, query = {}, acceptEncoding = '') {
  const presented = presentMap(frame, query);
  const body = typeof presented === 'string' ? presented : JSON.stringify(presented);
  if (!/(?:^|[\s,])gzip(?:[\s,;]|$)/i.test(acceptEncoding))
    return {body, contentEncoding: null};
  return {body: zlib.gzipSync(body, {level: zlib.constants.Z_BEST_SPEED}),
    contentEncoding: 'gzip'};
}

function gzip(body) {
  return new Promise((resolve, reject) => zlib.gzip(body,
    {level: zlib.constants.Z_BEST_SPEED}, (error, result) =>
      error ? reject(error) : resolve(result)));
}

// Compression of a full desktop frame can consume tens of milliseconds.
// zlib's asynchronous API runs on the libuv worker pool, and this cache lets
// simultaneous browsers share one compressed representation of a frame.
async function encodeMapResponseAsync(frame, query = {}, acceptEncoding = '') {
  const presented = presentMap(frame, query);
  const body = typeof presented === 'string' ? presented : JSON.stringify(presented);
  if (!/(?:^|[\s,])gzip(?:[\s,;]|$)/i.test(acceptEncoding))
    return {body, contentEncoding: null};
  if (cachedEncodedSource === presented && cachedEncodedResult)
    return cachedEncodedResult;
  if (pendingEncodedResults.has(presented)) return pendingEncodedResults.get(presented);
  const pending = gzip(body).then(compressed => {
    const result = {body: compressed, contentEncoding: 'gzip'};
    cachedEncodedSource = presented;
    cachedEncodedResult = result;
    return result;
  }).finally(() => pendingEncodedResults.delete(presented));
  pendingEncodedResults.set(presented, pending);
  return pending;
}

module.exports = {encodeMapResponse, encodeMapResponseAsync, presentMap,
  presentationQuery, requestedLimit};
