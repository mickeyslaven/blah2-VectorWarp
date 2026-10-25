'use strict';

// Package-metadata regression for distro/vendor UHD stacks. This remains a
// pure test: it never invokes APT or changes the host package database.
const assert = require('assert/strict');
const fs = require('fs');
const path = require('path');

const root = path.resolve(__dirname, '../..');
const packager = fs.readFileSync(path.join(root, 'script/package-native.sh'), 'utf8');
const dragonDocs = fs.readFileSync(path.join(root, 'docs/DRAGONOS.md'), 'utf8');
const controlLiteral = packager.match(
  /printf '(Package: vectorwarp\\nVersion:.*?Description:.*?\\n)' \\\n/s)?.[1];
assert.ok(controlLiteral, 'Debian control template must remain discoverable');
const control = controlLiteral.replaceAll('\\n', '\n');
const field = name => control.match(new RegExp(`^${name}: (.*)$`, 'm'))?.[1];

assert.match(field('Depends'), /^%s, /,
  'dpkg-shlibdeps output must remain the first hard runtime dependency');
assert.doesNotMatch(field('Depends'), /(?:^|, )uhd-host(?:,|$)/,
  'Ubuntu uhd-host cannot be mandatory for compatible distro/vendor UHD stacks');
assert.equal(field('Recommends'), 'uhd-host',
  'normal stock APT installs must retain B210 tools, images and udev readiness');

// This is the observed older DragonOS Jammy stack that exposed the regression.
// uhd-host cannot coexist with it because the stock host package demands an
// exact different libuhd version, but VectorWarp's ABI dependency can still be
// satisfied by the installed package name/version.
const customDragonUhd = {
  Package: 'libuhd4.1.0',
  Version: '20230414-1',
  Maintainer: 'root@dragon'
};
const stockJammyHostDepends = 'libuhd4.1.0 (= 4.1.0.5-3)';
assert.equal(customDragonUhd.Package, stockJammyHostDepends.match(/^[^ ]+/)[0]);
assert.notEqual(customDragonUhd.Version,
  stockJammyHostDepends.match(/= ([^)]+)/)[1]);
assert.ok(!field('Depends').includes('uhd-host') && field('Recommends') === 'uhd-host',
  'the incompatible stock host pair must be optional while the ELF ABI dependency remains hard');

for (const mapping of [
  ['FocalX R37.1', 'Jammy', '22.04'],
  ['Noble R9', 'Noble', '24.04'],
  ['Resolute R1', 'Resolute', '26.04']
]) {
  for (const value of mapping) assert.match(dragonDocs, new RegExp(value.replace('.', '\\.')));
}

console.log('Debian/DragonOS UHD package relationship checks passed.');
