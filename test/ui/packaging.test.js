'use strict';

// Static/staged packaging acceptance. This test never invokes a package
// manager, modifies system configuration, starts services or touches hardware.
const assert = require('assert');
const fs = require('fs');
const path = require('path');
const {spawnSync} = require('child_process');

const root = path.resolve(__dirname, '../..');
const read = relative => fs.readFileSync(path.join(root, relative), 'utf8');
const packageScript = read('script/package-native.sh');
const releaseInstaller = read('script/install-release.sh');
const buildScript = read('script/build-native.sh');
const debPostinst = read('packaging/deb/postinst');
const rpmSpec = read('packaging/rpm/vectorwarp.spec.in');
const nodePin = read('packaging/node-runtime.env');

// Matrix jobs must not upload immutable artifacts under the same name.
assert.ok(read('.github/workflows/ci.yml').includes(
  'name: upstream-diff-${{ matrix.runner }}-${{ github.sha }}'
));

for (const relative of [
  'script/package-native.sh',
  'script/install-release.sh',
  'script/build-native.sh',
  'packaging/deb/postinst',
  'packaging/deb/prerm',
  'packaging/deb/postrm'
]) {
  const check = spawnSync('bash', ['-n', path.join(root, relative)], {encoding: 'utf8'});
  assert.equal(check.status, 0, `${relative}: ${check.stderr}`);
}

assert.match(packageScript, /Ubuntu 22\.04/);
assert.match(packageScript, /Ubuntu 24\.04/);
assert.match(packageScript, /Fedora 44/);
assert.match(packageScript, /artifact was not built natively on this exact distribution and architecture/);
assert.match(packageScript, /dpkg-shlibdeps/);
assert.match(packageScript, /rpmbuild/);
assert.match(packageScript, /dpkg-deb --build --root-owner-group/);
assert.match(packageScript, /rpm -qp --qf/);
assert.match(packageScript, /install-native\.sh.*--artifact.*--destdir/s);
assert.match(packageScript, /visudo -cf/);
assert.match(packageScript, /v24\.21\.0/);
assert.match(packageScript, /Node runtime architecture mismatch/);
assert.match(packageScript, /bin\/blah2-gpu-vulkan\.so/);
assert.match(packageScript, /RPM incorrectly depends on system Node/);
assert.match(packageScript, /api\/node_modules\/\.bin/);
assert.match(packageScript, /refusing to replace existing output/);
assert.match(packageScript, /ln "\$WORK_DIR\/\$ASSET" "\$OUTPUT_DIR\/\$ASSET"/);
assert.doesNotMatch(packageScript, /apt(-get)? install|dnf install|curl |wget /);

assert.match(nodePin, /^NODE_VERSION=24\.21\.0$/m);
assert.match(nodePin, /^NODE_X64_SHA256=[0-9a-f]{64}$/m);
assert.match(nodePin, /^NODE_ARM64_SHA256=[0-9a-f]{64}$/m);
assert.match(buildScript, /build_os_id=/);
assert.match(buildScript, /build_os_version=/);
assert.match(buildScript, /build_arch=/);
assert.match(buildScript, /licenses\/VkFFT\.txt/);
assert.match(buildScript, /find .*api.*-name '\*\.test\.js' -delete/);
assert.match(buildScript, /UI_CONFIG_REVIEW\.md/);
assert.match(buildScript, /REAL_IQ_BENCHMARK_PLAN\.md/);

assert.match(debPostinst, /systemd-sysusers/);
assert.match(debPostinst, /systemd-tmpfiles/);
assert.doesNotMatch(debPostinst, /systemctl (enable|start|restart)/);
assert.match(rpmSpec, /%config\(noreplace\).*\/etc\/vectorwarp\/config\.yml/);
assert.doesNotMatch(rpmSpec.match(/%post\n([\s\S]*?)\n%postun/)[1],
  /systemctl (enable|start|restart)/);

assert.match(releaseInstaller, /EXPECTED_FINGERPRINT=@SIGNING_FINGERPRINT@/);
assert.match(releaseInstaller, /\[0-9A-F\]\{40\}/);
assert.match(releaseInstaller, /Signed-By: \/usr\/share\/keyrings\/vectorwarp-archive-keyring\.gpg/);
assert.match(releaseInstaller, /gpgcheck=1/);
assert.match(releaseInstaller, /repo_gpgcheck=1/);
assert.doesNotMatch(releaseInstaller, /trusted=yes|no-gpg-check|gpgcheck=0/);
assert.match(releaseInstaller, /--start-web/);
assert.match(releaseInstaller, /systemctl enable --now vectorwarp-api\.service/);
assert.doesNotMatch(releaseInstaller, /systemctl enable --now vectorwarp-processor\.service/);

console.log('Native package acceptance passed: native distro checks, pinned runtime, preserved config, signed repositories and no automatic radar start.');
