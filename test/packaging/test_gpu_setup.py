"""Offline Pi/native-package/access fault acceptance; never touch host packages."""
import importlib.machinery
import importlib.util
import json
import pathlib
import types
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[2]
loader = importlib.machinery.SourceFileLoader('gpu_setup', str(ROOT / 'script/vectorwarp-gpu-setup'))
spec = importlib.util.spec_from_loader(loader.name, loader)
gpu = importlib.util.module_from_spec(spec); loader.exec_module(gpu)
HOST = {'pi': True, 'model': 'Raspberry Pi 4 Model B', 'distribution': 'ubuntu', 'version': '24.04', 'architecture': 'aarch64'}
ACCESS = {'state': 'available', 'nodes': []}


class SetupTests(unittest.TestCase):
    def test_bounded_child_exit_and_timeout(self):
        self.assertEqual(gpu.run_readonly([gpu.sys.executable, '-c', 'print("fixture")']), 'fixture')
        self.assertIsNone(gpu.run_readonly([gpu.sys.executable, '-c', 'print("not installed"); raise SystemExit(1)']))
        self.assertIsNone(gpu.run_readonly([gpu.sys.executable, '-c', 'import time; time.sleep(2)'], timeout=0.02))
        self.assertIsNone(gpu.run_readonly([gpu.sys.executable, '-c', 'print("x"*100)'], limit=10))

    def test_parent_cancellation_reaps_its_own_diagnostic(self):
        child = mock.MagicMock()
        child.__enter__.return_value = child
        callbacks = []
        def communicate(**kwargs):
            if kwargs: callbacks[0](gpu.signal.SIGTERM, None)
            return b'', None
        child.communicate.side_effect = communicate
        def handler(_sig, callback):
            callbacks.append(callback)
            return gpu.signal.SIG_DFL
        with mock.patch.object(gpu.subprocess, 'Popen', return_value=child), \
             mock.patch.object(gpu.signal, 'signal', side_effect=handler):
            with self.assertRaises(SystemExit): gpu.run_readonly(['/fixture/worker'])
        child.kill.assert_called_once()

    def test_render_node_identity_not_arbitrary_groups_or_symlinks(self):
        directory_info = types.SimpleNamespace(st_mode=gpu.stat.S_IFDIR | 0o755, st_uid=0)
        info = types.SimpleNamespace(st_mode=gpu.stat.S_IFCHR | 0o660, st_uid=0, st_gid=44,
                                     st_rdev=gpu.os.makedev(226, 128))
        class Node:
            name = 'renderD128'
            def __str__(self): return '/dev/dri/renderD128'
            def lstat(self): return info
        directory = types.SimpleNamespace(lstat=lambda: directory_info, glob=lambda _: [Node()])
        with mock.patch.object(gpu.pathlib, 'Path', return_value=directory), \
             mock.patch.object(gpu.pwd, 'getpwnam', return_value=types.SimpleNamespace(pw_name='vectorwarp', pw_uid=200, pw_gid=200)), \
             mock.patch.object(gpu.os, 'getgrouplist', return_value=[200]), \
             mock.patch.object(gpu.grp, 'getgrgid', return_value=types.SimpleNamespace(gr_name='render')):
            self.assertEqual(gpu.render_access()['nodes'][0]['group'], 'render')
            for attribute, bad in [('st_mode', gpu.stat.S_IFLNK | 0o777), ('st_mode', gpu.stat.S_IFREG | 0o660),
                                   ('st_rdev', gpu.os.makedev(1, 128)), ('st_rdev', gpu.os.makedev(226, 0)), ('st_uid', 1000)]:
                previous = getattr(info, attribute); setattr(info, attribute, bad)
                self.assertEqual(gpu.render_access()['nodes'], [])
                setattr(info, attribute, previous)
            directory_info.st_mode = gpu.stat.S_IFLNK | 0o777
            self.assertEqual(gpu.render_access()['nodes'], [])
            directory_info.st_mode = gpu.stat.S_IFDIR | 0o777
            self.assertEqual(gpu.render_access()['nodes'], [])
            directory_info.st_mode = gpu.stat.S_IFDIR | 0o755
            directory_info.st_uid = 1000
            self.assertEqual(gpu.render_access()['nodes'], [])
            directory_info.st_uid = 0
            info.st_mode = gpu.stat.S_IFCHR | 0o600
            self.assertFalse(gpu.render_access()['nodes'][0]['groupAllowsAccess'])
            info.st_mode = gpu.stat.S_IFCHR | 0o660
            for minor, expected in ((127, False), (128, True), (255, True), (256, False)):
                info.st_rdev = gpu.os.makedev(226, minor)
                self.assertEqual(bool(gpu.render_access()['nodes']), expected)
            info.st_rdev = gpu.os.makedev(226, 128)
            with mock.patch.object(gpu.os, 'getgrouplist', return_value=[200, 44]):
                self.assertEqual(gpu.render_access()['state'], 'available')

    def test_pi_detection_is_device_tree_not_cpu(self):
        for model, expected in [('Raspberry Pi 4 Model B\0', True), ('Generic ARM64 board', False), ('', False)]:
            with mock.patch.object(gpu, 'read_text', side_effect=lambda path, limit=0: model if 'model' in path else 'ID=ubuntu\nVERSION_ID="24.04"'), \
                 mock.patch.object(gpu.os, 'uname', return_value=types.SimpleNamespace(machine='aarch64')):
                self.assertEqual(gpu.host_info()['pi'], expected)

    def test_non_pi_checks_service_access_not_api_driver_or_package(self):
        with mock.patch.object(gpu, 'host_info', return_value={**HOST, 'pi': False}), \
             mock.patch.object(gpu, 'installed_version') as package, mock.patch.object(gpu, 'enumerate_driver') as driver, \
             mock.patch.object(gpu, 'service_render_access', return_value=ACCESS):
            self.assertEqual(gpu.status()['state'], 'access-configured')
            package.assert_not_called(); driver.assert_not_called()
        for distribution in ('ubuntu', 'debian', 'fedora'):
            result = gpu.classify({**HOST, 'pi': False, 'distribution': distribution}, None, {},
                                 {'state': 'group-access-needed', 'nodes': []})
            self.assertEqual(result['state'], 'service-access-needed')
            self.assertEqual(result['qualification'], 'not-run')
        unavailable = gpu.classify({**HOST, 'pi': False}, None, {}, {'state': 'unavailable', 'nodes': []})
        self.assertEqual(unavailable['state'], 'driver-unverified')
        self.assertIn('Activate the updated receiver helper', unavailable['message'])

    def test_private_devices_api_uses_readonly_broker_metadata(self):
        receipt = {'ok': True, 'serviceAccess': {'state': 'group-access-needed', 'nodes': []}}
        connection = mock.MagicMock()
        connection.__enter__.return_value = connection
        connection.makefile.return_value.readline.return_value = (json.dumps(receipt) + '\n').encode()
        with mock.patch.object(gpu.pwd, 'getpwuid', return_value=types.SimpleNamespace(pw_name='vectorwarp-api')), \
             mock.patch.object(gpu.socket, 'socket', return_value=connection), \
             mock.patch.object(gpu, 'render_access') as local:
            self.assertEqual(gpu.service_render_access(), receipt['serviceAccess'])
            connection.sendall.assert_called_once_with(b'{"verb":"gpu-access"}\n')
            connection.connect.assert_called_once_with('/run/vectorwarp-receiver/management.sock')
            connection.settimeout.assert_called_once_with(2)
            local.assert_not_called()
            for response in (b'{}\n', b'[]\n', b'bad\n', b'x' * 16385, b'{"ok":false}\n'):
                connection.makefile.return_value.readline.return_value = response
                self.assertEqual(gpu.service_render_access()['state'], 'unavailable')
            connection.connect.side_effect = OSError('broker stopped')
            self.assertEqual(gpu.service_render_access()['state'], 'unavailable')

    def test_local_status_does_not_need_root_broker(self):
        with mock.patch.object(gpu.pwd, 'getpwuid', return_value=types.SimpleNamespace(pw_name='regular-user')), \
             mock.patch.object(gpu, 'render_access', return_value=ACCESS), mock.patch.object(gpu.socket, 'socket') as connect:
            self.assertEqual(gpu.service_render_access(), ACCESS)
            connect.assert_not_called()

    def test_missing_and_inaccessible_are_not_qualification(self):
        missing = {'available': False, 'devices': []}
        self.assertEqual(gpu.classify(HOST, None, missing, ACCESS)['state'], 'driver-unavailable')
        self.assertEqual(gpu.classify(HOST, '26.1.8', missing, ACCESS)['state'], 'driver-unverified')
        self.assertEqual(gpu.classify(HOST, '26.1.8', missing, ACCESS)['qualification'], 'not-run')

    def test_version_is_v3dv_only_hint_and_never_acceptance(self):
        for version, expected in [('26.0.3', 'compiler-risk'), ('26.0.6', 'compiler-risk'),
                                  ('26.0.7', 'unqualified'), ('26.1.0', 'unqualified'), ('26.1.8', 'unqualified'),
                                  ('unknown', 'compiler-risk'), ('24.0.9', 'compiler-risk')]:
            driver = {'devices': [{'driverId': 19, 'mesaVersion': version}]}
            result = gpu.classify(HOST, version, driver, ACCESS)
            self.assertEqual(result['state'], expected)
            self.assertEqual(result['qualification'], 'not-run')
        self.assertEqual(gpu.classify(HOST, '26.1.8', {'devices': [{'driverId': 4, 'mesaVersion': '26.1.8'}]}, ACCESS)['state'], 'driver-unverified')

    def test_package_query_failure_is_not_an_installed_version(self):
        with mock.patch.object(gpu, 'run_readonly', return_value='not-installed\t'):
            self.assertIsNone(gpu.installed_version(HOST))
        with mock.patch.object(gpu, 'run_readonly', return_value='installed\t26.1.8-1'):
            self.assertEqual(gpu.installed_version(HOST), '26.1.8-1')

    def test_native_enumeration_schema_and_timeout_are_safe(self):
        with mock.patch.object(gpu, 'worker_path', return_value='/fixture/worker'), \
             mock.patch.object(gpu, 'run_readonly', return_value=None):
            self.assertFalse(gpu.enumerate_driver()['available'])
        for output in ['[]', '{}', 'garbage', json.dumps({'version': 1, 'devices': [{}] * 33})]:
            with mock.patch.object(gpu, 'worker_path', return_value='/fixture/worker'), \
                 mock.patch.object(gpu, 'run_readonly', return_value=output):
                self.assertFalse(gpu.enumerate_driver()['available'])

    def test_no_web_or_unattended_privilege(self):
        with mock.patch.object(gpu.os, 'geteuid', return_value=0), \
             mock.patch.object(gpu.sys.stdin, 'isatty', return_value=False):
            with self.assertRaises(gpu.Refused): gpu.interactive_guard()
        with mock.patch.object(gpu.os, 'geteuid', return_value=0), \
             mock.patch.object(gpu.sys.stdin, 'isatty', return_value=True), \
             mock.patch.object(gpu.sys.stdout, 'isatty', return_value=True), \
             mock.patch.object(gpu, 'host_info', return_value={**HOST, 'pi': False}):
            with self.assertRaises(gpu.Refused): gpu.interactive_guard()

    def test_group_access_is_explicit_append_only(self):
        original = {'state': 'group-access-needed', 'nodes': [{'path': '/dev/dri/renderD128', 'group': 'render', 'accessibleByService': False, 'groupAllowsAccess': True}]}
        with mock.patch.object(gpu, 'interactive_guard'), mock.patch('builtins.input', return_value='ENABLE GPU ACCESS'), \
             mock.patch.object(gpu, 'render_access', side_effect=[original, original, ACCESS]), \
             mock.patch.object(gpu.subprocess, 'run') as run:
            gpu.enable_access()
            self.assertEqual(run.call_args[0][0], ['/usr/sbin/usermod', '--append', '--groups', 'render', 'vectorwarp'])
        for condition in ['cancel', 'changed', 'foreign']:
            nodes = original if condition != 'foreign' else {'state': 'group-access-needed', 'nodes': [{'path': '/dev/dri/renderD128', 'group': 'wheel', 'accessibleByService': False}]}
            with mock.patch.object(gpu, 'interactive_guard'), mock.patch('builtins.input', return_value='NO' if condition == 'cancel' else 'ENABLE GPU ACCESS'), \
                 mock.patch.object(gpu, 'render_access', side_effect=[nodes, ACCESS]), \
                 mock.patch.object(gpu.subprocess, 'run') as run:
                with self.assertRaises(gpu.Refused): gpu.enable_access()
                run.assert_not_called()

    def test_package_access_is_root_only_bounded_and_idempotent(self):
        nodes = [{'path': '/dev/dri/renderD128', 'group': 'render', 'accessibleByService': False, 'groupAllowsAccess': True},
                 {'path': '/dev/dri/renderD129', 'group': 'video', 'accessibleByService': False, 'groupAllowsAccess': True}]
        access = {'state': 'group-access-needed', 'nodes': nodes}
        with mock.patch.object(gpu.os, 'geteuid', return_value=0), \
             mock.patch.object(gpu, 'render_access', side_effect=[access, access, ACCESS]), \
             mock.patch('builtins.input') as prompt, mock.patch.object(gpu.subprocess, 'run') as run:
            gpu.enable_access(automatic=True)
            run.assert_called_once()
            self.assertEqual(run.call_args.args[0], ['/usr/sbin/usermod', '--append', '--groups', 'render,video', 'vectorwarp'])
            prompt.assert_not_called()
        with mock.patch.object(gpu.os, 'geteuid', return_value=1000), mock.patch.object(gpu.subprocess, 'run') as run:
            with self.assertRaises(gpu.Refused): gpu.enable_access(automatic=True)
            run.assert_not_called()
        for state in ('available', 'render-node-missing', 'service-user-missing'):
            with mock.patch.object(gpu.os, 'geteuid', return_value=0), \
                 mock.patch.object(gpu, 'render_access', return_value={'state': state, 'nodes': []}), \
                 mock.patch.object(gpu.subprocess, 'run') as run:
                gpu.enable_access(automatic=True)
                run.assert_not_called()
        for change in ({'group': 'root'}, {'group': 'wheel'}, {'groupAllowsAccess': False}):
            unsafe = {'state': 'group-access-needed', 'nodes': [{**nodes[0], **change}]}
            with mock.patch.object(gpu.os, 'geteuid', return_value=0), \
                 mock.patch.object(gpu, 'render_access', return_value=unsafe), \
                 mock.patch.object(gpu.subprocess, 'run') as run:
                with self.assertRaises(gpu.Refused): gpu.enable_access(automatic=True)
                run.assert_not_called()

    def test_non_pi_access_interactive_but_driver_install_stays_pi_only(self):
        with mock.patch.object(gpu.os, 'geteuid', return_value=0), \
             mock.patch.object(gpu.sys.stdin, 'isatty', return_value=True), \
             mock.patch.object(gpu.sys.stdout, 'isatty', return_value=True), \
             mock.patch.object(gpu, 'host_info', return_value={**HOST, 'pi': False, 'architecture': 'x86_64'}):
            self.assertFalse(gpu.interactive_guard(pi_only=False)['pi'])
            with self.assertRaises(gpu.Refused): gpu.interactive_guard()

    def test_install_delegates_native_confirmation_without_injection(self):
        command = ['/usr/bin/apt-get', '--no-remove', 'install', 'mesa-vulkan-drivers=26.1.8-1']
        with mock.patch.object(gpu, 'interactive_guard', return_value=HOST), \
             mock.patch.object(gpu, 'apt_plan', return_value=command), mock.patch.object(gpu.os, 'execve') as execute:
            gpu.install_driver()
            self.assertEqual(execute.call_args[0][:2], ('/usr/bin/apt-get', command))
            self.assertEqual(set(execute.call_args[0][2]), {'PATH', 'LC_ALL', 'TERM', 'DEBIAN_FRONTEND'})

    def test_staging_and_package_integration(self):
        build = (ROOT / 'script/build-native.sh').read_text()
        install = (ROOT / 'script/install-native.sh').read_text()
        release = (ROOT / 'script/install-release.sh').read_text()
        self.assertIn('$SOURCE_DIR/script/vectorwarp-gpu-setup', build)
        self.assertIn('$target_prefix/libexec/vectorwarp-gpu-setup', install)
        self.assertIn('--setup-pi-gpu is forbidden with --destdir', install)
        self.assertLess(release.index('run apt-get install vectorwarp'), release.index('run /opt/vectorwarp/libexec/vectorwarp-gpu-setup --install-driver'))
        for filename in ['packaging/deb/postinst', 'packaging/rpm/vectorwarp.spec.in']:
            text = (ROOT / filename).read_text()
            self.assertNotIn('--install-driver', text, 'Never invoke another package manager while its install lock is held')
            self.assertIn('--configure-service-access', text)
        self.assertIn('--configure-service-access', install)
        self.assertIn('Requires:       shadow-utils', (ROOT / 'packaging/rpm/vectorwarp.spec.in').read_text())
        self.assertIn('python3-apt, passwd,', (ROOT / 'script/package-native.sh').read_text())


class AptPlanTests(unittest.TestCase):
    def setUp(self):
        version = types.SimpleNamespace(version='26.1.8-1', origins=[types.SimpleNamespace(trusted=True, origin='Ubuntu', site='ports.ubuntu.com')], sha256='a' * 64)
        self.root = types.SimpleNamespace(name=gpu.PACKAGE, candidate=version, installed=None,
                                         marked_delete=False, mark_install=mock.Mock())
        class Cache(dict):
            broken_count, dpkg_journal_dirty = 0, False
            def get_changes(inner): return list(inner.values())
        self.cache = Cache({gpu.PACKAGE: self.root})
        self.apt = types.SimpleNamespace(Cache=lambda: self.cache)
        self.apt_pkg = types.SimpleNamespace(version_compare=lambda a, b: (a > b) - (a < b))
        self.common = types.SimpleNamespace(verify_sources=lambda: None, configure=lambda _: None,
                                           NATIVE_SITES={'ubuntu': {'ports.ubuntu.com'}})

    def plan(self):
        with mock.patch.dict('sys.modules', {'apt': self.apt, 'apt_pkg': self.apt_pkg}), \
             mock.patch.object(gpu, 'adapter', return_value=self.common):
            return gpu.apt_plan(HOST)

    def test_exact_native_signed_candidate_with_interactive_manager(self):
        command = self.plan()
        self.assertIn('mesa-vulkan-drivers=26.1.8-1', command)
        self.assertIn('--no-remove', command)
        self.assertIn('APT::Get::Assume-Yes=false', command)
        self.assertNotIn('-y', command)

    def test_third_party_unsigned_hashless_removal_downgrade_refused(self):
        for mutate in [lambda: setattr(self.root.candidate.origins[0], 'trusted', False),
                       lambda: setattr(self.root.candidate.origins[0], 'site', 'ppa.launchpad.net'),
                       lambda: setattr(self.root.candidate, 'sha256', ''),
                       lambda: setattr(self.root, 'marked_delete', True),
                       lambda: setattr(self.root, 'installed', types.SimpleNamespace(version='99.0'))]:
            self.setUp(); mutate()
            with self.assertRaises(gpu.Refused): self.plan()

    def test_missing_candidate_no_update_and_broken_state_refuse(self):
        for mutate in [lambda: setattr(self.root, 'candidate', None), lambda: self.cache.clear(),
                       lambda: setattr(self.cache, 'broken_count', 1),
                       lambda: setattr(self.cache, 'get_changes', lambda: [])]:
            self.setUp(); mutate()
            with self.assertRaises(gpu.Refused): self.plan()


class DnfPlanTests(unittest.TestCase):
    def setUp(self):
        self.version, self.installed, self.options, self.checked = '26.1.8', '26.0.3', {}, []
        owner = self
        class Config:
            def __getattr__(self, name):
                return lambda: types.SimpleNamespace(set=lambda value: owner.options.update({name: value}))
        class Repo:
            def __init__(self, name): self.name = name
            def get_id(self): return self.name
            def get_config(self): return Config()
        class Package:
            def __init__(self, version): self.version = version
            def get_version(self): return self.version
            def get_epoch(self): return '0'
            def get_release(self): return '1.fc44'
            def get_evr(self): return self.version + '-1.fc44'
            def get_full_nevra(self): return 'mesa-vulkan-drivers-0:' + self.get_evr() + '.aarch64'
        class Query:
            def __init__(self, base): self.available = False
            def filter_name(self, names): assert names == [gpu.PACKAGE]
            def filter_arch(self, arches): assert arches == ['aarch64']
            def filter_available(self): self.available = True
            def filter_latest_evr(self): pass
            def filter_installed(self): self.available = False
            def __iter__(self):
                version = owner.version if self.available else owner.installed
                return iter([Package(version)] if version else [])
        sack = types.SimpleNamespace(create_repos_from_system_configuration=lambda: None, load_repos=lambda: None)
        base = types.SimpleNamespace(load_config=lambda: None, get_config=lambda: Config(), setup=lambda: None,
            get_repo_sack=lambda: sack, get_vars=lambda: types.SimpleNamespace(get_value=lambda _: 'aarch64'))
        self.lib = types.SimpleNamespace(base=types.SimpleNamespace(Base=lambda: base),
            rpm=types.SimpleNamespace(PackageQuery=Query), repo=types.SimpleNamespace(RepoQuery=lambda _: [Repo('fedora'), Repo('updates'), Repo('copr')]))
        self.rpm = types.SimpleNamespace(labelCompare=lambda a, b: (a > b) - (a < b))
        self.common = types.SimpleNamespace(verify_repo=lambda repo, host, arch: self.checked.append(repo.get_id()))

    def plan(self, version='44'):
        with mock.patch.dict('sys.modules', {'libdnf5': self.lib, 'rpm': self.rpm}), \
             mock.patch.object(gpu, 'adapter', return_value=self.common):
            return gpu.dnf_plan({**HOST, 'distribution': 'fedora', 'version': version})

    def test_signed_native_repo_exact_candidate_and_native_prompt(self):
        command = self.plan()
        self.assertEqual(command[0], '/usr/bin/dnf')
        self.assertIn('--repo=fedora,updates', command)
        self.assertIn('--setopt=gpgcheck=1', command)
        self.assertIn('--setopt=fedora.gpgkey=', command, 'No automatic key import')
        self.assertIn('--setopt=assumeyes=False', command)
        self.assertEqual(self.checked, ['fedora', 'updates'])
        self.assertFalse(self.options['get_enabled_option'], 'Third-party repositories disabled')
        self.assertNotIn('--allowerasing', command)
        self.assertTrue(command[-1].startswith('mesa-vulkan-drivers-0:26.1.8-1.fc44.'))

    def test_absent_equal_or_older_candidate_and_other_distro_refuse(self):
        for version in [None, '26.0.3', '25.0.0']:
            self.version = version
            with self.assertRaises(gpu.Refused): self.plan()
        with self.assertRaises(gpu.Refused): self.plan('43')

    def test_untrusted_repo_refuses_before_native_command(self):
        self.common.verify_repo = lambda *args: (_ for _ in ()).throw(gpu.Refused('untrusted'))
        with self.assertRaises(gpu.Refused): self.plan()


if __name__ == '__main__':
    unittest.main()
