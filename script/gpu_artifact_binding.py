"""ABI-neutral artifact pins/early worker receipts shared by offline runners."""
import hashlib
import json
import os
from pathlib import Path
import secrets
import select
import socket
import stat
import threading

PREFIX = "VECTORWARP_GPU_ARTIFACT "
ENV = "VECTORWARP_GPU_ARTIFACT_"
CONFIRMATION = "offline-gpu-artifact-binding-v1"
MAX_RECEIPT = 4096
MAX_RECEIPTS = 1024
MODULES = {"portable": ("blah2-gpu-vulkan.so", 4),
           "wideband": ("blah2-gpu-wideband-vulkan.so", 1),
           "resident": ("blah2-gpu-resident-vulkan.so", 15)}


def stamp(info):
    return ":".join(str(value) for value in (info.st_dev, info.st_ino, info.st_size,
        info.st_mtime_ns // 10**9, info.st_mtime_ns % 10**9,
        info.st_ctime_ns // 10**9, info.st_ctime_ns % 10**9))


def pin(path):
    path = Path(path).resolve(strict=True)
    initial = path.stat()
    if not stat.S_ISREG(initial.st_mode) or not 0 < initial.st_size <= 512 << 20:
        raise ValueError(f"artifact is not a bounded regular file: {path}")
    with path.open("rb") as source:
        before = stamp(os.fstat(source.fileno()))
        if before != stamp(initial):
            raise ValueError(f"artifact changed before hashing: {path}")
        digest, remaining = hashlib.sha256(), initial.st_size
        while remaining:
            data = source.read(min(remaining, 1 << 20))
            if not data:
                raise ValueError(f"artifact truncated while hashing: {path}")
            digest.update(data)
            remaining -= len(data)
        if source.read(1):
            raise ValueError(f"artifact grew while hashing: {path}")
        if stamp(os.fstat(source.fileno())) != before or stamp(path.stat()) != before:
            raise ValueError(f"artifact changed while hashing: {path}")
    return {"path": str(path), "sha256": digest.hexdigest(), "stat": before}


def prepare(binary, gpu, expected_binary=None):
    """Pin before launch; resolve modules beside the *resolved* worker image."""
    artifacts = {"processor": pin(binary)}
    if expected_binary is not None and artifacts["processor"]["sha256"] != expected_binary:
        raise ValueError("processor artifact SHA-256 mismatch")
    if gpu:
        artifacts["worker"] = pin(Path(artifacts["processor"]["path"]).with_name("blah2-gpu-worker"))
        directory = Path(artifacts["worker"]["path"]).parent
        for role, (name, _) in MODULES.items():
            # An absent or unused optional module is not a qualified artifact.
            # An attempt to load it will lack a valid loaded receipt and fail.
            path = directory / name
            if path.exists():
                artifacts[role] = pin(path)
    return {"schema": 1, "nonce": secrets.token_hex(16), "artifacts": artifacts}


def environment(binding, base=None):
    env = {key: value for key, value in (os.environ if base is None else base).items()
           if not key.startswith(ENV)}
    env[ENV + "NONCE"] = binding["nonce"]
    for role, artifact in binding["artifacts"].items():
        for field in ("path", "sha256", "stat"):
            env[ENV + role.upper() + "_" + field.upper()] = artifact[field]
    return env


class ReceiptChannel:
    """Runner-owned bounded datagrams, separate from arbitrary processor logs.

    The descriptor and fresh launch nonce are explicitly authorized by offline
    replay config. Merely inheriting the environment never enables the hook.
    """
    def __init__(self, binding, config, base=None):
        replay = config.get("capture", {}).get("replay", {})
        if replay.get("state") is not True or replay.get("format") not in ("auto", "blah2", "mchq"):
            raise ValueError("artifact receipts require explicit offline replay")
        if not isinstance(binding.get("nonce"), str) or len(binding["nonce"]) != 32 or \
                any(c not in "0123456789abcdef" for c in binding["nonce"]):
            raise ValueError("invalid artifact launch nonce")
        self.receiver, self.sender = socket.socketpair(socket.AF_UNIX, socket.SOCK_DGRAM)
        self.receiver.setblocking(False)
        self.sender.setblocking(False)
        self.environment = environment(binding, base)
        self.pass_fds = (self.sender.fileno(),)
        replay["artifacts"] = {"enabled": True, "confirmation": CONFIRMATION,
                               "descriptor": self.sender.fileno(), "nonce": binding["nonce"]}
        self.records, self.error = [], None
        self.stopping = threading.Event()
        self.thread = threading.Thread(target=self._collect, name="gpu-artifact-receipts", daemon=True)
        self.thread.start()

    def _collect(self):
        try:
            while True:
                if not self.stopping.is_set():
                    select.select([self.receiver], [], [], .05)
                # Bound each drain batch even if a hostile child floods it.
                for _ in range(MAX_RECEIPTS + 1):
                    try:
                        data = self.receiver.recv(MAX_RECEIPT + 1)
                    except ConnectionResetError:
                        # Darwin reports a closed datagram peer as a reset.
                        # Preserve drained receipts and any earlier parse error;
                        # validate() still rejects missing receipts or changed pins.
                        return
                    except BlockingIOError:
                        if self.stopping.is_set():
                            return
                        break
                    if self.error is not None:
                        continue
                    if len(data) > MAX_RECEIPT or len(self.records) >= MAX_RECEIPTS:
                        self.error = "GPU artifact receipt channel exceeded its bound"
                        continue
                    try:
                        line = data.decode("utf-8")
                        if not line.startswith(PREFIX) or not line.endswith("\n") or line.count("\n") != 1:
                            raise ValueError("malformed GPU artifact datagram")
                        json.loads(line[len(PREFIX):])
                        self.records.append(line)
                    except (UnicodeError, ValueError) as error:
                        self.error = str(error)
                if self.stopping.is_set():
                    return
        except (OSError, ValueError) as error:
            self.error = str(error)

    def launched(self):
        """Close the runner's copy once Popen has passed the explicit fd."""
        self.sender.close()

    def finish(self):
        self.stopping.set()
        self.thread.join(2)
        if self.thread.is_alive():
            raise ValueError("GPU artifact collector did not stop")
        if self.error:
            raise ValueError(self.error)
        return "".join(self.records)

    def close(self):
        self.stopping.set()
        self.thread.join(2)
        self.sender.close()
        self.receiver.close()


def verify_files(binding, roles=None):
    for role, artifact in binding["artifacts"].items():
        if roles is not None and role not in roles:
            continue
        if pin(artifact["path"]) != artifact:
            raise ValueError(f"pinned artifact changed: {artifact['path']}")


def validate(binding, output, processor_pid, require_worker=True):
    """Reject absent/late receipts, not merely a matching final directory hash."""
    workers, spawned, attempted, loaded = {}, set(), [], []
    for line in output.splitlines():
        if PREFIX not in line:
            continue
        if not line.startswith(PREFIX):
            raise ValueError("interleaved or malformed GPU artifact receipt")
        item = json.loads(line[len(PREFIX):])
        if item.get("schema") != 1 or item.get("nonce") != binding["nonce"]:
            raise ValueError("GPU artifact receipt belongs to another launch")
        pid, event = item.get("pid"), item.get("event")
        if event == "worker-spawn":
            child = item.get("workerPid")
            if pid != processor_pid or type(child) is not int or child <= 0 or child in spawned or \
                    item.get("executablePath") != binding["artifacts"].get("worker", {}).get("path"):
                raise ValueError("unexpected spawned GPU worker")
            spawned.add(child)
            continue  # Cross-process scheduling can put worker-start first.
        if type(pid) is not int or pid <= 0 or item.get("parent") != processor_pid:
            raise ValueError("GPU artifact receipt has unexpected process identity")
        if event == "worker-start":
            if pid in workers or item.get("worker") != binding["artifacts"].get("worker"):
                raise ValueError("unexpected worker path/hash or duplicate worker receipt")
            workers[pid] = {"phase": "start", "role": None}
            continue
        worker = workers.get(pid)
        if worker is None:
            raise ValueError("missing early worker artifact receipt")
        role = item.get("role")
        if role not in MODULES:
            raise ValueError("unexpected GPU module role")
        if event == "module-attempt":
            if worker["phase"] != "start":
                raise ValueError("late or duplicate GPU module attempt receipt")
            worker.update(phase="attempt", role=role)
            attempted.append({"pid": pid, "role": role})
        elif event == "module-loaded":
            if worker != {"phase": "attempt", "role": role}:
                raise ValueError("missing/late GPU module load receipt")
            expected = binding["artifacts"].get(role)
            if expected is None or item.get("module") != expected or item.get("abi") != MODULES[role][1]:
                raise ValueError("unexpected module path, hash or ABI")
            identity = item.get("driverStatus", {})
            devices = identity.get("devices", [])
            if identity.get("available") is not True or not devices or any(
                    not isinstance(device.get("id"), str) or not device["id"] or
                    any(type(device.get(key)) is not int for key in
                        ("vendorId", "deviceId", "driverVersionRaw")) for device in devices):
                raise ValueError("missing early device/driver identity")
            requested = item.get("deviceRequest")
            if not requested or (requested != "auto" and requested not in {d["id"] for d in devices}):
                raise ValueError("module receipt does not identify requested device")
            worker["phase"] = "loaded"
            loaded.append(item)
        elif event == "backend-start":
            if worker != {"phase": "loaded", "role": role}:
                raise ValueError("backend construction preceded module receipt")
            worker["phase"] = "construct"
        else:
            raise ValueError("unknown GPU artifact receipt event")
    if require_worker and not workers:
        raise ValueError("missing GPU worker artifact receipt")
    if spawned != workers.keys():
        raise ValueError("spawned GPU worker lacks its exact early artifact receipt")
    if any(worker["phase"] not in ("start", "construct") for worker in workers.values()):
        raise ValueError("attempted GPU module lacks early load/construction receipt")
    used = {"processor"} | ({"worker"} if workers else set()) | {item["role"] for item in attempted}
    verify_files(binding, used)
    # Only attempted modules enter the accepted runtime cohort. Pre-launch pins
    # for unused optional modules are retained separately, not claimed as loaded.
    return {"passed": True, "workers": sorted(workers), "attempted": attempted,
            "loaded": loaded}
