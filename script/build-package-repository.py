#!/usr/bin/env python3
"""Build a signed, distro-specific APT/RPM site from verified release packages.

Never downloads, installs packages, imports private keys, or deploys a site.
The release job supplies an isolated GNUPGHOME containing its signing key.
"""
import argparse
from datetime import datetime, timedelta, timezone
from email.utils import format_datetime
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


MAX_BYTES = 900 * 1024 * 1024
FINGERPRINT = re.compile(r"(?:[0-9A-F]{40}|[0-9A-F]{64})\Z")
FILENAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9._+~-]*\Z")
TARGETS = {
    ("deb", "ubuntu", "22.04"): ("jammy", {"amd64", "arm64"}),
    ("deb", "ubuntu", "24.04"): ("noble", {"amd64", "arm64"}),
    ("deb", "ubuntu", "26.04"): ("resolute", {"amd64", "arm64"}),
    ("rpm", "fedora", "44"): (None, {"x86_64", "aarch64"}),
}


def run(command, **kwargs):
    result = subprocess.run(command, capture_output=True, timeout=180,
                            check=False, **kwargs)
    if result.returncode:
        error = result.stderr.decode(errors="replace")[-2000:]
        raise ValueError(f"{command[0]} failed: {error.strip()}")
    return result.stdout


def sha256(file):
    digest = hashlib.sha256()
    with file.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def load_manifest(file, packages):
    if file.stat().st_size > 1024 * 1024:
        raise ValueError("Package manifest is too large")
    manifest = json.loads(file.read_text())
    if not isinstance(manifest, dict):
        raise ValueError("Package manifest must be an object")
    entries = manifest.get("packages")
    if manifest.get("schema") != 1 or not isinstance(entries, list) or not 1 <= len(entries) <= 64:
        raise ValueError("Expected schema 1 with 1–64 packages")
    names = set()
    identities = set()
    total = 0
    for entry in entries:
        if not isinstance(entry, dict):
            raise ValueError("Every package must be a metadata object")
        for field in ("format", "distro", "distro_version", "arch", "name", "version", "release"):
            if not isinstance(entry.get(field), str):
                raise ValueError(f"Package {field} must be a string")
        filename = entry.get("filename", "")
        if not isinstance(filename, str) or not FILENAME.fullmatch(filename) or filename in names:
            raise ValueError("Package filenames must be unique plain basenames")
        names.add(filename)
        target = TARGETS.get((entry.get("format"), entry.get("distro"), entry.get("distro_version")))
        if not target or entry.get("arch") not in target[1] or entry.get("codename") != target[0]:
            raise ValueError(f"Unsupported or inconsistent package target: {filename}")
        version = entry.get("version", "")
        release = entry.get("release", "")
        if not isinstance(version, str) or not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version):
            raise ValueError("Repository publication requires a stable numeric version")
        if release != ("1.fc44" if entry["format"] == "rpm" else "1"):
            raise ValueError(f"Unexpected package release: {filename}")
        if entry.get("name") != "vectorwarp" or not filename.endswith("." + entry["format"]):
            raise ValueError(f"Wrong package name or extension: {filename}")
        identity = (entry["format"], entry["distro_version"], entry["arch"], version, release)
        if identity in identities:
            raise ValueError("Duplicate package target/version")
        identities.add(identity)
        source = packages / filename
        if source.is_symlink() or not source.is_file():
            raise ValueError(f"Missing regular package file: {filename}")
        size = source.stat().st_size
        if type(entry.get("size")) is not int or entry["size"] != size or size <= 0:
            raise ValueError(f"Package size mismatch: {filename}")
        if entry.get("sha256") != sha256(source):
            raise ValueError(f"Package checksum mismatch: {filename}")
        total += size
    if total > MAX_BYTES:
        raise ValueError("Packages exceed the 900-MiB repository budget; reduce retained versions")
    return entries


def verify_metadata(entry, file):
    if entry["format"] == "deb":
        actual = run(["dpkg-deb", "--field", str(file), "Package", "Version", "Architecture"]).decode()
        fields = dict(line.split(": ", 1) for line in actual.splitlines() if ": " in line)
        expected = {"Package": entry["name"], "Version": entry["version"] + "-" + entry["release"],
                    "Architecture": entry["arch"]}
        if fields != expected:
            raise ValueError(f"DEB metadata disagrees with manifest: {file.name}")
    else:
        actual = run(["rpm", "-qp", "--queryformat", "%{NAME}\n%{VERSION}\n%{RELEASE}\n%{ARCH}\n", str(file)]).decode().splitlines()
        if actual != [entry[key] for key in ("name", "version", "release", "arch")]:
            raise ValueError(f"RPM metadata disagrees with manifest: {file.name}")


def public_fingerprint(public_key):
    records = run(["gpg", "--batch", "--with-colons", "--show-keys", str(public_key)]).decode().splitlines()
    keys = [line for line in records if line.startswith("pub:")]
    fingerprints = [line.split(":")[9] for line in records if line.startswith("fpr:")]
    if any(line.startswith(("sec:", "ssb:")) for line in records):
        raise ValueError("The public-key file must not contain secret keys")
    if len(keys) != 1 or not fingerprints:
        raise ValueError("Exactly one public signing key is required")
    return fingerprints[0]


def verified_rpm(file, database):
    # Only a signature verifiable by the isolated, pinned-key database counts.
    # An unsigned RPM can pass checksig on its digests alone.
    result = subprocess.run(["rpm", "--dbpath", str(database), "--checksig", "--verbose", str(file)],
                            capture_output=True, timeout=180, check=False)
    report = result.stdout.decode(errors="replace")
    signatures = [line.strip() for line in report.splitlines() if "Signature" in line]
    return (result.returncode == 0 and bool(signatures) and
            all(line.endswith(": OK") for line in signatures) and
            not any(marker in report for marker in ("NOKEY", "NOT OK", "BAD")))


def sign(file, signer, clear=False):
    output = file.with_name("InRelease") if clear else Path(str(file) + (".gpg" if file.name == "Release" else ".asc"))
    command = ["gpg", "--batch", "--yes", "--pinentry-mode", "error", "--local-user", signer,
               "--digest-algo", "SHA256", "--output", str(output)]
    command += ["--clearsign"] if clear else ["--armor", "--detach-sign"]
    run(command + [str(file)])
    return output


def build(args):
    fingerprint = args.fingerprint.upper()
    signer = args.signing_key.upper()
    if not FINGERPRINT.fullmatch(fingerprint) or signer != fingerprint:
        raise ValueError("Use the same full primary fingerprint for expected and signing keys")
    key_home = os.environ.get("GNUPGHOME", "")
    if not re.fullmatch(r"/[A-Za-z0-9_./-]+", key_home) or not Path(key_home).is_dir():
        raise ValueError("Set GNUPGHOME to an isolated absolute signing-key directory")
    if Path(key_home).stat().st_mode & 0o077:
        raise ValueError("Signing-key directory permissions must be private (0700)")
    packages = Path(args.packages).resolve(strict=True)
    if not packages.is_dir():
        raise ValueError("Packages must be a directory")
    entries = load_manifest(Path(args.manifest), packages)
    public_key = Path(args.public_key).resolve(strict=True)
    if public_fingerprint(public_key) != fingerprint:
        raise ValueError("Public key fingerprint does not match the pinned release key")
    requested_output = Path(args.output).absolute()
    if requested_output.exists() or requested_output.is_symlink():
        raise ValueError("Output must not exist; existing repositories are never overwritten in place")
    output = requested_output.resolve()
    if not output.parent.is_dir() or output == packages or packages in output.parents:
        raise ValueError("Output must be outside the package input directory")
    for entry in entries:
        verify_metadata(entry, packages / entry["filename"])
    with tempfile.TemporaryDirectory(prefix=".vectorwarp-repository-", dir=output.parent) as temporary:
        scratch = Path(temporary)
        site = scratch / "site"
        keys = site / "keys"
        keys.mkdir(parents=True)
        # Export public packets from the keyring for BOTH encodings. Never
        # dearmor an input file directly into a publicly served keyring.
        (keys / "vectorwarp.gpg").write_bytes(run(["gpg", "--batch", "--export", fingerprint]))
        (keys / "vectorwarp.asc").write_bytes(run(["gpg", "--batch", "--armor", "--export", fingerprint]))
        if not (keys / "vectorwarp.asc").stat().st_size:
            raise ValueError("Pinned key is not imported in the signing keyring")
        if public_fingerprint(keys / "vectorwarp.gpg") != fingerprint:
            raise ValueError("Exported key does not match the pinned release key")
        (keys / "fingerprint.txt").write_text(fingerprint + "\n")
        apt_groups = set()
        rpm_groups = set()
        rpm_db = scratch / "rpmdb"
        if any(entry["format"] == "rpm" for entry in entries):
            rpm_db.mkdir()
            run(["rpm", "--dbpath", str(rpm_db), "--import", str(keys / "vectorwarp.asc")])
        published = []
        for entry in entries:
            if entry["format"] == "deb":
                relative = Path("apt/pool") / entry["codename"] / entry["arch"] / entry["filename"]
                apt_groups.add((entry["codename"], entry["arch"]))
            else:
                relative = Path("rpm/fedora") / entry["distro_version"] / entry["arch"] / "Packages" / entry["filename"]
                rpm_groups.add((entry["distro_version"], entry["arch"]))
            destination = site / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(packages / entry["filename"], destination)
            if entry["format"] == "rpm":
                # Weekly index refresh must preserve already signed package
                # bytes so a published immutable package never changes hash.
                if not verified_rpm(destination, rpm_db):
                    run(["rpmsign", "--addsign", "--define", f"_gpg_name {signer}",
                         "--define", f"_gpg_path {key_home}", "--define", "__gpg /usr/bin/gpg",
                         "--define", "_gpg_sign_cmd_extra_args --batch --pinentry-mode error", str(destination)])
                if not verified_rpm(destination, rpm_db):
                    raise ValueError(f"RPM signature could not be verified: {entry['filename']}")
            published.append({**entry, "sha256": sha256(destination), "size": destination.stat().st_size,
                              "repository_path": relative.as_posix()})
        apt = site / "apt"
        for codename, arch in sorted(apt_groups):
            folder = apt / "dists" / codename / "main" / f"binary-{arch}"
            folder.mkdir(parents=True)
            data = run(["apt-ftparchive", "packages", f"pool/{codename}/{arch}"], cwd=apt)
            (folder / "Packages").write_bytes(data)
            (folder / "Packages.gz").write_bytes(gzip.compress(data, mtime=0))
            by_hash = folder / "by-hash/SHA256"
            by_hash.mkdir(parents=True)
            for name in ("Packages", "Packages.gz"):
                shutil.copyfile(folder / name, by_hash / sha256(folder / name))
        valid_until = format_datetime(datetime.now(timezone.utc) + timedelta(days=30), usegmt=True)
        for codename in sorted({group[0] for group in apt_groups}):
            distro = apt / "dists" / codename
            architectures = " ".join(sorted(arch for suite, arch in apt_groups if suite == codename))
            settings = {"Origin": "VectorWarp", "Label": "VectorWarp", "Suite": codename,
                        "Codename": codename, "Architectures": architectures, "Components": "main",
                        "Acquire-By-Hash": "yes"}
            command = ["apt-ftparchive"]
            for key, value in settings.items():
                command += ["-o", f"APT::FTPArchive::Release::{key}={value}"]
            release = distro / "Release"
            # Older apt-ftparchive silently ignores the Valid-Until setting.
            # Add the field explicitly BEFORE signing on every supported host.
            release_data = run(command + ["release", "."], cwd=distro)
            release.write_bytes(f"Valid-Until: {valid_until}\n".encode() + release_data)
            for signature in (sign(release, signer), sign(release, signer, clear=True)):
                run(["gpgv", "--keyring", str(keys / "vectorwarp.gpg"), str(signature)] +
                    ([] if signature.name == "InRelease" else [str(release)]))
        for version, arch in sorted(rpm_groups):
            folder = site / "rpm/fedora" / version / arch
            run(["createrepo_c", "--checksum", "sha256", str(folder)])
            metadata = folder / "repodata/repomd.xml"
            signature = sign(metadata, signer)
            run(["gpgv", "--keyring", str(keys / "vectorwarp.gpg"), str(signature), str(metadata)])
        document = {"schema": 1, "generated_at": datetime.now(timezone.utc).isoformat(),
                    "signing_fingerprint": fingerprint, "packages": published}
        (site / "repository-manifest.json").write_text(json.dumps(document, indent=2) + "\n")
        if args.installer_template:
            template = Path(args.installer_template).read_text()
            if "@SIGNING_FINGERPRINT@" not in template:
                raise ValueError("Installer template has no signing-fingerprint placeholder")
            (site / "install.sh").write_text(template.replace("@SIGNING_FINGERPRINT@", fingerprint))
        (site / ".nojekyll").touch()
        (site / "index.html").write_text('<!doctype html><title>VectorWarp packages</title><h1>VectorWarp packages</h1>'
            '<p>Signed native Linux packages. See the <a href="https://github.com/mickeyslaven/blah2-VectorWarp">'
            'installation guide</a> before installing.</p>\n')
        if sum(file.stat().st_size for file in site.rglob("*") if file.is_file()) > MAX_BYTES:
            raise ValueError("Signed repository exceeds the 900-MiB Pages budget")
        site.rename(output)
    return document


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("packages", "manifest", "output", "public-key", "fingerprint", "signing-key"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--installer-template")
    args = parser.parse_args()
    try:
        document = build(args)
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        parser.exit(1, f"Repository not published: {error}\n")
    print(f"Verified signed repository: {len(document['packages'])} packages at {args.output}")


if __name__ == "__main__":
    main()
