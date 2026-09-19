#!/usr/bin/env python3
import hashlib
from io import BytesIO
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "script/package-homebrew-release.py"
COMMIT = "a" * 40
VERSION = "0.1.7"
REVISION = "351"
URL = f"https://github.com/mickeyslaven/blah2-VectorWarp/archive/{COMMIT}.tar.gz"


class HomebrewReleaseTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.archive = self.root / "source.tar.gz"
        self.write_archive()

    def tearDown(self):
        self.temporary.cleanup()

    def write_archive(self, extra=None):
        prefix = f"blah2-VectorWarp-{COMMIT}"
        with tarfile.open(self.archive, "w:gz") as archive:
            for name in ("vectorwarp.rb.in", "vectorwarp-heimdall.rb.in"):
                data = (ROOT / "Formula" / name).read_bytes()
                member = tarfile.TarInfo(f"{prefix}/Formula/{name}")
                member.size = len(data)
                archive.addfile(member, BytesIO(data))
            data = b"# source\n"
            member = tarfile.TarInfo(f"{prefix}/README.md")
            member.size = len(data)
            archive.addfile(member, BytesIO(data))
            if extra:
                member = tarfile.TarInfo(f"{prefix}/{extra}")
                member.size = 8
                archive.addfile(member, BytesIO(b"blocked\n"))

    def command(self, *extra, check=True):
        return subprocess.run([sys.executable, str(SCRIPT), "--source-archive", str(self.archive),
                               "--source-url", URL, "--version", VERSION, "--revision", REVISION, *extra],
                              text=True, capture_output=True, check=check)

    def test_renders_public_formulas_and_receipt(self):
        output = self.root / "tap"
        self.command("--output-tap-dir", str(output))
        checksum = hashlib.sha256(self.archive.read_bytes()).hexdigest()
        app = (output / "Formula/vectorwarp.rb").read_text()
        companion = (output / "Formula/vectorwarp-heimdall.rb").read_text()
        self.assertIn(URL, app)
        self.assertIn(f'version "{VERSION}"', app)
        self.assertIn(f'revision {REVISION}', app)
        self.assertIn(f'version "{VERSION}"', companion)
        self.assertIn(f'sha256 "{checksum}"', app)
        self.assertIn('depends_on "mickeyslaven/vectorwarp/vectorwarp-heimdall"', app)
        self.assertNotIn("vectorwarp/local", app)
        self.assertNotIn("for VectorWarp development", companion)
        manifest = (output / "homebrew-release.json").read_text()
        self.assertIn(COMMIT, manifest)
        self.assertIn(checksum, manifest)

    def test_rejects_unpinned_urls_private_members_and_bad_revision(self):
        for bad_url in ("file:///tmp/source.tar.gz", URL.replace("https://", "http://"),
                        URL.replace(COMMIT, "main"), URL + "?x=1"):
            result = self.command("--source-url", bad_url, "--output-tap-dir", str(self.root / "bad"), check=False)
            self.assertNotEqual(result.returncode, 0, bad_url)
        self.write_archive("private/secret.txt")
        result = self.command("--output-tap-dir", str(self.root / "private"), check=False)
        self.assertNotEqual(result.returncode, 0)
        result = self.command("--revision", "0", "--output-tap-dir", str(self.root / "revision"), check=False)
        self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
