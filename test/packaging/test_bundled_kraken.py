#!/usr/bin/env python3
"""No-radio contract tests for package provenance and stable helper paths."""
import hashlib
import io
import json
import os
from pathlib import Path
import runpy
import shutil
import subprocess
import tempfile
import tarfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]


class BundledKraken(unittest.TestCase):
    def test_rpm_manifest_owns_kraken_system_integration(self):
        spec = (ROOT / 'packaging/rpm/vectorwarp.spec.in').read_text()
        files = set(spec.split('\n%files\n', 1)[1].split('\n%changelog', 1)[0].splitlines())
        for path in (
            '/usr/lib/systemd/system/vectorwarp-heimdall.service',
            '/usr/lib/systemd/system/vectorwarp-kraken-build.service',
            '/usr/lib/udev/rules.d/72-vectorwarp-kraken.rules',
        ):
            with self.subTest(path=path):
                self.assertIn(path, files)

    def test_old_python_safe_extractor_rejects_escape_links_and_devices(self):
        extract = runpy.run_path(str(ROOT / 'script/build-kraken-macos.py'))['extract_sources']
        for name, kind in (('/escape', tarfile.REGTYPE), ('../escape', tarfile.REGTYPE),
                           ('root/link', tarfile.SYMTYPE), ('root/hard', tarfile.LNKTYPE),
                           ('root/fifo', tarfile.FIFOTYPE), ('root/device', tarfile.CHRTYPE)):
            member = tarfile.TarInfo(name); member.type = kind; member.linkname = '../../escape'
            class OldTar:
                def getmembers(self): return [member]
                def extractall(self, path): raise AssertionError('unsafe legacy extraction')
            with tempfile.TemporaryDirectory() as temp, self.assertRaises(ValueError):
                extract(OldTar(), Path(temp))
        data = io.BytesIO()
        with tarfile.open(fileobj=data, mode='w') as bundle:
            member = tarfile.TarInfo('root/a'); member.size = 2; member.mode = 0o4755
            bundle.addfile(member, io.BytesIO(b'ok'))
        data.seek(0)
        with tempfile.TemporaryDirectory() as temp, tarfile.open(fileobj=data) as bundle:
            with patch.object(bundle, 'extractall', side_effect=AssertionError('requires modern Python')):
                extract(bundle, Path(temp))
            self.assertEqual((Path(temp) / 'root/a').read_bytes(), b'ok')
            self.assertEqual((Path(temp) / 'root/a').stat().st_mode & 0o7777, 0o755)

    def test_source_kit_is_only_pinned_build_instructions(self):
        stage = runpy.run_path(str(ROOT / 'script/stage-kraken-kit.py'))['stage']
        identity = runpy.run_path(str(ROOT / 'script/vectorwarp-build-kraken.py'))['kit_identity']
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp)
            key = stage(ROOT, output)
            kit = json.loads((output / 'kit.json').read_text())
            self.assertEqual(key, identity(kit))
            self.assertEqual(key, stage(ROOT, output))
            self.assertFalse((output / 'bin').exists())
            self.assertFalse(any(path.suffix in ('.so', '.dylib', '.gz') for path in output.rglob('*')))
            self.assertEqual(set(kit['files']), {str(p.relative_to(output)) for p in output.rglob('*') if p.is_file()} - {'kit.json'})
            kit['files']['script/build-kraken.py'] = 'bad'
            with self.assertRaises(ValueError): identity(kit)

    def test_publication_rejects_links_and_special_files(self):
        copy = runpy.run_path(str(ROOT / 'script/vectorwarp-build-kraken.py'))['copy_regular']
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp).resolve()
            original = root / 'original'; original.write_bytes(b'artifact')
            link = root / 'link'; link.symlink_to(original)
            with self.assertRaises(OSError): copy(link, root / 'published')
            fifo = root / 'fifo'; os.mkfifo(fifo)
            with self.assertRaises(ValueError): copy(fifo, root / 'published')
            copy(original, root / 'published')
            self.assertEqual((root / 'published').read_bytes(), b'artifact')

    def test_release_rollback_selects_exact_kit_never_ambient_executable(self):
        module = runpy.run_path(str(ROOT / 'script/vectorwarp-heimdall.py'))
        select = module['companion_root']
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); release = root / 'release'; kitdir = release / 'receiver-source/kraken'
            kitdir.mkdir(parents=True)
            files = {'config/kraken-macos-sources.json': 'pins'}
            key = hashlib.sha256(json.dumps(files, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
            kit = dict(schema=1, kit_id=key, files=files)
            (kitdir / 'kit.json').write_text(json.dumps(kit))
            sandbox = {'read_json': lambda path: json.loads(path.read_text()),
                       'trusted': lambda path: path.stat()}
            with patch('runpy.run_path', return_value=sandbox):
                with self.assertRaisesRegex(ValueError, 'build-kraken'): select(release, root / 'generations')
                final = root / 'generations' / key / 'current'
                (final / 'bin').mkdir(parents=True); (final / 'share/heimdall').mkdir(parents=True)
                (final / 'kit-id').write_text(key)
                (final / 'bin/heimdall').write_bytes(b'pinned')
                (final / 'share/heimdall/index.html').write_text('local')
                receipt = dict(schema=2, driverLinkage='pinned-static', protocol='MCHT-v1;MCHQ-v2',
                    sourceManifestSha256='pins', artifacts={'bin/heimdall': hashlib.sha256(b'pinned').hexdigest()})
                (final / 'share/heimdall/build.json').write_text(json.dumps(receipt))
                self.assertEqual(select(release, root / 'generations'), final.resolve())
                # An upgraded kit cannot silently use the old generation; a
                # rollback restoring this kit selects the original again.
                kit['files']['config/kraken-macos-sources.json'] = 'new'
                kit['kit_id'] = hashlib.sha256(json.dumps(kit['files'], sort_keys=True, separators=(',', ':')).encode()).hexdigest()
                (kitdir / 'kit.json').write_text(json.dumps(kit))
                with self.assertRaisesRegex(ValueError, 'build-kraken'): select(release, root / 'generations')

    def test_saved_per_channel_gains_require_owned_command_and_readback(self):
        adapter = runpy.run_path(str(ROOT / 'script/vectorwarp-kraken-macos.py'))
        profile = dict(frequency=101000000, channels=2, gain=20, channel_gains=[19.7, 22.9])
        live = dict(instance_token='owned', settings=dict(center_freq=101000000, sample_rate=2400000, gain=20),
                    num_channels=2, operating_mode='coherent', calibration_state='CALIBRATED', channel_gains=[20, 20])
        self.assertIsNone(adapter['matching_status'](live, profile))
        live['channel_gains'] = profile['channel_gains']
        self.assertEqual(adapter['matching_status'](live, profile), 'CALIBRATED')
        class Connection:
            sent = None
            def __enter__(self): return self
            def __exit__(self, *_): pass
            def recv(self, _): return (json.dumps(live) + '\n').encode()
            def sendall(self, value): self.sent = json.loads(value)
        connection = Connection()
        with patch('socket.create_connection', return_value=connection):
            adapter['telemetry'](8092, profile['channel_gains'], 'owned')
            self.assertEqual(connection.sent, {'command': 'set_channel_gains', 'gains': [19.7, 22.9]})
            connection.sent = None
            with self.assertRaises(ValueError): adapter['telemetry'](8092, profile['channel_gains'], 'foreign')
            self.assertIsNone(connection.sent)

    def test_receipt_rejects_missing_external_or_replaced_receiver(self):
        verify = runpy.run_path(str(ROOT / 'script/vectorwarp-heimdall.py'))['verify']
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'bin').mkdir()
            (root / 'share/heimdall').mkdir(parents=True)
            binary = root / 'bin/heimdall'; binary.write_bytes(b'pinned test binary')
            receipt = {'schema': 2, 'driverLinkage': 'pinned-static', 'protocol': 'MCHT-v1;MCHQ-v2',
                       'artifacts': {'bin/heimdall': hashlib.sha256(binary.read_bytes()).hexdigest()}}
            file = root / 'share/heimdall/build.json'; file.write_text(json.dumps(receipt))
            self.assertEqual(verify(root), receipt)
            binary.write_bytes(b'uncontrolled replacement')
            with self.assertRaises(ValueError): verify(root)
            receipt['driverLinkage'] = 'system'; file.write_text(json.dumps(receipt))
            with self.assertRaises(ValueError): verify(root)

    @unittest.skipUnless(shutil.which('node') and (ROOT / 'api/node_modules').is_dir(), 'install API npm dependencies')
    def test_stable_watchdog_and_config_guard_resolve_current_release(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'current').mkdir(); (root / 'libexec').mkdir()
            (root / 'current/api').symlink_to(ROOT / 'api', target_is_directory=True)
            for name in ('vectorwarp-health-watchdog.js', 'vectorwarp-config-guard.js'):
                helper = root / 'libexec' / name
                shutil.copy2(ROOT / 'script' / name, helper)
                subprocess.run(['node', '-e', 'require(process.argv[1])', str(helper)], check=True,
                               cwd='/', capture_output=True)

    def test_package_and_lifecycle_share_release_owner(self):
        unit = (ROOT / 'contrib/systemd/vectorwarp-heimdall.service.in').read_text()
        self.assertIn('PartOf=vectorwarp-processor.service', unit)
        self.assertIn('Type=notify', unit)
        self.assertIn('@PREFIX@/current/share/heimdall/service.py', unit)
        package = (ROOT / 'script/package-native.sh').read_text()
        for item in ('bin/heimdall', 'share/heimdall/build.json', 'share/heimdall/sbom.cdx.json'):
            self.assertIn(item, package)
        self.assertIn('--node /opt/vectorwarp/runtime/node/bin/node', package)
        self.assertIn('run rm -f "$STAGE/opt/vectorwarp/current/bin/heimdall"', package)
        self.assertIn('"$STAGE/opt/vectorwarp/current/share/heimdall"', package)
        self.assertNotIn('"$STAGE/opt/vectorwarp/current/bin/heimdall"\n    "$STAGE', package)
        build_unit = (ROOT / 'contrib/systemd/vectorwarp-kraken-build.service.in').read_text()
        self.assertIn('PrivateDevices=yes', build_unit)
        self.assertIn('ProtectSystem=strict', build_unit)
        self.assertNotIn('[Install]', build_unit)


if __name__ == '__main__': unittest.main()
