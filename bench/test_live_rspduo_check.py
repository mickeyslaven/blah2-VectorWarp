#!/usr/bin/env python3
"""Offline subprocess regression tests. The fake processor never opens an SDR."""
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("live_check", HERE / "live-rspduo-check.py")
CHECK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECK)

# This independently encodes the actual RspDuo startup-receipt contract. Do not
# use accepted_receipt() to manufacture the oracle's input.
FAKE = r'''
import http.client, json, signal, socket, sys, time
from pathlib import Path
cfg = json.loads(Path(sys.argv[sys.argv.index('--config') + 1]).read_text())
ports = cfg['network']['ports']
mode = MODE
running = True
def stop(*_):
    global running
    running = False
signal.signal(signal.SIGTERM, stop)
if mode == 'early_exit':
    raise SystemExit(0)
if mode == 'verbose':
    sys.stdout.write('processor log\n' * 100000)
    sys.stdout.flush()
if mode == 'slow_start':
    time.sleep(.4)
receipt = {
    'schema': 1, 'receiver': 'RspDuo', 'status': 'accepted',
    'hardwareVerified': False, 'readbackAvailable': False,
    'requested': {'serial': '', 'frequency': 551000000, 'sampleRate': 2000000,
                  'agcSetPoint': -20, 'bandwidthNumber': 5, 'gainReduction': [50,45],
                  'lnaState': 1, 'dabNotch': False, 'rfNotch': False,
                  'ifBandwidthKhz': 1536, 'ifFrequencyKhz': 1620, 'decimation': 1},
    'selected': {'serial': 'offline-fixture', 'deviceIndex': 0, 'hardwareVersion': 3,
                 'tuner': 'Both', 'mode': 'Dual_Tuner'},
    'sdk': {'version': 3.15, 'stages': {key: True for key in
           ('open','apiVersion','lock','enumerate','select','unlock','debugEnable',
            'getDeviceParams','init','gainUpdateA','gainUpdateB')}}}
if mode == 'invented_receipt':
    receipt = {'appliedHardware': True}
if mode == 'failed_stage':
    receipt['sdk']['stages']['gainUpdateB'] = False
state = {'input': 'live', 'receiver': 'RspDuo', 'state': 'live', 'error': '',
         'sampleRate': 2000000, 'frequency': 551000000, 'receiverStartup': receipt}
if mode == 'fault':
    state.update(state='error', error='mock callback failure')
client = http.client.HTTPConnection('127.0.0.1', ports['api'], timeout=1)
client.request('POST', '/api/processor/status', json.dumps(state))
client.getresponse().read(); client.close()
peers = {name: socket.create_connection(('127.0.0.1', port), timeout=1)
         for name, port in ports.items() if name != 'api'}
for name, peer in peers.items():
    if name != 'timing': peer.sendall(b'{}')
if mode == 'reconnect_partial':
    peers['timing'].sendall(b'{"cut":"\xf0\x9f')
    peers['timing'].close()
    peers['timing'] = socket.create_connection(('127.0.0.1', ports['timing']), timeout=1)
number = 0
while running:
    number += 1
    value = {'nCpi': number, 'cpi': 230.5,
             'captureDroppedSamples': [0,0], 'captureBacklogSamples': [1000000,1000000],
             'note': 'fragmented \u2603'}
    if mode == 'dropped': value['captureDroppedSamples'] = [1,1]
    if mode == 'missing_counter': value.pop('captureDroppedSamples')
    if mode == 'bad_backlog': value['captureBacklogSamples'] = [False,0]
    if mode == 'missing_cpi': value.pop('cpi')
    if mode == 'bad_cpi': value['cpi'] = '230.5'
    if mode == 'skipped_cpi': value['nCpi'] += 1
    wire = json.dumps(value, ensure_ascii=False).encode()
    if mode == 'invalid_utf8': wire = b'{"cpi":"\xff"}'
    if mode == 'malformed': wire = b'{"cpi":]}'
    if mode == 'nonfinite': wire = b'{"cpi":1e999}'
    if mode == 'huge_integer': wire = b'{"cpi":' + b'9' * 400 + b'}'
    if mode == 'nested_json': wire = b'{"deep":' + b'[' * 1100 + b'0' + b']' * 1100 + b'}'
    if mode != 'missing_timing':
        try:
            # Fragment UTF-8 and JSON at arbitrary byte boundaries.
            peers['timing'].sendall(wire[:7])
            peers['timing'].sendall(wire[7:])
            if mode == 'reconnect_complete':
                peers['timing'].close()
                peers['timing'] = socket.create_connection(('127.0.0.1', ports['timing']), timeout=1)
        except OSError:
            break
    if mode == 'early_after_receipt':
        break
    time.sleep(.06)
for peer in peers.values(): peer.close()
if mode != 'missing_final':
    final = dict(state, captureStopped=True, captureBacklogSamples=[200000,200000],
                 captureDroppedSamples=[0,0])
    if mode == 'final_dropped': final['captureDroppedSamples'] = [7,7]
    if mode == 'final_missing_counter': final.pop('captureDroppedSamples')
    if mode == 'final_bad_backlog': final['captureBacklogSamples'] = [0,False]
    if mode == 'final_wrong_channels': final['captureDroppedSamples'] = [0]
    if mode == 'final_bad_marker': final['captureStopped'] = 'true'
    if mode == 'final_missing_receipt': final.pop('receiverStartup')
    if mode == 'final_invalid_state': final['state'] = 'unknown'
    if mode == 'final_fault': final.update(state='error', error='mock tail callback failure')
    client = http.client.HTTPConnection('127.0.0.1', ports['api'], timeout=1)
    client.request('POST', '/api/processor/status', json.dumps(final))
    client.getresponse().read(); client.close()
'''


class LiveCheckTest(unittest.TestCase):
    def run_fake(self, mode, seconds=.25):
        with tempfile.TemporaryDirectory(prefix="vectorwarp-live-check-test-") as temporary:
            root = Path(temporary)
            binary = root / "fake-processor"
            binary.write_text(f"#!{sys.executable}\nMODE = {mode!r}\n" + FAKE)
            binary.chmod(0o755)
            output = root / "result"
            result = subprocess.run(
                [sys.executable, str(HERE / "live-rspduo-check.py"), "--binary", str(binary),
                 "--seconds", str(seconds), "--startup-timeout", "2", "--output", str(output)],
                capture_output=True, text=True, timeout=10)
            self.assertIn(result.returncode, (0, 1), result.stderr)
            self.assertEqual(result.stderr, "")
            self.assertTrue(output.with_suffix(".evidence.json").is_file(), result.stdout)
            evidence = json.loads(output.with_suffix(".evidence.json").read_text())
            self.assertEqual(evidence, json.loads(result.stdout))
            self.assertEqual(result.returncode == 0, evidence["ok"])
            if mode == "verbose":
                self.assertGreater(output.with_suffix(".log").stat().st_size, 1 << 20)
            return evidence

    def test_success_verbose_reconnect_and_startup_window(self):
        for mode in ("success", "verbose", "reconnect_complete", "slow_start"):
            with self.subTest(mode=mode):
                result = self.run_fake(mode)
                self.assertTrue(result["ok"], result["failures"])
                self.assertTrue(result["sdkStartupAccepted"])
                self.assertIs(result["hardwareVerified"], False)
                self.assertGreaterEqual(result["measuredSeconds"], .25)
                self.assertIs(result["finalCaptureStatus"]["captureStopped"], True)
                self.assertEqual(result["perChannelEndDrops"], [0, 0])
                self.assertEqual(result["perChannelEndBacklog"], [200000, 200000])
                if mode == "slow_start":
                    self.assertGreater(result["startupSeconds"], .4)

    def test_failure_modes_produce_evidence(self):
        for mode in ("early_exit", "early_after_receipt", "dropped", "missing_counter",
                     "bad_backlog", "missing_cpi", "bad_cpi", "skipped_cpi", "missing_timing",
                     "invalid_utf8", "malformed", "nonfinite", "huge_integer", "nested_json", "invented_receipt", "failed_stage", "fault"):
            with self.subTest(mode=mode):
                result = self.run_fake(mode)
                self.assertFalse(result["ok"])
                self.assertTrue(result["failures"])

    def test_final_status_catches_tail_drops_faults_and_missing_counters(self):
        for mode in ("missing_final", "final_dropped", "final_missing_counter", "final_bad_backlog",
                     "final_wrong_channels", "final_bad_marker", "final_missing_receipt",
                     "final_invalid_state", "final_fault"):
            with self.subTest(mode=mode):
                result = self.run_fake(mode)
                self.assertFalse(result["ok"])
                self.assertTrue(result["normalStop"])
                self.assertTrue(all(frame["captureDroppedSamples"] == [0, 0]
                                    for frame in result["timing"]))
                if mode == "final_dropped":
                    self.assertEqual(result["perChannelEndDrops"], [7, 7])
                if mode == "final_fault":
                    self.assertEqual(result["finalCaptureStatus"]["error"], "mock tail callback failure")
                    self.assertTrue(result["faults"])

    def test_partial_connection_is_rejected_and_parser_is_reset(self):
        result = self.run_fake("reconnect_partial")
        self.assertFalse(result["ok"])
        self.assertTrue(result["sinkExceptions"])
        # A new connection's complete object is still decoded independently.
        self.assertGreater(result["observedCPIcount"], 0)
        self.assertEqual(result["timing"][0]["nCpi"], 1)

    def test_timing_parser_handles_coalesced_utf8_and_rejects_eof_tail(self):
        values = []
        parser = CHECK.JsonObjects(values.append)
        wire = '{"text":"☃"}{"nCpi":2}'.encode()
        for byte in wire:
            parser.feed(bytes([byte]))
        parser.feed(b"", final=True)
        self.assertEqual(values, [{"text": "☃"}, {"nCpi": 2}])
        for payload in (b'{"cut":', b'{"cut":"\xf0\x9f', b'{"n":NaN}', b'{"n":1e999}',
                        b'{"deep":' + b'[' * 80 + b'0' + b']' * 80 + b'}',
                        b'{"large":"' + b'x' * CHECK.MAX_FRAME_BYTES + b'"}'):
            with self.subTest(payload=payload), self.assertRaises((ValueError, UnicodeError)):
                CHECK.JsonObjects(values.append).feed(payload, final=True)


if __name__ == "__main__":
    unittest.main()
