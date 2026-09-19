#!/usr/bin/env python3
"""Encrypt or restore an audited standalone macOS runtime for CI artifact transfer.

CMS encryption and sidecar hashes do not authenticate a sender. Decrypt callers
must obtain the artifact and expected identity from a trusted GitHub Actions record.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import sys
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("package_macos_standalone",
                                               ROOT / "script/package-macos-standalone.py")
packager = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(packager)

MAX_FILES = 200_000
MAX_BYTES = 2 * 1024 * 1024 * 1024
CHUNK = 1024 * 1024


def sha256(path):
    value = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(CHUNK), b""):
            value.update(block)
    return value.hexdigest()


def openssl(*args):
    return subprocess.run(["/usr/bin/openssl", *map(str, args)], text=True,
                          capture_output=True, check=True)


def certificate_fingerprint(certificate):
    result = openssl("x509", "-in", certificate, "-noout", "-fingerprint", "-sha256")
    value = result.stdout.strip().split("=", 1)
    if len(value) != 2:
        raise ValueError("unable to read certificate SHA-256 fingerprint")
    return value[1].replace(":", "").lower()


def checked_output(path):
    path = Path(path)
    if path.exists() or path.is_symlink():
        raise ValueError(f"refusing to replace existing output: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    return path


def protected_key(path):
    path = Path(path).resolve(strict=True)
    mode = path.stat().st_mode
    if not path.is_file() or mode & 0o077:
        raise ValueError("recipient key must be a regular owner-only file")
    return path


def metadata_for(audit, ciphertext, plaintext_tar, certificate):
    return {
        "schema": 1,
        "ciphertext_sha256": sha256(ciphertext),
        "tar_sha256": sha256(plaintext_tar),
        "source_id": audit["source_id"],
        "arch": audit["arch"],
        "minimum_os": audit["minimum_os"],
        "certificate_sha256": certificate_fingerprint(certificate),
        "ci": {"github_run_id": os.environ.get("GITHUB_RUN_ID"),
               "github_job": os.environ.get("GITHUB_JOB")},
    }


def add_runtime_to_tar(runtime, archive):
    packager.safe_tree(runtime)
    with tarfile.open(archive, "w:gz") as stream:
        for path in sorted(runtime.rglob("*")):
            stream.add(path, arcname=path.relative_to(runtime).as_posix(), recursive=False)


def encrypt(args):
    runtime = Path(args.runtime).resolve(strict=True)
    if not runtime.is_dir():
        raise ValueError("runtime must be a directory")
    output = checked_output(args.output)
    metadata_output = checked_output(args.metadata)
    if output.absolute() == metadata_output.absolute():
        raise ValueError("ciphertext and metadata outputs must differ")
    certificate = Path(args.certificate).resolve(strict=True)
    before = packager.audit_runtime(runtime)
    before_files = packager.files_manifest(runtime)
    with tempfile.TemporaryDirectory(prefix="vectorwarp-runtime-transfer-") as temporary:
        archive = Path(temporary) / "runtime.tar"
        add_runtime_to_tar(runtime, archive)
        try:
            after = packager.audit_runtime(runtime)
            if before_files != packager.files_manifest(runtime) or before != after:
                raise ValueError("runtime changed while preparing encrypted transfer")
            openssl("cms", "-encrypt", "-binary", "-aes256", "-outform", "DER",
                    "-in", archive, "-out", output, certificate)
            metadata_output.write_text(json.dumps(metadata_for(after, output, archive, certificate),
                                                  sort_keys=True, indent=2) + "\n")
        except Exception:
            output.unlink(missing_ok=True)
            metadata_output.unlink(missing_ok=True)
            raise
    print(json.dumps({"ciphertext_sha256": sha256(output), "metadata": metadata_output.name},
                     sort_keys=True))


def normalized_archive_path(value, description, allow_root=False):
    value = PurePosixPath(value)
    if value.is_absolute():
        raise ValueError(f"unsafe {description}: {str(value)!r}")
    parts = []
    for part in value.parts:
        if part in ("", "."):
            continue
        if part == "..":
            if not parts:
                raise ValueError(f"{description} escapes runtime: {str(value)!r}")
            parts.pop()
        else:
            parts.append(part)
    if not parts and not allow_root:
        raise ValueError(f"unsafe {description}: {str(value)!r}")
    return PurePosixPath(*parts) if parts else PurePosixPath(".")


def safe_member_name(name):
    if not name:
        raise ValueError(f"unsafe archive member: {name!r}")
    return normalized_archive_path(name, "archive member")


def safe_link_target(member, target):
    if not target:
        raise ValueError(f"unsafe archive symlink: {member.as_posix()!r}")
    return normalized_archive_path(member.parent / PurePosixPath(target),
                                   "archive symlink", allow_root=True)


def require_inside(destination, path):
    try:
        Path(path).resolve(strict=False).relative_to(destination.resolve(strict=True))
    except ValueError as error:
        raise ValueError(f"archive extraction escapes runtime: {path}") from error


def safe_extract(archive, destination):
    destination = Path(destination)
    destination.mkdir(mode=0o700)
    with tarfile.open(archive, "r") as stream:
        members = stream.getmembers()
        if len(members) > MAX_FILES:
            raise ValueError("archive has too many members")
        total = 0
        names = set()
        checked = []
        for member in members:
            name = safe_member_name(member.name)
            canonical = name.as_posix()
            if canonical in names:
                raise ValueError(f"duplicate archive member: {canonical!r}")
            names.add(canonical)
            if member.islnk() or member.isdev() or member.isfifo() or member.issparse():
                raise ValueError(f"unsupported archive member type: {member.name!r}")
            if not (member.isdir() or member.isfile() or member.issym()):
                raise ValueError(f"unsupported archive member type: {member.name!r}")
            if member.issym():
                safe_link_target(name, member.linkname)
            if member.isfile():
                total += member.size
                if total > MAX_BYTES:
                    raise ValueError("archive exceeds size limit")
            checked.append((member, name))
        for member, name in checked:
            target = destination / name
            require_inside(destination, target.parent)
            target.parent.mkdir(parents=True, exist_ok=True)
            require_inside(destination, target.parent)
            if member.isdir():
                target.mkdir(exist_ok=True)
                require_inside(destination, target)
                target.chmod(member.mode & 0o777)
            elif member.issym():
                target.symlink_to(member.linkname)
                require_inside(destination, target)
            else:
                source = stream.extractfile(member)
                if source is None:
                    raise ValueError(f"cannot read archive member: {member.name!r}")
                with target.open("xb") as output:
                    shutil.copyfileobj(source, output, CHUNK)
                require_inside(destination, target)
                target.chmod(member.mode & 0o777)
    packager.safe_tree(destination)


def read_metadata(path):
    value = json.loads(Path(path).read_text())
    expected = {"schema", "ciphertext_sha256", "tar_sha256", "source_id", "arch",
                "minimum_os", "certificate_sha256", "ci"}
    if set(value) != expected or value["schema"] != 1:
        raise ValueError("invalid transfer metadata")
    if value["arch"] not in packager.ARCHES:
        raise ValueError("invalid transfer architecture")
    if not isinstance(value["ci"], dict) or set(value["ci"]) != {"github_run_id", "github_job"}:
        raise ValueError("invalid transfer CI metadata")
    return value


def validate_expected_identity(metadata, args):
    if metadata["source_id"] != args.expected_source_id:
        raise ValueError("transfer source identity does not match trusted expected value")
    if metadata["arch"] != args.expected_arch:
        raise ValueError("transfer architecture does not match trusted expected value")
    expected_run = None if args.expected_run_id == "local" else args.expected_run_id
    ci = metadata["ci"]
    if ci["github_run_id"] != expected_run:
        raise ValueError("transfer GitHub run does not match trusted expected value")
    if expected_run is None:
        if ci["github_job"] is not None:
            raise ValueError("local transfer metadata must not name a GitHub job")
    elif not isinstance(ci["github_job"], str) or not ci["github_job"]:
        raise ValueError("GitHub transfer metadata must name its job")


def decrypt(args):
    source = Path(args.input).resolve(strict=True)
    metadata = read_metadata(args.metadata)
    validate_expected_identity(metadata, args)
    if sha256(source) != metadata["ciphertext_sha256"]:
        raise ValueError("ciphertext digest does not match metadata")
    certificate = Path(args.certificate).resolve(strict=True)
    if certificate_fingerprint(certificate) != metadata["certificate_sha256"]:
        raise ValueError("certificate does not match metadata")
    key = protected_key(args.key)
    output = checked_output(args.output)
    temporary = Path(tempfile.mkdtemp(prefix=".vectorwarp-runtime-", dir=output.parent))
    archive = temporary / "runtime.tar"
    extracted = temporary / "runtime"
    try:
        openssl("cms", "-decrypt", "-binary", "-inform", "DER", "-in", source,
                "-inkey", key, "-recip", certificate, "-out", archive)
        if sha256(archive) != metadata["tar_sha256"]:
            raise ValueError("plaintext tar digest does not match metadata")
        safe_extract(archive, extracted)
        audit = packager.audit_runtime(extracted, metadata["arch"])
        for name in ("source_id", "arch", "minimum_os"):
            if audit[name] != metadata[name]:
                raise ValueError(f"decrypted runtime {name} does not match metadata")
        os.replace(extracted, output)
    finally:
        shutil.rmtree(temporary, ignore_errors=True)
    print(json.dumps({"ciphertext_sha256": metadata["ciphertext_sha256"],
                      "arch": metadata["arch"]}, sort_keys=True))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    encrypt_parser = commands.add_parser("encrypt")
    encrypt_parser.add_argument("--runtime", type=Path, required=True)
    encrypt_parser.add_argument("--certificate", type=Path, required=True)
    encrypt_parser.add_argument("--output", type=Path, required=True)
    encrypt_parser.add_argument("--metadata", type=Path, required=True)
    decrypt_parser = commands.add_parser("decrypt")
    decrypt_parser.add_argument("--input", type=Path, required=True)
    decrypt_parser.add_argument("--metadata", type=Path, required=True)
    decrypt_parser.add_argument("--key", type=Path, required=True)
    decrypt_parser.add_argument("--certificate", type=Path, required=True)
    decrypt_parser.add_argument("--output", type=Path, required=True)
    decrypt_parser.add_argument("--expected-source-id", required=True)
    decrypt_parser.add_argument("--expected-arch", choices=packager.ARCHES, required=True)
    decrypt_parser.add_argument("--expected-run-id", required=True,
                                help="trusted GitHub run ID, or literal local for offline transfer")
    args = parser.parse_args()
    if sys.platform != "darwin":
        parser.error("macOS host required")
    try:
        {"encrypt": encrypt, "decrypt": decrypt}[args.command](args)
    except (OSError, ValueError, subprocess.CalledProcessError, tarfile.TarError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
