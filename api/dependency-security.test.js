'use strict';

const assert = require('assert');
const qs = require('qs');
const version = require('qs/package.json').version;

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
