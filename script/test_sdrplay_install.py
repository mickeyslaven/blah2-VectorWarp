#!/usr/bin/env python3
"""Source-only contract checks for the fixed SDRplay first-install hook."""

from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
URL = 'https://sdrplay.com/hardware-api/'


class SdrplayInstallContractTest(unittest.TestCase):
    def test_build_and_native_install_keep_new_helpers_optional(self):
        build = (ROOT / 'script/build-native.sh').read_text()
        install = (ROOT / 'script/install-native.sh').read_text()
        self.assertIn('vectorwarp-sdrplay-service.py', build)
        self.assertIn('vectorwarp-prepare-sdrplay.js', build)
        self.assertIn('if [[ -f $SOURCE_DIR/script/vectorwarp-sdrplay-service.py ]]', build)
        self.assertIn('render "$ARTIFACT/libexec/vectorwarp-sdrplay-service.py"', install)
        self.assertIn('install -m 0755 "$temporary/vectorwarp-sdrplay-service" "$target_prefix/libexec/vectorwarp-sdrplay-service"', install)
        self.assertIn('install -m 0644 "$ARTIFACT/libexec/vectorwarp-prepare-sdrplay.js"', install)

    def test_native_hook_is_first_real_install_only_and_nonfatal(self):
        text = (ROOT / 'script/install-native.sh').read_text()
        self.assertIn('had_current_link=false', text)
        self.assertIn('$had_current_link == false', text)
        self.assertIn('$EUID -eq 0', text)
        self.assertIn('$DRY_RUN == false', text)
        self.assertIn('$PREFLIGHT_ONLY == false', text)
        self.assertIn('-d /run/systemd/system', text)
        self.assertIn('$RECEIVER_TYPES == *RspDuo* || $LOCAL_BUILD_RECEIVER_TYPES == *RspDuo*', text)
        self.assertIn('/usr/bin/python3 -I "$target_prefix/libexec/vectorwarp-sdrplay-service" install ||', text)
        self.assertIn(URL, text)

    def test_copied_release_code_is_not_group_or_world_writable(self):
        text = (ROOT / 'script/install-native.sh').read_text()
        copy = text.index('run cp -a "$ARTIFACT" "$release"')
        normalize = text.index('run chmod -R go-w "$release"')
        current_link = text.index('run ln -sfn "releases/$build_id" "$target_prefix/current.next"')
        self.assertLess(copy, normalize)
        self.assertLess(normalize, current_link)
        self.assertNotIn('chmod -R go-w "$target_sysconf"', text)

    def test_package_hooks_are_first_install_only_and_do_not_fail_install(self):
        deb = (ROOT / 'packaging/deb/postinst').read_text()
        rpm = (ROOT / 'packaging/rpm/vectorwarp.spec.in').read_text()
        self.assertIn('[ "$1" = configure ] && [ -z "${2:-}" ]', deb)
        self.assertIn('/usr/bin/python3 -I /opt/vectorwarp/libexec/vectorwarp-sdrplay-service install ||', deb)
        self.assertIn('[ "$1" -eq 1 ]', rpm)
        self.assertIn('/usr/bin/python3 -I /opt/vectorwarp/libexec/vectorwarp-sdrplay-service install ||', rpm)
        self.assertIn(URL, deb)
        self.assertIn(URL, rpm)
        for text in (deb, rpm):
            self.assertNotIn('enable vectorwarp-', text)
            self.assertNotIn('start vectorwarp-', text)
            self.assertNotIn('curl ', text)
            self.assertNotIn('wget ', text)

    def test_rendered_deb_hook_calls_fixed_helper_only_on_first_configure(self):
        with tempfile.TemporaryDirectory(prefix='vectorwarp-sdrplay-postinst-') as temporary:
            temp = Path(temporary)
            log = temp / 'calls'
            helper = temp / 'vectorwarp-sdrplay-service'
            helper.write_text(f'import pathlib,sys\npathlib.Path({str(log)!r}).write_text(" ".join(sys.argv[1:]) + "\\n")\nsys.exit(1)\n')
            helper.chmod(0o755)
            body = (ROOT / 'packaging/deb/postinst').read_text()
            body = body.replace('/run/systemd/system', str(temp / 'systemd'))
            body = body.replace('/opt/vectorwarp/libexec/vectorwarp-sdrplay-service', str(helper))
            script = temp / 'postinst'; script.write_text(body); script.chmod(0o755)
            (temp / 'systemd').mkdir()
            tools = temp / 'bin'; tools.mkdir()
            for name in ('systemd-sysusers', 'systemd-tmpfiles', 'systemctl'):
                path = tools / name; path.write_text('#!/bin/sh\nexit 0\n'); path.chmod(0o755)
            getent = tools / 'getent'; getent.write_text('#!/bin/sh\nexit 1\n'); getent.chmod(0o755)
            environment = os.environ | {'PATH': f'{tools}:{os.environ["PATH"]}'}
            first = subprocess.run(['sh', str(script), 'configure'], env=environment,
                                   text=True, capture_output=True, check=False)
            self.assertEqual(first.returncode, 0, first.stderr)
            self.assertEqual(log.read_text(), 'install\n')
            log.unlink()
            upgrade = subprocess.run(['sh', str(script), 'configure', '1.0.0'], env=environment,
                                     text=True, capture_output=True, check=False)
            self.assertEqual(upgrade.returncode, 0, upgrade.stderr)
            self.assertFalse(log.exists(), 'an upgrade must not invoke the SDRplay helper')

    def test_installed_helper_name_mode_and_isolated_wrapper_refuse_untrusted_staging(self):
        """The no-extension helper imports under -I but cannot trust a staging tree."""
        with tempfile.TemporaryDirectory(prefix='vectorwarp-sdrplay-wrapper-') as temporary:
            prefix = Path(temporary)
            libexec = prefix / 'libexec'
            current = prefix / 'current'
            libexec.mkdir(); current.mkdir()
            # This is the build artifact name followed by the install-native
            # destination name/mode, including the sibling broker it imports.
            staged = libexec / 'vectorwarp-sdrplay-service.py'
            installed = libexec / 'vectorwarp-sdrplay-service'
            shutil.copy2(ROOT / 'script/vectorwarp-sdrplay-service.py', staged)
            staged.write_text(staged.read_text().replace('@PREFIX@', str(prefix)))
            os.chmod(staged, 0o755)
            shutil.copy2(staged, installed)
            os.chmod(installed, 0o755)
            shutil.copy2(ROOT / 'script/vectorwarp-receiver-helper.py',
                         libexec / 'vectorwarp-receiver-helper')
            os.chmod(libexec / 'vectorwarp-receiver-helper', 0o755)
            (current / '.vectorwarp-build').write_text('compiled_receivers=Kraken\n')
            self.assertTrue(os.access(installed, os.X_OK))
            result = subprocess.run(['/usr/bin/python3', '-I', str(installed), 'install'],
                                    text=True, capture_output=True, check=False)
            self.assertNotEqual(result.returncode, 0)
            if os.geteuid() == 0:
                self.assertIn('root-owned, non-writable installed files', result.stderr)
            else:
                self.assertIn('requires the installed privileged restart helper', result.stderr)


if __name__ == '__main__':
    unittest.main()
