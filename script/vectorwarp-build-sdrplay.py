#!/usr/bin/python3 -I
"""Build the locally installed SDRplay adapter from a root-published source kit.

This never obtains vendor software or accepts a vendor licence.  It deliberately
accepts no paths, flags, service names, or URLs from a caller.
"""
import ctypes, fcntl, glob, hashlib, json, os, pathlib, pwd, re, resource, selectors, signal, stat, subprocess, sys, tempfile, time

PREFIX = '@PREFIX@'
KIT = pathlib.Path(PREFIX) / 'current/receiver-source/rspduo'
MANIFEST = KIT / 'kit.json'
CORE = pathlib.Path(PREFIX) / 'current/bin/libblah2-capture-core.so.1'
INCLUDE = pathlib.Path('/usr/local/include')
LIBRARY = pathlib.Path('/usr/local/lib/libsdrplay_api.so.3.15')
OUTROOT = pathlib.Path('/var/lib/vectorwarp-adapters/rspduo')
LOCK = pathlib.Path('/run/vectorwarp-rspduo-build.lock')
PROGRESS = pathlib.Path('/run/vectorwarp-sdrplay-build/status.json')
COMPILER = '/usr/bin/c++'
BUILD_USER = 'vectorwarp-build'
MAX_FILE = 16 * 1024 * 1024
MAX_OUTPUT = 65536
REQUIRED = {'src/capture/ReceiverFactory.cpp', 'src/capture/ReceiverModule.h',
            'src/capture/Source.h', 'src/capture/Recording.h',
            'src/capture/kraken/HeimdallFrame.h', 'src/data/IqData.h',
            'src/capture/rspduo/RspDuo.cpp', 'src/capture/rspduo/RspDuo.h',
            'src/capture/rspduo/SampleSequence.h', 'generated/ReceiverCohort.h', 'LICENSE'}

class Refused(Exception): pass
def require(ok, message):
    if not ok: raise Refused(message)
def sha(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(65536), b''): h.update(block)
    return h.hexdigest()
def trusted(path, regular=True, symlinks=True):
    """Check each symlink hop, including intermediate targets, before following it."""
    path = pathlib.Path(path)
    require(path.is_absolute(), 'trusted paths must be absolute')
    require(symlinks or not path.is_symlink(), 'unexpected symlink')
    pending = list(path.parts[1:]); resolved = pathlib.Path('/'); links = 0
    root = resolved.stat()
    require(root.st_uid == 0 and not root.st_mode & 0o022, 'filesystem root is untrusted')
    while pending:
        part = pending.pop(0)
        if part in ('', '.'):
            continue
        if part == '..':
            resolved = resolved.parent
            continue
        candidate = resolved / part
        info = candidate.lstat()
        require(info.st_uid == 0 and (stat.S_ISLNK(info.st_mode) or not info.st_mode & 0o022),
                'required installed file is not root-owned and non-writable')
        if stat.S_ISLNK(info.st_mode):
            links += 1; require(links <= 40, 'installed path contains too many symlinks')
            target = pathlib.Path(os.readlink(candidate))
            if target.is_absolute():
                resolved = pathlib.Path('/')
                pending = list(target.parts[1:]) + pending
            else:
                pending = list(target.parts) + pending
        else:
            resolved = candidate
            require(not pending or stat.S_ISDIR(info.st_mode), 'installed path component is not a directory')
    if regular:
        info = resolved.stat(); require(stat.S_ISREG(info.st_mode) and info.st_size <= MAX_FILE,
                                     'required installed file is not a bounded regular file')
    return resolved
def read_json(path):
    trusted(path); raw = pathlib.Path(path).read_bytes(); require(len(raw) <= MAX_FILE, 'kit manifest is oversized')
    def unique(pairs):
        value={}
        for key,item in pairs:
            if key in value: raise ValueError('duplicate JSON key')
            value[key]=item
        return value
    try: return json.loads(raw, object_pairs_hook=unique)
    except ValueError: raise Refused('kit manifest is not valid JSON')
def fixed(command):
    code, output = bounded(command, 5)
    require(code == 0 and len(output) <= 4096, 'local compiler inspection failed')
    return output.decode('utf8', errors='replace').strip()


def reap_children(timeout=3):
    """The standalone helper is a subreaper; no compiler descendant may survive."""
    deadline = time.monotonic() + timeout
    children = pathlib.Path(f'/proc/self/task/{os.getpid()}/children')
    while True:
        pids = [int(value) for value in children.read_text().split()]
        if not pids:
            return
        for pid in pids:
            try:
                descriptor = os.pidfd_open(pid)
                try:
                    signal.pidfd_send_signal(descriptor, signal.SIGKILL)
                finally:
                    os.close(descriptor)
            except ProcessLookupError:
                pass
            try:
                os.waitpid(pid, os.WNOHANG)
            except ChildProcessError:
                pass
        require(time.monotonic() < deadline, 'compiler descendants did not stop; no adapter was published')
        time.sleep(.01)


def bounded(argv, timeout, preexec=None):
    """Bound output/time and reap descendants even when they close stdout or setsid."""
    libc = ctypes.CDLL(None, use_errno=True)
    previous = ctypes.c_int()
    require(libc.prctl(37, ctypes.byref(previous), 0, 0, 0) == 0 and
            libc.prctl(36, 1, 0, 0, 0) == 0, 'cannot isolate compiler descendants')
    child = None
    try:
        child = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, cwd='/',
                                 env={'PATH':'/usr/bin:/bin','LANG':'C','LC_ALL':'C'},
                                 start_new_session=True, preexec_fn=preexec)
        os.set_blocking(child.stdout.fileno(), False)
        with selectors.DefaultSelector() as selector:
            selector.register(child.stdout, selectors.EVENT_READ)
            data = bytearray(); deadline = time.monotonic() + timeout
            while selector.get_map() or child.poll() is None:
                require(time.monotonic() < deadline, 'adapter compilation timed out')
                if not selector.get_map():
                    time.sleep(.01)
                    continue
                for key, _ in selector.select(min(.1, max(0, deadline - time.monotonic()))):
                    block = os.read(key.fd, 4096)
                    if not block:
                        selector.unregister(key.fileobj)
                        continue
                    require(len(data) + len(block) <= MAX_OUTPUT, 'adapter compiler exceeded output limit')
                    data.extend(block)
            code = child.wait(timeout=max(.01, deadline - time.monotonic()))
        return code, bytes(data)
    finally:
        # The helper has no unrelated children. Subreaping also catches a
        # descendant that escaped the original process group with setsid().
        try:
            try:
                if child is not None and child.poll() is None:
                    child.kill()
                    child.wait(timeout=2)
            finally:
                reap_children()
        finally:
            if child is not None:
                child.stdout.close()
            libc.prctl(36, previous.value, 0, 0, 0)
def compiler_identity():
    trusted(COMPILER); version = fixed([COMPILER, '-dumpfullversion', '-dumpversion']).splitlines()[0]
    target = fixed([COMPILER, '-dumpmachine'])
    require(re.fullmatch(r'[0-9]+(?:\.[0-9]+){1,3}', version) and re.fullmatch(r'[A-Za-z0-9_.+-]+', target),
            'local compiler identity is malformed')
    return {'id':'GNU', 'version':version, 'target':target}
def kit_state(check_compiler=False):
    manifest = read_json(MANIFEST)
    require(isinstance(manifest, dict) and set(manifest) == {'schema','receiver','kit_id','cohort','compiler','sources','core_sha256'}, 'kit manifest schema is invalid')
    require(manifest['schema'] == 1 and manifest['receiver'] == 'RspDuo', 'kit is not an RSPduo schema-1 kit')
    for key in ('kit_id','cohort','core_sha256'):
        require(isinstance(manifest[key], str) and re.fullmatch(r'[a-f0-9]{64}', manifest[key]), 'kit identity is invalid')
    c = manifest['compiler']; require(isinstance(c, dict) and set(c) == {'id','version','target','cxx_flags'} and c['id'] == 'GNU' and isinstance(c['cxx_flags'], list) and len(c['cxx_flags']) <= 32, 'kit compiler contract is invalid')
    require(isinstance(c['version'], str) and re.fullmatch(r'[0-9]+(?:\.[0-9]+){1,3}', c['version']) and
            isinstance(c['target'], str) and re.fullmatch(r'[A-Za-z0-9_.+-]+', c['target']), 'kit compiler identity is malformed')
    require(all(isinstance(x,str) and re.fullmatch(r'-[A-Za-z0-9_+=,./:-]{1,240}', x) for x in c['cxx_flags']), 'kit compiler flags are unsafe')
    sources = manifest['sources']; require(isinstance(sources, dict) and set(sources) == REQUIRED, 'kit source list is incomplete')
    for name, digest in sources.items():
        require(isinstance(digest,str) and re.fullmatch(r'[a-f0-9]{64}', digest) and not name.startswith('/') and '..' not in pathlib.PurePosixPath(name).parts, 'kit source digest is invalid')
        file = KIT / name; trusted(file); require(sha(file) == digest, 'kit source changed after publication')
    trusted(CORE); require(sha(CORE) == manifest['core_sha256'], 'capture core differs from the source kit')
    # Read-only status executes no child processes. A verified compiled module
    # remains usable after compiler removal/update; new builds must match.
    if check_compiler:
        require(compiler_identity() == {k:c[k] for k in ('id','version','target')}, 'local compiler does not match the source kit')
    return manifest
def sdk_state():
    require(LIBRARY.is_file(), 'SDRplay API library is missing; install the Hardware API from SDRplay yourself')
    trusted(LIBRARY); headers = sorted(pathlib.Path(x) for x in glob.glob(str(INCLUDE / 'sdrplay_api*.h')))
    require((INCLUDE / 'sdrplay_api.h') in headers and headers, 'SDRplay API headers are missing; install them from SDRplay yourself')
    result = {}
    for h in headers: trusted(h); result[str(h)] = sha(h)
    return {'headers':result, 'library':{'path':str(LIBRARY),'sha256':sha(LIBRARY)}}
def status():
    try:
        kit = kit_state(); sdk = sdk_state(); base = OUTROOT / kit['kit_id']; current=base/'current'; module = current / 'blah2-receiver-rspduo.so'; receipt = current / 'receipt.json'
        trusted(OUTROOT, regular=False)
        if not current.exists() and not current.is_symlink(): return {'ok':True,'state':'missing','kit_id':kit['kit_id'],'cohort':kit['cohort']}
        require(current.is_symlink() and re.fullmatch(r'generations/[a-f0-9]{64}', os.readlink(current)), 'adapter current pointer is invalid')
        if not module.exists() or not receipt.exists(): return {'ok':True,'state':'missing','kit_id':kit['kit_id'],'cohort':kit['cohort']}
        generation = trusted(current, regular=False)
        module = generation / 'blah2-receiver-rspduo.so'; receipt = generation / 'receipt.json'
        trusted(module, symlinks=False); trusted(receipt, symlinks=False)
        data = read_json(receipt)
        expected = {'schema':1,'kit_id':kit['kit_id'],'cohort':kit['cohort'],'core_sha256':kit['core_sha256'],'compiler':kit['compiler'],'sdk':sdk,'module_sha256':sha(module)}
        return {'ok':True,'state':'current' if data == expected else 'stale','kit_id':kit['kit_id'],'cohort':kit['cohort']}
    except (OSError, Refused, ValueError, TypeError, KeyError, subprocess.SubprocessError) as e: return {'ok':False,'state':'unavailable','reason':str(e)[:1000]}
def compile_module(kit, sdk, destination):
    user = pwd.getpwnam(BUILD_USER)
    require(user.pw_uid != 0 and user.pw_gid != 0, 'the dedicated compiler account must not be root')
    def limits():
        os.setgroups([]); os.setgid(user.pw_gid); os.setuid(user.pw_uid)
        resource.setrlimit(resource.RLIMIT_CPU, (90,90)); resource.setrlimit(resource.RLIMIT_AS, (1024*1024*1024,)*2)
        resource.setrlimit(resource.RLIMIT_FSIZE, (32*1024*1024,)*2)
        resource.setrlimit(resource.RLIMIT_NPROC, (32,32))
    argv = [COMPILER, '-std=c++17', '-shared', '-fPIC', '-fvisibility=hidden', '-fvisibility-inlines-hidden', '-DBLAH2_MODULE_RSPDUO=1', *kit['compiler']['cxx_flags'], '-I', str(KIT/'src'), '-I', str(KIT/'generated'), '-I', str(INCLUDE), str(KIT/'src/capture/ReceiverFactory.cpp'), str(KIT/'src/capture/rspduo/RspDuo.cpp'), str(CORE), str(LIBRARY), '-pthread', '-Wl,-z,defs', '-o', str(destination)]
    code, output = bounded(argv, 120, limits)
    require(code == 0, 'adapter compilation failed: ' + output.decode(errors='replace')[-1000:])
def validate_output(path):
    fd=os.open(path, os.O_RDONLY|os.O_NOFOLLOW|os.O_NONBLOCK)
    try:
        before=os.fstat(fd); require(stat.S_ISREG(before.st_mode) and 0 < before.st_size <= 32*1024*1024, 'compiler output is invalid')
        require(os.read(fd,4)==b'\x7fELF','compiler output is not an ELF shared object')
        after=os.fstat(fd); require(after.st_size==before.st_size,'compiler output changed while checked')
    finally: os.close(fd)
    code, text=bounded(['/usr/bin/readelf','-h',str(path)], 5)
    machine={'x86_64':b'Advanced Micro Devices X86-64','aarch64':b'AArch64','arm64':b'AArch64'}.get(os.uname().machine)
    require(machine is not None and code==0 and b'Type:' in text and b'DYN (Shared object file)' in text and machine in text, 'compiler output is not a loadable shared object for this architecture')
def publish(staged, final):
    """Copy only a checked regular compiler result into a root-owned final file."""
    source = os.open(staged, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        info = os.fstat(source)
        require(stat.S_ISREG(info.st_mode) and 0 < info.st_size <= 32 * 1024 * 1024,
                'compiler output is invalid')
        temporary = final / '.module.new'
        target = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o644)
        digest = hashlib.sha256()
        try:
            copied = 0
            while True:
                block = os.read(source, 65536)
                if not block: break
                copied += len(block)
                require(copied <= info.st_size and copied <= 32 * 1024 * 1024,
                        'compiler output grew while copying')
                digest.update(block)
                offset=0
                while offset < len(block):
                    count = os.write(target, block[offset:])
                    require(count > 0, 'compiler output copy failed')
                    offset += count
            os.fsync(target)
            after = os.fstat(source)
            require(copied == info.st_size and
                    (after.st_size, after.st_mtime_ns, after.st_ctime_ns) ==
                    (info.st_size, info.st_mtime_ns, info.st_ctime_ns),
                    'compiler output changed while copying')
        finally: os.close(target)
        validate_output(temporary)  # Root-only staging, never a build-user pathname.
        os.replace(temporary, final / 'blah2-receiver-rspduo.so')
    finally: os.close(source)
    return digest.hexdigest()
def write_receipt(final, receipt):
    fd = os.open(final / 'receipt.json', os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o644)
    with os.fdopen(fd, 'w') as f:
        json.dump(receipt, f, sort_keys=True, separators=(',', ':')); f.flush(); os.fsync(f.fileno())
def receipt_id(receipt):
    return hashlib.sha256(json.dumps(receipt, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
def swap_current(base, generation):
    target = 'generations/' + generation.name
    private = pathlib.Path(tempfile.mkdtemp(prefix='.pointer-', dir=base))
    try:
        temporary = private / 'current'
        os.symlink(target, temporary)
        os.replace(temporary, base / 'current')
        descriptor = os.open(base, os.O_RDONLY | os.O_DIRECTORY)
        try: os.fsync(descriptor)
        finally: os.close(descriptor)
    finally:
        if (private / 'current').is_symlink(): (private / 'current').unlink()
        private.rmdir()
def build():
    require(os.geteuid() == 0, 'adapter build requires the installed root helper')
    trusted(LOCK.parent, regular=False)
    lockfd = os.open(LOCK, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o644)
    with os.fdopen(lockfd, 'r+') as lock:
        info = os.fstat(lock.fileno()); require(stat.S_ISREG(info.st_mode) and info.st_uid == 0 and not info.st_mode & 0o022,
                                                 'adapter build lock is untrusted')
        os.fchmod(lock.fileno(), 0o644)
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        current = status()
        if current.get('state') == 'current': return current
        kit = kit_state(check_compiler=True); sdk = sdk_state(); trusted(OUTROOT, regular=False)
        base = OUTROOT / kit['kit_id']; base.mkdir(mode=0o755, exist_ok=True)
        trusted(base, regular=False, symlinks=False)
        (base/'generations').mkdir(mode=0o755, exist_ok=True)
        trusted(base/'generations', regular=False, symlinks=False)
        work = pathlib.Path(tempfile.mkdtemp(prefix='.rspduo-', dir=OUTROOT)); output = work/'module.so'
        user = pwd.getpwnam(BUILD_USER)
        require(user.pw_uid != 0 and user.pw_gid != 0, 'the dedicated compiler account must not be root')
        os.chown(work, user.pw_uid, user.pw_gid)
        staging = None
        try:
            compile_module(kit, sdk, output)
            # The service intentionally has no DAC override. Reclaim this
            # known directory only after every compiler descendant has exited.
            os.chown(work, 0, 0, follow_symlinks=False)
            work.chmod(0o700)
            require(kit_state(check_compiler=True) == kit and sdk_state() == sdk, 'kit, core, compiler or SDK changed during compilation')
            staging = pathlib.Path(tempfile.mkdtemp(prefix='.staging-', dir=base/'generations'))
            copied = publish(output, staging)
            receipt = {'schema':1,'kit_id':kit['kit_id'],'cohort':kit['cohort'],'core_sha256':kit['core_sha256'],'compiler':kit['compiler'],'sdk':sdk,'module_sha256':copied}
            write_receipt(staging, receipt)
            require(kit_state(check_compiler=True) == kit and sdk_state() == sdk, 'build inputs changed before publication')
            descriptor = os.open(staging, os.O_RDONLY | os.O_DIRECTORY)
            try: os.fsync(descriptor)
            finally: os.close(descriptor)
            final=base/'generations'/receipt_id(receipt)
            if final.exists():
                trusted(final/'blah2-receiver-rspduo.so'); trusted(final/'receipt.json')
                require(read_json(final/'receipt.json')==receipt and sha(final/'blah2-receiver-rspduo.so')==receipt['module_sha256'], 'existing generation is invalid')
            else:
                staging.chmod(0o755)
                os.rename(staging, final)
                staging = None
                directory = os.open(base/'generations', os.O_RDONLY | os.O_DIRECTORY)
                try: os.fsync(directory)
                finally: os.close(directory)
            swap_current(base, final)
            return {'ok':True,'state':'built','kit_id':kit['kit_id']}
        finally:
            if staging is not None:
                for name in ('.module.new', 'blah2-receiver-rspduo.so', 'receipt.json'):
                    try: (staging/name).unlink()
                    except FileNotFoundError: pass
                staging.rmdir()
            try:
                os.chown(work, 0, 0, follow_symlinks=False)
                work.chmod(0o700)
                for p in work.iterdir():
                    if p.is_file() or p.is_symlink(): p.unlink()
                work.rmdir()
            except OSError: pass
def progress(state, reason=None, kit_id=None):
    require(os.geteuid() == 0, 'only the build service may publish progress')
    trusted(PROGRESS.parent, regular=False, symlinks=False)
    value = {'schema':1, 'state':state, 'updatedAt':int(time.time() * 1000)}
    if reason: value['reason'] = str(reason)[:1000]
    if kit_id: value['kit_id'] = kit_id
    descriptor, name = tempfile.mkstemp(prefix='.status-', dir=PROGRESS.parent)
    try:
        with os.fdopen(descriptor, 'w') as stream:
            json.dump(value, stream, sort_keys=True)
            stream.flush(); os.fsync(stream.fileno()); os.fchmod(stream.fileno(), 0o644)
        os.replace(name, PROGRESS)
    finally:
        if os.path.exists(name): os.unlink(name)


def service():
    require(os.geteuid() == 0, 'adapter build service requires root')
    def stopped(signum, frame):
        raise Refused('Local adapter build stopped before completion; retry from Settings')
    previous = signal.signal(signal.SIGTERM, stopped)
    kit_id = None
    try:
        progress('running')
        kit_id = kit_state()['kit_id']
        progress('running', kit_id=kit_id)
        result = build()
        progress('current', kit_id=kit_id)
        return result
    except Exception as error:
        progress('failed', error, kit_id)
        raise
    finally:
        signal.signal(signal.SIGTERM, previous)


def main(argv):
    require(argv in (['status'],['build'],['service']), 'usage: vectorwarp-build-sdrplay.py status|build|service')
    return status() if argv == ['status'] else service() if argv == ['service'] else build()
if __name__ == '__main__':
    try: print(json.dumps(main(sys.argv[1:]), sort_keys=True))
    except (Refused,OSError,KeyError,subprocess.SubprocessError) as e:
        print(json.dumps({'ok':False,'state':'unavailable','reason':str(e)})); sys.exit(1)
