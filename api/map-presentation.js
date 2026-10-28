'use strict';

let cachedSource = null;
let cachedKey = '';
let cachedResult = null;

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
  if (!maxRows || !maxColumns || !frame || !Array.isArray(frame.data) ||
      !Array.isArray(frame.doppler) || !Array.isArray(frame.delay) ||
      frame.data.length !== frame.doppler.length || !frame.data.length ||
      !frame.data.every(row => Array.isArray(row) && row.length === frame.delay.length)) return frame;

  const rowStep = Math.max(1, Math.ceil(frame.doppler.length / maxRows));
  const columnStep = Math.max(1, Math.ceil(frame.delay.length / maxColumns));
  if (rowStep === 1 && columnStep === 1) return frame;
  const key = `${maxRows}:${maxColumns}`;
  if (cachedSource === frame && cachedKey === key) return cachedResult;

  const data = [];
  const doppler = [];
  for (let row = 0; row < frame.data.length; row += rowStep) {
    const rowEnd = Math.min(frame.data.length, row + rowStep);
    doppler.push(frame.doppler[Math.min(rowEnd - 1, row + Math.floor((rowEnd - row - 1) / 2))]);
    const output = [];
    for (let column = 0; column < frame.delay.length; column += columnStep) {
      const columnEnd = Math.min(frame.delay.length, column + columnStep);
      let peak = -Infinity;
      for (let sourceRow = row; sourceRow < rowEnd; sourceRow++)
        for (let sourceColumn = column; sourceColumn < columnEnd; sourceColumn++)
          peak = Math.max(peak, frame.data[sourceRow][sourceColumn]);
      output.push(peak);
    }
    data.push(output);
  }
  const delay = [];
  for (let column = 0; column < frame.delay.length; column += columnStep) {
    const columnEnd = Math.min(frame.delay.length, column + columnStep);
    delay.push(frame.delay[Math.min(columnEnd - 1,
      column + Math.floor((columnEnd - column - 1) / 2))]);
  }
  cachedSource = frame;
  cachedKey = key;
  cachedResult = {...frame, data, delay, doppler, nRows: data.length, nCols: delay.length,
    presentation: {peak_preserving: true, source_rows: frame.doppler.length,
      source_cols: frame.delay.length, row_step: rowStep, column_step: columnStep}};
  return cachedResult;
}

module.exports = {presentMap, presentationQuery, requestedLimit};
