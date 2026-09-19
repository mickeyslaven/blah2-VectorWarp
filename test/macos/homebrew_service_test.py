#!/usr/bin/env python3
"""Isolated `brew services run` smoke/recovery test; never registers at login."""
import importlib.util, json, os, pathlib, shutil, socket, subprocess, tempfile, time, urllib.request
ROOT=pathlib.Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('replay',ROOT/'test/recording/processor_replay_test.py'); replay=importlib.util.module_from_spec(spec); spec.loader.exec_module(replay)
def wait(check, seconds=30):
    end=time.monotonic()+seconds; last=None
    while time.monotonic()<end:
        try:
            value=check()
            if value:return value
        except Exception as error: last=error
        time.sleep(.2)
    raise AssertionError(f'timed out: {last}')
def main():
  prefix=pathlib.Path(subprocess.check_output(['brew','--prefix','vectorwarp'],text=True).strip())
  assert not (pathlib.Path.home()/'Library/LaunchAgents/homebrew.mxcl.vectorwarp.plist').exists()
  assert subprocess.run(['launchctl','print',f'gui/{os.getuid()}/homebrew.mxcl.vectorwarp'],capture_output=True).returncode != 0
  with tempfile.TemporaryDirectory(prefix='vectorwarp-brew-service-') as temp:
    work=pathlib.Path(temp); state=work/'state'; config=work/'config.json'; recording=work/'loop.blah2iq'; replay.write_blah2iq(recording,2,2000000,204640000,frames=5)
    sockets=[socket.socket() for _ in range(8)]
    for item in sockets:item.bind(('127.0.0.1',0))
    ports=[item.getsockname()[1] for item in sockets]
    for item in sockets:item.close()
    payload=replay.config('Usrp',2,recording,ports[0],dict(zip(replay.PORT_NAMES,ports[1:7])),loop=True); payload['network']['ports']['config']=ports[7]
    base=json.loads(subprocess.check_output([shutil.which('node'),'-e','process.stdout.write(JSON.stringify(require("js-yaml").load(require("fs").readFileSync(process.argv[1],"utf8"))))',str(prefix/'libexec/config/config.yml')],cwd=prefix/'libexec/api',text=True))
    base.update(payload); base.setdefault('truth',{}).setdefault('adsb',{})['enabled']=False; base['process']['performance']['acceleration']='cpu'; base['save']['path']=str(work)+'/' ; config.write_text(json.dumps(base))
    cfg=work/'xdg/homebrew/services'; cfg.mkdir(parents=True); (cfg/'vectorwarp.env').write_text('\n'.join([f'VECTORWARP_MACOS_STATE={state}',f'VECTORWARP_MACOS_CONFIG={config}',f'VECTORWARP_MACOS_NODE={shutil.which("node")}', '']) )
    env=dict(os.environ,XDG_CONFIG_HOME=str(work/'xdg'))
    subprocess.run(['brew','trust','--formula','vectorwarp/local/vectorwarp'],env=env,check=True,capture_output=True,text=True)
    url=f'http://127.0.0.1:{ports[0]}/api/system/status'
    def receiving():
      with urllib.request.urlopen(url,timeout=2) as response:
        value=json.load(response); return value if value.get('radar')=='receiving' else None
    original=config.read_bytes()
    try:
      subprocess.run(['brew','services','run','vectorwarp'],env=env,check=True,capture_output=True,text=True)
      wait(receiving); before=[json.loads((state/f'{name}.json').read_text())['pid'] for name in ('api','processor')]
      os.kill(before[1],9)
      def replaced():
        value=[json.loads((state/f'{name}.json').read_text())['pid'] for name in ('api','processor')]
        return value if value[0]!=before[0] and value[1]!=before[1] else None
      after=wait(replaced,40); wait(receiving,40); assert all(pid>1 for pid in after)
    finally:
      stopped=subprocess.run(['brew','services','stop','vectorwarp'],env=env,capture_output=True,text=True); assert stopped.returncode==0, stopped.stdout+stopped.stderr
    assert config.read_bytes()==original and not any((state/f'{name}.json').exists() for name in ('api','processor'))
    assert subprocess.run(['launchctl','print',f'gui/{os.getuid()}/homebrew.mxcl.vectorwarp'],capture_output=True).returncode != 0
  print('isolated Homebrew service run/recovery/cleanup passed')
if __name__=='__main__':main()
