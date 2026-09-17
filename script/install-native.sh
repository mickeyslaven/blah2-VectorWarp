#!/usr/bin/env bash
set -euo pipefail

SOURCE_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
ARTIFACT="$SOURCE_DIR/build/native/artifact"
PREFIX=/opt/vectorwarp
SYSCONFDIR=/etc/vectorwarp
DESTDIR=
TARGET_DISTRO=
WITH_SYSTEMD=true
SETUP_PI_GPU=false
DRY_RUN=false
PREFLIGHT_ONLY=false

usage() {
  cat <<'EOF'
Usage: script/install-native.sh [options]

Install a completed native artifact without enabling or starting VectorWarp.
A first install with the local RSPduo kit can start an already-installed SDRplay API service.

  --artifact PATH         Artifact made by build-native.sh
  --prefix PATH           Application prefix (default: /opt/vectorwarp)
  --sysconfdir PATH       Configuration directory (default: /etc/vectorwarp)
  --destdir PATH          Stage beneath a packaging root without host changes
  --target-distro NAME    Required with --destdir: ubuntu, debian or fedora
  --no-systemd            Do not install users, units, tmpfiles or restart policy
  --setup-pi-gpu          After install, offer a signed native Pi Mesa transaction
  --preflight             Validate inputs and destinations, then stop
  --dry-run               Print planned operations without changing files
  -h, --help              Show this help

Existing SYSCONFDIR/config.yml is never overwritten, remapped or replaced.
EOF
}

die() { printf 'install-native: %s\n' "$*" >&2; exit 1; }
say() { printf 'install-native: %s\n' "$*"; }
quote_command() { printf ' %q' "$@"; printf '\n'; }
run() { if $DRY_RUN; then printf '+'; quote_command "$@"; else "$@"; fi; }

while (($#)); do
  case "$1" in
    --artifact) (($# >= 2)) || die '--artifact needs a value'; ARTIFACT=$2; shift 2 ;;
    --prefix) (($# >= 2)) || die '--prefix needs a value'; PREFIX=$2; shift 2 ;;
    --sysconfdir) (($# >= 2)) || die '--sysconfdir needs a value'; SYSCONFDIR=$2; shift 2 ;;
    --destdir) (($# >= 2)) || die '--destdir needs a value'; DESTDIR=$2; shift 2 ;;
    --target-distro) (($# >= 2)) || die '--target-distro needs a value'; TARGET_DISTRO=$2; shift 2 ;;
    --no-systemd) WITH_SYSTEMD=false; shift ;;
    --setup-pi-gpu) SETUP_PI_GPU=true; shift ;;
    --preflight) PREFLIGHT_ONLY=true; shift ;;
    --dry-run) DRY_RUN=true; shift ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown option: $1" ;;
  esac
done

ARTIFACT=$(realpath -m "$ARTIFACT")
[[ $PREFIX == /* && $PREFIX != / ]] || die '--prefix must be an absolute non-root path'
[[ $SYSCONFDIR == /* && $SYSCONFDIR != / ]] || die '--sysconfdir must be an absolute non-root path'
[[ $PREFIX =~ ^[-A-Za-z0-9_./+]+$ && $SYSCONFDIR =~ ^[-A-Za-z0-9_./+]+$ ]] ||
  die 'prefix and sysconfdir contain unsupported characters'
if [[ -n $DESTDIR ]]; then
  [[ $DESTDIR == /* && $DESTDIR != / ]] || die '--destdir must be an absolute non-root path'
  DESTDIR=${DESTDIR%/}
  [[ $TARGET_DISTRO == ubuntu || $TARGET_DISTRO == debian || $TARGET_DISTRO == fedora ]] ||
    die '--destdir requires --target-distro ubuntu, debian or fedora'
else
  [[ -z $TARGET_DISTRO ]] || die '--target-distro is only valid with --destdir'
  [[ -r /etc/os-release ]] || die 'cannot identify host distribution'
  TARGET_DISTRO=$(. /etc/os-release; printf '%s' "$ID")
fi
if $SETUP_PI_GPU; then
  [[ -z $DESTDIR ]] || die '--setup-pi-gpu is forbidden with --destdir; staging never probes or changes the host GPU'
  $WITH_SYSTEMD || die '--setup-pi-gpu requires the installed integration helpers'
fi
for file in .vectorwarp-build bin/blah2 api/server.js html/index.html config-examples/config.yml; do
  [[ -e $ARTIFACT/$file ]] || die "artifact is incomplete: $file"
done
[[ -x $ARTIFACT/bin/blah2 ]] || die 'artifact processor is not executable'
read_manifest_field() {
  local field=$1 output=$2 count value
  count=$(grep -c "^${field}=" "$ARTIFACT/.vectorwarp-build" || true)
  [[ $count == 1 ]] || die "artifact manifest must contain exactly one $field"
  value=$(sed -n "s/^${field}=//p" "$ARTIFACT/.vectorwarp-build")
  printf -v "$output" '%s' "$value"
}

read_manifest_field build_id build_id
[[ $build_id =~ ^[A-Za-z0-9._:-]+$ ]] || die 'artifact has an invalid build_id'
read_manifest_field backend backend
test_only=false
test_only_count=$(grep -c '^test_only=' "$ARTIFACT/.vectorwarp-build" || true)
((test_only_count <= 1)) || die 'artifact manifest has duplicate test_only fields'
if ((test_only_count == 1)); then
  read_manifest_field test_only test_only
  [[ $test_only == true || $test_only == false ]] || die 'artifact has an invalid test_only marker'
fi
case "$backend" in
  open-test)
    expected_receivers=Usrp,HackRF,Kraken; initial_config=config-usrp.yml
    [[ $test_only == true ]] || die 'open-test artifacts must be explicitly marked test-only' ;;
  kraken) expected_receivers=Kraken; initial_config=config-kraken.yml ;;
  rspduo) expected_receivers=RspDuo,Kraken; initial_config=config.yml ;;
  usrp) expected_receivers=Usrp,Kraken; initial_config=config-usrp.yml ;;
  hackrf) expected_receivers=HackRF,Kraken; initial_config=config-hackrf.yml ;;
  all) expected_receivers=Usrp,HackRF,Kraken; initial_config=config.yml ;;
  *) die 'artifact has an invalid backend' ;;
esac
[[ $backend == open-test || $test_only == false ]] || die 'only open-test artifacts may be marked test-only'

# New artifacts state their exact live receiver set. Accept the two historical
# backend manifests without this field, but never infer or pass through unknown
# receiver names from a manifest that does provide it.
compiled_count=$(grep -c '^compiled_receivers=' "$ARTIFACT/.vectorwarp-build" || true)
((compiled_count <= 1)) || die 'artifact manifest has duplicate compiled_receivers fields'
if ((compiled_count == 0)); then
  [[ $backend == kraken || $backend == all ]] ||
    die 'selected-backend artifact lacks compiled_receivers'
  RECEIVER_TYPES=$expected_receivers
else
  read_manifest_field compiled_receivers compiled_receivers
  [[ $compiled_receivers =~ ^(RspDuo|Usrp|HackRF|Kraken)(,(RspDuo|Usrp|HackRF|Kraken))*$ ]] ||
    die 'artifact has an invalid compiled_receivers list'
  seen_rspduo=false; seen_usrp=false; seen_hackrf=false; seen_kraken=false
  IFS=, read -r -a receiver_items <<<"$compiled_receivers"
  for receiver in "${receiver_items[@]}"; do
    case "$receiver" in
      RspDuo) $seen_rspduo && die 'artifact has duplicate compiled receiver RspDuo'; seen_rspduo=true ;;
      Usrp) $seen_usrp && die 'artifact has duplicate compiled receiver Usrp'; seen_usrp=true ;;
      HackRF) $seen_hackrf && die 'artifact has duplicate compiled receiver HackRF'; seen_hackrf=true ;;
      Kraken) $seen_kraken && die 'artifact has duplicate compiled receiver Kraken'; seen_kraken=true ;;
      *) die 'artifact has an unknown compiled receiver' ;;
    esac
  done
  RECEIVER_TYPES=
  for receiver in RspDuo Usrp HackRF Kraken; do
    case "$receiver" in
      RspDuo) present=$seen_rspduo ;; Usrp) present=$seen_usrp ;;
      HackRF) present=$seen_hackrf ;; Kraken) present=$seen_kraken ;;
    esac
    if $present; then
      [[ -z $RECEIVER_TYPES ]] || RECEIVER_TYPES+=,
      RECEIVER_TYPES+=$receiver
    fi
  done
  [[ $RECEIVER_TYPES == "$expected_receivers" ]] ||
    die 'artifact backend and compiled_receivers disagree'
fi

# A local source kit is deliberately separate from compiled live adapters.
# Historical artifacts omit this field; new stable all builds require exactly
# the one reviewed RSPduo kit and do not advertise it as compiled.
local_build_count=$(grep -c '^local_build_receivers=' "$ARTIFACT/.vectorwarp-build" || true)
((local_build_count <= 1)) || die 'artifact manifest has duplicate local_build_receivers fields'
LOCAL_BUILD_RECEIVER_TYPES=
LOCAL_BUILD_ENABLED=false
if ((local_build_count == 1)); then
  read_manifest_field local_build_receivers LOCAL_BUILD_RECEIVER_TYPES
  [[ -z $LOCAL_BUILD_RECEIVER_TYPES || $LOCAL_BUILD_RECEIVER_TYPES == RspDuo ]] ||
    die 'artifact has an invalid local_build_receivers list'
fi
[[ -z $LOCAL_BUILD_RECEIVER_TYPES ]] || LOCAL_BUILD_ENABLED=true
if [[ $backend == all && $compiled_count -gt 0 ]]; then
  [[ $RECEIVER_TYPES == Usrp,HackRF,Kraken && $LOCAL_BUILD_RECEIVER_TYPES == RspDuo ]] ||
    die 'stable artifact must have three compiled receivers and the RSPduo local build kit'
  [[ -f $ARTIFACT/receiver-source/rspduo/kit.json && -x $ARTIFACT/libexec/vectorwarp-build-sdrplay.py ]] ||
    die 'stable artifact lacks the local RSPduo build kit or helper'
  [[ -f $ARTIFACT/systemd/vectorwarp-sdrplay-build.service.in ]] ||
    die 'stable artifact lacks the local RSPduo build service template'
fi

if $WITH_SYSTEMD; then
  [[ -f $ARTIFACT/config-examples/$initial_config ]] || die "artifact lacks $initial_config"
  if [[ $TARGET_DISTRO == fedora && $RECEIVER_TYPES == *HackRF* ]]; then
    [[ -f $ARTIFACT/systemd/72-vectorwarp-hackrf.rules ]] ||
      die 'Fedora HackRF rule is missing from artifact'
  fi
  for file in vectorwarp-api.service.in vectorwarp-processor.service.in vectorwarp-restart.service.in vectorwarp.sysusers vectorwarp.tmpfiles vectorwarp.sudoers.in; do
    [[ -f $ARTIFACT/systemd/$file ]] || die "systemd artifact is incomplete: $file"
  done
  [[ -x $ARTIFACT/libexec/vectorwarp-restart && -x $ARTIFACT/libexec/vectorwarp-wait-api.js &&
     -x $ARTIFACT/libexec/vectorwarp-activate-web ]] ||
    die 'restart helpers are missing'
  [[ -x $ARTIFACT/libexec/vectorwarp ]] || die 'launcher is missing'
  [[ ! -L $DESTDIR/usr/bin && ! -L $DESTDIR/usr/bin/vectorwarp ]] ||
    die 'launcher install path must not be a symlink'
  if [[ -f $ARTIFACT/libexec/vectorwarp-receiver-helper ]]; then
    [[ -f $ARTIFACT/libexec/vectorwarp-receiver-apt.py ]] || die 'receiver package adapter is missing'
    [[ -f $ARTIFACT/libexec/vectorwarp-receiver-dnf.py ]] || die 'receiver DNF adapter is missing'
    for file in vectorwarp-receiver.service.in vectorwarp-receiver.socket vectorwarp-receiver-policy.json.in; do
      [[ -f $ARTIFACT/systemd/$file ]] || die "receiver management artifact is incomplete: $file"
    done
    management_policy_dir="$DESTDIR/etc/vectorwarp-management"
    [[ ! -L $management_policy_dir && ! -L $management_policy_dir/receivers.json ]] ||
      die 'receiver management policy must not be a symlink'
    if [[ -z $DESTDIR && -d $management_policy_dir ]]; then
      policy_owner=$(stat -c %u "$management_policy_dir")
      policy_mode=$(stat -c %a "$management_policy_dir")
      [[ $policy_owner == 0 && $((8#$policy_mode & 0022)) -eq 0 ]] ||
        die 'receiver management policy directory must be root-owned and not writable by other users'
    fi
  fi
  if [[ -z $DESTDIR && $EUID -ne 0 ]]; then die 'system integration requires root (or use --destdir)'; fi
fi
if [[ -z $DESTDIR ]]; then
  [[ -x /usr/bin/node ]] || die 'native services require Node.js at /usr/bin/node'
  node_major=$(/usr/bin/node -p 'Number(process.versions.node.split(".")[0])')
  ((node_major >= 22)) || die 'native services require Node.js 22 or newer at /usr/bin/node'
  if ldd "$ARTIFACT/bin/blah2" 2>&1 | grep -q 'not found'; then
    ldd "$ARTIFACT/bin/blah2" >&2
    die 'processor runtime libraries are missing; install SDK libraries and run ldconfig'
  fi
  for runtime in "$ARTIFACT/bin/blah2-gpu-worker" "$ARTIFACT/bin/blah2-gpu-vulkan.so"; do
    if [[ -e $runtime ]] && ldd "$runtime" 2>&1 | grep -q 'not found'; then
      ldd "$runtime" >&2
      die "runtime libraries are missing for $(basename "$runtime")"
    fi
  done
fi

target_prefix="$DESTDIR$PREFIX"
target_sysconf="$DESTDIR$SYSCONFDIR"
release="$target_prefix/releases/$build_id"
had_current_link=false
[[ -L $target_prefix/current ]] && had_current_link=true
for path_to_check in "$target_prefix" "$target_prefix/releases" "$target_prefix/libexec" "$target_sysconf"; do
  [[ ! -L $path_to_check ]] || die "refusing to follow installation symlink: $path_to_check"
done
if [[ -e $target_prefix/current && ! -L $target_prefix/current ]]; then
  die "current release path is not a symlink: $target_prefix/current"
fi
if [[ -L $target_sysconf/config.yml ]]; then
  die "refusing symlink configuration (left unchanged): $target_sysconf/config.yml"
fi
if [[ -z $DESTDIR && $EUID -eq 0 && -d $target_prefix ]]; then
  prefix_owner=$(stat -c %u "$target_prefix")
  prefix_mode=$(stat -c %a "$target_prefix")
  [[ $prefix_owner == 0 && $((8#$prefix_mode & 0022)) -eq 0 ]] ||
    die "existing prefix must be root-owned and not group/world-writable: $target_prefix"
fi
[[ ! -e $release ]] || die "release already exists: $release"

say "artifact: $ARTIFACT"
say "release: $release"
say "config: $target_sysconf/config.yml (preserved when present)"
$WITH_SYSTEMD && say 'VectorWarp systemd units will be installed but not enabled or started'
if $PREFLIGHT_ONLY; then say 'preflight passed'; exit 0; fi

render() {
  local input=$1 output=$2
  if $DRY_RUN; then
    printf '+ render %q -> %q\n' "$input" "$output"
  else
    sed -e "s|@PREFIX@|$PREFIX|g" -e "s|@SYSCONFDIR@|$SYSCONFDIR|g" \
      -e "s|@RECEIVER_TYPES@|$RECEIVER_TYPES|g" \
      -e "s|@LOCAL_BUILD_RECEIVER_TYPES@|$LOCAL_BUILD_RECEIVER_TYPES|g" \
      -e "s|@LOCAL_BUILD_ENABLED@|$LOCAL_BUILD_ENABLED|g" "$input" >"$output"
  fi
}

run install -d -m 0755 "$target_prefix/releases"
run cp -a "$ARTIFACT" "$release"
# Artifacts may have been built from a collaborative umask.  This applies only
# to the newly copied, root-owned release code; saved configuration is untouched.
run chmod -R go-w "$release"
if [[ -z $DESTDIR && $EUID -eq 0 ]]; then run chown -R root:root "$release"; fi
run ln -sfn "releases/$build_id" "$target_prefix/current.next"
run mv -Tf "$target_prefix/current.next" "$target_prefix/current"

if $WITH_SYSTEMD; then
  unit_dir="$DESTDIR/usr/lib/systemd/system"
  udev_rule_dir="$DESTDIR/usr/lib/udev/rules.d"
  sysusers_dir="$DESTDIR/usr/lib/sysusers.d"
  tmpfiles_dir="$DESTDIR/usr/lib/tmpfiles.d"
  sudoers_dir="$DESTDIR/etc/sudoers.d"
  run install -d -m 0755 "$unit_dir" "$udev_rule_dir" "$sysusers_dir" "$tmpfiles_dir" "$sudoers_dir"
  if $DRY_RUN; then
    temporary=/tmp/vectorwarp-install-dry-run
  else
    temporary=$(mktemp -d)
    trap 'rm -rf "$temporary"' EXIT
  fi
  render "$ARTIFACT/libexec/vectorwarp" "$temporary/vectorwarp-launcher"
  run install -d -m 0755 "$DESTDIR/usr/bin"
  run install -m 0755 "$temporary/vectorwarp-launcher" "$DESTDIR/usr/bin/vectorwarp"
  render "$ARTIFACT/systemd/vectorwarp-api.service.in" "$temporary/vectorwarp-api.service"
  render "$ARTIFACT/systemd/vectorwarp-processor.service.in" "$temporary/vectorwarp-processor.service"
  render "$ARTIFACT/systemd/vectorwarp-restart.service.in" "$temporary/vectorwarp-restart.service"
  render "$ARTIFACT/systemd/vectorwarp.sudoers.in" "$temporary/vectorwarp"
  run install -m 0644 "$temporary/vectorwarp-api.service" "$unit_dir/vectorwarp-api.service"
  run install -m 0644 "$temporary/vectorwarp-processor.service" "$unit_dir/vectorwarp-processor.service"
  run install -m 0644 "$temporary/vectorwarp-restart.service" "$unit_dir/vectorwarp-restart.service"
  run install -m 0644 "$ARTIFACT/systemd/vectorwarp.sysusers" "$sysusers_dir/vectorwarp.conf"
  run install -m 0644 "$ARTIFACT/systemd/vectorwarp.tmpfiles" "$tmpfiles_dir/vectorwarp.conf"
  # Fedora's packaged HackRF rule has no group; Debian/Ubuntu already grant
  # plugdev. Never override that existing distro group with a second rule.
  if [[ $TARGET_DISTRO == fedora && $RECEIVER_TYPES == *HackRF* ]]; then
    run install -m 0644 "$ARTIFACT/systemd/72-vectorwarp-hackrf.rules" "$udev_rule_dir/72-vectorwarp-hackrf.rules"
  fi
  if command -v visudo >/dev/null 2>&1 && ! $DRY_RUN; then visudo -cf "$temporary/vectorwarp"; fi
  run install -d -m 0755 "$target_prefix/libexec"
  if [[ -f $ARTIFACT/libexec/vectorwarp-sudoers-migrate ]]; then
    run install -m 0755 "$ARTIFACT/libexec/vectorwarp-sudoers-migrate" "$target_prefix/libexec/vectorwarp-sudoers-migrate"
  fi
  if [[ -e $sudoers_dir/vectorwarp || -L $sudoers_dir/vectorwarp ]]; then
    if [[ -z $DESTDIR && $EUID -eq 0 && $DRY_RUN == false && -x $target_prefix/libexec/vectorwarp-sudoers-migrate ]]; then
      /usr/bin/python3 -I "$target_prefix/libexec/vectorwarp-sudoers-migrate" ||
        say 'existing VectorWarp sudoers needs administrator review; preserved without replacement'
    else
      say 'existing VectorWarp sudoers preserved; review obsolete API grants locally'
    fi
  else
    run install -m 0440 "$temporary/vectorwarp" "$sudoers_dir/vectorwarp"
  fi
  render "$ARTIFACT/libexec/vectorwarp-restart" "$temporary/vectorwarp-restart"
  run install -m 0755 "$temporary/vectorwarp-restart" "$target_prefix/libexec/vectorwarp-restart"
  run install -m 0755 "$ARTIFACT/libexec/vectorwarp-activate-web" "$target_prefix/libexec/vectorwarp-activate-web"
  run install -m 0755 "$ARTIFACT/libexec/vectorwarp-wait-api.js" "$target_prefix/libexec/vectorwarp-wait-api.js"
  if [[ -f $ARTIFACT/libexec/vectorwarp-gpu-setup ]]; then
    run install -m 0755 "$ARTIFACT/libexec/vectorwarp-gpu-setup" "$target_prefix/libexec/vectorwarp-gpu-setup"
  fi
  if [[ -f $ARTIFACT/libexec/vectorwarp-sdrplay-service.py ]]; then
    render "$ARTIFACT/libexec/vectorwarp-sdrplay-service.py" "$temporary/vectorwarp-sdrplay-service"
    run install -m 0755 "$temporary/vectorwarp-sdrplay-service" "$target_prefix/libexec/vectorwarp-sdrplay-service"
  fi
  if [[ -f $ARTIFACT/libexec/vectorwarp-prepare-sdrplay.js ]]; then
    run install -m 0644 "$ARTIFACT/libexec/vectorwarp-prepare-sdrplay.js" "$target_prefix/libexec/vectorwarp-prepare-sdrplay.js"
  fi
  if [[ $LOCAL_BUILD_ENABLED == true && -f $ARTIFACT/libexec/vectorwarp-build-sdrplay.py ]]; then
    render "$ARTIFACT/libexec/vectorwarp-build-sdrplay.py" "$temporary/vectorwarp-build-sdrplay"
    run install -m 0755 "$temporary/vectorwarp-build-sdrplay" "$target_prefix/libexec/vectorwarp-build-sdrplay"
  fi
  if [[ $LOCAL_BUILD_ENABLED == true && -f $ARTIFACT/systemd/vectorwarp-sdrplay-build.service.in ]]; then
    render "$ARTIFACT/systemd/vectorwarp-sdrplay-build.service.in" "$temporary/vectorwarp-sdrplay-build.service"
    run install -m 0644 "$temporary/vectorwarp-sdrplay-build.service" "$unit_dir/vectorwarp-sdrplay-build.service"
  fi
  if [[ -f $ARTIFACT/libexec/vectorwarp-receiver-helper ]]; then
    render "$ARTIFACT/systemd/vectorwarp-receiver.service.in" "$temporary/vectorwarp-receiver.service"
    render "$ARTIFACT/systemd/vectorwarp-receiver-policy.json.in" "$temporary/receivers.json"
    run install -m 0755 "$ARTIFACT/libexec/vectorwarp-receiver-helper" "$target_prefix/libexec/vectorwarp-receiver-helper"
    run install -m 0755 "$ARTIFACT/libexec/vectorwarp-receiver-apt.py" "$target_prefix/libexec/vectorwarp-receiver-apt.py"
    run install -m 0755 "$ARTIFACT/libexec/vectorwarp-receiver-dnf.py" "$target_prefix/libexec/vectorwarp-receiver-dnf.py"
    run install -m 0644 "$temporary/vectorwarp-receiver.service" "$unit_dir/vectorwarp-receiver.service"
    run install -m 0644 "$ARTIFACT/systemd/vectorwarp-receiver.socket" "$unit_dir/vectorwarp-receiver.socket"
    # The API can write its config directory, so privileged policy must live
    # outside it. Existing reviewed policy is preserved on every upgrade.
    management_policy_dir="$DESTDIR/etc/vectorwarp-management"
    [[ ! -L $management_policy_dir && ! -L $management_policy_dir/receivers.json ]] ||
      die 'receiver management policy must not be a symlink'
    run install -d -m 0755 "$management_policy_dir"
    if [[ ! -e $management_policy_dir/receivers.json ]]; then
      run install -m 0644 "$temporary/receivers.json" "$management_policy_dir/receivers.json"
    fi
    # Socket activation enables read-only discovery. Receiver software actions
    # need reviewed policy and a one-use local grant; the fixed restart request
    # is separately limited to the API account by peer credentials.
    run install -d -m 0755 "$unit_dir/vectorwarp-api.service.wants"
    run ln -sfn ../vectorwarp-receiver.socket "$unit_dir/vectorwarp-api.service.wants/vectorwarp-receiver.socket"
  fi
  if [[ -z $DESTDIR && $EUID -eq 0 ]]; then run chown -R root:root "$target_prefix/libexec"; fi
  if [[ -z $DESTDIR ]]; then
    run systemd-sysusers /usr/lib/sysusers.d/vectorwarp.conf
    run systemd-tmpfiles --create /usr/lib/tmpfiles.d/vectorwarp.conf
    if [[ $RECEIVER_TYPES == *HackRF* && $TARGET_DISTRO != fedora ]]; then
      if getent group plugdev >/dev/null && getent passwd vectorwarp >/dev/null; then
        run usermod --append --groups plugdev vectorwarp
      else
        say 'HackRF access needs local review: expected plugdev group and vectorwarp account'
      fi
    fi
    if command -v udevadm >/dev/null 2>&1; then
      run udevadm control --reload || say 'udev rule reload needs local review; reconnect HackRF after reloading rules'
    fi
    if [[ -x $ARTIFACT/libexec/vectorwarp-gpu-setup ]]; then
      run /usr/bin/python3 -I "$target_prefix/libexec/vectorwarp-gpu-setup" --configure-service-access ||
        say 'GPU access needs local review; run vectorwarp-gpu-setup --enable-service-access'
    fi
  fi
fi

if [[ ! -d $target_sysconf ]]; then
  run install -d -m 0770 "$target_sysconf"
  if $WITH_SYSTEMD && [[ -z $DESTDIR ]]; then run chown root:vectorwarp-config "$target_sysconf"; fi
fi
if [[ ! -e $target_sysconf/config.yml ]]; then
  run install -m 0660 "$ARTIFACT/config-examples/$initial_config" "$target_sysconf/config.yml"
  if $WITH_SYSTEMD && [[ -z $DESTDIR ]]; then
    run chown vectorwarp-api:vectorwarp-config "$target_sysconf/config.yml"
  fi
  say 'installed the first-run example; review every receiver, frequency, site and path value'
else
  say 'kept the existing configuration byte-for-byte'
fi

if $WITH_SYSTEMD && [[ -z $DESTDIR ]]; then run systemctl daemon-reload; fi
say 'installation complete; no VectorWarp service was enabled or started'
# A first real native install may ask the fixed local SDRplay helper to start
# an already-installed vendor service. It never downloads vendor software,
# accepts a license, enables boot, or starts VectorWarp services. The helper
# independently verifies either a compiled RSPduo adapter or the local-kit
# declaration and local policy.
if $WITH_SYSTEMD && [[ -z $DESTDIR && $EUID -eq 0 && $DRY_RUN == false && $PREFLIGHT_ONLY == false &&
    $had_current_link == false && -d /run/systemd/system &&
    ( $RECEIVER_TYPES == *RspDuo* || $LOCAL_BUILD_RECEIVER_TYPES == *RspDuo* ) &&
    -x $target_prefix/libexec/vectorwarp-sdrplay-service ]]; then
  /usr/bin/python3 -I "$target_prefix/libexec/vectorwarp-sdrplay-service" install ||
    printf '%s\n' 'SDRplay was not prepared; install its Hardware API yourself from https://sdrplay.com/hardware-api/ and recheck in Settings.' >&2
fi
if $SETUP_PI_GPU; then
  run "$target_prefix/libexec/vectorwarp-gpu-setup" --install-driver ||
    die 'application installed; Pi driver setup was cancelled or unavailable; no GPU acceptance was inferred'
elif $WITH_SYSTEMD; then
  say "Pi GPU setup (read-only): $PREFIX/libexec/vectorwarp-gpu-setup --status"
fi
say "after review, an administrator may run: systemctl enable --now vectorwarp-api.service vectorwarp-processor.service"
