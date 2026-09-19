#!/usr/bin/env python3
"""Run real native Heimdall against a test-only driver with no USB dependency.

The disposable executable's RTL-SDR load command is replaced, then verified before
launch. This qualifies software behavior with simulated samples, not RF hardware,
USB bandwidth, clock drift, or physical Kraken calibration.
"""
from __future__ import annotations

import argparse
import collections
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import signal
import socket
import struct
import subprocess
import threading
import time


def checked(argv, **kwargs):
    return subprocess.run(argv, check=True, text=True, capture_output=True, **kwargs).stdout


def load_commands(binary):
    return [line.strip().split(" (", 1)[0]
            for line in checked(["otool", "-L", str(binary)]).splitlines()[1:]]


def prepare(args, work):
    fake = work / "libvectorwarp-simulated-rtlsdr.dylib"
    checked(["xcrun", "clang++", "-std=c++17", "-O2", "-dynamiclib", "-pthread",
             "-I", str(args.include), str(Path(__file__).with_name("fake_rtlsdr.cpp")),
             "-Wl,-install_name," + str(fake), "-o", str(fake)])
    # A test-driver dependency on libusb or librtlsdr would invalidate isolation.
    assert not any("libusb" in p or "librtlsdr" in p for p in load_commands(fake))
    binary = work / "heimdall-simulated"
    shutil.copy2(args.heimdall, binary)
    rtl = [p for p in load_commands(binary) if "librtlsdr" in p]
    assert len(rtl) == 1, f"Expected exactly one RTL-SDR load command, got {rtl}"
    checked(["install_name_tool", "-change", rtl[0], str(fake), str(binary)])
    checked(["codesign", "--force", "--sign", "-", str(binary)])
    linked = load_commands(binary)
    assert str(fake) in linked and not any("librtlsdr" in p for p in linked), linked
    assert "_libusb_" not in checked(["nm", "-u", str(binary)]), "Heimdall calls libusb directly"
    (work / "binary-manifest.json").write_text(json.dumps({
        "simulatedOnly": True, "originalBinary": str(args.heimdall),
        "originalSha256": hashlib.sha256(args.heimdall.read_bytes()).hexdigest(),
        "injectedBinarySha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "fakeDriverSha256": hashlib.sha256(fake.read_bytes()).hexdigest(),
        "loadCommands": linked,
    }, indent=2) + "\n")
    # Keep libraries referred to by @executable_path available in the disposable
    # directory, without introducing any RTL-SDR or USB alternative.
    for item in linked:
        if item.startswith("@executable_path/"):
            relative = item.removeprefix("@executable_path/")
            assert "/" not in relative, f"Unexpected relative dependency: {item}"
            source = args.heimdall.parent / relative
            assert source.is_file(), source
            (work / relative).symlink_to(source.resolve())
    return binary


def reserve_ports(ports):
    for port in ports:
        with socket.socket() as sock:
            try:
                sock.bind(("127.0.0.1", port))
            except OSError as exc:
                raise RuntimeError(f"Test requires unused loopback port {port}") from exc


def connect(port, process, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise AssertionError(f"Heimdall exited before listening: {process.returncode}")
        try:
            sock = socket.create_connection(("127.0.0.1", port), timeout=.5)
            sock.settimeout(.5)
            return sock
        except (ConnectionRefusedError, TimeoutError):
            time.sleep(.05)
    raise AssertionError(f"No simulated Heimdall listener on port {port}")


def verify_websocket_origins(port, process):
    for origin, expected in (("https://foreign.invalid", 403), (None, 403),
                             (f"http://127.0.0.1:{port}", 101),
                             (f"http://localhost:{port}", 101)):
        with connect(port, process) as sock:
            request = (f"GET /ws HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\n"
                       "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                       "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                       "Sec-WebSocket-Version: 13\r\n")
            if origin:
                request += f"Origin: {origin}\r\n"
            sock.sendall((request + "\r\n").encode())
            response = b""
            deadline = time.monotonic() + 5
            while b"\r\n" not in response and time.monotonic() < deadline:
                try:
                    block = sock.recv(4096)
                except socket.timeout:
                    continue
                assert block, (origin, "WebSocket handshake closed without a response")
                response += block
                assert len(response) <= 65536
            assert response.startswith(f"HTTP/1.1 {expected} ".encode()), (origin, response[:200])


def websocket_command(port, process, value):
    with connect(port, process) as sock:
        sock.sendall((f"GET /ws HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\n"
                      f"Origin: http://127.0.0.1:{port}\r\n"
                      "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                      "Sec-WebSocket-Version: 13\r\n\r\n").encode())
        response = bytearray()
        while b"\r\n\r\n" not in response:
            block = sock.recv(4096)
            assert block and len(response) < 65536
            response.extend(block)
        assert response.startswith(b"HTTP/1.1 101 "), response[:200]
        data = value.encode()
        assert len(data) < 126
        mask = b"test"
        sock.sendall(bytes((0x81, 0x80 | len(data))) + mask +
                     bytes(byte ^ mask[i % 4] for i, byte in enumerate(data)))
        time.sleep(.05)


class Frames:
    def __init__(self, sock):
        self.sock = sock
        self.lock = threading.Lock()
        self.frames = collections.deque(maxlen=256)
        self.latest_valid = None
        self.count = 0
        self.error = None
        self.stopped = threading.Event()
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def exact(self, length):
        data = bytearray()
        while len(data) < length and not self.stopped.is_set():
            try:
                block = self.sock.recv(length - len(data))
            except socket.timeout:
                continue
            if not block:
                raise EOFError("Heimdall MCHQ stream closed")
            data.extend(block)
        if len(data) != length:
            raise EOFError("Stopped")
        return bytes(data)

    def run(self):
        try:
            while not self.stopped.is_set():
                fields = struct.unpack(">8I", self.exact(32))
                magic, channels, samples, phase, noise, epoch, group, retuning = fields
                assert magic == 0x4D434851 and channels == 5 and 0 < samples <= 262144, fields
                metadata = struct.unpack("<" + "ff" * channels, self.exact(channels * 8))
                payload = self.exact(channels * samples * 2)
                valid = (phase & 0xff) == 4 and not phase & 0x200 and noise == 0 and retuning == 0
                # Deterministic test driver has a twofold noise-on envelope.
                # Sample the reference channel only, avoiding hot-path Python work.
                envelope = math.sqrt(sum((payload[i] - 127.5) ** 2
                                         for i in range(32, min(samples * 2, 8192), 16)) /
                                     len(range(32, min(samples * 2, 8192), 16)))
                if valid:
                    assert envelope < 28.0, f"Calibration-noise payload labeled valid: RMS={envelope}"
                    assert max(abs(value - 127.5) for value in payload[:64]) < 80, \
                        "Calibration marker leaked from FIR history into the valid block head"
                with self.lock:
                    self.count += 1
                    frame = dict(number=self.count, phase=phase, noise=noise, epoch=epoch,
                                 retuning=retuning, frequency=metadata[0], gain=metadata[1],
                                 valid=valid, samples=samples, referenceRms=envelope)
                    self.frames.append(frame)
                    if valid:
                        self.latest_valid = (frame, payload)
        except BaseException as exc:
            if not self.stopped.is_set():
                self.error = repr(exc)

    def mark(self):
        with self.lock:
            return self.count

    def wait(self, predicate, after=0, timeout=60):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            with self.lock:
                found = next((f.copy() for f in self.frames
                              if f["number"] > after and predicate(f)), None)
            if found:
                return found
            if self.error:
                raise AssertionError(self.error)
            time.sleep(.02)
        with self.lock:
            tail = list(self.frames)[-3:]
        raise AssertionError(f"Expected frame not observed after {timeout}s; last={tail}")

    def close(self):
        self.stopped.set()
        self.sock.close()
        self.thread.join(timeout=2)


class Control:
    def __init__(self, sock):
        self.sock = sock
        self.lock = threading.Lock()
        self.messages = collections.deque(maxlen=128)
        self.count = 0
        self.error = None
        self.stopped = threading.Event()
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        pending = b""
        try:
            while not self.stopped.is_set():
                try:
                    data = self.sock.recv(65536)
                except socket.timeout:
                    continue
                if not data:
                    break
                pending += data
                assert len(pending) <= 262144
                while b"\n" in pending:
                    line, pending = pending.split(b"\n", 1)
                    if not line.strip():
                        continue
                    message = json.loads(line)
                    if message.get("calibration_state") == "CONVERGED":
                        assert not message.get("recovering") and not message.get("reconfiguring"), message
                        assert message.get("noise_source") in (False, 0), message
                    with self.lock:
                        self.count += 1
                        message["_fixtureSequence"] = self.count
                        self.messages.append(message)
        except BaseException as exc:
            if not self.stopped.is_set():
                self.error = repr(exc)

    def mark(self):
        with self.lock:
            return self.count

    def wait_state(self, state, after, timeout=5):
        states = (state,) if isinstance(state, str) else state
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.error:
                raise AssertionError(self.error)
            with self.lock:
                found = next((m.copy() for m in self.messages if m["_fixtureSequence"] > after
                              and m.get("calibration_state") in states), None)
                if found:
                    return found
            time.sleep(.02)
        raise AssertionError(f"No control state {state} after message {after}")

    def wait_calibrated(self, frequency, timeout=5, gain=None):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.error:
                raise AssertionError(self.error)
            with self.lock:
                messages = list(self.messages)
            if any(m.get("calibration_state") == "CONVERGED" and
                   m.get("settings", {}).get("center_freq") == frequency and
                   (gain is None or m.get("settings", {}).get("gain") == gain) for m in messages):
                return
            time.sleep(.02)
        raise AssertionError(f"No CONVERGED control status at {frequency}; last={messages[-2:]}")

    def close(self):
        self.stopped.set()
        self.sock.close()
        self.thread.join(timeout=2)


def numerical_check(frames):
    with frames.lock:
        frame, payload = frames.latest_valid
    samples = frame["samples"]
    channels = []
    # Avoid FIR startup samples and use the unchanged native convergence bounds:
    # <=1 degree residual phase and <=2dB residual amplitude; integer lag must be0.
    for ch in range(5):
        raw = payload[ch * samples * 2:(ch + 1) * samples * 2]
        channels.append([complex(raw[2*i] - 127.5, raw[2*i+1] - 127.5)
                         for i in range(16, min(samples, 4096))])
    reference = channels[0]
    results = []
    for channel, values in enumerate(channels[1:], 1):
        cross = sum(a.conjugate() * b for a, b in zip(reference, values))
        phase = math.degrees(math.atan2(cross.imag, cross.real))
        ratio = math.sqrt(sum(abs(x)**2 for x in values) / sum(abs(x)**2 for x in reference))
        db = 20 * math.log10(ratio)
        lag_scores = {}
        for lag in range(-3, 4):
            a = reference[max(0, -lag):len(reference) - max(0, lag)]
            b = values[max(0, lag):len(values) - max(0, -lag)]
            lag_scores[lag] = abs(sum(x.conjugate() * y for x, y in zip(a, b)))
        best_lag = max(lag_scores, key=lag_scores.get)
        assert abs(phase) <= 1.0 and abs(db) <= 2.0 and best_lag == 0, (channel, phase, db, best_lag)
        results.append(dict(channel=channel, phaseDegrees=phase, amplitudeDb=db, integerLag=best_lag))
    return results


def command(sock, value):
    # Fragment deliberately; the server must retain incomplete JSON objects.
    data = json.dumps(value, separators=(",", ":")).encode() + b"\n"
    sock.sendall(data[:7])
    time.sleep(.005)
    sock.sendall(data[7:])


def driver_events(work):
    path = work / "driver.jsonl"
    return [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []


def stop(process):
    if process.poll() is None:
        process.send_signal(signal.SIGTERM)
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)
        raise AssertionError("Simulated Heimdall did not stop within10s")


def verify_shutdown(events):
    assert not any(e["event"] in ("close_while_reading", "write_after_close") for e in events)
    for channel in range(5):
        reads = [e for e in events if e["channel"] == channel and e["event"] == "read_exit"]
        closes = [e for e in events if e["channel"] == channel and e["event"] == "close"]
        assert reads and closes and closes[-1]["timeMs"] >= reads[-1]["timeMs"], channel
        assert closes[-1]["value"] == 0, f"Noise source left on at close: {channel}"


def run_case(args, binary, work, name, fail=None, count=5):
    case = work / name
    case.mkdir(mode=0o700)
    (case / "command").write_text("fail_noise_off" if name == "noise_failure" else "")
    (case / "index.html").write_text("<!doctype html><title>Simulated Heimdall test</title>\n")
    # Preserve only ordinary user PATH/HOME etc; do not inherit library injection.
    env = {k: v for k, v in os.environ.items() if not k.startswith(
        ("DYLD_", "LD_", "VECTORWARP_KRAKEN_SIM_", "HEIMDALL_")) and k != "LIBRTLSDR_OPT"}
    env.update(VECTORWARP_KRAKEN_SIM_ROOT=str(case), VECTORWARP_KRAKEN_SIM_COUNT=str(count),
               HEIMDALL_VERBOSE_LOG="1", HOME=str(case), HEIMDALL_BIND_HOST="127.0.0.1",
               HEIMDALL_DATA_PORT=str(args.data_port), HEIMDALL_CONTROL_PORT=str(args.control_port),
               HEIMDALL_WEB_PORT=str(args.web_port), HEIMDALL_RTL_PORT=str(args.rtl_port))
    if fail:
        env["VECTORWARP_KRAKEN_SIM_FAIL"] = fail
    if name == "restart_retry":
        env.update(HEIMDALL_CENTER_FREQ_HZ="101000000", HEIMDALL_GAIN_TENTHS="500")
    log = open(case / "heimdall.log", "w")
    process = subprocess.Popen([str(binary), "-n", "5", "--serials", "1000,1001,1002,1003,1004"],
                               cwd=case, env=env, stdout=log, stderr=subprocess.STDOUT)
    frames = control = control_reader = None
    report = {"case": name, "simulatedOnly": True}
    try:
        if fail or count == 0:
            process.wait(timeout=15)
            assert process.returncode != 0, "Required setup failure was accepted"
            events = driver_events(case)
            assert events and not any(e["event"] == "read_start" for e in events)
            opened = {e["channel"] for e in events if e["event"] == "open"}
            closed = {e["channel"] for e in events if e["event"] == "close"}
            assert opened == closed, (opened, closed)
            assert all(e["value"] == 0 for e in events if e["event"] == "close"), \
                "Partial startup left simulated calibration noise enabled"
            report["startupRejected"] = True
        elif name in ("noise_failure", "settings_failure", "short_callback"):
            frames = Frames(connect(args.data_port, process))
            control = connect(args.control_port, process)
            control_reader = Control(control)
            injection = "injected_noise_failure"
            if name in ("settings_failure", "short_callback"):
                frames.wait(lambda f: f["valid"], timeout=args.calibration_timeout)
                control_reader.wait_calibrated(100000000)
                mark = frames.mark()
                control_mark = control_reader.mark()
                (case / "command").write_text("fail_frequency" if name == "settings_failure" else "short_callback")
                if name == "settings_failure":
                    command(control, {"command": "set_frequency", "frequency": 101000000})
                if name == "settings_failure":
                    # A latched hardware failure may stop data entirely. FAILED
                    # control status plus no subsequent usable IQ is required.
                    control_reader.wait_state("FAILED", after=control_mark)
                    invalid = {"number": frames.mark()}
                else:
                    invalid = frames.wait(lambda f: not f["valid"], after=mark, timeout=5)
                injection = "injected_runtime_failure" if name == "settings_failure" else "injected_short_callback"
            deadline = time.monotonic() + args.calibration_timeout
            while time.monotonic() < deadline:
                if any(e["event"] == injection for e in driver_events(case)):
                    break
                assert process.poll() is None and not frames.error, frames.error
                time.sleep(.05)
            else:
                raise AssertionError(f"Simulation did not reach {injection}")
            time.sleep(2)
            assert not frames.error, frames.error
            if name == "noise_failure":
                assert frames.latest_valid is None
            else:
                with frames.lock:
                    following = [f for f in frames.frames if f["number"] > invalid["number"]]
                assert (following or name == "settings_failure") and not any(f["valid"] for f in following), following[-3:]
                if name == "settings_failure":
                    report["controlFailureState"] = "FAILED"
                    report["dataStoppedAfterFailure"] = not following
            assert not control_reader.error, control_reader.error
            (case / "command").write_text("")
            stop(process)
            verify_shutdown(driver_events(case))
            report["failureRemainedInvalid"] = True
            report["orderedShutdown"] = True
        elif name == "restart_retry":
            frames = Frames(connect(args.data_port, process))
            control = connect(args.control_port, process)
            control_reader = Control(control)
            frames.wait(lambda f: f["valid"] and f["frequency"] == 101000000 and f["gain"] == 50,
                        timeout=args.calibration_timeout)
            report["numerical"] = numerical_check(frames)
            control_reader.wait_calibrated(101000000, gain=50)
            assert not frames.error and not control_reader.error
            stop(process)
            verify_shutdown(driver_events(case))
            report["explicitRestartRecovered"] = True
            report["orderedShutdown"] = True
        elif name == "reconfigure_shutdown":
            frames = Frames(connect(args.data_port, process))
            control = connect(args.control_port, process)
            control_reader = Control(control)
            frames.wait(lambda f: f["valid"], timeout=args.calibration_timeout)
            before = len(driver_events(case))
            command(control, {"command": "set_num_elements", "num_elements": 4})
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                if any(e["event"] == "cancel" for e in driver_events(case)[before:]):
                    break
                assert process.poll() is None
                time.sleep(.005)
            else:
                raise AssertionError("Element-count reconfiguration did not enter reader shutdown")
            # SIGTERM lands while the detached reconfiguration worker is waiting
            # for the deliberately delayed readers. Main must wait for ownership.
            stop(process)
            assert process.returncode == 0, process.returncode
            verify_shutdown(driver_events(case))
            report["inFlightReconfigureShutdown"] = True
            report["orderedShutdown"] = True
        else:
            frames = Frames(connect(args.data_port, process))
            control = connect(args.control_port, process)
            control_reader = Control(control)
            verify_websocket_origins(args.web_port, process)
            report["websocketOriginsChecked"] = True
            frames.wait(lambda f: not f["valid"], timeout=10)
            first = frames.wait(lambda f: f["valid"], timeout=args.calibration_timeout)
            report["initialCalibratedFrame"] = first["number"]
            report["numerical"] = numerical_check(frames)
            control_reader.wait_calibrated(100000000)
            print(json.dumps({"case": name, "stage": "calibrated", "numerical": report["numerical"]}), flush=True)

            mark = frames.mark()
            command(control, {"command": "set_frequency", "frequency": 101000000})
            invalid = frames.wait(lambda f: not f["valid"], after=mark, timeout=5)
            retuned = frames.wait(lambda f: f["valid"] and f["frequency"] == 101000000,
                                  after=invalid["number"], timeout=args.calibration_timeout)
            report["retuneCalibratedFrame"] = retuned["number"]
            report["retuneNumerical"] = numerical_check(frames)
            control_reader.wait_calibrated(101000000)
            print(json.dumps({"case": name, "stage": "retuned"}), flush=True)

            report["gainCalibratedFrames"] = {}
            for gain in (20, 40, 50):
                mark = frames.mark()
                command(control, {"command": "set_gain", "gain": gain})
                invalid = frames.wait(lambda f: not f["valid"], after=mark, timeout=5)
                changed = frames.wait(lambda f: f["valid"] and f["gain"] == gain,
                                      after=invalid["number"], timeout=args.calibration_timeout)
                control_reader.wait_calibrated(101000000, gain=gain)
                report["gainCalibratedFrames"][str(gain)] = dict(frame=changed["number"],
                                                               numerical=numerical_check(frames))
                print(json.dumps({"case": name, "stage": "gain_changed", "requestedGainDb": gain}), flush=True)

            mark = frames.mark()
            loss_control_mark = control_reader.mark()
            temp = case / "command.next"
            temp.write_text("stall_channel_3")
            temp.replace(case / "command")
            # A missing device may stop all frames. Driver-side events/logs prove
            # loss, then resumed output must advertise invalidity before reuse.
            time.sleep(4.0)
            loss_status = control_reader.wait_state(("PENDING", "FAILED"), after=loss_control_mark)
            assert loss_status.get("coherence_events", 0) > 0, loss_status
            reject_after = frames.mark()
            temp.write_text("")
            temp.replace(case / "command")
            # A resumed source must either recover verified coherence or report
            # bounded failure with a working deliberate retry. PENDING forever
            # cannot pass, even when it safely withholds all IQ.
            for attempt in range(2):
                deadline = time.monotonic() + args.recovery_timeout
                while time.monotonic() < deadline:
                    assert process.poll() is None and not frames.error, frames.error
                    assert not control_reader.error, control_reader.error
                    with frames.lock:
                        recovered = frames.latest_valid and frames.latest_valid[0]["number"] > reject_after
                    with control_reader.lock:
                        failed = any(m["_fixtureSequence"] > loss_status["_fixtureSequence"] and
                                     m.get("calibration_state") == "FAILED" for m in control_reader.messages)
                    if recovered or failed:
                        break
                    time.sleep(.05)
                else:
                    raise AssertionError("Loss recovery remained PENDING past its bounded deadline")
                if recovered:
                    report["lossRecoveryNumerical"] = numerical_check(frames)
                    control_reader.wait_calibrated(101000000, gain=50)
                    time.sleep(1)
                    report["lossRecoveryStableNumerical"] = numerical_check(frames)
                    report["lossRecoveredSimulated"] = True
                    break
                assert attempt == 0, "Deliberate calibration retry also failed"
                report["lossReportedFailed"] = True
                for channel in range(5):
                    changes = [e for e in driver_events(case) if e["event"] == "noise" and e["channel"] == channel]
                    assert changes[-1]["value"] == 0, (channel, changes[-1])
                # The fake deliberately cannot actuate clock correction. Prove
                # the documented explicit restart path with a fresh coherent
                # source, not a fabricated claim of physical servo recovery.
                frames.close()
                control_reader.close()
                stop(process)
                verify_shutdown(driver_events(case))
                report["restartRetry"] = run_case(args, binary, case, "restart_retry")
                break
            assert not frames.error, frames.error
            assert not control_reader.error, control_reader.error

            stop(process)
            events = driver_events(case)
            assert process.returncode == 0, process.returncode
            verify_shutdown(events)
            assert {e["channel"] for e in events if e["event"] == "delayed_noise_completion"} == set(range(5))
            for actual in (207, 402, 496):
                assert {e["channel"] for e in events if e["event"] == "actual_gain" and e["value"] == actual} == set(range(5))
            report["frames"] = frames.mark()
            report["delayedOldNoiseCallbacksRejected"] = True
            report["orderedShutdown"] = True
        events = driver_events(case)
        assert events and any(e["event"] == "simulated_enumeration" for e in events)
        report["driverEvents"] = len(events)
        return report
    finally:
        if process.poll() is None:
            (case / "command").write_text("")
            stop(process)
        if frames:
            frames.close()
            with frames.lock:
                (case / "frame-tail.json").write_text(json.dumps(list(frames.frames), indent=2) + "\n")
        if control_reader:
            control_reader.close()
            with control_reader.lock:
                (case / "control-tail.json").write_text(json.dumps(list(control_reader.messages), indent=2) + "\n")
        elif control:
            control.close()
        log.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--heimdall", type=Path, required=True)
    parser.add_argument("--include", type=Path, required=True, help="Kraken fork rtl-sdr.h directory")
    parser.add_argument("--output", type=Path, required=True, help="New directory for retained test evidence")
    parser.add_argument("--data-port", type=int, default=29191)
    parser.add_argument("--control-port", type=int, default=29192)
    parser.add_argument("--web-port", type=int, default=29193)
    parser.add_argument("--rtl-port", type=int, default=29194)
    parser.add_argument("--calibration-timeout", type=float, default=90)
    parser.add_argument("--recovery-timeout", type=float, default=135)
    parser.add_argument("--prepare-only", action="store_true")
    args = parser.parse_args()
    args.heimdall = args.heimdall.resolve(strict=True)
    args.include = args.include.resolve(strict=True)
    args.output = args.output.resolve()
    args.output.mkdir(mode=0o700, parents=True, exist_ok=False)
    binary = prepare(args, args.output)
    if args.prepare_only:
        print(json.dumps({"prepared": str(binary), "simulatedOnly": True}))
        return
    reserve_ports([args.data_port, args.control_port, args.web_port, args.rtl_port])
    reports = []
    for name, fail, count in [("no_devices", None, 0), ("setup_failure", "sample_rate", 5),
                              ("noise_failure", None, 5), ("settings_failure", None, 5),
                              ("short_callback", None, 5),
                              ("reconfigure_shutdown", None, 5),
                              ("coherence", None, 5)]:
        report = run_case(args, binary, args.output, name, fail, count)
        reports.append(report)
        (args.output / "result.json").write_text(json.dumps(reports, indent=2) + "\n")
        print(json.dumps(report), flush=True)


if __name__ == "__main__":
    main()
