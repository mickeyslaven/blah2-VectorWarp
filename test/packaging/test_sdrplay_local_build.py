#!/usr/bin/env python3
"""Offline contracts for the fixed-path local RSPduo adapter builder."""
import fcntl, hashlib, importlib.util, json, os, pathlib, shutil, signal, stat, subprocess, tempfile, unittest
from unittest import mock
ROOT=pathlib.Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('builder',ROOT/'script/vectorwarp-build-sdrplay.py'); builder=importlib.util.module_from_spec(spec); spec.loader.exec_module(builder)
stage_spec=importlib.util.spec_from_file_location('stage_rspduo_kit',ROOT/'script/stage-rspduo-kit.py'); stage=importlib.util.module_from_spec(stage_spec); stage_spec.loader.exec_module(stage)
def digest(p): return hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest()
class LocalBuild(unittest.TestCase):
  def test_callback_headers_are_in_every_source_kit_allowlist(self):
    headers={'src/capture/rspduo/SdkSampleClock.h','src/capture/rspduo/UsbMode.h'}
    self.assertTrue(headers <= builder.REQUIRED)
    self.assertTrue(headers <= stage.SOURCES)
    cmake=(ROOT/'cmake/RspduoLocalKit.cmake').read_text()
    for header in headers: self.assertIn(header,cmake)
  def setUp(self):
    self.t=tempfile.TemporaryDirectory(); base=pathlib.Path(self.t.name); self.k=base/'kit'; self.k.mkdir(); self.o=base/'out'; self.o.mkdir(); self.core=base/'core'; self.core.write_bytes(b'core'); self.inc=base/'inc'; self.inc.mkdir(); (self.inc/'sdrplay_api.h').write_bytes(b'#define SDRPLAY_API_VERSION (float)(3.15)\n'); self.lib=base/'lib'; self.lib.write_bytes(b'\x7fELF\x02\x01\x01'+bytes(9)+(3).to_bytes(2,'little')+({'x86_64':62,'aarch64':183,'arm64':183}.get(os.uname().machine,62)).to_bytes(2,'little'))
    for n in builder.REQUIRED:
      p=self.k/n; p.parent.mkdir(parents=True,exist_ok=True); p.write_text(n)
    self.man={'schema':1,'receiver':'RspDuo','kit_id':'a'*64,'cohort':'b'*64,'compiler':{'id':'GNU','version':'12.2.0','target':'x86_64-linux-gnu','cxx_flags':['-D_GLIBCXX_USE_CXX11_ABI=1']},'sources':{n:digest(self.k/n) for n in builder.REQUIRED},'core_sha256':digest(self.core)}; (self.k/'kit.json').write_text(json.dumps(self.man))
    self.p=mock.patch.multiple(builder,KIT=self.k,MANIFEST=self.k/'kit.json',CORE=self.core,INCLUDE=self.inc,LIBRARY=self.lib,OUTROOT=self.o,LOCK=base/'lock'); self.p.start(); self.addCleanup(self.p.stop)
    self.real_trusted=builder.trusted; self.trust=mock.patch.object(builder,'trusted',side_effect=lambda p,regular=True,symlinks=True:pathlib.Path(p)); self.trust.start(); self.addCleanup(self.trust.stop)
    self.ident=mock.patch.object(builder,'compiler_identity',return_value={k:self.man['compiler'][k] for k in ('id','version','target')}); self.ident.start(); self.addCleanup(self.ident.stop)
  def tearDown(self): self.t.cleanup()
  def test_status_missing_and_stale_inputs(self):
    self.assertEqual(builder.status()['state'],'missing'); (self.k/'src/capture/Source.h').write_text('changed'); self.assertEqual(builder.status()['state'],'unavailable')
  def test_status_never_executes_compiler(self):
    with mock.patch.object(builder,'compiler_identity',side_effect=AssertionError('status executed compiler')):
      self.assertEqual(builder.status()['state'],'missing')
  def test_sdk_version_and_host_architecture_are_checked_before_build(self):
    header=self.inc/'sdrplay_api.h'
    header.write_bytes(b'#define SDRPLAY_API_VERSION (float)(3.16)\n')
    self.assertIn('not version 3.15', builder.status()['reason'])
    header.write_bytes(b'#define SDRPLAY_API_VERSION (float)(3.15)\n')
    original=self.lib.read_bytes()
    other=183 if int.from_bytes(original[18:20],'little')==62 else 62
    self.lib.write_bytes(original[:18]+other.to_bytes(2,'little'))
    self.assertIn('host architecture', builder.status()['reason'])
    self.lib.write_bytes(b'not an ELF library')
    self.assertIn('ELF64', builder.status()['reason'])
    self.lib.write_bytes(original)
    self.assertEqual(builder.status()['state'],'missing')
  def test_missing_sdk_guides_to_vendor_without_download(self):
    self.lib.unlink()
    result=builder.status()
    self.assertEqual(result['state'],'unavailable')
    self.assertIn('https://sdrplay.com/hardware-api/', result['reason'])
  def test_build_rejects_wrong_compiler(self):
    with mock.patch.object(builder,'compiler_identity',return_value={'id':'GNU','version':'0.0','target':'wrong'}):
      with self.assertRaisesRegex(builder.Refused,'does not match'): builder.kit_state(check_compiler=True)
  def test_rejects_unexpected_manifest_or_flags(self):
    self.man['compiler']['cxx_flags']=['@rsp']; (self.k/'kit.json').write_text(json.dumps(self.man))
    with self.assertRaises(builder.Refused): builder.kit_state()
  def test_duplicate_manifest_key_is_refused(self):
    (self.k/'kit.json').write_text('{"schema":1,"schema":1}')
    with self.assertRaises(builder.Refused): builder.kit_state()
  @unittest.skipUnless(os.geteuid()==0,'root publication fixture')
  def test_build_uses_fixed_sources_and_publishes_receipt(self):
    def fake(kit,sdk,out): pathlib.Path(out).write_bytes(b'module')
    with mock.patch.object(builder.os,'geteuid',return_value=0), mock.patch.object(builder.pwd,'getpwnam',return_value=type('U',(),{'pw_uid':65534,'pw_gid':65534})()), mock.patch.object(builder.os,'chown'), mock.patch.object(builder,'compile_module',side_effect=fake), mock.patch.object(builder,'validate_output'):
      self.assertEqual(builder.build()['state'],'built')
    final=self.o/('a'*64)/'current'; self.assertTrue((final/'receipt.json').is_file()); self.assertEqual(builder.status()['state'],'current')
  def test_no_network_or_dynamic_arguments(self):
    text=(ROOT/'script/vectorwarp-build-sdrplay.py').read_text(); self.assertNotIn('urllib',text); self.assertNotIn('requests',text)
    for argv in (['build','--path','/tmp'], ['status','extra'], ['download']):
      with self.assertRaises(builder.Refused): builder.main(argv)
  def test_packaged_adapter_needs_only_kit_and_vendor_headers(self):
    # Use the standalone helper's source list and include roots. A dependency
    # scan fails if a future adapter accidentally pulls a host-only header into
    # the Pi source kit, even if that header happens to exist on this host.
    include=pathlib.Path(os.environ.get('BLAH2_SDRPLAY_TEST_INCLUDE','/usr/local/include'))
    if not (include/'sdrplay_api.h').is_file(): self.skipTest('local SDRplay header unavailable')
    with tempfile.TemporaryDirectory() as temporary:
      kit=pathlib.Path(temporary)
      for name in builder.REQUIRED - {'generated/ReceiverCohort.h'}:
        target=kit/name; target.parent.mkdir(parents=True,exist_ok=True)
        shutil.copy2(ROOT/name,target)
      generated=kit/'generated/ReceiverCohort.h'; generated.parent.mkdir(parents=True,exist_ok=True)
      generated.write_text('#define BLAH2_RECEIVER_COHORT "fixture"\n')
      for name in ('src/capture/ReceiverFactory.cpp','src/capture/rspduo/RspDuo.cpp'):
        dependencies=kit/(pathlib.Path(name).name+'.d')
        command=[builder.COMPILER,'-std=c++17','-fsyntax-only','-DBLAH2_MODULE_RSPDUO=1',
          '-I',str(kit/'src'),'-I',str(kit/'generated'),'-I',str(include),
          '-MD','-MF',str(dependencies),str(kit/name)]
        result=subprocess.run(command,capture_output=True,text=True,timeout=30)
        self.assertEqual(result.returncode,0,result.stderr[-2000:])
        self.assertNotIn('rapidjson',dependencies.read_text().lower())
  def test_real_tiny_shared_elf_passes_fixed_readelf_validation(self):
    with tempfile.TemporaryDirectory() as temporary:
      root=pathlib.Path(temporary); source=root/'tiny.cpp'; output=root/'tiny.so'
      source.write_text('extern "C" int vectorwarp_fixture(){ return 7; }\n')
      subprocess.run(['/usr/bin/c++','-shared','-fPIC',str(source),'-o',str(output)],check=True)
      builder.validate_output(output)
  def test_bounded_child_output_and_timeout_are_refused(self):
    with self.assertRaises(builder.Refused):
      builder.bounded(['/usr/bin/python3','-c','import sys;sys.stdout.write("x"*70000)'],2)
    with self.assertRaises(builder.Refused):
      builder.bounded(['/usr/bin/python3','-c','import time;time.sleep(3)'],.05)
  def test_untrusted_world_writable_parent_is_refused(self):
    with tempfile.NamedTemporaryFile() as file:
      with self.assertRaises(builder.Refused): self.real_trusted(pathlib.Path(file.name))

  def test_intermediate_symlink_hop_is_not_hidden_by_safe_final_path(self):
    if os.getuid() != 0: self.skipTest('root-owned trust boundary')
    with tempfile.TemporaryDirectory(dir='/run', prefix='vectorwarp-trust-') as temporary:
      base=pathlib.Path(temporary); safe=base/'safe'; safe.write_text('safe')
      writable=base/'writable'; writable.mkdir(); writable.chmod(0o777)
      (writable/'link').symlink_to(safe); (base/'entry').symlink_to(writable/'link')
      self.assertEqual(self.real_trusted(safe), safe)
      with self.assertRaises(builder.Refused): self.real_trusted(base/'entry')

  def test_root_compiler_identity_is_rejected(self):
    with mock.patch.object(builder.pwd,'getpwnam',return_value=type('U',(),{'pw_uid':0,'pw_gid':0})()):
      with self.assertRaisesRegex(builder.Refused,'must not be root'):
        builder.compile_module(self.man, {}, self.o/'out.so')

  def test_descendants_are_reaped_after_leader_exit_and_timeout(self):
    # A detached grandchild closes its pipe and ignores TERM. Neither EOF nor
    # the leader's successful exit may permit it to outlive the helper call.
    for leader_sleep in (False, True):
      with self.subTest(leader_sleep=leader_sleep):
        pidfile=self.o/'child.pid'
        if pidfile.exists(): pidfile.unlink()
        code=('import os,signal,time,pathlib; p=os.fork(); '
              '\nif p == 0:\n os.setsid(); signal.signal(signal.SIGTERM,signal.SIG_IGN); '
              f'pathlib.Path({str(pidfile)!r}).write_text(str(os.getpid())); '
              'os.close(1); os.close(2); time.sleep(60)\n'
              'else:\n'
              f' while not pathlib.Path({str(pidfile)!r}).exists(): time.sleep(.005)\n'
              + (' time.sleep(60)\n' if leader_sleep else ' os._exit(0)\n'))
        if leader_sleep:
          with self.assertRaises(builder.Refused): builder.bounded(['/usr/bin/python3','-c',code],.2)
        else: self.assertEqual(builder.bounded(['/usr/bin/python3','-c',code],2)[0],0)
        pid=int(pidfile.read_text())
        with self.assertRaises(ProcessLookupError): os.kill(pid,0)

  def test_closed_stdout_does_not_escape_timeout(self):
    with self.assertRaises(builder.Refused):
      builder.bounded(['/usr/bin/python3','-c','import os,time;os.close(1);os.close(2);time.sleep(60)'],.1)

  def test_fifo_and_symlink_output_are_rejected(self):
    final=self.o/'generation'; final.mkdir()
    for kind in ('fifo','symlink'):
      path=self.o/kind
      if kind=='fifo': os.mkfifo(path)
      else: path.symlink_to(self.core)
      with self.assertRaises((OSError,builder.Refused)): builder.publish(path,final)
      self.assertFalse((final/'blah2-receiver-rspduo.so').exists())

  @unittest.skipUnless(os.geteuid()==0,'root publication fixture')
  def test_concurrent_build_lock_is_rejected(self):
    with open(builder.LOCK,'w') as lock:
      fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
      with mock.patch.object(builder.os,'geteuid',return_value=0):
        with self.assertRaises(BlockingIOError): builder.build()

  @unittest.skipUnless(os.geteuid()==0,'root publication fixture')
  def test_failed_pointer_publication_preserves_old_generation_and_retry(self):
    def fake(kit,sdk,out): pathlib.Path(out).write_bytes(b'module')
    with mock.patch.object(builder.os,'geteuid',return_value=0), mock.patch.object(builder.pwd,'getpwnam',return_value=type('U',(),{'pw_uid':65534,'pw_gid':65534})()), mock.patch.object(builder.os,'chown'), mock.patch.object(builder,'compile_module',side_effect=fake), mock.patch.object(builder,'validate_output'):
      builder.build(); base=self.o/('a'*64); old=os.readlink(base/'current')
      self.lib.write_bytes(self.lib.read_bytes()+b'updated SDK')
      with mock.patch.object(builder,'swap_current',side_effect=OSError('injected interruption')):
        with self.assertRaises(OSError): builder.build()
      self.assertEqual(os.readlink(base/'current'),old)
      self.assertEqual(builder.build()['state'],'built')
      self.assertNotEqual(os.readlink(base/'current'),old)
      self.assertEqual(builder.status()['state'],'current')
      self.assertEqual(stat.S_IMODE(builder.LOCK.stat().st_mode),0o644)

  def test_output_is_frozen_before_validation(self):
    output=self.o/'output'; output.write_bytes(b'original')
    final=self.o/'generation'; final.mkdir()
    def check(path):
      self.assertEqual(path.parent,final)
      output.write_bytes(b'changed after freeze')
      self.assertEqual(path.read_bytes(),b'original')
    with mock.patch.object(builder,'validate_output',side_effect=check):
      self.assertEqual(builder.publish(output,final),hashlib.sha256(b'original').hexdigest())
    self.assertEqual((final/'blah2-receiver-rspduo.so').read_bytes(),b'original')

  def test_service_progress_reports_failure_without_starting_radar(self):
    with mock.patch.object(builder,'PROGRESS',self.o/'status.json'), mock.patch.object(builder.os,'geteuid',return_value=0), mock.patch.object(builder,'build',side_effect=builder.Refused('missing SDK')):
      with self.assertRaises(builder.Refused): builder.service()
      self.assertEqual(json.loads((self.o/'status.json').read_text())['state'],'failed')
  def test_service_sandbox_and_boot_lock_contract(self):
    unit=(ROOT/'contrib/systemd/vectorwarp-sdrplay-build.service.in').read_text()
    for directive in ('NoNewPrivileges=yes','PrivateDevices=yes','PrivateNetwork=yes',
                      'ProtectSystem=strict','ProtectHome=yes','RestrictNamespaces=yes',
                      'KillMode=control-group','TimeoutStartSec=180','TimeoutStopSec=5',
                      'MemoryMax=1536M','CPUQuota=100%','TasksMax=40'):
      self.assertIn(directive,unit)
    self.assertIn('CAP_CHOWN CAP_FOWNER CAP_SETGID CAP_SETUID CAP_KILL',unit)
    self.assertNotIn('CAP_DAC_OVERRIDE',unit)
    self.assertNotIn('vectorwarp-processor.service',unit)
    tmpfiles=(ROOT/'contrib/systemd/vectorwarp.tmpfiles').read_text()
    self.assertIn('f /run/vectorwarp-rspduo-build.lock 0644 root root -',tmpfiles)
if __name__=='__main__': unittest.main()
