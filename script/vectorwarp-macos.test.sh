#!/bin/sh
# Isolated launcher regression tests.  No physical receiver or global service.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
TEMP=$(mktemp -d "${TMPDIR:-/tmp}/vectorwarp-macos-test.XXXXXX")
cleanup() {
  status=$?
  trap - EXIT INT TERM
  # The supervisor owns a detached pair; stop and reap it before touching its
  # state. `$!` below is the external launcher/Python process, never a shell
  # function wrapper, so waiting here closes the restart race.
  [ -z "${SUPERVISOR:-}" ] || kill -TERM "$SUPERVISOR" 2>/dev/null || true
  [ -z "${SUPERVISOR:-}" ] || wait "$SUPERVISOR" 2>/dev/null || true
  [ -z "${HOLDER:-}" ] || kill -TERM "$HOLDER" 2>/dev/null || true
  [ -z "${LOCKER:-}" ] || kill -TERM "$LOCKER" 2>/dev/null || true
  [ -z "${WAITER:-}" ] || kill -TERM "$WAITER" 2>/dev/null || true
  [ -z "${HOLDER:-}" ] || wait "$HOLDER" 2>/dev/null || true
  [ -z "${LOCKER:-}" ] || wait "$LOCKER" 2>/dev/null || true
  [ -z "${WAITER:-}" ] || wait "$WAITER" 2>/dev/null || true
  run stop >/dev/null 2>&1 || true
  run_other stop >/dev/null 2>&1 || true
  rm -rf "$TEMP"
  if [ -e "$TEMP" ]; then
    echo "lifecycle test cleanup left $TEMP" >&2
    [ "$status" -ne 0 ] || status=1
  fi
  exit "$status"
}
trap cleanup EXIT INT TERM
mkdir -p "$TEMP/root/api" "$TEMP/root/config" "$TEMP/root/bin" "$TEMP/root/script" "$TEMP/tools"
rsync -a --exclude node_modules "$ROOT/api/" "$TEMP/root/api/"
ln -s "$ROOT/html" "$TEMP/root/html"
ln -s "$ROOT/api/node_modules" "$TEMP/root/api/node_modules"
printf '%s\n' '#!/bin/sh' 'exit 0' >"$TEMP/tools/open"; chmod 755 "$TEMP/tools/open"
cat >"$TEMP/root/api/server.js" <<'EOF'
require('http').createServer((request,response) => { response.end(JSON.stringify({serverId:`${process.pid}-test`})); }).listen(Number(process.env.BLAH2_SETUP_PORT),'127.0.0.1');
EOF
cp "$ROOT/config/config.yml" "$TEMP/root/config/config.yml"
cat >"$TEMP/worker" <<'EOF'
#!/bin/sh
if [ "${1:-}" = --receiver-status ]; then printf '%s\n' '{"schema":1,"hardwareProbed":false,"receivers":[{"receiver":"Kraken","compiled":true},{"receiver":"HackRF","compiled":true}]}'; exit 0; fi
trap 'exit 0' TERM INT
while :; do sleep 1; done
EOF
chmod 755 "$TEMP/worker"; cp "$TEMP/worker" "$TEMP/root/bin/blah2"; chmod 755 "$TEMP/root/bin/blah2"
cat >"$TEMP/root/script/vectorwarp-kraken-macos.py" <<'EOF'
#!/usr/bin/env python3
import json, os, signal, subprocess, sys, time
args = sys.argv[1:]
def value(flag): return args[args.index(flag) + 1]
ready, token, state = value('--ready-file'), value('--instance-token'), value('--state-dir')
os.makedirs(state, exist_ok=True)
child = subprocess.Popen([sys.executable, '-c', 'import signal,time; signal.signal(signal.SIGTERM, lambda *_: exit()); time.sleep(120)'])
open(os.path.join(os.path.dirname(ready), 'kraken-child'), 'w').write(str(child.pid))
def stop(*_):
    child.terminate()
    try: child.wait(3)
    except subprocess.TimeoutExpired: child.kill()
    raise SystemExit(0)
signal.signal(signal.SIGTERM, stop); signal.signal(signal.SIGINT, stop)
payload = {'schema': 1, 'instanceToken': token, 'pid': os.getpid(),
 'status': 'error' if os.environ.get('KRAKEN_FIXTURE_ERROR') else 'ready',
 'dataPort': int(value('--iq-port')), 'controlPort': int(value('--control-port')),
 'bindHost': value('--bind-host'), 'calibration': 'PENDING'}
if payload['status'] == 'error': payload['error'] = 'synthetic readiness failure'
temporary = ready + '.tmp'; open(temporary, 'w').write(json.dumps(payload)); os.chmod(temporary, 0o600); os.replace(temporary, ready)
while True: time.sleep(1)
EOF
chmod 755 "$TEMP/root/script/vectorwarp-kraken-macos.py"
PORT=$(node -e 'require("net").createServer().listen(0,"127.0.0.1",function(){console.log(this.address().port);this.close()})')
run() { PATH="$TEMP/tools:$PATH" VECTORWARP_MACOS_ROOT="$TEMP/root" VECTORWARP_MACOS_STATE="$TEMP/state" VECTORWARP_MACOS_API_PORT="$PORT" VECTORWARP_MACOS_NODE=node "$ROOT/script/vectorwarp-macos" "$@"; }
json_ok() { python3 -c 'import json,sys; x=json.load(open(sys.argv[1])); assert set(("pid","birth","command","argv")) <= set(x); assert isinstance(x["argv"],list)' "$1"; }

# open is web-only and records the launched API atomically as JSON.
run open >/dev/null
json_ok "$TEMP/state/api.json"; test ! -e "$TEMP/state/processor.json"
run start >/dev/null; json_ok "$TEMP/state/processor.json"

# Kernel flock is held by a separate owner, never removed by a contender, and
# is released automatically when its owner exits.
python3 -c 'import fcntl,sys,time; f=open(sys.argv[1],"a+"); fcntl.flock(f,fcntl.LOCK_EX); print("locked",flush=True); time.sleep(30)' "$TEMP/state/lifecycle.lock" >"$TEMP/lock.out" & LOCKER=$!
while ! grep -q locked "$TEMP/lock.out"; do sleep .02; done
PATH="$TEMP/tools:$PATH" VECTORWARP_MACOS_ROOT="$TEMP/root" VECTORWARP_MACOS_STATE="$TEMP/state" \
  VECTORWARP_MACOS_API_PORT="$PORT" VECTORWARP_MACOS_NODE=node \
  "$ROOT/script/vectorwarp-macos" restart-processing >/dev/null & WAITER=$!
sleep .2; kill -0 "$WAITER"; kill -0 "$LOCKER"
kill -TERM "$LOCKER"; wait "$LOCKER" 2>/dev/null || true
LOCKER=
wait "$WAITER"; WAITER=
json_ok "$TEMP/state/processor.json"

# Stop this state's own processor before replacing its record. A copied
# authentic record from another state/config must be refused, leaving
# that live process alone when this state is stopped.
run stop >/dev/null
run_other() { PATH="$TEMP/tools:$PATH" VECTORWARP_MACOS_ROOT="$TEMP/root" VECTORWARP_MACOS_STATE="$TEMP/other-state" VECTORWARP_MACOS_API_PORT="$PORT" VECTORWARP_MACOS_NODE=node "$ROOT/script/vectorwarp-macos" "$@"; }
run_other restart-processing >/dev/null
cp "$TEMP/other-state/processor.json" "$TEMP/state/processor.json"
copied=$(cat "$TEMP/state/processor.json")
if run stop >/dev/null 2>&1; then echo 'foreign installation record was accepted' >&2; exit 1; fi
test "$(cat "$TEMP/state/processor.json")" = "$copied"
OTHER=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["pid"])' "$TEMP/other-state/processor.json")
kill -0 "$OTHER"
run_other stop >/dev/null

# A Homebrew upgrade can replace both the Node Cellar path and the VectorWarp
# Cellar release while an owned API is alive. Its old exact config remains
# stoppable; a config-name prefix never qualifies.
python3 - "$ROOT/script/vectorwarp-macos.py" "$TEMP" <<'PY'
import importlib.util, json, os, pathlib, sys
script, temp = map(pathlib.Path, sys.argv[1:])
spec = importlib.util.spec_from_file_location('launcher', script)
module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
def release(version):
    root = temp/'Cellar'/'vectorwarp'/version/'libexec'
    (root/'api').mkdir(parents=True); (root/'api/server.js').write_text('')
    return root
old, new = release('0.1.7-macos-dev'), release('0.1.8-macos-dev')
state = temp/'upgrade-state'; state.mkdir(); config = state/'config.yml'; config.write_text('')
old_argv = ['/opt/homebrew/Cellar/node@24/24.18.0/bin/node', str((old/'api/server.js').resolve()), str(config.resolve())]
record = {'pid': 42, 'birth': [1, 2], 'argv': old_argv, 'command': old_argv}
(state/'api.json').write_text(json.dumps(record))
module.process_identity = lambda pid: {'birth': [1, 2], 'command': old_argv}
life = module.Lifecycle({'VECTORWARP_MACOS_ROOT': str(new), 'VECTORWARP_MACOS_STATE': str(state), 'VECTORWARP_MACOS_CONFIG': str(config), 'VECTORWARP_MACOS_NODE': '/usr/local/bin/node'})
assert life.live('api'), 'old Cellar API should remain safely owned after upgrade'
default_state = module.Lifecycle({'VECTORWARP_MACOS_ROOT': str(new), 'VECTORWARP_MACOS_NODE': '/usr/local/bin/node'})
assert default_state.runtime_env()['VECTORWARP_MACOS_STATE'] == str(default_state.state), 'default state must reach native children'
calls = []
module.subprocess.check_output = lambda *args, **kwargs: (calls.append(kwargs) or json.dumps({'schema': 1, 'hardwareProbed': False, 'receivers': [{'receiver': 'Usrp', 'compiled': True}]}).encode())
standalone = module.Lifecycle({'VECTORWARP_MACOS_ROOT': str(new), 'VECTORWARP_MACOS_NODE': '/usr/local/bin/node', 'VECTORWARP_MACOS_DISTRIBUTION': 'standalone'})
assert standalone.receiver_types() == 'Usrp' and calls[-1]['timeout'] == 20
assert default_state.receiver_types() == 'Usrp' and calls[-1]['timeout'] == 5
old_processor = old/'bin/blah2'; old_processor.parent.mkdir(); old_processor.write_text('')
new_processor = new/'bin/blah2'; new_processor.parent.mkdir(); new_processor.write_text('')
processor_argv = [str(old_processor.resolve()), '--config', str(config.resolve())]
(state/'processor.json').write_text(json.dumps({'pid': 43, 'birth': [3, 4], 'argv': processor_argv, 'command': processor_argv}))
module.process_identity = lambda pid: {'birth': [1, 2], 'command': old_argv} if pid == 42 else {'birth': [3, 4], 'command': processor_argv}
life.processor = new_processor.resolve()
assert life.live('processor'), 'old Cellar processor should remain safely owned after upgrade'
(old/'api/server.js').unlink(); old_processor.unlink()
assert life.live('api') and life.live('processor'), 'unlinked old release must remain safely stoppable'
record['argv'][2] += '.other'; record['command'] = record['argv']; (state/'api.json').write_text(json.dumps(record))
assert not life.live('api'), 'config prefix must not authorize an old API'
PY

# A foreign HTTP 200 cannot satisfy the API serverId check or start processor.
node -e 'require("http").createServer((q,s)=>s.end(JSON.stringify({serverId:"foreign-1"}))).listen(Number(process.argv[1]),"127.0.0.1")' "$PORT" & HOLDER=$!
sleep .1
if run start >/dev/null 2>&1; then echo 'foreign web service accepted' >&2; exit 1; fi
test ! -e "$TEMP/state/api.json" && test ! -e "$TEMP/state/processor.json"
kill -TERM "$HOLDER"; wait "$HOLDER" 2>/dev/null || true; HOLDER=

# Early processor exit never leaves a state record.
printf '%s\n' '#!/bin/sh' 'exit 12' >"$TEMP/fail"; chmod 755 "$TEMP/fail"
if VECTORWARP_MACOS_ROOT="$TEMP/root" VECTORWARP_MACOS_STATE="$TEMP/state" VECTORWARP_MACOS_API_PORT="$PORT" VECTORWARP_MACOS_NODE=node VECTORWARP_MACOS_PROCESSOR="$TEMP/fail" "$ROOT/script/vectorwarp-macos" start >/dev/null 2>&1; then exit 1; fi
test ! -e "$TEMP/state/processor.json"

# A local live Kraken profile launches the fixed foreground adapter before the
# processor, records only that adapter, and terminates its owned child on stop.
cp "$ROOT/config/config-kraken.yml" "$TEMP/state/config.yml"
run start >/dev/null
json_ok "$TEMP/state/kraken.json"; json_ok "$TEMP/state/processor.json"
KRAKEN_CHILD=$(cat "$TEMP/state/kraken-child")
kill -0 "$KRAKEN_CHILD"
# A Save can change profile, replay state, or ports before the serialized
# restart tears down the already-owned controller. Its immutable launch record
# must still authorize this stop.
cp "$ROOT/config/config.yml" "$TEMP/state/config.yml"
run stop >/dev/null
if kill -0 "$KRAKEN_CHILD" 2>/dev/null; then echo 'Kraken fixture child leaked after stop' >&2; exit 1; fi

# Web-only never opens a Kraken controller, and a signed error readiness
# leaves no controller or processor record.
run open >/dev/null
test ! -e "$TEMP/state/kraken.json" && test ! -e "$TEMP/state/processor.json"
run stop >/dev/null
cp "$ROOT/config/config-kraken.yml" "$TEMP/state/config.yml"
if KRAKEN_FIXTURE_ERROR=1 PATH="$TEMP/tools:$PATH" VECTORWARP_MACOS_ROOT="$TEMP/root" VECTORWARP_MACOS_STATE="$TEMP/state" \
  VECTORWARP_MACOS_API_PORT="$PORT" VECTORWARP_MACOS_NODE=node "$ROOT/script/vectorwarp-macos" start >/dev/null 2>&1; then
  echo 'failed Kraken readiness accepted' >&2; exit 1
fi
test ! -e "$TEMP/state/kraken.json" && test ! -e "$TEMP/state/processor.json"
cp "$ROOT/config/config.yml" "$TEMP/state/config.yml"

# The service target stays in the foreground, restarts the complete owned pair
# after a child crash, and uses SIGTERM to clean both child records up.
PATH="$TEMP/tools:$PATH" VECTORWARP_MACOS_ROOT="$TEMP/root" VECTORWARP_MACOS_STATE="$TEMP/state" \
  VECTORWARP_MACOS_API_PORT="$PORT" VECTORWARP_MACOS_NODE=node \
  "$ROOT/script/vectorwarp-macos" supervise >"$TEMP/supervisor.out" 2>&1 & SUPERVISOR=$!
deadline=$(( $(date +%s) + 12 ))
while [ ! -e "$TEMP/state/processor.json" ] || [ ! -e "$TEMP/state/api.json" ]; do
  [ "$(date +%s)" -lt "$deadline" ] || { cat "$TEMP/supervisor.out" >&2; exit 1; }
  sleep .1
done
# Save & Restart invokes this same serialized action from the web service. It
# must remain compatible with the foreground supervisor and replace both IDs.
OLD_API=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["pid"])' "$TEMP/state/api.json")
OLD_PROCESSOR=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["pid"])' "$TEMP/state/processor.json")
run restart >/dev/null
test "$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["pid"])' "$TEMP/state/api.json")" != "$OLD_API"
test "$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["pid"])' "$TEMP/state/processor.json")" != "$OLD_PROCESSOR"
OLD_PROCESSOR=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["pid"])' "$TEMP/state/processor.json")
kill -TERM "$OLD_PROCESSOR"
deadline=$(( $(date +%s) + 12 ))
while :; do
  NEW_PROCESSOR=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["pid"])' "$TEMP/state/processor.json" 2>/dev/null || true)
  [ -n "$NEW_PROCESSOR" ] && [ "$NEW_PROCESSOR" != "$OLD_PROCESSOR" ] && break
  [ "$(date +%s)" -lt "$deadline" ] || { cat "$TEMP/supervisor.out" >&2; exit 1; }
  sleep .1
done
kill -TERM "$SUPERVISOR"; wait "$SUPERVISOR" 2>/dev/null || true; SUPERVISOR=
test ! -e "$TEMP/state/api.json" && test ! -e "$TEMP/state/processor.json"
printf '%s\n' 'macOS local lifecycle JSON-state tests passed.'
