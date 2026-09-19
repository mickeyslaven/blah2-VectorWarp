#!/usr/bin/env python3
"""Real processor replay GPU telemetry check; no SDR, SDK, or GUI."""
import argparse, importlib.util, json, subprocess, tempfile, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('replay', ROOT/'test/recording/processor_replay_test.py')
replay = importlib.util.module_from_spec(spec); spec.loader.exec_module(replay)
def last_json(payload):
    text=bytes(payload).decode(); decoder=json.JSONDecoder(); index=0; value=None
    while index < len(text):
        while index < len(text) and text[index].isspace(): index += 1
        if index >= len(text): break
        value,index=decoder.raw_decode(text,index)
    return value

def run(binary, requested, clutter):
    with tempfile.TemporaryDirectory(prefix='vectorwarp-gpu-replay-') as temporary:
        root=Path(temporary); recording=root/'input.blah2iq'; replay.write_blah2iq(recording, 2, 6000000, 204640000, frame_samples=3600000)
        status=replay.StatusServer(); sinks=replay.SinkGroup(); process=None; log=None
        try:
            status.start(); sinks.start(); config=replay.config('Usrp',2,recording,status.port,sinks.ports())
            config['capture']['fs']=6000000; config['process']['data']['cpi']=.2; config['process']['performance']['acceleration']=requested; config['process']['clutter']['enable']=clutter
            cfg=root/'config.json'; cfg.write_text(json.dumps(config)); log=(root/'processor.log').open('w+')
            process=subprocess.Popen([str(binary),'-c',str(cfg)],cwd=root,stdout=log,stderr=subprocess.STDOUT,text=True,start_new_session=True)
            if not replay.wait_for(lambda:any(x.get('state')=='complete' for x in status.snapshot()),time.monotonic()+60):
                log.seek(0); raise AssertionError(f'{status.snapshot()[-3:]} {log.read()[-4000:]}')
            forced=replay.stop_process(process); log.seek(0); tail=log.read()[-4000:]
            assert not forced and process.returncode==0, tail
            replay.assert_processor_outputs(sinks); timing=last_json(sinks.payloads['timing']); replay.assert_finite(replay.first_json(sinks.payloads['map'],'map'))
            replay.assert_finite(timing, 'timing')
            acceleration=timing['acceleration']; clutter_status=timing['clutterAcceleration']; assert acceleration['requested']==requested
            if requested=='cpu': assert acceleration['active']=='cpu'
            elif requested=='gpu': assert acceleration['active']=='vulkan', acceleration
            else: assert acceleration['active'] in ('cpu','vulkan'), acceleration
            if clutter:
                assert clutter_status['active'] in ('cpu','vulkan') and clutter_status['cpuExecuted'] != clutter_status['gpuExecuted'], clutter_status
            if requested=='gpu' and clutter:
                assert clutter_status['active']=='vulkan' and clutter_status['gpuExecuted'] is True and clutter_status['cpuExecuted'] is False, clutter_status
            return {'requested':requested,'clutter':clutter,'active':acceleration['active'],'clutterActive':clutter_status['active'],
                    'clutterGpuExecuted':clutter_status['gpuExecuted'],'clutterCpuExecuted':clutter_status['cpuExecuted']}
        finally:
            if process and process.poll() is None: replay.stop_process(process)
            if log: log.close()
            status.close(); sinks.close()

if __name__=='__main__':
    parser=argparse.ArgumentParser(); parser.add_argument('binary',type=Path); args=parser.parse_args(); args.binary=args.binary.resolve()
    results=[run(args.binary,'cpu',False),run(args.binary,'auto',False),run(args.binary,'gpu',True)]
    print(json.dumps(results,sort_keys=True))
