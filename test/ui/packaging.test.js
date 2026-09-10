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
  'script/smoke-native-package.sh',
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
assert.match(packageScript, /Ubuntu 26\.04/);
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

const packageSmoke = read('script/smoke-native-package.sh');
assert.match(packageSmoke, /apt-get --yes install/);
assert.match(packageSmoke, /dnf --assumeyes install/);
assert.match(packageSmoke, /opt\/vectorwarp\/runtime\/node\/bin\/node/);
assert.match(packageSmoke, /\/api\/system\/status/);
assert.match(packageSmoke, /\/api\/config\/capabilities/);
assert.match(packageSmoke, /runuser --user vectorwarp-api/);
assert.match(packageSmoke, /--supp-group vectorwarp-config/);
assert.match(packageSmoke, /exec sudo -- bash "\$0" "\$@"/);
assert.match(packageSmoke, /\/display\/configuration\//);
assert.doesNotMatch(packageSmoke, /systemctl|vectorwarp-processor/);
const releaseWorkflow = read('.github/workflows/release-packages.yml');
assert.equal((releaseWorkflow.match(/bash script\/smoke-native-package\.sh/g) || []).length, 3,
  'the Ubuntu 24, Ubuntu 22/26, and Fedora target groups must smoke every native package');

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

// These are os-release metadata fixtures, not DragonOS ISO boots. Sourcing is
// deliberate: install-release.sh returns before parsing arguments or touching
// the host when it is not its own process.
function detectPlatform(id, version, ubuntuCodename, versionCodename, machine) {
  const command = [
    'source "$1"',
    'detect_platform "$2" "$3" "$4" "$5" "$6"',
    'printf "%s/%s/%s" "$manager" "$codename" "$package_arch"'
  ].join('; ');
  return spawnSync('bash', ['-c', command, 'packaging-detector',
    path.join(root, 'script/install-release.sh'), id, version, ubuntuCodename,
    versionCodename, machine], {encoding: 'utf8'});
}

for (const [id, version, ubuntuCodename, versionCodename, machine, expected] of [
  ['ubuntu', '22.04', 'jammy', 'jammy', 'x86_64', 'apt/jammy/amd64'],
  ['ubuntu', '22.04', 'jammy', 'jammy', 'aarch64', 'apt/jammy/arm64'],
  ['ubuntu', '24.04', 'noble', 'noble', 'x86_64', 'apt/noble/amd64'],
  ['ubuntu', '24.04', 'noble', 'noble', 'arm64', 'apt/noble/arm64'],
  ['ubuntu', '26.04', 'resolute', 'resolute', 'x86_64', 'apt/resolute/amd64'],
  ['ubuntu', '26.04', 'resolute', 'resolute', 'arm64', 'apt/resolute/arm64'],
  ['fedora', '44', '', '', 'x86_64', 'dnf//x86_64'],
  ['fedora', '44', '', '', 'aarch64', 'dnf//aarch64'],
  ['dragonos-focalx', 'FocalX', 'jammy', 'jammy', 'x86_64', 'apt/jammy/amd64'],
  ['dragonos-focalx', 'FocalX', 'jammy', 'jammy', 'arm64', 'apt/jammy/arm64'],
  ['dragonos-noble', 'Noble', 'noble', 'noble', 'x86_64', 'apt/noble/amd64'],
  ['dragonos-noble', 'Noble', 'noble', 'noble', 'arm64', 'apt/noble/arm64'],
  ['dragonos-resolute', 'R1', 'noble', 'dragonos-r1', 'x86_64', 'apt/noble/amd64'],
  ['dragonos', 'R1', 'resolute', '', 'x86_64', 'apt/resolute/amd64'],
  ['dragonos-resolute', 'R1', '', 'resolute', 'arm64', 'apt/resolute/arm64'],
  ['dragonos-noble', '24.04', '', '', 'x86_64', 'apt/noble/amd64']
]) {
  const actual = detectPlatform(id, version, ubuntuCodename, versionCodename, machine);
  assert.equal(actual.status, 0, `${id}/${version}: ${actual.stderr}`);
  assert.equal(actual.stdout, expected);
}
for (const fixture of [
  ['ubuntu', '20.04', 'focal', 'focal', 'x86_64'],
  ['ubuntu', '27.04', 'questing', 'questing', 'x86_64'],
  ['linuxmint', '22', 'noble', 'noble', 'x86_64'],
  ['dragonos', 'R1', 'focal', '', 'x86_64'],
  ['dragonos-resolute', 'R1', '', '', 'x86_64'],
  ['dragonos-resolute', 'R1', 'r1', '', 'x86_64'],
  ['dragonos', '26.04', 'noble', '', 'x86_64'],
  ['dragonos', 'R1', 'resolute', 'noble', 'x86_64'],
  ['ubuntu', '24.04', 'jammy', 'jammy', 'x86_64'],
  ['fedora', '44', '', '', 'riscv64']
]) {
  const actual = detectPlatform(...fixture);
  assert.notEqual(actual.status, 0, `unexpected supported fixture: ${fixture.join('/')}`);
  assert.equal(actual.stdout, '', `rejected fixture wrote stdout: ${fixture.join('/')}`);
  assert.match(actual.stderr, /install-release: .+/, `missing rejection: ${fixture.join('/')}`);
}

console.log('Native package acceptance passed: native distro checks, pinned runtime, preserved config, signed repositories and no automatic radar start.');
