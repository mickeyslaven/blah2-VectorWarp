#!/usr/bin/env python3
"""No-radio contract tests for package provenance and stable helper paths."""
import hashlib
import json
import os
from pathlib import Path
import runpy
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]


class BundledKraken(unittest.TestCase):
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


if __name__ == '__main__': unittest.main()
