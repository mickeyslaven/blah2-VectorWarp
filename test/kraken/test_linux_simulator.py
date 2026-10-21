#!/usr/bin/env python3
"""Negative isolation checks without compiling or executing any native binary."""
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('linux_simulator', Path(__file__).with_name('build_linux_simulator.py'))
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)


class IsolationAudit(unittest.TestCase):
    def test_linux_production_driver_detaches_kernel_driver(self):
        self.assertIn('-DDETACH_KERNEL_DRIVER=ON', builder.LINUX_DRIVER_FLAGS)
        self.assertNotIn('-DDETACH_KERNEL_DRIVER=OFF', builder.LINUX_DRIVER_FLAGS)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.binary, self.fake = self.base / 'heimdall-simulated', self.base / builder.SONAME
        self.link, self.map = self.base / 'link.txt', self.base / 'link.map'
        self.link.write_text(str(self.fake))
        self.map.write_text('LOAD ' + str(self.fake))
        self.symbols = '                 U rtlsdr_open\n'
        self.fake_symbols = '0000000000001110 T rtlsdr_open\n'
        self.closure = f'{builder.SONAME} => {self.fake} (0x1)\nlibc.so.6 => /lib/libc.so.6 (0x2)\n'
        self.dynamic = f'(NEEDED) Shared library: [{builder.SONAME}]\n(RUNPATH) Library runpath: [$ORIGIN]\n'

    def command(self, command):
        if command[0] == 'readelf':
            return self.dynamic if command[-1] == self.binary else f'(SONAME) Library soname: [{builder.SONAME}]\n'
        if command[0] == 'nm':
            if '--defined-only' in command:
                return ''
            return self.symbols if command[-1] == self.binary else self.fake_symbols
        if command[0] == 'ldd':
            return self.closure
        self.fail(command)

    def audit(self):
        with patch.object(builder, 'checked', side_effect=self.command), patch.dict(os.environ, {}, clear=True):
            return builder.audit(self.binary, self.fake, self.link, self.map)

    def test_accepts_test_driver(self):
        self.assertEqual(self.audit()['rtlImports'], ['rtlsdr_open'])

    def test_rejects_static_driver_in_link_map(self):
        self.map.write_text(str(self.fake) + '\nLOAD librtlsdr.a')
        with self.assertRaisesRegex(ValueError, 'real RTL/USB'):
            self.audit()

    def test_rejects_embedded_static_implementation(self):
        self.symbols += '0000000000012220 T rtlsdr_close\n'
        with self.assertRaisesRegex(ValueError, 'embeds static'):
            self.audit()

    def test_rejects_transitive_usb(self):
        self.closure += 'libusb-1.0.so => /lib/libusb.so (0x3)\n'
        with self.assertRaisesRegex(ValueError, 'closure includes'):
            self.audit()

    def test_rejects_dynamic_fallback(self):
        self.symbols += '                 U dlopen@GLIBC_2.34\n'
        with self.assertRaisesRegex(ValueError, 'fallback symbols'):
            self.audit()

    def test_rejects_wrong_fake_resolution(self):
        self.closure = self.closure.replace(str(self.fake), '/tmp/unrelated/' + builder.SONAME)
        with self.assertRaisesRegex(ValueError, 'outside prepared'):
            self.audit()

    def test_rejects_missing_fake_export(self):
        self.fake_symbols = ''
        with self.assertRaisesRegex(ValueError, 'all RTL imports'):
            self.audit()

    def test_rejects_ambient_loader_injection(self):
        for name in ('LD_PRELOAD', 'LD_LIBRARY_PATH', 'LD_AUDIT'):
            with self.subTest(name=name), patch.dict(os.environ, {name: '/tmp/injected'}, clear=True):
                with self.assertRaisesRegex(ValueError, name):
                    builder.clean_environment()


class ManifestVerification(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.binary = self.base / 'heimdall-simulated'
        self.manifest = self.base / 'binary-manifest.json'
        self.pins = self.base / 'pins.json'
        self.pins.write_text('{"suite": {"revision": "pinned", "sha256": "archive"}}')
        self.source = self.base / 'fake.cpp'
        self.source.write_text('test-only fake source')
        self.patch = self.base / 'upstream.patch'
        self.patch.write_text('reviewed upstream patch')
        artifacts = ['heimdall-simulated', builder.SONAME, 'simulated-link.txt',
                     'simulated-link.map', 'heimdall-production-compile-only']
        for name in artifacts:
            (self.base / name).write_text('artifact ' + name)
        self.data = dict(schema=1, simulatedOnly=True, platform='linux', hardwareTested=False,
                         sources=json.loads(self.pins.read_text()),
                         patchSha256={self.patch.name: builder.digest(self.patch)},
                         fakeSourceSha256=builder.digest(self.source),
                         artifactSha256={name: builder.digest(self.base / name) for name in artifacts})
        self.save()
        for name, value in [('PINS', self.pins), ('FAKE', self.source), ('PATCHES', [self.patch])]:
            replacement = patch.object(builder, name, value)
            replacement.start()
            self.addCleanup(replacement.stop)
        self.auditor = patch.object(builder, 'audit', return_value={}).start()
        self.addCleanup(patch.stopall)

    def save(self):
        self.manifest.write_text(json.dumps(self.data))

    def test_accepts_intact_manifest(self):
        builder.verify_prepared(self.binary, self.manifest)
        self.auditor.assert_called_once()

    def test_rejects_tampered_artifact_before_elf_audit(self):
        self.binary.write_text('changed binary')
        with self.assertRaisesRegex(ValueError, 'Artifact changed'):
            builder.verify_prepared(self.binary, self.manifest)
        self.auditor.assert_not_called()

    def test_rejects_changed_source_pins(self):
        self.pins.write_text('{"suite": {"revision": "new"}}')
        with self.assertRaisesRegex(ValueError, 'Source pins changed'):
            builder.verify_prepared(self.binary, self.manifest)

    def test_rejects_changed_patch(self):
        self.patch.write_text('new patch')
        with self.assertRaisesRegex(ValueError, 'patches changed'):
            builder.verify_prepared(self.binary, self.manifest)

    def test_rejects_changed_fake_source(self):
        self.source.write_text('new fake source')
        with self.assertRaisesRegex(ValueError, 'driver source changed'):
            builder.verify_prepared(self.binary, self.manifest)

    def test_rejects_missing_artifact_entry(self):
        del self.data['artifactSha256'][builder.SONAME]
        self.save()
        with self.assertRaisesRegex(ValueError, 'Manifest lacks'):
            builder.verify_prepared(self.binary, self.manifest)

    def test_rejects_nonlocal_artifact(self):
        self.data['artifactSha256']['../unrelated'] = 'untrusted'
        self.save()
        with self.assertRaisesRegex(ValueError, 'nonlocal artifact'):
            builder.verify_prepared(self.binary, self.manifest)


if __name__ == '__main__':
    unittest.main()
