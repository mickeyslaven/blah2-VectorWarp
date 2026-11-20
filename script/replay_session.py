"""One bounded, artifact-bound offline processor lifecycle; no truth inputs or SDR I/O."""
import http.server
import importlib.util
import json
import math
import os
from pathlib import Path
import signal
import socket
import subprocess
import threading
import time

_SPEC = importlib.util.spec_from_file_location("replay_artifact_binding", Path(__file__).with_name("gpu_artifact_binding.py"))
ARTIFACTS = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(ARTIFACTS)

HOST = "127.0.0.1"
PORT_NAMES = ("map", "detection", "track", "illuminator", "timestamp", "timing",
              "iqdata", "manifold")
STATUS_LOCK = threading.Lock()
MAX_RETAINED_OUTPUT = 8 << 20


def free_listener():
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind((HOST, 0))
    listener.listen()
    listener.settimeout(.2)
    return listener


class SinkGroup:
    """Accept and drain processor output so Socket writes cannot block."""
    def __init__(self, max_retained_output=MAX_RETAINED_OUTPUT, retained=None):
        self.listeners = {name: free_listener() for name in PORT_NAMES}
        self.max_retained_output = max_retained_output
        self.drained = {name: threading.Event() for name in PORT_NAMES}
        self.bytes = {name: 0 for name in PORT_NAMES}
        # Retain bounded copies of every mathematical product. Receiver parity
        # must cover detections and tracker evolution, not merely prove that a
        # processor emitted one finite map.
        self.payloads = {name: bytearray() for name in
                         ("map", "detection", "track", "illuminator", "timestamp", "timing")}
        if retained is not None:
            self.payloads = {name: bytearray() for name in retained}
        self.payload_limits = {name: (retained[name] if retained is not None else max_retained_output)
                               for name in self.payloads}
        self.truncated = set()
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
                self.drained[name].clear()
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
                    if name in self.payloads:
                        remaining = self.payload_limits[name] - len(self.payloads[name])
                        if len(block) > remaining:
                            self.truncated.add(name)
                        if remaining > 0:
                            self.payloads[name].extend(block[:remaining])
            self.drained[name].set()

    def ports(self):
        return {name: listener.getsockname()[1] for name, listener in self.listeners.items()}

    def close(self):
        self.stop.set()
        for listener in self.listeners.values():
            listener.close()
        for thread in self.threads:
            if thread.is_alive():
                thread.join(1)


class StatusServer:
    def __init__(self):
        self.statuses = []
        self.overflow = False
        parent = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                if self.path == "/capture/request":
                    self.send_response(200)
                    self.send_header("Content-Type", "application/json")
                    self.end_headers()
                    self.wfile.write(b"{}")
                elif self.path == "/capture":
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
                    if not 0 < size <= 1 << 20:
                        raise ValueError("status exceeds bounded size")
                    value = json.loads(self.rfile.read(size))
                    if not isinstance(value, dict):
                        raise ValueError("status is not an object")
                except (ValueError, json.JSONDecodeError):
                    self.send_error(400)
                    return
                with STATUS_LOCK:
                    if len(parent.statuses) >= 32768:
                        parent.overflow = True
                    else:
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
        if self.thread.is_alive():
            self.server.shutdown()
        self.server.server_close()
        if self.thread.is_alive():
            self.thread.join(2)


def stop_group(process, grace=5):
    """The child was started with start_new_session: never signal another job."""
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    forced = False
    try:
        process.wait(timeout=grace)
    except subprocess.TimeoutExpired:
        forced = True
    # A worker may outlive the group leader. Reap the leader AND bound the
    # entire private group; a successful leader exit cannot hide a leaked child.
    deadline = time.monotonic() + (0 if forced else grace)
    while True:
        try:
            os.killpg(process.pid, 0)
        except ProcessLookupError:
            return forced
        if time.monotonic() >= deadline:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait(timeout=grace)
            return True
        time.sleep(.05)


def group_alive(process):
    try:
        os.killpg(process.pid, 0)
        return True
    except ProcessLookupError:
        return False


def finish_after_eof(process, timeout):
    """Let the leader drain its workers; group signals are failure cleanup only.

    Replay deliberately idles at complete waiting for controls. Its terminal
    capture-status post follows SIGTERM; terminal timing is live-only. Waiting
    for that post before requesting shutdown would deadlock this contract.
    """
    requested = process.poll() is None
    if requested:
        process.send_signal(signal.SIGTERM)
    process.wait(timeout=timeout)
    if process.returncode != 0 or group_alive(process):
        raise RuntimeError("unclean EOF shutdown or surviving private worker")
    return "graceful-eof-stop" if requested else "natural-exit"


def completed_status(statuses):
    return next((item for item in reversed(statuses) if item.get("state") == "complete"), None)


class CaptureErrorWatch:
    """A transient error can recover; only sustained errors without progress fail."""
    def __init__(self, stall_seconds):
        self.stall_seconds = stall_seconds
        self.since = self.high_water = None

    def observe(self, status, now):
        if status.get("state") != "error":
            self.since = self.high_water = None
            return None
        position = status.get("positionSamples")
        if type(position) is not int or position < 0:
            position = None
        if self.since is None or (position is not None and
                                 (self.high_water is None or position > self.high_water)):
            self.since, self.high_water = now, position
        elapsed = now - self.since
        if elapsed < self.stall_seconds:
            return None
        return {"state": "error", "position_samples": position,
                "highest_position_samples": self.high_water,
                "no_progress_seconds": elapsed, "limit_seconds": self.stall_seconds,
                "error": str(status.get("error", ""))[:4000]}


class BoundedLog:
    def __init__(self, pipe, path, limit):
        self.pipe, self.path, self.limit = pipe, path, limit
        self.overflow = threading.Event()
        self.error = None
        self.thread = threading.Thread(target=self.copy, daemon=True)

    def copy(self):
        try:
            with self.path.open("xb") as output:
                count = 0
                while block := self.pipe.read(65536):
                    retained = block[:max(0, self.limit - count)]
                    output.write(retained)
                    count += len(retained)
                    if len(retained) != len(block):
                        self.overflow.set()
        except Exception as error:
            self.error = str(error)


def json_values(payload, name):
    text = bytes(payload).decode()
    decoder = json.JSONDecoder()
    values = []
    position = 0
    while position < len(text):
        while position < len(text) and text[position].isspace():
            position += 1
        if position == len(text):
            break
        try:
            value, position = decoder.raw_decode(text, position)
        except json.JSONDecodeError as error:
            raise AssertionError(f"{name} output is not concatenated JSON: {error}") from error
        values.append(value)
    return values


def assert_finite(value, name="map"):
    if isinstance(value, dict):
        for key, item in value.items():
            assert_finite(item, f"{name}.{key}")
    elif isinstance(value, list):
        for index, item in enumerate(value):
            assert_finite(item, f"{name}[{index}]")
    elif isinstance(value, (int, float)) and not isinstance(value, bool) and not math.isfinite(value):
        raise AssertionError(f"{name} contains a non-finite number")


class Experiment:
    """Execution policy only; timeline/admission truth cannot enter the child."""
    def __init__(self, binary, timeout, *, gpu=False, binary_sha256=None,
                 required=("timing",), retained=None, eof=None, error_stall_seconds=5):
        if not math.isfinite(timeout) or not 0 < timeout <= 1800:
            raise ValueError("timeout must be positive and at most 1800 seconds")
        if not math.isfinite(error_stall_seconds) or not 0 < error_stall_seconds <= 60:
            raise ValueError("error stall limit must be positive and at most 60 seconds")
        self.binary, self.timeout, self.gpu = Path(binary).resolve(strict=True), timeout, gpu
        self.binary_sha256, self.required = binary_sha256, tuple(required)
        self.retained = dict(retained) if retained is not None else None
        self.eof, self.error_stall_seconds = dict(eof or {}), error_stall_seconds
        if (not self.required or len(set(self.required)) != len(self.required) or
                not set(self.required) <= set(PORT_NAMES) - {"iqdata"}):
            raise ValueError("required replay products are invalid")
        if retained is not None and (not set(self.required) <= retained.keys() or
                not retained.keys() <= set(PORT_NAMES) - {"iqdata"} or
                any(type(limit) is not int or not 0 < limit <= 128 << 20 for limit in retained.values())):
            raise ValueError("retained replay products/limits are invalid")


class ReplaySession:
    """Own exactly one private processor group and drain every evidence channel."""
    def __init__(self, experiment, root, receipt=None):
        self.experiment, self.root = experiment, Path(root)
        self.receipt = receipt if receipt is not None else {}
        self.status = self.sinks = self.process = self.logger = self.channel = None
        self.stopped = False

    def __enter__(self):
        try:
            spec = self.experiment
            self.receipt["artifact_binding"] = ARTIFACTS.prepare(spec.binary, spec.gpu, spec.binary_sha256)
            self.receipt["experiment"] = {"required_products": list(spec.required),
                "retained_bytes": spec.retained, "expected_eof": spec.eof,
                "timeout_seconds": spec.timeout, "error_stall_seconds": spec.error_stall_seconds}
            self.status = StatusServer()
            self.sinks = SinkGroup(max_retained_output=32 << 20, retained=spec.retained)
            self.status.start()
            self.sinks.start()
            return self
        except BaseException:
            self.__exit__(None, None, None)
            raise

    def execute(self, config):
        if self.process is not None:
            raise RuntimeError("a replay session can launch only one processor")
        spec, receipt = self.experiment, self.receipt
        self.channel = ARTIFACTS.ReceiptChannel(receipt["artifact_binding"], config)
        config_path = self.root / "config.json"
        config_path.write_text(json.dumps(config, indent=2, allow_nan=False) + "\n")
        env = self.channel.environment
        env.update({name: "1" for name in ("OPENBLAS_NUM_THREADS", "OMP_NUM_THREADS",
                   "BLIS_NUM_THREADS", "MKL_NUM_THREADS", "VECLIB_MAXIMUM_THREADS", "NUMEXPR_NUM_THREADS")})
        env["VECTORWARP_FFTW_PLAN"] = "estimate"
        ARTIFACTS.verify_files(receipt["artifact_binding"])
        self.process = subprocess.Popen([str(spec.binary), "-c", str(config_path)], cwd=self.root,
            env=env, pass_fds=self.channel.pass_fds, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, start_new_session=True)
        self.channel.launched()
        self.logger = BoundedLog(self.process.stdout, self.root / "processor.log", 16 << 20)
        self.logger.thread.start()
        deadline = time.monotonic() + spec.timeout
        error_watch = CaptureErrorWatch(spec.error_stall_seconds)
        while True:
            if self.logger.overflow.is_set() or self.logger.error:
                raise RuntimeError("processor log exceeded its bound or could not be retained")
            if self.sinks.truncated or self.status.overflow:
                raise RuntimeError("replay evidence retention exceeded")
            states = self.status.snapshot()
            latest = states[-1] if states else {}
            stalled = error_watch.observe(latest, time.monotonic())
            if stalled is not None:
                receipt["capture_error_stall"] = stalled
                raise RuntimeError("persistent capture error without sample-position progress")
            complete = completed_status(states)
            if complete is not None and latest.get("state") in ("complete", "stopped"):
                expected = {"input": "replay", "error": "", "loops": 0, **spec.eof}
                position, total = complete.get("positionSamples"), complete.get("totalSamples")
                if (any(complete.get(key) != value for key, value in expected.items()) or
                        type(position) is not int or position < 0 or type(total) is not int or position != total):
                    raise RuntimeError("completed EOF does not match expected sample contract")
                receipt["eof_complete_status"] = complete
                break
            if self.process.poll() is not None:
                # Temporary replay roots are intentionally removed on exit.
                # Preserve a bounded tail in the exception so an early native
                # startup failure is diagnosable instead of deleting its only
                # evidence during context-manager cleanup.
                self.logger.thread.join(2)
                log_path = self.root / "processor.log"
                tail = log_path.read_text(errors="replace")[-4096:] if log_path.exists() else ""
                raise RuntimeError(
                    f"processor exited before EOF: {self.process.returncode}\n{tail}")
            if time.monotonic() >= deadline:
                raise RuntimeError("replay deadline expired")
            time.sleep(.05)
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise RuntimeError("replay deadline expired before terminal drain")
        receipt["shutdown"] = finish_after_eof(self.process, timeout=min(30, remaining))
        self.stopped = True
        for name in spec.required:
            if not self.sinks.drained[name].wait(2):
                raise RuntimeError(f"{name} sink did not drain after child shutdown")
        self.logger.thread.join(2)
        if (self.logger.thread.is_alive() or self.logger.overflow.is_set() or self.logger.error or
                self.sinks.truncated or self.status.overflow):
            raise RuntimeError("unclean child shutdown or truncated evidence")
        final = self.status.snapshot()[-1]
        channels = config["capture"]["device"]["channel_count"]
        if (final.get("captureStopped") is not True or final.get("error") or
                final.get("captureDroppedSamples") != [0] * channels or
                final.get("captureBacklogSamples") != [0] * channels):
            raise RuntimeError("missing/lossy terminal stopped-capture status")
        receipt["artifact_acceptance"] = ARTIFACTS.validate(receipt["artifact_binding"],
            self.channel.finish(), self.process.pid, require_worker=spec.gpu)
        products = {name: json_values(self.sinks.payloads[name], name) for name in spec.required}
        for name, frames in products.items():
            assert_finite(frames, name)
        return products

    def __exit__(self, *_):
        try:
            if self.process is not None:
                forced = not self.stopped and stop_group(self.process)
                self.receipt.update(exit_code=self.process.returncode, forced_stop=bool(forced))
        finally:
            if self.logger is not None:
                self.logger.thread.join(2)
            if self.process is not None and self.process.stdout is not None:
                self.process.stdout.close()
            if self.channel is not None:
                self.channel.close()
            if self.sinks is not None:
                self.sinks.close()
                self.receipt.update(sink_bytes=self.sinks.bytes, truncated_sinks=sorted(self.sinks.truncated))
                for name in self.experiment.required:
                    (self.root / f"{name}.raw").write_bytes(self.sinks.payloads[name])
            if self.status is not None:
                self.status.close()
                (self.root / "status.json").write_text(json.dumps(self.status.snapshot(), indent=2) + "\n")
