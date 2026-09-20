#!/usr/bin/env python3
"""Explicit, bounded live RSPduo check. Only the supplied binary opens hardware.

Startup has a separate deadline. The measured interval starts after both an
accepted SDK receipt and a complete timing frame arrive. An accepted receipt
records successful SDK calls; it does not establish hardware readback or RF
coherence. A final stopped-capture status covers overflow after the last CPI.
"""
import argparse
import codecs
import hashlib
import http.server
import json
import math
import signal
import socket
import subprocess
import tempfile
import threading
import time
from pathlib import Path

HOST = "127.0.0.1"
PORT_NAMES = ("map", "detection", "track", "timestamp", "timing", "iqdata")
SDK_STAGES = ("open", "apiVersion", "lock", "enumerate", "select", "unlock",
              "debugEnable", "getDeviceParams", "init", "gainUpdateA", "gainUpdateB")
MAX_FRAME_BYTES = 1 << 20
MAX_TELEMETRY_RECORDS = 10000  # > 2 Hz for the maximum 3600-second observation, plus startup/stop.


def invalid_constant(value):
    raise ValueError(f"non-finite JSON number: {value}")


def finite_float(value):
    result = float(value)
    if not math.isfinite(result):
        invalid_constant(value)
    return result


def bounded_json(value, depth=0):
    if depth > 64:
        raise ValueError("telemetry JSON nesting exceeds 64 levels")
    children = value.values() if isinstance(value, dict) else value if isinstance(value, list) else ()
    for child in children:
        bounded_json(child, depth + 1)
    return value


def strict_json(text):
    try:
        return bounded_json(json.loads(text, parse_constant=invalid_constant, parse_float=finite_float))
    except RecursionError as error:
        raise ValueError("telemetry JSON is nested too deeply") from error


class JsonObjects:
    """One UTF-8 decoder and incomplete JSON buffer per TCP connection."""
    def __init__(self, emit):
        self.emit = emit
        self.utf8 = codecs.getincrementaldecoder("utf-8")()
        self.text = ""
        self.json = json.JSONDecoder(parse_constant=invalid_constant, parse_float=finite_float)

    def feed(self, block, final=False):
        self.text += self.utf8.decode(block, final=final)
        while self.text.strip():
            self.text = self.text.lstrip()
            try:
                value, end = self.json.raw_decode(self.text)
                bounded_json(value)
            except RecursionError as error:
                raise ValueError("timing JSON is nested too deeply") from error
            except json.JSONDecodeError:
                if final:
                    raise ValueError("incomplete or malformed timing JSON at disconnect")
                if len(self.text.encode("utf-8")) > MAX_FRAME_BYTES:
                    raise ValueError("timing JSON frame exceeds 1 MiB")
                return
            if len(self.text[:end].encode("utf-8")) > MAX_FRAME_BYTES:
                raise ValueError("timing JSON frame exceeds 1 MiB")
            if not isinstance(value, dict):
                raise ValueError("timing frame is not an object")
            self.emit(value)
            self.text = self.text[end:]
        self.text = ""


class Sinks:
    """Drain every output stream without letting a decoder failure escape."""
    def __init__(self):
        self.listeners = {}
        for name in PORT_NAMES:
            listener = socket.socket()
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            listener.bind((HOST, 0))
            listener.listen()
            listener.settimeout(.1)
            self.listeners[name] = listener
        self.bytes = dict.fromkeys(PORT_NAMES, 0)
        self.timing = []
        self.received = []
        self.exceptions = []
        self.lock = threading.Lock()
        self.stop = threading.Event()
        self.threads = [threading.Thread(target=self.serve, args=(name, listener), daemon=True)
                        for name, listener in self.listeners.items()]

    def ports(self):
        return {name: listener.getsockname()[1] for name, listener in self.listeners.items()}

    def start(self):
        for thread in self.threads:
            thread.start()

    def emit(self, value):
        with self.lock:
            if len(self.timing) >= MAX_TELEMETRY_RECORDS:
                raise ValueError("too many timing frames")
            self.timing.append(value)
            self.received.append(time.monotonic())

    def serve(self, name, listener):
        while True:
            try:
                connection, _ = listener.accept()
            except TimeoutError:
                if self.stop.is_set():
                    return
                continue
            except OSError:
                return
            # Never carry a partial object or partial UTF-8 character across a reconnect.
            parser = JsonObjects(self.emit) if name == "timing" else None
            with connection:
                connection.settimeout(.1)
                while True:
                    try:
                        block = connection.recv(65536)
                    except TimeoutError:
                        if self.stop.is_set():
                            break
                        continue
                    except OSError as error:
                        if parser is not None:
                            with self.lock:
                                self.exceptions.append(f"timing socket: {error}")
                        break
                    with self.lock:
                        self.bytes[name] += len(block)
                    if parser is not None:
                        try:
                            parser.feed(block)
                        except (ValueError, UnicodeError) as error:
                            with self.lock:
                                self.exceptions.append(str(error))
                            parser = None  # Keep draining this connection after rejection.
                    if not block:
                        break
                if parser is not None:
                    try:
                        parser.feed(b"", final=True)
                    except (ValueError, UnicodeError) as error:
                        with self.lock:
                            self.exceptions.append(str(error))

    def snapshot(self, start=0):
        with self.lock:
            return (list(self.timing[start:]), list(self.received[start:]),
                    list(self.exceptions), dict(self.bytes))

    def close(self):
        # Called after child exit: active sockets drain to EOF before the thread exits.
        self.stop.set()
        for thread in self.threads:
            thread.join(2)
            if thread.is_alive():
                with self.lock:
                    self.exceptions.append("output sink did not terminate")
        for listener in self.listeners.values():
            listener.close()


class StatusServer:
    def __init__(self):
        self.states, self.errors = [], []
        self.lock = threading.Lock()
        owner = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                if self.path != "/capture":
                    self.send_error(404)
                    return
                self.send_response(200)
                self.end_headers()
                self.wfile.write(b"false")

            def do_POST(self):
                if self.path != "/api/processor/status":
                    self.send_error(404)
                    return
                try:
                    length = int(self.headers.get("Content-Length", "0"))
                    if not 0 < length <= MAX_FRAME_BYTES:
                        raise ValueError("invalid processor status length")
                    self.connection.settimeout(1)
                    value = strict_json(self.rfile.read(length))
                    if not isinstance(value, dict):
                        raise ValueError("processor status is not an object")
                    with owner.lock:
                        if len(owner.states) >= MAX_TELEMETRY_RECORDS:
                            raise ValueError("too many processor statuses")
                        owner.states.append(value)
                except (ValueError, OSError) as error:
                    with owner.lock:
                        owner.errors.append(str(error))
                    self.send_error(400)
                    return
                self.send_response(204)
                self.end_headers()

            def log_message(self, *_):
                pass

        self.server = http.server.ThreadingHTTPServer((HOST, 0), Handler)
        self.server.daemon_threads = True
        self.thread = threading.Thread(target=self.server.serve_forever,
                                       kwargs={"poll_interval": .05}, daemon=True)

    @property
    def port(self):
        return self.server.server_address[1]

    def start(self):
        self.thread.start()

    def snapshot(self, start=0):
        with self.lock:
            return list(self.states[start:]), list(self.errors)

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(2)


def config(api, ports, acceleration):
    return {
        "capture": {"fs": 2000000, "fc": 551000000,
                    "device": {"type": "RspDuo", "agcSetPoint": -20, "bandwidthNumber": 5,
                               "gainReduction": [50, 45], "lnaState": 1,
                               "dabNotch": False, "rfNotch": False},
                    "replay": {"state": False, "loop": False, "file": "", "format": "auto"}},
        "process": {
            "performance": {"surveillance_workers": 1, "fft_threads": 4, "acceleration": acceleration},
            "data": {"cpi": .5, "buffer": 2, "overlap": 0},
            "ambiguity": {"delayMin": -10, "delayMax": 400, "dopplerMin": -300, "dopplerMax": 300},
            "clutter": {"enable": True, "delayMin": -10, "delayMax": 400},
            "detection": {"enable": True, "pfa": 1e-5, "nGuard": 2, "nTrain": 6,
                          "minDelay": 5, "minDoppler": 15, "nCentroid": 6},
            "tracker": {"enable": True, "initiate": {"M": 3, "N": 5, "maxAcc": 10},
                        "delete": 10, "smooth": "none"}},
        "network": {"ip": HOST, "ports": dict(api=api, **ports)},
        "save": {"iq": False, "map": False, "detection": False, "timing": False, "path": "/tmp"}}


def finite_number(value):
    if type(value) not in (int, float):
        return False
    try:
        return math.isfinite(value)
    except OverflowError:
        return False


def accepted_receipt(state, cfg):
    """Match RspDuo::startup_receipt_json; successful SDK calls are not readback."""
    receipt = state.get("receiverStartup")
    if not isinstance(receipt, dict):
        return False
    requested, selected, sdk = (receipt.get(key) for key in ("requested", "selected", "sdk"))
    if not all(isinstance(value, dict) for value in (requested, selected, sdk)):
        return False
    stages = sdk.get("stages")
    expected = {key: value for key, value in cfg["capture"]["device"].items() if key != "type"}
    expected.update(frequency=cfg["capture"]["fc"], sampleRate=cfg["capture"]["fs"],
                    serial="", ifBandwidthKhz=1536, ifFrequencyKhz=1620, decimation=1)
    return (state.get("input") == "live" and state.get("receiver") == "RspDuo"
            and state.get("state") == "live"
            and state.get("frequency") == expected["frequency"]
            and state.get("sampleRate") == expected["sampleRate"]
            and receipt.get("schema") == 1 and receipt.get("receiver") == "RspDuo"
            and receipt.get("status") == "accepted"
            and receipt.get("hardwareVerified") is False and receipt.get("readbackAvailable") is False
            and all(requested.get(key) == value for key, value in expected.items())
            and isinstance(selected.get("serial"), str) and bool(selected["serial"])
            and selected.get("tuner") == "Both" and selected.get("mode") == "Dual_Tuner"
            and all(type(selected.get(key)) is int and selected[key] >= 0
                    for key in ("deviceIndex", "hardwareVersion"))
            and finite_number(sdk.get("version")) and 0 < sdk["version"] <= 100
            and isinstance(stages, dict) and all(stages.get(stage) is True for stage in SDK_STAGES))


def channel_counts(value, channels=2):
    return (isinstance(value, list) and len(value) == channels
            and all(type(count) is int and 0 <= count <= (1 << 64) - 1 for count in value))


def final_status_errors(states, cfg):
    final = states[-1] if states and states[-1].get("captureStopped") is True else None
    if final is None:
        return None, ["missing final stopped-capture status"]
    errors = []
    if (final.get("input") != "live" or final.get("receiver") != "RspDuo"
            or final.get("frequency") != cfg["capture"]["fc"]
            or final.get("sampleRate") != cfg["capture"]["fs"]):
        errors.append("final capture status does not match the requested receiver")
    if final.get("state") != "error" and not accepted_receipt(final, cfg):
        errors.append("final capture status has an invalid state or SDK receipt")
    for field in ("captureDroppedSamples", "captureBacklogSamples"):
        counts = final.get(field)
        if not channel_counts(counts):
            errors.append(f"final capture status: invalid {field}")
        elif field == "captureDroppedSamples" and any(counts):
            errors.append("final capture status: capture dropped samples")
        elif field == "captureBacklogSamples" and any(counts):
            errors.append("final capture status: complete CPI backlog remained at stop")
    return final, errors


def timing_errors(frames, start_index=0, require_nonempty=True):
    errors = []
    if require_nonempty and not frames:
        errors.append("no timing frames")
    for index, frame in enumerate(frames, start_index + 1):
        if type(frame.get("nCpi")) is not int or frame["nCpi"] != index:
            errors.append(f"frame {index}: missing or nonsequential nCpi")
        if not finite_number(frame.get("cpi")) or frame["cpi"] < 0:
            errors.append(f"frame {index}: missing or invalid cpi")
        for field in ("captureDroppedSamples", "captureBacklogSamples"):
            values = frame.get(field)
            if not channel_counts(values):
                errors.append(f"frame {index}: invalid {field}")
            elif field == "captureDroppedSamples" and any(values):
                errors.append(f"frame {index}: capture dropped samples")
    return errors


def percentile(values, fraction):
    if not values:
        return None
    values = sorted(values)
    position = (len(values) - 1) * fraction
    lower = int(position)
    upper = min(len(values) - 1, lower + 1)
    return values[lower] + (values[upper] - values[lower]) * (position - lower)


def stop_process(process):
    if process.poll() is not None:
        return False
    process.send_signal(signal.SIGTERM)
    try:
        process.wait(4)
        return False
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(4)
        return True


def run_check(binary, seconds, output, acceleration="cpu", startup_timeout=60, observer=None):
    output.parent.mkdir(parents=True, exist_ok=True)
    status, sinks, process = StatusServer(), Sinks(), None
    started = time.monotonic()
    measurement_start = None
    measurement_end = None
    failures = []
    forced = False
    frame_count = 0
    state_count = 0
    startup_accepted = False
    cfg = config(status.port, sinks.ports(), acceleration)
    try:
        status.start()
        sinks.start()
        if observer is not None:
            observer.start()
        with tempfile.TemporaryDirectory(prefix="vectorwarp-live-rspduo-") as temporary:
            path = Path(temporary) / "live.yml"
            path.write_text(json.dumps(cfg))
            with output.with_suffix(".config.json").open("x") as stream:
                json.dump(cfg, stream, indent=2)
            # Direct file output cannot deadlock on an undrained stdout/stderr pipe.
            with output.with_suffix(".log").open("x") as log:
                try:
                    process = subprocess.Popen([str(binary.resolve()), "--config", str(path)],
                                               stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
                    if observer is not None:
                        observer.processor_started(process.pid)
                except OSError as error:
                    failures.append(f"processor launch failed: {error}")
                startup_deadline = time.monotonic() + startup_timeout
                while process is not None:
                    now = time.monotonic()
                    if process.poll() is not None:
                        failures.append("processor exited before the observation interval completed")
                        break
                    frames, arrivals, sink_errors, _ = sinks.snapshot(frame_count)
                    states, status_errors = status.snapshot(state_count)
                    frame_start = frame_count
                    frame_count += len(frames)
                    state_count += len(states)
                    if observer is not None:
                        try:
                            observer.observe(frames, arrivals, states)
                        except (OSError, ValueError) as error:
                            failures.append(f"diagnostic recording failed: {error}")
                            break
                        if observer.errors:
                            failures.append("diagnostic collector failed during capture")
                            break
                    startup_accepted = startup_accepted or any(
                        accepted_receipt(state, cfg) for state in states)
                    if sink_errors or status_errors:
                        failures.append("telemetry receiver rejected input")
                        break
                    if any(state.get("state") in ("error", "fault") or state.get("error") or state.get("fault") for state in states):
                        failures.append("processor reported a fault")
                        break
                    if frames and timing_errors(frames, frame_start):
                        failures.append("timing telemetry failed validation")
                        break
                    if measurement_start is None:
                        if frame_count and startup_accepted:
                            measurement_start = now
                        elif now >= startup_deadline:
                            failures.append("startup deadline expired before accepted receipt and timing")
                            break
                    elif now - measurement_start >= seconds:
                        measurement_end = now
                        break
                    time.sleep(.02)
                if process is not None:
                    if observer is not None and failures:
                        observer.trigger("harness_failure", {"failures": failures})
                    if observer is not None:
                        observer.processor_stopping()
                    forced = stop_process(process)
    finally:
        if process is not None and process.poll() is None:
            if observer is not None:
                observer.processor_stopping()
            forced = stop_process(process) or forced
        sinks.close()
        status.close()
        if observer is not None:
            tail_frames, tail_arrivals, _, _ = sinks.snapshot(frame_count)
            tail_states, _ = status.snapshot(state_count)
            if observer.started:
                try:
                    observer.observe(tail_frames, tail_arrivals, tail_states)
                except (OSError, ValueError) as error:
                    failures.append(f"final diagnostic recording failed: {error}")
            try:
                observer.close()
            except (OSError, ValueError) as error:
                failures.append(f"diagnostic finalization failed: {error}")
            failures.extend(f"diagnostics: {error}" for error in observer.errors)

    frames, received, sink_errors, stream_bytes = sinks.snapshot()
    states, status_errors = status.snapshot()
    errors = timing_errors(frames)
    failures.extend(errors)
    failures.extend(sink_errors + status_errors)
    final_status, final_errors = final_status_errors(states, cfg)
    failures.extend(final_errors)
    accepted = any(accepted_receipt(state, cfg) for state in states)
    if not accepted:
        failures.append("no accepted SDK startup receipt matching the requested configuration")
    faults = [state for state in states if state.get("state") in ("error", "fault") or state.get("error") or state.get("fault")]
    if faults:
        failures.append("processor reported a fault")
    measured = [frame for frame, arrival in zip(frames, received)
                if measurement_start is not None and measurement_end is not None
                and measurement_start <= arrival <= measurement_end]
    required = max(2, math.floor(seconds / cfg["process"]["data"]["cpi"]) - 1)
    if len(measured) < required:
        failures.append(f"insufficient measured timing frames: {len(measured)} < {required}")
    if measurement_end is None:
        failures.append("observation interval did not complete")
    normal_stop = process is not None and not forced and process.returncode == 0
    if not normal_stop:
        failures.append("processor did not stop cleanly")
    valid_backlog = [frame["captureBacklogSamples"] for frame in frames
                     if channel_counts(frame.get("captureBacklogSamples"))]
    if final_status and channel_counts(final_status.get("captureBacklogSamples")):
        valid_backlog.append(final_status["captureBacklogSamples"])
    cpi = [frame["cpi"] for frame in measured if finite_number(frame.get("cpi")) and frame["cpi"] >= 0]
    evidence = {
        "ok": not failures, "failures": list(dict.fromkeys(failures)),
        "binary": str(binary.resolve()), "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "seconds": seconds, "startupTimeoutSeconds": startup_timeout,
        "startupSeconds": measurement_start - started if measurement_start is not None else None,
        "measuredSeconds": measurement_end - measurement_start if measurement_end is not None else 0,
        "elapsedSeconds": time.monotonic() - started,
        "observedCPIcount": len(frames), "measuredCPIcount": len(measured), "requiredCPIcount": required,
        "processingMs": {"p50": percentile(cpi, .5), "p95": percentile(cpi, .95), "max": max(cpi) if cpi else None},
        "perChannelMaxBacklog": [max(row[index] for row in valid_backlog) for index in range(2)] if valid_backlog else [],
        "perChannelEndDrops": final_status.get("captureDroppedSamples") if final_status else [],
        "perChannelEndBacklog": final_status.get("captureBacklogSamples") if final_status else [],
        "finalCaptureStatus": final_status,
        "sdkStartupAccepted": accepted, "hardwareVerified": False,
        "observationScope": "Cumulative capture FIFO or paired CPI queue loss through receiver stop and reported capture faults; no RF/coherence verification",
        "faults": faults[-8:], "statuses": states, "timing": frames,
        "sinkExceptions": sink_errors, "statusExceptions": status_errors,
        "normalStop": normal_stop, "returnCode": process.returncode if process is not None else None,
        "streamBytes": stream_bytes}
    if observer is not None:
        evidence["diagnostics"] = observer.summary()
    with output.with_suffix(".evidence.json").open("x") as stream:
        json.dump(evidence, stream, indent=2, allow_nan=False)
    return evidence


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--seconds", type=float, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--acceleration", choices=("cpu", "auto", "gpu"), default="cpu")
    parser.add_argument("--startup-timeout", type=float, default=60)
    parser.add_argument("--spike-diagnostics", action="store_true",
                        help="Pi/Linux: bounded system-wide perf flight recorder (requires root)")
    parser.add_argument("--spike-ms", type=float, default=750)
    args = parser.parse_args()
    if not args.binary.is_file() or not math.isfinite(args.seconds) or not 0 < args.seconds <= 3600:
        parser.error("binary must exist and seconds must be finite with 0 < seconds <= 3600")
    if not math.isfinite(args.startup_timeout) or not 0 < args.startup_timeout <= 120:
        parser.error("startup-timeout must be finite with 0 < value <= 120")
    if not math.isfinite(args.spike_ms) or args.spike_ms < 500:
        parser.error("spike-ms must be finite and at least 500")
    observer = None
    if args.spike_diagnostics:
        from pi_spike_diagnostics import SpikeDiagnostics
        observer = SpikeDiagnostics(args.output, spike_ms=args.spike_ms)
    evidence = run_check(args.binary, args.seconds, args.output, args.acceleration,
                         args.startup_timeout, observer)
    print(json.dumps(evidence, allow_nan=False))
    return 0 if evidence["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
