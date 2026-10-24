#!/usr/bin/env node
'use strict';

// PHP2Shell prompted a complete runtime inventory. Keep source/Homebrew builds
// on Node release lines containing the 2026-10-21 security fixes as well.
const minimum = new Map([
  [22, [23, 4]],
  [24, [22, 1]],
  [26, [12, 1]]
]);
function patched(version) {
  const parts = version.split('.').map(Number);
  const required = minimum.get(parts[0]);
  const current = parts.slice(1, 3);
  return Boolean(required && (current[0] > required[0] ||
    (current[0] === required[0] && current[1] >= required[1])));
}

if (require.main === module && !patched(process.versions.node)) {
  console.error(`VectorWarp requires patched Node.js 22.23.4+, 24.22.1+, or 26.12.1+; found ${process.versions.node}`);
  process.exit(1);
}

module.exports = {patched};
