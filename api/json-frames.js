'use strict';

const {StringDecoder} = require('string_decoder');

const MAX_FRAME_BYTES = 16 * 1024 * 1024;

// TCP is a byte stream: a chunk can end in the middle of UTF-8 or contain
// several JSON objects.  This parser is intentionally owned by one socket.
module.exports = function jsonFrames(onFrame, {maxBytes = MAX_FRAME_BYTES} = {}) {
  const decoder = new StringDecoder('utf8');
  let buffer = '';
  let start = -1;
  let scan = 0;
  let depth = 0;
  let quoted = false;
  let escaped = false;
  let pendingFrame = null;

  function ensureBound() {
    if (Buffer.byteLength(buffer, 'utf8') + (pendingFrame ? Buffer.byteLength(pendingFrame, 'utf8') : 0) > maxBytes)
      throw new Error('Radar frame exceeds limit');
  }

  function deliver(frame) {
    try {
      const value = JSON.parse(frame);
      if (!value || Array.isArray(value) || typeof value !== 'object')
        throw new Error('Expected radar JSON object');
    } catch (error) {
      if (error.message === 'Expected radar JSON object') throw error;
      throw new Error(`Invalid radar JSON object: ${error.message}`);
    }
    return onFrame(frame);
  }

  return function feed(chunk) {
    if (chunk && chunk.length) buffer += typeof chunk === 'string' ? chunk : decoder.write(chunk);
    ensureBound();
    if (pendingFrame !== null) {
      if (deliver(pendingFrame) === false) return false;
      pendingFrame = null;
    }
    for (; scan < buffer.length; ++scan) {
      const character = buffer[scan];
      if (start < 0) {
        if (/\s/.test(character)) continue;
        if (character !== '{') throw new Error('Expected radar JSON object');
        start = scan;
        depth = 1;
        continue;
      }
      if (quoted) {
        if (escaped) escaped = false;
        else if (character === '\\') escaped = true;
        else if (character === '"') quoted = false;
        continue;
      }
      if (character === '"') quoted = true;
      else if (character === '{' || character === '[') ++depth;
      else if (character === '}' || character === ']') {
        if (--depth < 0) throw new Error('Invalid radar JSON object');
        if (depth === 0) {
          const frame = buffer.slice(start, scan + 1);
          buffer = buffer.slice(scan + 1);
          scan = -1;
          start = -1;
          if (deliver(frame) === false) {
            pendingFrame = frame;
            scan = 0;
            return false;
          }
        }
      }
    }
    if (start < 0) {
      buffer = '';
      scan = 0;
    }
    return true;
  };
};

module.exports.MAX_FRAME_BYTES = MAX_FRAME_BYTES;
