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

assert.match(buildScript, /cmake -G Ninja/);
assert.match(buildScript, /arm\*\|aarch64\|s390x\|ppc64le\|riscv\*/);
assert.match(buildScript, /VCPKG_FORCE_SYSTEM_BINARIES=1/);
assert.match(buildScript, /vcpkg_cmake_prefix=\(env CMAKE_POLICY_VERSION_MINIMUM=3\.5\)/);
assert.match(buildScript, /native-compiler-aliases/);
assert.match(buildScript, /-dumpmachine/);
assert.match(buildScript, /does not match host/);
assert.doesNotMatch(read('cmake/RapidJson.cmake'), /Wno-error=template-body/);
for (const manifestName of ['lib/vcpkg.json', 'lib/vcpkg-kraken.json']) {
  const manifest = JSON.parse(read(manifestName));
  assert.deepEqual(manifest.dependencies.find(({name}) => name === 'rapidjson'),
    {name: 'rapidjson', 'version>=': '2023-07-17'});
  assert.deepEqual(manifest.overrides.find(({name}) => name === 'rapidjson'),
    {name: 'rapidjson', 'version-date': '2023-07-17'});
}

const receiverBuildCheck = spawnSync('python3', [path.join(root,
  'test/packaging/test_receiver_build.py')], {encoding: 'utf8'});
assert.equal(receiverBuildCheck.status, 0,
  `receiver build/install contract: ${receiverBuildCheck.stdout}${receiverBuildCheck.stderr}`);
const receiverSmokeCheck = spawnSync('python3', [path.join(root,
  'test/packaging/test_smoke_receiver_status.py')], {encoding: 'utf8'});
assert.equal(receiverSmokeCheck.status, 0,
  `receiver smoke schema: ${receiverSmokeCheck.stdout}${receiverSmokeCheck.stderr}`);
const sdkBuildCheck = spawnSync('python3', [path.join(root,
  'test/packaging/test_sdrplay_build_sdk.py')], {encoding: 'utf8'});
assert.equal(sdkBuildCheck.status, 0,
  `build-only SDK consent/extraction: ${sdkBuildCheck.stdout}${sdkBuildCheck.stderr}`);
const gpuSetupCheck = spawnSync('python3', [path.join(root,
  'test/packaging/test_gpu_setup.py')], {encoding: 'utf8'});
assert.equal(gpuSetupCheck.status, 0,
  `Pi-only signed-native GPU setup/access contract: ${gpuSetupCheck.stdout}${gpuSetupCheck.stderr}`);
assert.match(packageScript, /all receiver adapters in one build/);
assert.match(packageScript, /--test-only requires the exact open-test Kraken, UHD, and HackRF artifact/);
assert.match(packageScript, /published package receiver manifest is incomplete/);
assert.match(packageScript, /"test_only": %s/);
assert.match(packageScript, /It deliberately excludes RSPduo and is not for publication/);
assert.match(packageScript, /\^the separately installed SDRplay API/);
assert.match(buildScript, /open-test\)/);
assert.match(buildScript, /COMPILED_RECEIVERS=Usrp,HackRF,Kraken; TEST_ONLY=true/);
assert.match(buildScript, /test_only=%s/);
assert.match(packageScript, /must not contain the SDRplay vendor SDK or runtime/);
assert.match(rpmSpec, /__requires_exclude.*libsdrplay_api/);
const receiverModules = read('cmake/ReceiverModules.cmake');
assert.doesNotMatch(receiverModules, /INSTALL_RPATH[^\n]*\/usr\/local\/lib/);
assert.doesNotMatch(rpmSpec, /QA_RPATHS|__brp_check_rpaths/,
  'Universal packages must retain the normal RPM RPATH checks');
assert.match(read('src/capture/ReceiverLoader.cpp'),
  /open_receiver_library\(path, std::strcmp\(module.receiver, "RspDuo"\) == 0 \?\s*"\/usr\/local\/lib\/libsdrplay_api.so.3.15" : nullptr, "libsdrplay_api.so.3"\)/,
  'Only RSPduo may use the exact fixed vendor-library fallback');

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
assert.match(packageScript, /Debian 13 Trixie/);
assert.match(packageScript, /Fedora 44/);
assert.match(packageScript, /artifact was not built natively on this exact distribution and architecture/);
assert.match(packageScript, /dpkg-shlibdeps/);
assert.match(packageScript, /rpmbuild/);
assert.match(packageScript, /dpkg-deb --build --root-owner-group/);
assert.match(packageScript, /dpkg-deb -f "\$WORK_DIR\/\$ASSET" Package/);
assert.match(packageScript, /dpkg-deb -f "\$WORK_DIR\/\$ASSET" Version/);
assert.match(packageScript, /dpkg-deb -f "\$WORK_DIR\/\$ASSET" Architecture/);
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
assert.match(packageSmoke, /exec sudo -- bash "\$0" --test-only --package "\$package_arg"/);
assert.match(packageSmoke, /test-only smoke requires the exact open-test receiver metadata/);
assert.match(packageSmoke, /stable smoke requires the all-receiver package metadata/);
assert.match(packageSmoke, /\/display\/configuration\//);
assert.doesNotMatch(packageSmoke, /systemctl|vectorwarp-processor/);
const releaseWorkflow = read('.github/workflows/release-packages.yml');
const releaseCommands = releaseWorkflow.replace(/\\\n\s*/g, ' ').split('\n');
const debHackrfInstalls = releaseCommands.filter(line => /apt-get install/.test(line) && /\blibhackrf-dev\b/.test(line));
const rpmHackrfInstalls = releaseCommands.filter(line => /dnf --assumeyes install/.test(line) && /\bhackrf-devel\b/.test(line));
assert.doesNotMatch(releaseWorkflow, /\blibhackrf-devel\b/, 'Fedora calls its SDK package hackrf-devel');
assert.ok(debHackrfInstalls.length >= 3, 'Every Debian-family build path needs the complete SDK');
assert.ok(rpmHackrfInstalls.length >= 1, 'Every Fedora build path needs the complete SDK');
for (const command of debHackrfInstalls) {
  assert.match(command, /\blibusb-1\.0-0-dev\b/, 'HackRF pkg-config exposes libusb headers on Debian-family builds');
  assert.match(command, /\blibuhd-dev\b/);
  assert.match(command, /\buhd-host\b/, 'The native build preflight also requires uhd_config_info');
  assert.match(command, /\blibboost-dev\b/, 'UHD public headers require the Boost development headers');
}
for (const command of rpmHackrfInstalls) {
  assert.match(command, /\blibusb1-devel\b/, 'Fedora must also declare the transitive development dependency');
  assert.match(command, /\buhd-devel\b/);
  assert.match(command, /\bboost-devel\b/, 'Fedora UHD builds require the Boost development headers');
}
const sourceSetup = read('docs/SETUP.md').replace(/\\\n\s*/g, ' ');
assert.match(sourceSetup, /sudo apt install[^\n]*\blibuhd-dev\b[^\n]*\blibboost-dev\b[^\n]*\blibhackrf-dev\b[^\n]*\blibusb-1\.0-0-dev\b/,
  'The source quickstart must include both SDK header dependencies');
assert.match(sourceSetup, /sudo dnf install[^\n]*\buhd-devel\b[^\n]*\bboost-devel\b[^\n]*\bhackrf-devel\b[^\n]*\blibusb1-devel\b/);
assert.doesNotMatch(sourceSetup, /\blibhackrf-devel\b/);
const workflowCheck = spawnSync('node', ['-e',
  "const fs=require('fs'),yaml=require('js-yaml');process.stdout.write(JSON.stringify(yaml.load(fs.readFileSync(process.argv[1],'utf8'))));",
  path.join(root, '.github/workflows/release-packages.yml')], {cwd: path.join(root, 'api'), encoding: 'utf8'});
assert.equal(workflowCheck.status, 0, workflowCheck.stderr);
const packageJob = JSON.parse(workflowCheck.stdout).jobs.package;
const prPackageSteps = packageJob.steps.filter(step => /PR verification/.test(step.name || ''));
const trustedPackageSteps = packageJob.steps.filter(step =>
  /github\.event_name != 'pull_request'/.test(step.if || '') && /Build native/.test(step.name || ''));
assert.equal(prPackageSteps.length, 3, 'PRs must smoke Debian, Debian-family, and Fedora package groups');
assert.equal(trustedPackageSteps.length, 3, 'Trusted builds retain all three package groups');
assert.equal(prPackageSteps.filter(step => /--backend open-test/.test(step.run || '') &&
  /script\/package-native\.sh --test-only/.test(step.run || '') &&
  /smoke-native-package\.sh --test-only/.test(step.run || '')).length, 3,
  'every PR package group must compile, package, and smoke only open-test');
assert.equal(trustedPackageSteps.filter(step => /--backend all/.test(step.run || '') &&
  /smoke-native-package\.sh --package/.test(step.run || '') &&
  !/smoke-native-package\.sh --test-only/.test(step.run || '')).length, 3,
  'every trusted package group must compile and smoke the stable all-adapter build');
function matrixEntries(workflow) {
  return [...workflow.matchAll(/^\s+- \{([^}]+)\}$/gm)].map(match => Object.fromEntries(
    match[1].split(',').map(field => field.trim().split(/:\s+/, 2))));
}
function supportRows(markdown) {
  const lines = markdown.split('\n');
  const header = lines.findIndex(line => line === '| Operating system | Versions | Architectures | Package |');
  assert.notEqual(header, -1, 'README must contain the OS support table');
  const rows = [];
  for (let index = header + 2; index < lines.length && lines[index].startsWith('|'); index++) {
    const cells = lines[index].split('|').slice(1, -1).map(cell => cell.trim());
    rows.push({os: cells[0], versions: cells[1].split(',').map(value => value.trim()),
      architectures: cells[2].split(',').map(value => value.trim()), package: cells[3]});
  }
  return rows;
}
function nativeTarget(entry) {
  const ubuntu = /^ubuntu(22\.04|24\.04|26\.04)$/.exec(entry.distro);
  if (ubuntu) return {os: 'Ubuntu', version: ubuntu[1], format: 'deb'};
  if (entry.distro === 'debian13') return {os: 'Debian', version: '13 (Trixie)', format: 'deb'};
  if (entry.distro === 'fedora44') return {os: 'Fedora', version: '44', format: 'rpm'};
  throw new Error(`release target ${entry.distro} has no README support-table mapping`);
}
const architectureLabels = Object.freeze({
  amd64: 'x86-64 (amd64 / x86_64)',
  x86_64: 'x86-64 (amd64 / x86_64)',
  arm64: 'ARM64 (arm64 / aarch64)',
  aarch64: 'ARM64 (arm64 / aarch64)'
});
function assertMatrixDocumented(entries, rows) {
  assert.ok(entries.length, 'release package matrix must not be empty');
  assert.equal(new Set(entries.map(entry => `${entry.distro}/${entry.format}/${entry.arch}`)).size, entries.length,
    'release package matrix must not duplicate a target');
  for (const entry of entries) {
    const target = nativeTarget(entry);
    const row = rows.find(candidate => candidate.os === target.os);
    assert.ok(row, `README lacks an OS support row for ${target.os}`);
    assert.ok(row.versions.includes(target.version), `README lacks ${target.os} ${target.version}`);
    assert.ok(Object.hasOwn(architectureLabels, entry.arch) &&
      row.architectures.includes(architectureLabels[entry.arch]),
    `README lacks ${target.os} ${entry.arch}`);
    assert.match(row.package, target.format === 'deb' ? /DEB/ : /RPM/,
      `README package column disagrees with ${entry.distro} format`);
  }
}
const nativeMatrix = matrixEntries(releaseWorkflow).filter(entry => entry.distro && entry.format && entry.arch);
const readmeRows = supportRows(read('README.md'));
assert.equal(nativeMatrix.length, 10, 'release matrix must build all ten native package targets');
assertMatrixDocumented(nativeMatrix, readmeRows);
assert.throws(() => assertMatrixDocumented([...nativeMatrix, {...nativeMatrix[0], distro: 'ubuntu27.04'}], readmeRows),
  /support-table mapping/);
assert.throws(() => assertMatrixDocumented([...nativeMatrix, {...nativeMatrix[0], arch: 'riscv64'}], readmeRows),
  /lacks Ubuntu riscv64/);
assert.throws(() => assertMatrixDocumented(nativeMatrix, readmeRows.filter(row => row.os !== 'Debian')),
  /lacks an OS support row for Debian/);
for (const os of ['Ubuntu', 'Debian', 'Fedora', 'DragonOS'])
  assert.deepEqual(readmeRows.find(row => row.os === os)?.architectures,
    [architectureLabels.amd64, architectureLabels.arm64],
    `${os} must use the same public architecture labels`);
const piRow = readmeRows.find(row => row.os === 'Raspberry Pi OS');
assert.deepEqual(piRow?.architectures, [architectureLabels.arm64]);
assert.deepEqual(piRow.versions, ['Trixie', '64-bit']);
assert.match(piRow.package, /Debian 13 ARM64 DEB/);
assertMatrixDocumented(nativeMatrix.map(entry => ({...entry,
  arch: ({amd64: 'x86_64', x86_64: 'amd64', arm64: 'aarch64', aarch64: 'arm64'})[entry.arch]
})), readmeRows);
assert.throws(() => assertMatrixDocumented(nativeMatrix, readmeRows.map(row =>
  row.os === 'Fedora' ? {...row, architectures: ['x86_64', 'aarch64']} : row)),
  /lacks Fedora/, 'Native-only labels must not reintroduce inconsistent documentation');
for (const file of ['README.md', 'docs/INSTALL.md', 'docs/MAINTAINER_RELEASE.md',
  'packaging/README.md', 'docs/UPSTREAM_COMPARISON.md']) {
  const document = read(file);
  assert.ok(document.includes(architectureLabels.amd64), `${file}: explain both x86-64 aliases`);
  assert.ok(document.includes(architectureLabels.arm64), `${file}: explain both ARM64 aliases`);
}
assert.match(releaseWorkflow, /expected ten package manifests/);
assert.match(releaseWorkflow, /-eq 10/);
assert.match(releaseWorkflow, /Stable tags must point to the exact current main commit/);
assert.match(releaseWorkflow, /Unsigned test packages must be built from the exact current main commit/);
assert.match(releaseWorkflow, /'version': version, 'source_commit': source_commit\.lower\(\)/);
assert.match(releaseWorkflow, /--expected-source-commit .*--require-release-matrix/s);
assert.match(releaseWorkflow, /SHA256SUMS\.asc/);
assert.match(releaseWorkflow, /gpgv --keyring repository\/keys\/vectorwarp\.gpg/);
const publishWorkflow = read('.github/workflows/publish-package-repository.yml');
assert.match(publishWorkflow, /Release assets do not match the immutable release tag/);
assert.match(publishWorkflow, /--expected-version .*--expected-source-commit .*--require-release-matrix/s);
assert.match(publishWorkflow, /cmp packages\/vectorwarp-archive-key\.asc packaging\/keys\/vectorwarp\.asc/);
assert.match(publishWorkflow, /gpgv --keyring .*SHA256SUMS\.asc packages\/SHA256SUMS/s);

assert.match(nodePin, /^NODE_VERSION=24\.21\.0$/m);
assert.match(nodePin, /^NODE_X64_SHA256=[0-9a-f]{64}$/m);
assert.match(nodePin, /^NODE_ARM64_SHA256=[0-9a-f]{64}$/m);
assert.match(buildScript, /build_os_id=/);
assert.match(buildScript, /build_os_version=/);
assert.match(buildScript, /build_arch=/);
assert.match(buildScript, /for command in .*ninja/);
assert.match(buildScript, /env CMAKE_POLICY_VERSION_MINIMUM=3\.5/);
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
function detectPlatform(id, version, ubuntuCodename, versionCodename, machine,
  dpkgArch = (machine === 'x86_64' ? 'amd64' : 'arm64'), variantId = '') {
  const command = [
    'source "$1"',
    'detect_platform "$2" "$3" "$4" "$5" "$6" "$7" "$8"',
    'printf "%s/%s/%s" "$manager" "$codename" "$package_arch"'
  ].join('; ');
  return spawnSync('bash', ['-c', command, 'packaging-detector',
    path.join(root, 'script/install-release.sh'), id, version, ubuntuCodename,
    versionCodename, machine, dpkgArch, variantId], {encoding: 'utf8'});
}

for (const [id, version, ubuntuCodename, versionCodename, machine, expected, dpkgArch, variantId] of [
  ['ubuntu', '22.04', 'jammy', 'jammy', 'x86_64', 'apt/jammy/amd64'],
  ['ubuntu', '22.04', 'jammy', 'jammy', 'aarch64', 'apt/jammy/arm64'],
  ['ubuntu', '24.04', 'noble', 'noble', 'x86_64', 'apt/noble/amd64'],
  ['ubuntu', '24.04', 'noble', 'noble', 'arm64', 'apt/noble/arm64'],
  ['ubuntu', '26.04', 'resolute', 'resolute', 'x86_64', 'apt/resolute/amd64'],
  ['ubuntu', '26.04', 'resolute', 'resolute', 'arm64', 'apt/resolute/arm64'],
  ['debian', '13', '', 'trixie', 'x86_64', 'apt/trixie/amd64'],
  ['debian', '13', '', 'trixie', 'arm64', 'apt/trixie/arm64'],
  ['raspbian', '13', '', 'trixie', 'aarch64', 'apt/trixie/arm64'],
  ['raspbian', '13', '', 'trixie', 'arm64', 'apt/trixie/arm64'],
  ['debian', '13', '', 'trixie', 'aarch64', 'apt/trixie/arm64', 'arm64', 'raspbian'],
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
  const actual = detectPlatform(id, version, ubuntuCodename, versionCodename, machine, dpkgArch, variantId);
  assert.equal(actual.status, 0, `${id}/${version}: ${actual.stderr}`);
  assert.equal(actual.stdout, expected);
}
for (const fixture of [
  ['ubuntu', '20.04', 'focal', 'focal', 'x86_64'],
  ['ubuntu', '27.04', 'questing', 'questing', 'x86_64'],
  ['debian', '12', '', 'bookworm', 'x86_64'],
  ['debian', '13', '', 'bookworm', 'x86_64'],
  ['raspbian', '13', '', 'trixie', 'armv7l'],
  ['raspbian', '13', '', 'trixie', 'aarch64', 'armhf'],
  ['debian', '13', '', 'trixie', 'x86_64', 'amd64', 'raspbian'],
  ['raspbian', '12', '', 'trixie', 'aarch64'],
  ['raspbian', '13', '', 'bookworm', 'aarch64'],
  ['kali', '13', '', 'trixie', 'aarch64'],
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
