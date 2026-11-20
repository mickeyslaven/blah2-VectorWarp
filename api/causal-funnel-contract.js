'use strict';

// v2 uses exact uint64 decimal strings. Never coerce a rounded JSON Number.
const UINT64_MAX = '18446744073709551615';
function exactUint64(value) {
  return typeof value === 'string' && /^(0|[1-9][0-9]{0,19})$/.test(value) &&
    (value.length < 20 || value <= UINT64_MAX);
}
function encodeUint64(value) {
  if (typeof value === 'bigint') value = value.toString();
  else if (Number.isSafeInteger(value) && value >= 0) value = String(value);
  if (!exactUint64(value)) throw new Error('Exact unsigned decimal uint64 required');
  return value;
}
module.exports = {exactUint64, encodeUint64};
