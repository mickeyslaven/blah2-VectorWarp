#!/usr/bin/env python3
"""Offline full-processor replay acceptance harness.

This starts only a supplied processor binary, completed synthetic BLAH2IQ v1
files, and loopback HTTP/TCP peers.  It never discovers, opens, or configures an
SDR.  Run it explicitly; it is deliberately not wired into CMake yet.
"""
import argparse
import http.server
import json
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import tempfile
import threading
import time
import math


HOST = "127.0.0.1"
PORT_NAMES = ("map", "detection", "track", "timestamp", "timing", "iqdata")
RSP_TYPES = ("RspDuo", "Usrp", "HackRF")
STATUS_LOCK = threading.Lock()


def free_listener():
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind((HOST, 0))
    listener.listen()
    listener.settimeout(.2)
    return listener


class SinkGroup:
    """Accept and drain processor output so Socket writes cannot block."""
    def __init__(self):
        self.listeners = {name: free_listener() for name in PORT_NAMES}
        self.bytes = {name: 0 for name in PORT_NAMES}
        self.payloads = {name: bytearray() for name in ("map", "timestamp", "timing")}
        self.stop = threading.Event()
        self.threads = [threading.Thread(target=self._serve, args=(name, sock), daemon=True)
                        for name, sock in self.listeners.items()]

    def start(self):
        for thread in self.threads:
            thread.start()

    def _serve(self, name, listener):
        while not self.stop.is_set():
            try:
                connection, _ = listener.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            with connection:
                connection.settimeout(.2)
                while not self.stop.is_set():
                    try:
                        block = connection.recv(65536)
                    except socket.timeout:
                        continue
                    except OSError:
                        break
                    if not block:
                        break
                    self.bytes[name] += len(block)
                    if name in self.payloads and len(self.payloads[name]) < 1 << 20:
                        self.payloads[name].extend(block[:(1 << 20) - len(self.payloads[name])])

    def ports(self):
        return {name: listener.getsockname()[1] for name, listener in self.listeners.items()}

    def close(self):
        self.stop.set()
        for listener in self.listeners.values():
            listener.close()
        for thread in self.threads:
            thread.join(1)


class StatusServer:
    def __init__(self):
        self.statuses = []
        parent = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                if self.path == "/capture":
                    self.send_response(200)
                    self.send_header("Content-Type", "text/plain")
                    self.end_headers()
                    self.wfile.write(b"false")
                else:
                    self.send_error(404)

            def do_POST(self):
                if self.path != "/api/processor/status":
                    self.send_error(404)
                    return
                try:
                    size = int(self.headers.get("Content-Length", "0"))
                    value = json.loads(self.rfile.read(size))
                    if not isinstance(value, dict):
                        raise ValueError("status is not an object")
                except (ValueError, json.JSONDecodeError):
                    self.send_error(400)
                    return
                with STATUS_LOCK:
                    parent.statuses.append(value)
                self.send_response(204)
                self.end_headers()

            def log_message(self, *_):
                pass

        self.server = http.server.ThreadingHTTPServer((HOST, 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)

    @property
    def port(self):
        return self.server.server_address[1]

    def start(self):
        self.thread.start()

    def snapshot(self):
        with STATUS_LOCK:
            return list(self.statuses)

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(2)


def write_blah2iq(path, channels, sample_rate, frequency, frames=3, frame_samples=None):
    """Write the little-endian BLAH2IQ v1 layout used by Recording.cpp."""
    frame_samples = int(sample_rate * .02) if frame_samples is None else frame_samples
    samples = frames * frame_samples
    header = bytearray(64)
    header[:8] = b"BLAH2IQ\0"
    struct.pack_into("<IIIIII", header, 8, 1, 64, channels, sample_rate, frequency, 1)
    struct.pack_into("<Q", header, 40, samples)
    struct.pack_into("<Q", header, 48, 1)  # completed cleanly
    block = struct.pack("<IIQ", samples, 0, 0)
    with path.open("wb") as stream:
        stream.write(header)
        stream.write(block)
        for channel in range(channels):
            # Repeated deterministic broadband data avoids rank-deficient
            # constant-IQ inputs while keeping fixture creation bounded.
            state = 0x9E3779B9 ^ channel
            pattern = bytearray()
            for _ in range(1024):
                state = (1664525 * state + 1013904223) & 0xffffffff
                real = ((state >> 8) / 8388608.0) - 1.0
                state = (1664525 * state + 1013904223) & 0xffffffff
                imag = ((state >> 8) / 8388608.0) - 1.0
                pattern.extend(struct.pack("<ff", real, imag))
            whole, tail = divmod(samples, 1024)
            stream.write(pattern * whole)
            stream.write(pattern[:tail * 8])


def config(receiver, channels, recording, api_port, sink_ports, loop=False, mismatch=False):
    sample_rate = 2400000 if receiver == "Kraken" else 2000000
    frequency = 204640000
    file_channels = channels - 1 if mismatch else channels
    if mismatch:
        # The fixture writer uses this count; the processor configuration remains correct.
        write_blah2iq(recording, file_channels, sample_rate, frequency)
    device = {"type": receiver}
    if receiver == "Kraken":
        device.update(channel_count=channels, reference_channel=0,
                      surveillance_channels=[1])
    elif receiver == "RspDuo":
        device.update(agcSetPoint=-20, bandwidthNumber=5, gainReduction=[50, 45],
                      lnaState=1, dabNotch=False, rfNotch=False)
    elif receiver == "Usrp":
        device.update(address="127.0.0.1", subdev="A:A A:B", antenna=["RX2", "RX2"], gain=[20.0, 20.0])
    elif receiver == "HackRF":
        device.update(serial=["offline-reference", "offline-surveillance"], gain_lna=[0, 0],
                      gain_vga=[0, 0], amp_enable=[False, False])
    process = {
        "performance": {"surveillance_workers": 1, "fft_threads": 1, "acceleration": "cpu"},
        "data": {"cpi": .02, "buffer": 2, "overlap": 0},
        # ±1000 Hz produces a legal FFT geometry for the legacy CPU path.
        "ambiguity": {"delayMin": -4, "delayMax": 59, "dopplerMin": -1000, "dopplerMax": 1000},
        # Isolate replay ingress from clutter rank behaviour in this harness.
        "clutter": {"enable": False, "delayMin": -4, "delayMax": 59},
        "detection": {"enable": True, "pfa": .01, "nGuard": 2, "nTrain": 6,
                      "minDelay": 1, "minDoppler": 1, "nCentroid": 1},
        "tracker": {"enable": False, "initiate": {"M": 1, "N": 1, "maxAcc": 1}, "delete": 1, "smooth": "none"},
    }
    if receiver == "Kraken":
        process["reference_synthesis"] = {"mode": "dedicated", "channels": [0]}
    return {
        "capture": {"fs": sample_rate, "fc": frequency, "device": device,
                    "replay": {"state": True, "loop": loop, "file": str(recording), "format": "auto"}},
        "process": process,
        "network": {"ip": HOST, "ports": dict(api=api_port, **sink_ports)},
        "save": {"iq": False, "map": False, "detection": False, "timing": False, "path": str(recording.parent)},
    }


def stop_process(process, timeout=4):
    if process.poll() is None:
        process.send_signal(signal.SIGTERM)
        try:
            process.wait(timeout)
            return False
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout)
            return True
    return False


def wait_for(predicate, deadline):
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(.05)
    return False


def first_json(payload, name):
    try:
        value, _ = json.JSONDecoder().raw_decode(bytes(payload).decode().lstrip())
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise AssertionError(f"{name} output is not JSON: {error}") from error
    return value


def assert_finite(value, name="map"):
    if isinstance(value, dict):
        for key, item in value.items():
            assert_finite(item, f"{name}.{key}")
    elif isinstance(value, list):
        for index, item in enumerate(value):
            assert_finite(item, f"{name}[{index}]")
    elif isinstance(value, (int, float)) and not isinstance(value, bool) and not math.isfinite(value):
        raise AssertionError(f"{name} contains a non-finite number")


def assert_finite_numbers(value, name="map.data"):
    if isinstance(value, list):
        for index, item in enumerate(value):
            assert_finite_numbers(item, f"{name}[{index}]")
    elif not isinstance(value, (int, float)) or isinstance(value, bool) or not math.isfinite(value):
        raise AssertionError(f"{name} is not a finite numeric map value")


def assert_processor_outputs(sinks):
    if not all(sinks.bytes[name] for name in ("map", "timestamp", "timing")):
        raise AssertionError("processor did not produce map, timestamp and timing output")
    map_json = first_json(sinks.payloads["map"], "map")
    if not isinstance(map_json, dict) or not isinstance(map_json.get("data"), list):
        raise AssertionError("map output has no data array")
    assert_finite(map_json)
    assert_finite_numbers(map_json["data"])
    timing_json = first_json(sinks.payloads["timing"], "timing")
    if not isinstance(timing_json, dict) or timing_json.get("nCpi", 0) < 1:
        raise AssertionError("timing output has no completed CPI")
    timestamp = bytes(sinks.payloads["timestamp"]).strip()
    if not timestamp or not timestamp.isdigit():
        raise AssertionError("timestamp output is not numeric")


def run_case(binary, receiver, channels, kind="complete"):
    loop = kind == "loop"
    with tempfile.TemporaryDirectory(prefix="vectorwarp-replay-") as temporary:
        root = Path(temporary)
        recording = root / "input.blah2iq"
        sample_rate = 2400000 if receiver == "Kraken" else 2000000
        if kind != "missing":
            write_blah2iq(recording, channels, sample_rate, 204640000)
        status = StatusServer()
        sinks = SinkGroup()
        process = None
        try:
            status.start()
            sinks.start()
            payload = config(receiver, channels, recording, status.port, sinks.ports(), loop,
                             mismatch=kind == "mismatch")
            config_path = root / "config.json"
            config_path.write_text(json.dumps(payload))
            process = subprocess.Popen([str(binary), "-c", str(config_path)], cwd=root,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, start_new_session=True)
            # Leave time within the per-case 20-second allowance for SIGTERM/reap.
            deadline = time.monotonic() + 14
            if kind == "complete":
                ok = wait_for(lambda: any(item.get("state") == "complete" for item in status.snapshot()), deadline)
                expectation = "complete"
            elif kind == "loop":
                ok = wait_for(lambda: any(item.get("state") == "playing" and item.get("loops", 0) >= 2
                                          for item in status.snapshot()), deadline)
                expectation = "loop >= 2"
            else:
                ok = wait_for(lambda: any(item.get("state") == "error" and item.get("error")
                                          for item in status.snapshot()), deadline)
                expectation = "error"
            output = ""
            if not ok:
                raise AssertionError(f"{receiver}/{channels} did not report {expectation}: {status.snapshot()[-3:]}")
            if process.poll() is not None:
                raise AssertionError(f"{receiver}/{channels} exited before controlled shutdown")
            forced = stop_process(process)
            output = process.stdout.read() if process.stdout else ""
            if forced or process.returncode != 0:
                raise AssertionError(f"{receiver}/{channels} did not stop cleanly (returncode={process.returncode}): {output[-16000:]}")
            states = status.snapshot()
            if "Setting up device" in output:
                raise AssertionError(f"{receiver}/{channels} attempted hardware setup: {output}")
            if kind in ("complete", "loop"):
                required_state = "playing" if kind == "loop" else "complete"
                if not any(item.get("state") == required_state for item in states):
                    raise AssertionError(f"{receiver}/{channels} never reported {required_state}")
                assert_processor_outputs(sinks)
            return {"case": f"{receiver}-{channels}-{kind}", "ok": True,
                    "states": sorted({item.get("state") for item in states}),
                    "output_bytes": sum(sinks.bytes.values())}
        except BaseException as error:
            diagnostics = ""
            if process is not None:
                stop_process(process)
                if process.stdout:
                    diagnostics = process.stdout.read()
            raise AssertionError(f"{receiver}/{channels}/{kind}: {error}; processor output: {diagnostics[-16000:]}") from error
        finally:
            if process is not None:
                stop_process(process)
                if process.stdout:
                    process.stdout.close()
            sinks.close()
            status.close()


def invalid_startup_case(binary, field):
    """Reject invalid required numbers before networking or receiver setup."""
    with tempfile.TemporaryDirectory(prefix="vectorwarp-invalid-config-") as temporary:
        root = Path(temporary)
        payload = config("Kraken", 2, root / "unused.blah2iq", 3000,
                         {name: 3100 + index for index, name in enumerate(PORT_NAMES)})
        if field == "api-port":
            payload["network"]["ports"]["api"] = 0
        else:
            payload["capture"][field] = 0
        config_path = root / "config.json"
        config_path.write_text(json.dumps(payload))
        result = subprocess.run([str(binary), "-c", str(config_path)], cwd=root,
                                capture_output=True, text=True, timeout=5, check=False)
        output = result.stdout + result.stderr
        if result.returncode != 1 or "sample rate, centre frequency and API port must be positive" not in output:
            raise AssertionError(f"Invalid {field} must fail clearly before setup: {output[-4000:]}")
        if "Setting up device" in output or "Failed to initialize socket" in output:
            raise AssertionError(f"Invalid {field} attempted setup")
        return {"case": f"invalid-{field}", "ok": True, "output_bytes": 0}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    args = parser.parse_args()
    binary = args.binary.resolve()
    if not binary.is_file() or not os.access(binary, os.X_OK):
        parser.error("--binary must be an executable processor path")
    results = [invalid_startup_case(binary, field) for field in ("fs", "fc", "api-port")]
    for receiver in RSP_TYPES:
        results.append(run_case(binary, receiver, 2))
    for channels in range(2, 9):
        results.append(run_case(binary, "Kraken", channels))
    results.append(run_case(binary, "RspDuo", 2, "loop"))
    results.append(run_case(binary, "RspDuo", 2, "missing"))
    results.append(run_case(binary, "Kraken", 3, "mismatch"))
    print(json.dumps({"acceptance": "offline processor replay", "cases": results,
                      "count": len(results)}, separators=(",", ":")))


if __name__ == "__main__":
    main()
