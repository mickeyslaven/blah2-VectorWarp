'use strict';

const assert = require('assert');
const qs = require('qs');
const version = require('qs/package.json').version;
const proxyAddrVersion = require('proxy-addr/package.json').version;

assert.equal(version, '6.16.0', `Expected patched qs 6.16.0, found ${version}`);
for (const input of [
  '__proto__[polluted]=yes',
  'constructor[prototype][polluted]=yes',
  'a[b][c][d][e][f][g][h][i][j]=1'
]) {
  const result = qs.parse(input);
  assert.equal(Object.prototype.polluted, undefined, input);
  assert.equal(result.polluted, undefined, input);
}
console.log(`Dependency security test passed: qs ${version}.`);

const proxyAddrParts = proxyAddrVersion.split('.').map(Number);
assert.ok(
  proxyAddrParts.length === 3 &&
    (proxyAddrParts[0] > 2 ||
      (proxyAddrParts[0] === 2 &&
        (proxyAddrParts[1] > 0 || (proxyAddrParts[1] === 0 && proxyAddrParts[2] >= 8)))),
  `Expected proxy-addr 2.0.8 or newer, found ${proxyAddrVersion}`
);
console.log(`Dependency security test passed: proxy-addr ${proxyAddrVersion}.`);
