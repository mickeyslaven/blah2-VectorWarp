"""Check install-guide commands without installing packages or starting services."""
from pathlib import Path
import os
import re
import subprocess
import unittest
from urllib.parse import unquote

ROOT = Path(__file__).resolve().parents[2]
GUIDES = ("README.md", "docs/INSTALL.md", "docs/SETUP.md", "docs/SDRPLAY_SETUP.md", "docs/3LIPS_SETUP.md",
          "docs/PI_GPU_SETUP.md", "docs/DRAGONOS.md", "docs/GPU_ACCELERATION.md",
          "packaging/README.md", "docs/MAINTAINER_RELEASE.md", "docs/UPSTREAM_COMPARISON.md",
          "src/capture/rspduo/README.md", "src/capture/hackrf/README.md")


def project_documents():
    """Include new documentation pages without walking dependency/build trees."""
    excluded = {".git", "node_modules", "lib", "build", "dist", "__pycache__"}
    for directory, folders, files in os.walk(ROOT):
        folders[:] = [name for name in folders if name not in excluded]
        for name in files:
            path = Path(directory) / name
            if not path.is_symlink() and path.suffix in {".md", ".rst", ".adoc", ".html"}:
                yield path


def heading_ids(text):
    """GitHub-style anchors for the ordinary Markdown headings in these guides."""
    result, counts = set(), {}
    text = re.sub(r"```.*?```", "", text, flags=re.S)
    for heading in re.findall(r"^#{1,6}\s+(.+?)\s*#*\s*$", text, re.M):
        heading = re.sub(r"\[([^]]+)\]\([^)]+\)", r"\1", heading)
        slug = re.sub(r"[^\w\- ]", "", heading.lower()).replace(" ", "-")
        count = counts.get(slug, 0)
        counts[slug] = count + 1
        result.add(slug if count == 0 else f"{slug}-{count}")
    result.update(re.findall(r'\bid=["\']([^"\']+)["\']', text))
    return result


class InstallDocumentationTests(unittest.TestCase):
    def test_all_documentation_uses_simple_package_update_commands(self):
        for document in project_documents():
            with self.subTest(document=str(document.relative_to(ROOT))):
                text = " ".join(document.read_text().split())
                self.assertNotRegex(text, r"\bapt(?:-get)?\s+(?:install\s+)?--only-upgrade\b")
                self.assertNotRegex(text, r"\bdnf\s+upgrade\s+vectorwarp\b")

    def test_launcher_command_reference_matches_real_help(self):
        result = subprocess.run(["/usr/bin/python3", str(ROOT / "script/vectorwarp"), "--help"],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        install = " ".join((ROOT / "docs/INSTALL.md").read_text().split())
        for command in ("open", "start", "stop", "restart", "status", "logs", "version", "help"):
            with self.subTest(command=command):
                self.assertRegex(result.stdout, rf"(?m)^  {command}\s+")
                self.assertIn(f"vectorwarp {command}", install)
        self.assertIn("including the web interface", result.stdout)
        self.assertIn("web API", install)
        self.assertIn("including on the first run", install)
        self.assertIn("**Save for later** saves without starting it", install)

    def test_installer_help_uses_checked_launcher_not_direct_processor_start(self):
        result = subprocess.run(["bash", str(ROOT / "script/install-release.sh"), "--help"],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("vectorwarp start", result.stdout)
        self.assertIn("vectorwarp help", result.stdout)
        self.assertIn("previously running services", result.stdout)
        self.assertNotIn("before starting vectorwarp-processor.service", result.stdout)
        native = (ROOT / "script/install-native.sh").read_text()
        self.assertIn("vectorwarp help lists all commands", native)
        self.assertNotIn("systemctl enable --now vectorwarp-api.service vectorwarp-processor.service", native)
        release = (ROOT / "script/install-release.sh").read_text()
        self.assertNotIn("still needs the coordinated refresh", release)

    def test_active_install_routes_point_to_the_shared_package_page(self):
        package_guides = ("README.md", "docs/INSTALL.md", "docs/SETUP.md",
                          "docs/SDRPLAY_SETUP.md", "docs/PI_GPU_SETUP.md",
                          "docs/DRAGONOS.md", "packaging/README.md",
                          "docs/UPSTREAM_COMPARISON.md",
                          "src/capture/rspduo/README.md", "src/capture/hackrf/README.md")
        for relative in package_guides:
            with self.subTest(guide=relative):
                self.assertIn("(https://mickeyslaven.github.io/blah2-VectorWarp/#install)",
                              (ROOT / relative).read_text())
        comparison = (ROOT / "docs/UPSTREAM_COMPARISON.md").read_text()
        self.assertNotIn("No published repository or release yet", comparison)
        hackrf = (ROOT / "src/capture/hackrf/README.md").read_text()
        self.assertIn("RSPduo source kit", hackrf)
        self.assertNotIn("requires the SDRplay SDK", hackrf)

    def test_shell_examples_parse_without_execution(self):
        for relative in GUIDES:
            text = (ROOT / relative).read_text()
            for index, block in enumerate(re.findall(r"```(?:bash|sh)\n(.*?)```", text, re.S)):
                with self.subTest(guide=relative, block=index):
                    result = subprocess.run(["bash", "-n"], input=block,
                                            text=True, capture_output=True, timeout=10)
                    self.assertEqual(result.returncode, 0, result.stderr)

    def test_local_document_links_exist(self):
        for relative in GUIDES:
            document = ROOT / relative
            for target in re.findall(r"\]\(([^)]+)\)", document.read_text()):
                target = re.sub(r'''\s+(?:"[^"]*"|'[^']*')\s*$''', "", target)
                if "://" in target or target.startswith("mailto:"):
                    continue
                with self.subTest(guide=relative, target=target):
                    filename, _, anchor = unquote(target).partition("#")
                    destination = document.parent / filename if filename else document
                    self.assertTrue(destination.is_file(), target)
                    if anchor and destination.suffix == ".md":
                        self.assertIn(anchor, heading_ids(destination.read_text()), target)

    def test_source_instructions_use_supported_native_dependencies(self):
        for relative in ("docs/INSTALL.md",):
            text = (ROOT / relative).read_text()
            with self.subTest(guide=relative):
                self.assertNotIn("libhackrf-devel", text)
                self.assertIn("hackrf-devel", text)
                self.assertIn("libusb1-devel", text)
                self.assertIn("libusb-1.0-0-dev", text)
                self.assertIn("python3-apt", text)
                self.assertIn("python3-libdnf5", text)
                self.assertIn("python3-rpm", text)
                self.assertIn("gnupg", text)
                self.assertIn("gnupg2", text)
                self.assertIn("/usr/bin/node --version", text)
                self.assertIn("npm --version", text)
                self.assertIn("--backend", text)
                self.assertIn("--preflight", text)

    def test_setup_defers_build_commands_to_install_guide(self):
        setup = (ROOT / "docs/SETUP.md").read_text()
        self.assertIn("(INSTALL.md)", setup)
        self.assertNotIn("sudo apt install", setup)
        self.assertNotIn("sudo dnf install", setup)
        self.assertNotIn("git clone", setup)
        self.assertIn("## Build from source", setup)  # Preserve inbound anchors.
        for heading in ("## KrakenSDR Suite V2", "## SDRplay RSPduo", "## USRP / B210", "## Dual HackRF"):
            self.assertIn(heading, setup)
        self.assertNotIn("| Receiver | Settings sent to software", setup)

    def test_pi_is_in_support_table_not_an_oversized_heading(self):
        install = (ROOT / "docs/INSTALL.md").read_text()
        for system in ("Ubuntu", "Debian", "Fedora", "DragonOS", "Raspberry Pi OS"):
            self.assertRegex(install, rf"(?m)^\| .*{re.escape(system)}.*\|")
        self.assertNotRegex(install, r"(?m)^## Raspberry Pi")
        self.assertIn("### Raspberry Pi GPU", install)

    def test_current_receiver_guidance_agrees_with_policy(self):
        helper = (ROOT / "script/vectorwarp-receiver-helper.py").read_text()
        self.assertRegex(helper, r"\(4,\s*1(?:,\s*0)?\)")
        guide = (ROOT / "api/receiver-setup-guide.js").read_text()
        self.assertIn("UHD 4.1 or newer", guide)
        self.assertNotIn("UHD 4.8", guide)
        for relative in ("docs/INSTALL.md", "docs/SETUP.md"):
            self.assertIn("UHD 4.1", (ROOT / relative).read_text())
        rsp = (ROOT / "src/capture/rspduo/README.md").read_text()
        self.assertIn("gainReduction", rsp)
        self.assertIn("two values", rsp)
        self.assertIn("../../../api/config-manager.js", rsp)
        self.assertNotIn("default value of", rsp)

    def test_explicit_package_startup_does_not_enable_boot_capture(self):
        install = (ROOT / "docs/INSTALL.md").read_text()
        self.assertIn("sudo systemctl start vectorwarp-api.service", install)
        self.assertIn("sudo systemctl enable --now vectorwarp-api.service", install)
        self.assertIn("enables the web API at boot", install)
        self.assertIn("does not enable or start VectorWarp services", install)
        self.assertIn("Save for later", install)
        self.assertIn("Save & Restart", install)
        for unit in re.findall(r"\bvectorwarp-[a-z-]+\.service\b", install):
            self.assertTrue((ROOT / "contrib/systemd" / (unit + ".in")).is_file())

    def test_3lips_guide_documents_the_builtin_converter_change(self):
        guide = " ".join((ROOT / "docs/3LIPS_SETUP.md").read_text().split())
        api = (ROOT / "api/server.js").read_text()
        self.assertIn("event/algorithm/associator/AdsbAssociator.py", guide)
        self.assertIn("generate_api_url", guide)
        self.assertIn('return f"http://{radar}/api/adsb/delay-doppler"', guide)
        self.assertIn("/api/adsb/delay-doppler", api)
        self.assertIn("docker compose up -d --build event", guide)
        self.assertIn("separate aircraft map-feed setting", guide)
        self.assertIn("simulated data, not a live multi-node test", guide)
        self.assertNotIn("http://adsb2dd.30hours.dev/api/dd", guide)
        self.assertNotIn("Minimum delay bin", guide)
        self.assertNotIn("Maximum delay bin", guide)


if __name__ == "__main__":
    unittest.main()
