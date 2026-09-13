#!/usr/bin/env python3
"""Opt-in installed-layout test in a disposable, resource-bounded root container.

Supply existing locally installed SDK headers; never obtains or redistributes
them. The runtime is our failure-only stub, not SDRplay's library. Uses the
actual source kit, compiler UID drop, receipt publication and production loader.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import pwd
import shutil
import stat
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def run(argv, **kwargs):
    return subprocess.run([str(arg) for arg in argv], check=True, text=True,
                          capture_output=True, timeout=150, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk-headers', type=Path, required=True)
    args = parser.parse_args()
    if os.geteuid() or os.environ.get('VECTORWARP_LOCAL_INSTALL_TEST') != '1':
        raise RuntimeError('Run only in an explicitly opted-in disposable root test container')
    library = Path('/usr/local/lib/libsdrplay_api.so.3.15')
    headers = sorted(args.sdk_headers.glob('sdrplay_api*.h'))
    if not headers or not (args.sdk_headers/'sdrplay_api.h').is_file():
        raise RuntimeError('Provide the user-installed SDK headers outside the source checkout')
    destinations = [Path('/usr/local/include')/h.name for h in headers]
    cache = Path('/var/lib/vectorwarp-adapters')
    lock = Path('/run/vectorwarp-rspduo-build.lock')
    if any(p.exists() or p.is_symlink() for p in [library, cache, lock, *destinations]):
        raise RuntimeError('Fixture refuses to overwrite any existing SDK, adapter cache or lock')
    user = pwd.getpwnam('nobody')
    def unprivileged():
        os.setgroups([]); os.setgid(user.pw_gid); os.setuid(user.pw_uid)
    prefix = Path(tempfile.mkdtemp(prefix='vectorwarp-local-install-', dir='/opt'))
    prefix.chmod(0o755)
    created = []
    try:
        source = prefix/'project'; source.mkdir()
        (source/'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.16)
project(LocalReceiverFixture LANGUAGES CXX)
set(PROJECT_ROOT "${VW_SOURCE}")
set(BLAH2_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin")
set(BLAH2_LOCAL_BUILD_RSPDUO ON)
set(BLAH2_ENABLE_USRP OFF)
set(BLAH2_ENABLE_HACKRF OFF)
set(BLAH2_ENABLE_RSPDUO OFF)
set(BUILD_TESTING OFF)
find_package(Threads REQUIRED)
add_library(blah2RapidJson INTERFACE)
target_include_directories(blah2RapidJson INTERFACE /usr/include)
include_directories("${VW_SOURCE}/src")
include("${VW_SOURCE}/cmake/ReceiverModules.cmake")
add_executable(localReceiver "${VW_SOURCE}/test/capture/LocalReceiverFixture.cpp")
target_compile_features(localReceiver PRIVATE cxx_std_17)
target_compile_options(localReceiver PRIVATE -Wall -Wextra -Werror)
target_link_libraries(localReceiver PRIVATE blah2ReceiverLoader)
set_target_properties(localReceiver PROPERTIES BUILD_WITH_INSTALL_RPATH TRUE
  INSTALL_RPATH "$ORIGIN" RUNTIME_OUTPUT_DIRECTORY "${BLAH2_OUTPUT_DIR}")
''')
        build = prefix/'build'
        run(['cmake', '-S', source, '-B', build, '-DVW_SOURCE='+str(ROOT), '-DCMAKE_BUILD_TYPE=Release'])
        run(['cmake', '--build', build, '--parallel', '1'])
        release = prefix/'releases/fixture'; release.mkdir(parents=True)
        shutil.copytree(build/'bin', release/'bin', symlinks=True)
        (prefix/'current').symlink_to('releases/fixture')
        run(['python3', ROOT/'script/stage-rspduo-kit.py', '--source', ROOT,
             '--generated', build/'receiver-generated', '--core', release/'bin/libblah2-capture-core.so.1',
             '--output', release/'receiver-source/rspduo'])
        helper = prefix/'libexec/vectorwarp-build-sdrplay'; helper.parent.mkdir()
        helper.write_text((ROOT/'script/vectorwarp-build-sdrplay.py').read_text()
                          .replace('@PREFIX@', str(prefix)).replace("BUILD_USER = 'vectorwarp-build'", "BUILD_USER = 'nobody'"))
        helper.chmod(0o755)
        for original, destination in zip(headers, destinations):
            shutil.copyfile(original, destination); destination.chmod(0o644); created.append(destination)
        sdk = ROOT/'test/capture/SdrplayLocalRuntime.cpp'
        run(['c++','-shared','-fPIC','-Wl,-soname,libsdrplay_api.so.3',sdk,'-o',library])
        created.append(library)
        (cache/'rspduo').mkdir(parents=True)
        lock.touch(mode=0o644); created.append(lock)
        fixture = release/'bin/localReceiver'
        def build_adapter():
            # Same bounded service capability set: no DAC override/read-search.
            return json.loads(run(['setpriv',
                '--bounding-set=-all,+chown,+fowner,+setgid,+setuid,+kill',
                '--no-new-privs','python3','-I',helper,'build']).stdout)
        def status():
            value=json.loads(run([fixture,'status'],preexec_fn=unprivileged).stdout)
            return next(item for item in value['receivers'] if item['receiver']=='RspDuo')
        initial=status()
        assert initial['localBuildable'] and not initial['compiled'], initial
        result=build_adapter()
        assert result['state']=='built', result
        installed=status()
        assert installed['compiled'] and installed['moduleLoadable'], installed
        manifest=json.loads((release/'receiver-source/rspduo/kit.json').read_text())
        assert manifest['core_sha256']==hashlib.sha256((release/'bin/libblah2-capture-core.so.1').read_bytes()).hexdigest()
        module=cache/'rspduo'/manifest['kit_id']/'current/blah2-receiver-rspduo.so'
        run([fixture,'pinned',module,library,'none'],preexec_fn=unprivileged)
        competitor=prefix/'competing-sdk.so'
        run(['c++','-shared','-fPIC','-DFIXTURE_MARKER=99','-Wl,-soname,libsdrplay_api.so.3',sdk,'-o',competitor])
        blocked=subprocess.run([str(fixture),'pinned',str(module),str(library),str(competitor)],
                               text=True,capture_output=True,preexec_fn=unprivileged,timeout=10)
        assert blocked.returncode==42 and 'already loaded' in blocked.stdout, blocked
        replacement=prefix/'replace'; replacement.mkdir()
        os.chown(replacement,user.pw_uid,user.pw_gid)
        samepath=replacement/'same-path.so'; fresh=replacement/'new-runtime.so'
        shutil.copyfile(competitor,samepath); shutil.copyfile(library,fresh)
        blocked=subprocess.run([str(fixture),'pinned-replaced',str(module),str(samepath),str(fresh)],
                               text=True,capture_output=True,preexec_fn=unprivileged,timeout=10)
        assert blocked.returncode==42 and 'already loaded' in blocked.stdout, blocked
        blocked=subprocess.run([str(fixture),'pinned-reused-fd',str(module),str(library),str(competitor)],
                               text=True,capture_output=True,preexec_fn=unprivileged,timeout=10)
        assert blocked.returncode==42 and 'already loaded' in blocked.stdout, blocked
        # Boot-time tmpfiles recreation must leave the lock readable by the loader.
        lock.unlink(); lock.touch(mode=0o644)
        assert stat.S_IMODE(lock.stat().st_mode)==0o644 and status()['moduleLoadable']
        # Trust walking must reject a writable intermediate symlink, even though
        # the final file is the original root-owned helper.
        writable=prefix/'writable'; writable.mkdir(); writable.chmod(0o777)
        (writable/'hop').symlink_to(helper); entry=prefix/'entry'; entry.symlink_to(writable/'hop')
        bad=subprocess.run([str(fixture),'trust',str(entry)],preexec_fn=unprivileged)
        assert bad.returncode==42
        # Appending bytes keeps the fixture ELF valid but changes its receipt.
        with library.open('ab') as stream: stream.write(b'changed fixture')
        assert not status()['moduleLoadable']
        rebuilt=build_adapter()
        assert rebuilt['state']=='built' and status()['moduleLoadable'], rebuilt
        with (release/'bin/libblah2-capture-core.so.1').open('ab') as stream:
            stream.write(b'changed core')
        assert not status()['moduleLoadable']
        print('PASS: actual source kit, unprivileged compilation/loading, receipt/core/SDK identity, competing SONAME, reboot lock, symlink-hop, SDK rebuild and stale-core rejection')
    except subprocess.CalledProcessError as error:
        print(error.stdout, error.stderr)
        raise
    finally:
        for item in reversed(created): item.unlink(missing_ok=True)
        if cache.exists(): shutil.rmtree(cache)
        shutil.rmtree(prefix)


if __name__=='__main__': main()
