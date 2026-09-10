#!/usr/bin/env bash
# Install and smoke-test one native package in an ephemeral hosted build
# environment. It invokes neither systemd nor the radar processor.
set -euo pipefail

die() { printf 'smoke-native-package: %s\n' "$*" >&2; exit 1; }
[[ $# == 2 && $1 == --package ]] || die 'usage: smoke-native-package.sh --package PATH'
if [[ $EUID -ne 0 ]]; then
  command -v sudo >/dev/null || die 'sudo is required for a hosted package smoke test'
  exec sudo -- bash "$0" "$@"
fi
package=$(realpath "$2")
[[ -f $package && ! -L $package ]] || die 'package must be a regular file'
log=$(mktemp)
api_pid=
cleanup() {
  local status=$?
  trap - EXIT
  [[ -z ${api_pid:-} ]] || kill -- "-$api_pid" >/dev/null 2>&1 || true
  [[ -z ${api_pid:-} ]] || wait "$api_pid" 2>/dev/null || true
  if [[ $status -ne 0 && -s $log ]]; then
    printf '%s\n' 'smoke-native-package: packaged API log follows:' >&2
    cat "$log" >&2
  fi
  rm -f "$log"
  exit "$status"
}
trap cleanup EXIT

as_root() { if [[ $EUID -eq 0 ]]; then "$@"; else sudo "$@"; fi; }

case "$package" in
  *.deb)
    command -v apt-get >/dev/null || die 'apt-get is required for a DEB smoke test'
    # Do not suppress maintainer-script failures: this is the package's normal
    # installation path, including dependency resolution, in an ephemeral host.
    as_root apt-get --yes install "$package" ;;
  *.rpm)
    command -v dnf >/dev/null || die 'dnf is required for an RPM smoke test'
    as_root dnf --assumeyes install "$package" ;;
  *) die 'package must end in .deb or .rpm' ;;
esac

node=/opt/vectorwarp/runtime/node/bin/node
api=/opt/vectorwarp/current/api/server.js
config=/etc/vectorwarp/config.yml
[[ -x $node && -f $api && -f $config ]] || die 'installed package lacks its private runtime, API, or config'
[[ $($node --version) == v24.21.0 ]] || die 'installed private Node runtime is not v24.21.0'
test -f /opt/vectorwarp/current/html/display/configuration/index.html
test -f /opt/vectorwarp/current/html/js/common.js
getent passwd vectorwarp-api >/dev/null || die 'package did not create vectorwarp-api'
getent group vectorwarp-config >/dev/null || die 'package did not create vectorwarp-config'
command -v runuser >/dev/null || die 'runuser is required for service-account smoke'
command -v setsid >/dev/null || die 'setsid is required for service-account smoke cleanup'
api_uid=$(id -u vectorwarp-api) || die 'package did not create a usable vectorwarp-api identity'
[[ $api_uid =~ ^[0-9]+$ ]] || die 'vectorwarp-api has an invalid uid'
as_root runuser --user vectorwarp-api --group vectorwarp-api --supp-group vectorwarp-config -- \
  test -r "$config" -a -w "$config" || die 'vectorwarp-api cannot read and write the installed config'
receiver_types=$(sed -n 's/^Environment="BLAH2_RECEIVER_TYPES=\(.*\)"$/\1/p' \
  /usr/lib/systemd/system/vectorwarp-api.service)
[[ -n $receiver_types ]] || die 'installed API service lacks receiver-type build metadata'

# This is a directly launched package-local API/static-web smoke process, never
# a system service; BLAH2_PREVIEW prevents external truth polling.
setsid runuser --user vectorwarp-api --group vectorwarp-api --supp-group vectorwarp-config -- \
  env BLAH2_PREVIEW=true BLAH2_SETUP_PORT=39080 BLAH2_RECEIVER_TYPES="$receiver_types" \
  "$node" "$api" "$config" >"$log" 2>&1 &
api_pid=$!
for _ in $(seq 1 300); do
  if curl --fail --silent --show-error http://127.0.0.1:39080/api/system/status >/dev/null 2>&1; then break; fi
  sleep 0.1
done
curl --fail --silent --show-error http://127.0.0.1:39080/api/system/status >/dev/null || {
  die 'package-local API did not become ready'; }
curl --fail --silent --show-error http://127.0.0.1:39080/api/config/capabilities | "$node" -e '
  let body=""; process.stdin.on("data", data => { body += data; });
  process.stdin.on("end", () => {
    const value = JSON.parse(body);
    if (value.setupRequired || !value.editable || value.configPath !== "/etc/vectorwarp/config.yml") process.exit(1);
  });
' || die 'packaged API did not read a writable installed config without setup recovery'
curl --fail --silent --show-error http://127.0.0.1:39080/display/configuration/ | grep -q renderConfiguration ||
  die 'package-local configuration page is missing its static UI'
curl --fail --silent --show-error http://127.0.0.1:39080/js/common.js >/dev/null ||
  die 'package-local common UI script is missing'
printf 'native package smoke passed: %s\n' "$(basename "$package")"
