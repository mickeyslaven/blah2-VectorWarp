'use strict';
// macOS local RSPduo adapter builder: only an app-published source kit and a
// manually installed SDK named by the launcher environment are accepted.
const crypto = require('crypto');
const fs = require('fs');
const os = require('os');
const path = require('path');
const {spawnSync} = require('child_process');
const fail = message => { throw new Error(message); };
const hash = file => crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const regular = file => { const info = fs.lstatSync(file); return info.isFile() && !info.isSymbolicLink() && info.size > 0 && info.size <= 32 * 1024 * 1024; };
const absolute = value => typeof value === 'string' && path.isAbsolute(value) && !value.includes('\0');
function under(value, parent) { const resolved = fs.realpathSync(value), root = fs.realpathSync(parent); return resolved === root || resolved.startsWith(root + path.sep); }
function run(file, args, options = {}) {
  const result = spawnSync(file, args, {cwd: '/', encoding: 'utf8', timeout: 120000, maxBuffer: 65536,
    env: {PATH: '/usr/bin:/bin', LANG: 'C', LC_ALL: 'C'}, ...options});
  if (result.error || result.status !== 0) fail(`Adapter compilation failed: ${(result.stderr || result.stdout || result.error?.message || '').replace(/[\x00-\x1f\x7f]/g, ' ').slice(-1200)}`);
  return result.stdout.trim();
}
function main(env = process.env) {
  const root = env.VECTORWARP_MACOS_ROOT;
  const state = env.VECTORWARP_MACOS_STATE;
  const include = env.BLAH2_SDRPLAY_INCLUDE_DIR;
  const library = env.BLAH2_SDRPLAY_LIBRARY;
  if (![root, state, include, library].every(absolute)) fail('The launcher must provide absolute root, state, and manual SDRplay SDK paths.');
  const home = path.join(os.homedir(), 'Library', 'Application Support', 'VectorWarp');
  if (!under(state, home) && fs.realpathSync(state) !== fs.realpathSync(home)) fail('The local adapter state must remain under VectorWarp Application Support.');
  const kit = path.join(root, 'receiver-source', 'rspduo');
  const manifestPath = path.join(kit, 'kit.json');
  const core = fs.realpathSync(path.join(root, 'bin', 'libblah2-capture-core.dylib'));
  const header = fs.realpathSync(path.join(include, 'sdrplay_api.h'));
  const runtime = fs.realpathSync(library);
  if (![manifestPath, core, header, runtime].every(regular)) fail('The published source kit or manually installed SDRplay API is incomplete.');
  const manifest = JSON.parse(fs.readFileSync(manifestPath, 'utf8'));
  const required = ['schema', 'receiver', 'kit_id', 'cohort', 'compiler', 'sources', 'core_sha256'];
  if (Object.keys(manifest).sort().join() !== required.sort().join() || manifest.schema !== 1 || manifest.receiver !== 'RspDuo' ||
      !/^[a-f0-9]{64}$/.test(manifest.kit_id) || !/^[a-f0-9]{64}$/.test(manifest.cohort) || hash(core) !== manifest.core_sha256)
    fail('The published RSPduo source kit is invalid. Reinstall VectorWarp.');
  if (manifest.compiler?.id !== 'AppleClang' || !Array.isArray(manifest.compiler.cxx_flags) ||
      !manifest.compiler.cxx_flags.every(flag => /^-[A-Za-z0-9_+=,./: -]{1,240}$/.test(flag))) fail('The source-kit compiler contract is invalid.');
  for (const [name, digest] of Object.entries(manifest.sources || {})) {
    if (!/^[a-f0-9]{64}$/.test(digest) || name.startsWith('/') || name.split('/').includes('..')) fail('The source-kit manifest is invalid.');
    const file = path.join(kit, name); if (!under(file, kit) || !regular(file) || hash(file) !== digest) fail(`Source kit verification failed: ${name}`);
  }
  const clang = fs.realpathSync(run('/usr/bin/xcrun', ['--find', 'clang']));
  if (!absolute(clang) || !fs.statSync(clang).isFile()) fail('Apple clang is unavailable. Install Xcode Command Line Tools yourself.');
  const target = run(clang, ['-dumpmachine']);
  if (target !== manifest.compiler.target) fail('Apple clang target differs from this VectorWarp source kit. Reinstall the matching package.');
  const base = path.join(state, 'adapters', 'rspduo'); fs.mkdirSync(base, {recursive: true, mode: 0o700});
  if (!under(base, state)) fail('Adapter publication path is invalid.');
  const work = fs.mkdtempSync(path.join(base, '.build-'), {encoding: 'utf8'});
  try {
    const sdkBefore = {include: hash(header), library: hash(runtime)};
    const output = path.join(work, 'blah2-receiver-rspduo.dylib');
    const sdkroot = run('/usr/bin/xcrun', ['--show-sdk-path']);
    const args = ['-std=c++17', '-isysroot', sdkroot, '-dynamiclib', '-fvisibility=hidden', '-fvisibility-inlines-hidden', '-DBLAH2_MODULE_RSPDUO=1',
      ...manifest.compiler.cxx_flags, '-I', path.join(kit, 'src'), '-I', path.join(kit, 'generated'), '-I', include,
      path.join(kit, 'src/capture/ReceiverFactory.cpp'), path.join(kit, 'src/capture/rspduo/RspDuo.cpp'), core, runtime,
      `-Wl,-rpath,${path.join(root, 'bin')}`, `-Wl,-rpath,${path.dirname(runtime)}`, '-Wl,-undefined,dynamic_lookup', '-o', output];
    run(clang, args);
    if (!regular(output)) fail('Compiler output is not a bounded regular adapter.');
    // Inputs may not change between verification and publication.
    if (hash(core) !== manifest.core_sha256 || sdkBefore.include !== hash(header) || sdkBefore.library !== hash(runtime) || !Object.entries(manifest.sources).every(([name, digest]) => hash(path.join(kit, name)) === digest))
      fail('Source kit changed during compilation; no adapter was published.');
    const receipt = {schema: 1, kit_id: manifest.kit_id, cohort: manifest.cohort, core_sha256: manifest.core_sha256,
      sdk: sdkBefore, module_sha256: hash(output)};
    const generations = path.join(base, 'generations'); fs.mkdirSync(generations, {recursive: true, mode: 0o700});
    const generationInfo = fs.lstatSync(generations);
    if (!under(generations, state) || generationInfo.isSymbolicLink() || !generationInfo.isDirectory() || generationInfo.uid !== process.getuid() || (generationInfo.mode & 0o022))
      fail('Adapter generation directory is unsafe.');
    // A generation name is intentionally fresh even when inputs match: a
    // malformed or interrupted prior publication is never overwritten, and
    // `current` changes only after this complete generation is durable.
    const id = crypto.createHash('sha256').update(JSON.stringify(receipt)).update(crypto.randomBytes(32)).digest('hex'); const generation = path.join(generations, id);
    if (!fs.existsSync(generation)) { fs.mkdirSync(generation, {mode: 0o700}); fs.renameSync(output, path.join(generation, path.basename(output))); fs.writeFileSync(path.join(generation, 'receipt.json'), JSON.stringify(receipt)); }
    const temporary = path.join(base, `.current-${process.pid}-${Date.now()}`); fs.symlinkSync(path.join('generations', id), temporary); fs.renameSync(temporary, path.join(base, 'current'));
    process.stdout.write(JSON.stringify({ok: true, state: 'current', kit_id: manifest.kit_id}) + '\n');
  } finally { fs.rmSync(work, {recursive: true, force: true}); }
}
try { main(); } catch (error) { process.stderr.write(`${error.message}\n`); process.exitCode = 1; }
module.exports = {main};
