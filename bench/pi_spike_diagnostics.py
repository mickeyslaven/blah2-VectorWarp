#!/usr/bin/env python3
"""Bounded Pi soak diagnostics; no processing/RF settings are changed.

perf's overwrite ring retains recent samples *before* a trigger. CPU samples
include the processor, isolated mixed worker, SDRplay service and kernel. This
does not measure GPU occupancy. See docs/PI_SOAK_DIAGNOSTICS.md for limits.
"""
import collections
import json
import os
from pathlib import Path
import queue
import shutil
import signal
import subprocess
import threading
import time


def read_small(path, limit=16384):
    try:
        with Path(path).open() as stream:
            return stream.read(limit)
    except OSError as error:
        return f"unavailable: {error}"


class SpikeDiagnostics:
    def __init__(self, output, spike_ms=750, stall_seconds=1.5, perf_command=None):
        self.root = output.with_suffix(".diagnostics")
        self.spike_ms, self.stall_seconds = spike_ms, stall_seconds
        self.perf_command = perf_command
        self.perf = None
        self.pid = None
        self.last_frame = None
        self.last_stall = 0
        self.last_dump = -float("inf")
        self.snapshots = 0
        self.errors = []
        self.events = queue.Queue(maxsize=32)
        self.stop = threading.Event()
        self.thread = None
        self.files = []
        self.started = False
        self.stopping = False
        self.ring = collections.deque(maxlen=60)
        self.start_wall = time.time()

    def start(self):
        self.root.mkdir(parents=True, exist_ok=False)
        for name in ("frames", "statuses", "system", "events"):
            stream = (self.root / f"{name}.jsonl").open("x", buffering=1)
            setattr(self, name + "_stream", stream)
            self.files.append(stream)
        self.perf_log = (self.root / "perf.log").open("x", buffering=1)
        self.files.append(self.perf_log)
        command = self.perf_command
        if command is None:
            executable = shutil.which("perf")
            if not executable or os.geteuid() != 0:
                raise RuntimeError("Spike diagnostics require installed perf and root; no capture started")
            command = [executable, "record", "-a", "-e", "cpu-clock", "-F", "49",
                       "--call-graph", "dwarf,2048", "-m", "2M", "--overwrite",
                       "--switch-output=signal", "--switch-max-files=8",
                       "--clockid", "mono", "-o", str(self.root / "cpu.perf")]
        (self.root / "command.json").write_text(json.dumps(command, indent=2))
        self.perf = subprocess.Popen(command, stdout=self.perf_log,
                                     stderr=subprocess.STDOUT, start_new_session=True)
        # Fail closed before touching SDR hardware if profiling cannot start.
        time.sleep(.25)
        if self.perf.poll() is not None:
            raise RuntimeError("perf flight recorder failed to start; see diagnostics/perf.log")
        self.started = True
        self.thread = threading.Thread(target=self._run, name="spike-diagnostics", daemon=True)
        self.thread.start()

    @staticmethod
    def _write(stream, value):
        stream.write(json.dumps(value, allow_nan=False, separators=(",", ":")) + "\n")

    def processor_started(self, pid):
        self.pid = pid
        (self.root / "processor-pid.txt").write_text(str(pid) + "\n")

    def processor_stopping(self):
        self.stopping = True

    def observe(self, frames, arrivals, states):
        for frame, arrival in zip(frames, arrivals):
            self.last_frame = arrival
            self._write(self.frames_stream, {"monotonic": arrival, "frame": frame})
            value = frame.get("cpi")
            if isinstance(value, (int, float)) and value >= self.spike_ms:
                self.trigger("large_cpi", {"frame": frame, "arrivalMonotonic": arrival})
        for state in states:
            self._write(self.statuses_stream, {"monotonic": time.monotonic(), "status": state})

    def trigger(self, reason, details=None):
        event = {"reason": reason, "wallTime": time.time(), "monotonic": time.monotonic(),
                 "details": details or {}}
        try:
            self.events.put_nowait(event)
        except queue.Full:
            if "diagnostic event queue full" not in self.errors:
                self.errors.append("diagnostic event queue full")

    def _system(self):
        value = {"wallTime": time.time(), "monotonic": time.monotonic()}
        for name, path in {
            "cpu": "/proc/stat", "load": "/proc/loadavg", "memory": "/proc/meminfo",
            "cpuPressure": "/proc/pressure/cpu", "ioPressure": "/proc/pressure/io",
            "memoryPressure": "/proc/pressure/memory",
            "temperatureMilliC": "/sys/class/thermal/thermal_zone0/temp",
            "frequencyKHz": "/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq",
        }.items():
            value[name] = read_small(path)
        return value

    def _context(self, destination):
        # Only the workload's process tree; no stack-stopping debugger attaches.
        pending, seen, processes = [self.pid] if self.pid else [], set(), []
        while pending and len(seen) < 16:
            pid = pending.pop()
            if pid in seen:
                continue
            seen.add(pid)
            proc = Path(f"/proc/{pid}")
            entry = {"pid": pid, "status": read_small(proc / "status"),
                     "stat": read_small(proc / "stat"), "io": read_small(proc / "io"),
                     "maps": read_small(proc / "maps", 65536), "threads": []}
            for task in sorted((proc / "task").glob("*"))[:96]:
                entry["threads"].append({"tid": task.name, "comm": read_small(task / "comm"),
                                         "wchan": read_small(task / "wchan"),
                                         "schedstat": read_small(task / "schedstat"),
                                         "kernelStack": read_small(task / "stack")})
                children = read_small(task / "children")
                pending.extend(int(x) for x in children.split() if x.isdigit())
            processes.append(entry)
        (destination / "context.json").write_text(json.dumps(
            {"systemHistory": list(self.ring), "processes": processes}, indent=2))
        for name, command in {
            "kernel.log": ["journalctl", "-k", "--since", "-90 seconds", "-n", "200", "--no-pager"],
            "sdrplay.log": ["journalctl", "-u", "sdrplay", "--since", "-90 seconds", "-n", "200", "--no-pager"],
        }.items():
            with (destination / name).open("x") as stream:
                try:
                    subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, timeout=2)
                except (OSError, subprocess.TimeoutExpired) as error:
                    stream.write(str(error))

    def _event(self, event):
        now = time.monotonic()
        take = self.snapshots < 6 and (now - self.last_dump >= 30 or event["reason"] == "harness_failure")
        event["snapshotRequested"] = take
        if take:
            self.snapshots += 1
            self.last_dump = now
            destination = self.root / f"spike-{self.snapshots:02d}"
            destination.mkdir()
            event["contextDirectory"] = destination.name
            if self.perf and self.perf.poll() is None:
                self.perf.send_signal(signal.SIGUSR2)
                event["perfSignalMonotonic"] = time.monotonic()
            else:
                event["perfUnavailable"] = True
            self._context(destination)
        self._write(self.events_stream, event)

    def _run(self):
        next_sample, next_throttle = 0, 0
        while not self.stop.is_set() or not self.events.empty():
            try:
                now = time.monotonic()
                if now >= next_sample:
                    sample = self._system()
                    if now >= next_throttle:
                        try:
                            sample["throttled"] = subprocess.check_output(
                                ["vcgencmd", "get_throttled"], text=True, timeout=1).strip()
                        except (OSError, subprocess.SubprocessError) as error:
                            sample["throttled"] = str(error)
                        next_throttle = now + 5
                    self.ring.append(sample)
                    self._write(self.system_stream, sample)
                    next_sample = now + 1
                if not self.stopping and self.last_frame is not None and now - self.last_frame >= self.stall_seconds and now - self.last_stall >= 30:
                    self.last_stall = now
                    self.trigger("timing_stall", {"sinceLastFrameSeconds": now - self.last_frame})
                if self.perf and self.perf.poll() is not None:
                    error = f"perf exited during capture: {self.perf.returncode}"
                    if error not in self.errors:
                        self.errors.append(error)
                try:
                    self._event(self.events.get(timeout=.1))
                except queue.Empty:
                    pass
            except Exception as error:
                self.errors.append(str(error))
                return

    def close(self):
        self.stop.set()
        if self.thread:
            self.thread.join(12)
            if self.thread.is_alive():
                self.errors.append("diagnostic collector did not terminate")
        if self.perf and self.perf.poll() is None:
            self.perf.send_signal(signal.SIGINT)
            try:
                self.perf.wait(5)
            except subprocess.TimeoutExpired:
                self.perf.kill()
                self.perf.wait(2)
                self.errors.append("perf required forced termination")
        for stream in self.files:
            stream.close()
        if self.root.exists():
            (self.root / "summary.json").write_text(json.dumps(self.summary(), indent=2))

    def summary(self):
        return {"directory": str(self.root), "started": self.started, "errors": self.errors,
                "spikeThresholdMs": self.spike_ms, "stallThresholdSeconds": self.stall_seconds,
                "requestedSnapshots": self.snapshots, "maxTriggeredSnapshots": 6,
                "perfFrequencyHz": 49, "perfBufferPerCpu": "2 MiB",
                "callGraph": "dwarf,2048", "profilingOverheadIncludedInCpi": True}
