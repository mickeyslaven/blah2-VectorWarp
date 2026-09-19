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
from html import escape
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
    ("deb", "debian", "13"): ("trixie", {"amd64", "arm64"}),
    ("rpm", "fedora", "44"): (None, {"x86_64", "aarch64"}),
}
RELEASE_TARGETS = {
    (format, distro, version, arch)
    for (format, distro, version), (_, architectures) in TARGETS.items()
    for arch in architectures
}


def release_installation(manifest):
    """Render download links only for packages in the verified release manifest."""
    version = manifest["version"]
    if not isinstance(version, str) or not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version):
        raise ValueError("Download links require an immutable release version")
    base = f"https://github.com/mickeyslaven/blah2-VectorWarp/releases/download/v{version}"
    packages = {}
    for entry in manifest["packages"]:
        filename = entry["filename"]
        if not isinstance(filename, str) or not FILENAME.fullmatch(filename):
            raise ValueError("Download links require plain package filenames")
        if entry["format"] not in ("deb", "rpm"):
            raise ValueError("Unsupported package download format")
        key = (entry["distro"], entry["distro_version"], entry["arch"])
        if key in packages:
            raise ValueError("Duplicate package download target")
        packages[key] = f'<a href="{base}/{escape(filename)}">Download {entry["format"].upper()}</a>'

    rows = []

    def row(label, distro, distro_version, pi=False):
        architectures = ("x86_64", "aarch64") if distro == "fedora" else ("amd64", "arm64")
        links = [packages.get((distro, distro_version, arch)) for arch in architectures]
        if pi:
            links[0] = None
        if any(links):
            cells = "".join(f'<td>{link or "—"}</td>' for link in links)
            rows.append(f'<tr><th scope="row">{escape(label)}</th>{cells}</tr>')

    for distro, distro_version in (("ubuntu", "22.04"), ("ubuntu", "24.04"),
                                   ("ubuntu", "26.04"), ("debian", "13"), ("fedora", "44")):
        row(f"{distro.title()} {distro_version}", distro, distro_version)
    for distro_version in ("22.04", "24.04", "26.04"):
        row(f"DragonOS · Ubuntu {distro_version} base", "ubuntu", distro_version)
    row("Raspberry Pi OS · 64-bit Trixie", "debian", "13", pi=True)
    macos = manifest.get("macos_package")
    if macos:
        filename = macos["filename"]
        if filename != f"vectorwarp-{version}-macos-universal.pkg":
            raise ValueError("Mac package filename disagrees with release version")
        if not isinstance(macos.get("sha256"), str) or not re.fullmatch(r"[0-9a-f]{64}", macos["sha256"]):
            raise ValueError("Mac package checksum is missing")
        rows.append(f'<tr><th scope="row">macOS 15+ · universal</th>'
                    f'<td colspan="2"><a href="{base}/{escape(filename)}">Download signed PKG</a></td></tr>')
    table_rows = "\n".join(rows)
    macos_guidance = ('''For macOS, open the signed, notarized PKG in Finder, then launch
<code>/Applications/VectorWarp.app/Contents/MacOS/VectorWarp</code>.''' if macos else
                      '''For macOS, use the public Homebrew tap; this release has no PKG asset.''')
    macos_scope = ('''The macOS PKG is universal, with Apple Silicon validation on M2;
physical Intel Mac execution remains unverified. ''' if macos else "")
    macos_verify = (f'''<p>For the macOS PKG, compare its SHA-256 with
<code>{macos["sha256"]}</code>, then check the Apple installer signature and notarization:</p>
<pre><code>shasum -a 256 {escape(macos["filename"])}
pkgutil --check-signature {escape(macos["filename"])}
spctl --assess --type install {escape(macos["filename"])}</code></pre>
<p><a href="{base}/{escape(macos["source_archive"]["filename"])}">Mac corresponding source archive</a>
(SHA-256: <code>{macos["source_archive"]["sha256"]}</code>).</p>''' if macos else "")
    fingerprint = escape(manifest["signing_fingerprint"])
    # The site can be rebuilt from main before the next release exists. Describe
    # only behavior shipped by the verified packages in this manifest.
    has_launcher = tuple(map(int, version.split("."))) >= (0, 1, 7)
    download = ("curl --fail --location --proto '=https' --tlsv1.2 "
                "https://mickeyslaven.github.io/blah2-VectorWarp/install.sh "
                "--output vectorwarp-install.sh")
    fedora_install = ("sudo dnf install -y curl gnupg2 && " + download +
                      " && sudo bash vectorwarp-install.sh ")
    apt_install = ("sudo apt update && sudo apt install -y curl gnupg && " + download +
                   " && sudo bash vectorwarp-install.sh ")
    if has_launcher:
        fedora_install += '--repo-only && sudo dnf install -y vectorwarp && vectorwarp'
        apt_install += '--repo-only && sudo apt update && sudo apt install -y vectorwarp && vectorwarp'
        startup_guidance = '''Run <code>vectorwarp</code> to open the web interface, then open Settings
and configure your receiver. Choose <strong>Save &amp; Restart</strong> to save settings
and start radar, including the first time. No separate start command is needed.
<strong>Save for later</strong> saves without starting radar. On a headless
system, the command prints the web address.'''
        control_guidance = '''Use <code>vectorwarp start</code> to start the full VectorWarp stack with
saved settings, <code>vectorwarp stop</code> to stop it, or
<code>vectorwarp restart</code> for an ordered stop/start. Stop includes the web interface.
Shared receiver/vendor services are not stopped. Service changes may ask for
your administrator password. <code>vectorwarp status</code>, <code>vectorwarp logs</code>
and <code>vectorwarp version</code> show service state, logs and the installed version.
<code>vectorwarp help</code> lists all commands.'''
        upgrade_guidance = '''Successful upgrades restart previously running VectorWarp services and leave
intentionally stopped radar stopped. Finish receiver setup/build actions before
updating; a helper that cannot be safely stopped blocks unpacking with an error.'''
    else:
        fedora_install += '--start-web'
        apt_install += '--start-web'
        startup_guidance = '''Open <code>http://localhost:3000</code>, configure your receiver in Settings,
and choose <strong>Save &amp; Restart</strong> to start radar.'''
        control_guidance = '''If an earlier installation completed but the page does not load, run
<code>sudo systemctl enable --now vectorwarp-api.service</code> and inspect it
with <code>sudo systemctl status vectorwarp-api.service --no-pager</code>.'''
        upgrade_guidance = '''A package update does not restart a running API or receiver helper.
After receiver-management actions finish and pending authorizations expire, an
administrator may run <code>sudo systemctl restart vectorwarp-receiver.service &amp;&amp; sudo systemctl restart vectorwarp-api.service</code>
to activate updated code. Do not run it mid-transaction; it does not restart
<code>vectorwarp-processor.service</code> or radar processing.'''
    return f'''<section class="panel" aria-labelledby="install">
<h2 id="install">Install VectorWarp {version}</h2>
<h3>1. Install for your OS</h3>
<p>For Linux, copy the command for your OS. It installs the prerequisites, adds
our signed repository, installs VectorWarp and opens the web interface. On macOS,
use the Homebrew tap or the signed PKG in the download table. Enter your
administrator password if prompted.</p>
<h4>Fedora 44</h4>
<pre><code>{escape(fedora_install)}</code></pre>
<h4>Ubuntu, Debian and compatible DragonOS / Raspberry Pi OS</h4>
<pre><code>{escape(apt_install)}</code></pre>
<h4>macOS with Homebrew</h4>
<pre><code>brew tap mickeyslaven/vectorwarp
brew trust mickeyslaven/vectorwarp
brew install vectorwarp
vectorwarp</code></pre>
<p>The <a href="https://github.com/mickeyslaven/blah2-VectorWarp/blob/main/script/install-release.sh">installer source</a>
is available to read separately.</p>
<h3>2. Configure and start radar</h3>
<p>{startup_guidance}</p>
<p>Open <code>http://localhost:3000</code> on the installed machine, or
<code>http://&lt;server-IP&gt;:3000</code> from another device on your trusted network.
On a fresh install, radar processing stays stopped until you configure it and
choose Save &amp; Restart.</p>
<p>Each package includes Kraken, USRP and dual HackRF adapters, plus the source
kit to build RSPduo support from Settings after installing SDRplay's API. Receiver
hardware and external software are separate; RSPduo needs the locally installed
SDRplay API. Follow <a href="https://github.com/mickeyslaven/blah2-VectorWarp/blob/main/docs/SETUP.md">receiver setup</a>
after installing.</p>
<h3>3. Start, stop and check services</h3>
<p>{control_guidance}</p>
<p>On macOS, the Homebrew command is <code>vectorwarp</code>. The standalone PKG
uses <code>/Applications/VectorWarp.app/Contents/MacOS/VectorWarp</code>.
Append <code>start</code>, <code>stop</code>, <code>restart</code>, or
<code>status</code> to either launcher path as needed.</p>
<h3>4. Update</h3>
<p>For Linux, your repository is already configured; do not repeat the installation steps.</p>
<p>Ubuntu, Debian and compatible DragonOS / Raspberry Pi OS:</p>
<pre><code>sudo apt update &amp;&amp; sudo apt install vectorwarp</code></pre>
<p>Fedora:</p>
<pre><code>sudo dnf upgrade --refresh vectorwarp</code></pre>
<p>For macOS Homebrew: <code>brew upgrade vectorwarp vectorwarp-heimdall</code>.
For a standalone PKG, use the next release's PKG through the normal Installer flow.</p>
<p>{upgrade_guidance}</p>
<h3>Direct downloads</h3>
<p>For Linux, prefer the installer above for automatic updates. For a manual installation,
choose the package matching your OS version and architecture. Install Linux packages with
<code>sudo apt install ./matching.deb</code> on Ubuntu, Debian, or DragonOS, or
<code>sudo dnf install ./matching.rpm</code> on Fedora, so dependencies resolve.
Do not use <code>dpkg</code> alone or manually mix release libraries.
{macos_guidance}</p>
<div class="table-scroll" tabindex="0" role="region" aria-label="Package downloads by operating system and architecture">
<table><caption>VectorWarp {version} · verified packages by OS and architecture</caption>
<thead><tr><th scope="col">Operating system</th><th scope="col">x86-64<br>(amd64 / x86_64)</th><th scope="col">ARM64<br>(arm64 / aarch64)</th></tr></thead>
<tbody>{table_rows}</tbody></table></div>
<p class="scope">{macos_scope}x86-64 covers Intel and AMD PCs.
DragonOS uses its Ubuntu base;
check <code>/etc/os-release</code>. FocalX R37.1 is Ubuntu 22.04 (Jammy) amd64,
not Ubuntu 26.04. Raspberry Pi OS Trixie uses Debian 13 ARM64. No 32-bit package
is provided. For other systems, use the
<a href="https://github.com/mickeyslaven/blah2-VectorWarp/blob/main/docs/INSTALL.md">source installation guide</a>.</p>
<details><summary>Verify a direct download</summary>
{macos_verify}
<p>For Linux DEB/RPM packages, save your package, <a href="{base}/SHA256SUMS">checksums</a>,
<a href="{base}/SHA256SUMS.asc">checksum signature</a>, and
<a href="{base}/vectorwarp-archive-key.asc">public signing key</a> in the same folder.
The release key fingerprint is <code>{fingerprint}</code>; compare it with the
<a href="https://github.com/mickeyslaven/blah2-VectorWarp/blob/main/docs/MAINTAINER_RELEASE.md#one-time-setup">maintainer's published fingerprint</a>
before trusting the key.</p>
<pre><code>gpg --show-keys --with-fingerprint vectorwarp-archive-key.asc
gpg --dearmor --output vectorwarp-release-keyring.gpg vectorwarp-archive-key.asc &amp;&amp; \\
  gpgv --keyring ./vectorwarp-release-keyring.gpg SHA256SUMS.asc SHA256SUMS &amp;&amp; \\
  sha256sum --check --strict --ignore-missing SHA256SUMS</code></pre>
<p>Continue only if the key matches and both the signature and your package's
checksum pass. A checksum alone does not authenticate a download.</p></details>
<p><a href="https://github.com/mickeyslaven/blah2-VectorWarp/releases/tag/v{version}">Release notes and all assets</a>
· <a href="repository-manifest.json">Package manifest</a></p>
</section>'''


def repository_homepage(manifest=None):
    """Selected measured capabilities; full evidence stays in the linked report."""
    return '''<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="description" content="VectorWarp: more channels, wider Doppler and GPU-accelerated passive radar with browser controls.">
<title>VectorWarp — more channels, wider Doppler, faster radar</title>
<style>
*{box-sizing:border-box}body{margin:0;background:#faf8f5;color:#24221f;font:17px/1.6 system-ui,sans-serif}
main,header,footer{max-width:68rem;margin:auto;padding:1.4rem}
header{display:flex;align-items:center;gap:.8rem;font-weight:750}
.brand{display:inline-flex;align-items:center;justify-content:center;width:2.8rem;height:2.8rem;background:#ed7b24;color:#171717;border-radius:.35rem}
h1{font-size:clamp(2.2rem,6vw,4rem);line-height:1.08;letter-spacing:-.045em;max-width:17ch;margin:.5rem 0 1.3rem}
h2{font-size:1.5rem;line-height:1.3;margin:0 0 1rem}p{max-width:65ch}
a{color:#923700;text-underline-offset:.2em}a:focus-visible,.table-scroll:focus-visible{outline:3px solid #bf5007;outline-offset:4px}
.hero{padding:1rem 0 2.5rem}.actions{display:flex;gap:.8rem;flex-wrap:wrap;align-items:center}
.button{display:inline-block;padding:.65rem 1.1rem;border-radius:.4rem;background:#ed7b24;color:#171717;font-weight:700;text-decoration:none}
section{margin:0 0 2.5rem}.panel{background:#fff;border:1px solid #e6e0d7;border-radius:.75rem;padding:1.5rem}
.table-scroll{overflow-x:auto}table{border-collapse:collapse;width:100%;min-width:38rem;font-size:.95rem}
caption{text-align:left;font-weight:600;margin-bottom:.5rem}th,td{padding:.85rem;text-align:left;border-bottom:1px solid #e6e0d7;vertical-align:top}
th:first-child{padding-left:0}td:last-child{font-weight:700;color:#873200}
.scope{font-size:.85rem;color:#59544b}.features{display:grid;grid-template-columns:repeat(auto-fit,minmax(min(100%,16rem),1fr));gap:1rem}
pre{max-width:100%;overflow-x:auto;padding:1rem;background:#f3f0eb;border-radius:.4rem;font-size:.85rem;line-height:1.5}
code{overflow-wrap:anywhere}pre code{overflow-wrap:normal}summary{cursor:pointer;font-weight:650}details{margin-top:1rem}
.features h3{font-size:1rem;margin:0}.features p{margin:.3rem 0 0}footer{font-size:.85rem;color:#59544b}
@media(max-width:40rem){main,header,footer{padding:1rem}.panel{padding:1rem}}
</style></head><body>
<header><span class="brand" aria-hidden="true">VW</span> VectorWarp</header>
<main>
<section class="hero">
<h1>More channels. Wider Doppler. Faster radar.</h1>
<p><strong>THIS IS A DEVELOPMENT BUILD. EXPECT BUGS AND REPORT VIA GITHUB ISSUES PLEASE AND THANK YOU!</strong></p>
<p>Native passive radar for Linux and macOS with multicore processing, optional GPU acceleration,
and browser controls for live displays, settings, recording and replay.</p>
<p class="scope">Apple Silicon macOS is tested on M2; Intel Mac support remains experimental.
The download table lists only packages present in the verified release.</p>
<div class="actions"><a class="button" href="#install">Get started</a>
<a href="https://github.com/mickeyslaven/blah2-VectorWarp">Explore the project</a></div>
</section>
<!-- VERIFIED_RELEASE_INSTALLATION -->
<section class="panel" aria-labelledby="results">
<h2 id="results">Faster than regular blah2</h2>
<p>Same recorded IQ at its original rate. Same CPU budget on each host.</p>
<div class="table-scroll" tabindex="0" role="region" aria-label="Regular blah2 and VectorWarp processing comparison">
<table><caption>Matched 200 ms processing workloads; lower is better</caption>
<thead><tr><th scope="col">Hardware and workload</th><th scope="col">Regular blah2 CPU</th><th scope="col">VectorWarp CPU</th><th scope="col">VectorWarp GPU</th></tr></thead>
<tbody>
<tr><th scope="row">Strix, ±800 Hz</th><td>79.5 ms</td><td>65.5 ms</td><td>31.1 ms</td></tr>
<tr><th scope="row">RTX 4050 Laptop, ±800 Hz</th><td>230.5 ms</td><td>180.9 ms</td><td>63.6 ms</td></tr>
<tr><th scope="row">Pavilion AMD GPU, ±2400 Hz</th><td>306.8 ms</td><td>229.9 ms</td><td>88.1 ms</td></tr>
</tbody></table></div>
<p>On the RTX 4050 workload, regular blah2 missed 22 of 24 measured intervals;
VectorWarp GPU missed none. Accuracy qualification runs at startup; accepted
steady GPU frames do not repeat CPU clutter or complex-map accuracy calculations.</p>
<p>At 200 ms CPI and ±2400 Hz, these hosts used about 25% less processing time
in CPU mode and 64–71% less in GPU mode than original blah2.</p>
<p class="scope">GPU acceleration covers clutter FFT/filtering and delay–Doppler work;
the small FP64 coefficient solve and other radar stages remain on CPU.</p>
<p><strong>Earlier live array proof:</strong> at 527 MHz and 2.4 MS/s, a five-channel
array GPU run at ±800 Hz and 200 ms CPI averaged 95.5 ms. This is live capacity
evidence from the prior version with recurring CPU checks, not a new live run
or a matched upstream ratio.</p>
<h2>More radar per frame</h2>
<p>VectorWarp also completes wider and five-channel configurations where regular
blah2 has no equivalent mode or cannot safely represent the requested geometry.</p>
<p class="scope">Physical NVIDIA, AMD and Intel GPU checks compare complex maps to a
CPU reference. Results are tolerance-validated, not bit-exact or a guarantee for every host.</p>
<a href="https://github.com/mickeyslaven/blah2-VectorWarp/blob/main/docs/GPU_BENCHMARK_20260911.md">Full configurations, timing distributions and methodology →</a>
<h2>Equal-range Doppler tests</h2>
<p>All comparisons retain the standard 256-bin, 30.604 km excess-path
window at 527 MHz and 2.4 MS/s. At this full range and ±4800 Hz, Strix GPU
processing averaged 67.0 ms per 200 ms CPI and 325.5 ms per one-second CPI.
VectorWarp CPU took 173.8 ms and 886.0 ms respectively; both modes met all 24
steady deadlines in each case. These are capacity results where upstream's
Doppler buffer cannot safely represent the configuration.</p>
<p>The combined efficiency changes also reduced the Strix ±2400 Hz GPU workload
from 69.7 to 44.7 ms versus the preceding VectorWarp version in this campaign.
Heavier workloads still miss some deadlines; the full report includes all
configurations, a separate before/after comparison and remaining processing costs.</p>
<p>On Raspberry Pi 4, the CPU workload at 200 ms CPI and ±800 Hz took 798.2 ms
in VectorWarp, versus 905.1 ms in original blah2 and 905.9 ms in Off World Labs'
ARM fork, with NEON FFTW enabled for all three.
<a href="https://github.com/mickeyslaven/blah2-VectorWarp/blob/main/docs/PI4_PERFORMANCE_20260911.md">Pi comparison and workload limits →</a></p>
<p>A separate Pi 4 replay loaded a newer Mesa driver for the test and reduced
processing from 797.2 ms CPU-only to 578.1 ms with GPU Automatic (27.5% less
time). It still missed the 200 ms deadline; the driver was not installed system-wide.
<a href="https://github.com/mickeyslaven/blah2-VectorWarp/blob/main/docs/PI_GPU_SETUP.md#pi-4-driver-diagnostic">Pi GPU results and supported driver updates →</a></p>
</section>
<section aria-labelledby="features">
<h2 id="features">Everything in one interface</h2>
<div class="features">
<div><h3>Live radar and maps</h3><p>Delay–Doppler, delay ellipses, spectrum and fullscreen displays.</p></div>
<div><h3>Browser settings</h3><p>Clear controls, validation and Save &amp; Restart.</p></div>
<div><h3>Built-in ADS-B</h3><p>Use a local decoder or a remote tar1090 feed.</p></div>
<div><h3>Record and replay</h3><p>Capture IQ and return to the same recording for another look.</p></div>
</div>
</section>
</main>
<footer>Built on <a href="https://github.com/30hours/blah2">blah2 by 30hours</a>. MIT licensed.</footer>
</body></html>
'''.replace('<!-- VERIFIED_RELEASE_INSTALLATION -->', release_installation(manifest) if manifest else
            '<section id="install"><h2>Install VectorWarp</h2><p>Release downloads are not available in this preview. '
            '<a href="https://github.com/mickeyslaven/blah2-VectorWarp/blob/main/docs/INSTALL.md#build-from-source">Build from source</a>.</p></section>')


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
    version = manifest.get("version")
    source_commit = manifest.get("source_commit")
    if not isinstance(version, str) or not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version):
        raise ValueError("Package manifest must bind one stable numeric version")
    if not isinstance(source_commit, str) or not re.fullmatch(r"[0-9A-Fa-f]{40}", source_commit):
        raise ValueError("Package manifest must bind one full source commit")
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
        if version != manifest["version"]:
            raise ValueError("Package version disagrees with aggregate manifest")
        if release != ("1.fc44" if entry["format"] == "rpm" else "1"):
            raise ValueError(f"Unexpected package release: {filename}")
        if entry.get("name") != "vectorwarp" or not filename.endswith("." + entry["format"]):
            raise ValueError(f"Wrong package name or extension: {filename}")
        if entry["format"] == "deb":
            distro_label = "debian13" if entry["distro"] == "debian" else "ubuntu" + entry["distro_version"]
            expected_filename = (f"vectorwarp_{version}-{release}_{distro_label}_{entry['arch']}.deb")
        else:
            expected_filename = f"vectorwarp-{version}-{release}.{entry['arch']}.rpm"
        if filename != expected_filename:
            raise ValueError(f"Package filename disagrees with its immutable identity: {filename}")
        if entry.get("test_only", False) is not False:
            raise ValueError(f"Test-only packages cannot be published: {filename}")
        if (entry.get("backend"), entry.get("gpu"), entry.get("node_version")) != (
                "all", "auto", "24.21.0"):
            raise ValueError(f"Unexpected package build profile: {filename}")
        if (entry.get("compiled_receivers") != ["Usrp", "HackRF", "Kraken"] or
                entry.get("local_build_receivers") != ["RspDuo"]):
            raise ValueError(f"Package must contain three compiled adapters and the local RSPduo source kit: {filename}")
        identity = (entry["format"], entry["distro"], entry["distro_version"], entry["arch"], version, release)
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
    return manifest, entries


def verify_release_matrix(entries):
    actual = {(entry["format"], entry["distro"], entry["distro_version"], entry["arch"])
              for entry in entries}
    if actual != RELEASE_TARGETS or len(entries) != len(RELEASE_TARGETS):
        missing = sorted(RELEASE_TARGETS - actual)
        extra = sorted(actual - RELEASE_TARGETS)
        raise ValueError(f"Release matrix is incomplete or inconsistent (missing={missing}, extra={extra})")


def verify_macos_release(path, packages, version, source_commit):
    """Bind an Apple-reviewed release asset to this immutable release."""
    path = Path(path)
    if path.is_symlink() or not path.is_file():
        raise ValueError("Mac release receipt must be a regular file")
    entry = json.loads(path.read_text())
    filename = f"vectorwarp-{version}-macos-universal.pkg"
    source_name = f"vectorwarp-{version}-macos-corresponding-source.tar.gz"
    source = entry.get("source_archive") if isinstance(entry, dict) else None
    if (not isinstance(entry, dict) or entry.get("schema") != 1 or
            entry.get("version") != version or entry.get("publication_commit") != source_commit.lower() or
            entry.get("filename") != filename or entry.get("apple_team_id") != "DJGHPX8T7R" or
            entry.get("notary_status") != "Accepted" or
            entry.get("gatekeeper") != "Notarized Developer ID" or
            not isinstance(entry.get("runtime_source_id"), str) or
            not re.fullmatch(r"[0-9a-f]{40}", entry["runtime_source_id"]) or
            not isinstance(entry.get("notary_submission_id"), str) or
            not re.fullmatch(r"[0-9a-f-]{36}", entry["notary_submission_id"]) or
            not isinstance(entry.get("sha256"), str) or
            not re.fullmatch(r"[0-9a-f]{64}", entry["sha256"]) or
            not isinstance(source, dict) or source.get("filename") != source_name or
            not isinstance(source.get("sha256"), str) or
            not re.fullmatch(r"[0-9a-f]{64}", source["sha256"]) or
            type(source.get("size")) is not int or source["size"] <= 0 or
            type(entry.get("size")) is not int or entry["size"] <= 0):
        raise ValueError("Mac release receipt is incomplete or mismatched")
    package = packages / filename
    if package.is_symlink() or not package.is_file() or package.stat().st_size != entry["size"]:
        raise ValueError("Mac release package is missing or size differs")
    if sha256(package) != entry["sha256"]:
        raise ValueError("Mac release package checksum differs")
    source_file = packages / source_name
    if (source_file.is_symlink() or not source_file.is_file() or
            source_file.stat().st_size != source["size"] or sha256(source_file) != source["sha256"]):
        raise ValueError("Mac corresponding source archive differs")
    return entry


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
    manifest, entries = load_manifest(Path(args.manifest), packages)
    expected_version = getattr(args, "expected_version", None)
    expected_source_commit = getattr(args, "expected_source_commit", None)
    if expected_version is not None and manifest["version"] != expected_version:
        raise ValueError("Package manifest version does not match the selected release")
    if (expected_source_commit is not None and
            manifest["source_commit"].lower() != expected_source_commit.lower()):
        raise ValueError("Package manifest source commit does not match the selected release tag")
    if getattr(args, "require_release_matrix", False):
        verify_release_matrix(entries)
    macos_release = getattr(args, "macos_release", None)
    macos_entry = (verify_macos_release(macos_release, packages, manifest["version"],
                                        manifest["source_commit"]) if macos_release else None)
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
                    "version": manifest["version"], "source_commit": manifest["source_commit"].lower(),
                    "signing_fingerprint": fingerprint, "packages": published}
        if macos_entry:
            document["macos_package"] = macos_entry
        (site / "repository-manifest.json").write_text(json.dumps(document, indent=2) + "\n")
        if args.installer_template:
            template = Path(args.installer_template).read_text()
            if "@SIGNING_FINGERPRINT@" not in template:
                raise ValueError("Installer template has no signing-fingerprint placeholder")
            (site / "install.sh").write_text(template.replace("@SIGNING_FINGERPRINT@", fingerprint))
        (site / ".nojekyll").touch()
        (site / "index.html").write_text(repository_homepage(document))
        if sum(file.stat().st_size for file in site.rglob("*") if file.is_file()) > MAX_BYTES:
            raise ValueError("Signed repository exceeds the 900-MiB Pages budget")
        site.rename(output)
    return document


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("packages", "manifest", "output", "public-key", "fingerprint", "signing-key"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--expected-version")
    parser.add_argument("--expected-source-commit")
    parser.add_argument("--require-release-matrix", action="store_true")
    parser.add_argument("--installer-template")
    parser.add_argument("--macos-release", help="verified Mac release receipt beside its PKG in --packages")
    args = parser.parse_args()
    try:
        document = build(args)
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        parser.exit(1, f"Repository not published: {error}\n")
    print(f"Verified signed repository: {len(document['packages'])} packages at {args.output}")


if __name__ == "__main__":
    main()
