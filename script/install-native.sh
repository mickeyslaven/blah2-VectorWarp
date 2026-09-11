#!/usr/bin/env bash
set -euo pipefail

SOURCE_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
ARTIFACT="$SOURCE_DIR/build/native/artifact"
PREFIX=/opt/vectorwarp
SYSCONFDIR=/etc/vectorwarp
DESTDIR=
WITH_SYSTEMD=true
DRY_RUN=false
PREFLIGHT_ONLY=false

usage() {
  cat <<'EOF'
Usage: script/install-native.sh [options]

Install a completed native artifact. This command never enables or starts units.

  --artifact PATH         Artifact made by build-native.sh
  --prefix PATH           Application prefix (default: /opt/vectorwarp)
  --sysconfdir PATH       Configuration directory (default: /etc/vectorwarp)
  --destdir PATH          Stage beneath a packaging root without host changes
  --no-systemd            Do not install users, units, tmpfiles or restart policy
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
    --no-systemd) WITH_SYSTEMD=false; shift ;;
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
case "$backend" in
  kraken) expected_receivers=Kraken; initial_config=config-kraken.yml ;;
  rspduo) expected_receivers=RspDuo,Kraken; initial_config=config.yml ;;
  usrp) expected_receivers=Usrp,Kraken; initial_config=config-usrp.yml ;;
  hackrf) expected_receivers=HackRF,Kraken; initial_config=config-hackrf.yml ;;
  all) expected_receivers=RspDuo,Usrp,HackRF,Kraken; initial_config=config.yml ;;
  *) die 'artifact has an invalid backend' ;;
esac

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

if $WITH_SYSTEMD; then
  [[ -f $ARTIFACT/config-examples/$initial_config ]] || die "artifact lacks $initial_config"
  for file in vectorwarp-api.service.in vectorwarp-processor.service.in vectorwarp-restart.service.in vectorwarp.sysusers vectorwarp.tmpfiles vectorwarp.sudoers.in; do
    [[ -f $ARTIFACT/systemd/$file ]] || die "systemd artifact is incomplete: $file"
  done
  [[ -x $ARTIFACT/libexec/vectorwarp-restart && -x $ARTIFACT/libexec/vectorwarp-wait-api.js ]] ||
    die 'restart helpers are missing'
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
$WITH_SYSTEMD && say 'systemd units will be installed but not enabled or started'
if $PREFLIGHT_ONLY; then say 'preflight passed'; exit 0; fi

render() {
  local input=$1 output=$2
  if $DRY_RUN; then
    printf '+ render %q -> %q\n' "$input" "$output"
  else
    sed -e "s|@PREFIX@|$PREFIX|g" -e "s|@SYSCONFDIR@|$SYSCONFDIR|g" \
      -e "s|@RECEIVER_TYPES@|$RECEIVER_TYPES|g" "$input" >"$output"
  fi
}

run install -d -m 0755 "$target_prefix/releases"
run cp -a "$ARTIFACT" "$release"
if [[ -z $DESTDIR && $EUID -eq 0 ]]; then run chown -R root:root "$release"; fi
run ln -sfn "releases/$build_id" "$target_prefix/current.next"
run mv -Tf "$target_prefix/current.next" "$target_prefix/current"

if $WITH_SYSTEMD; then
  unit_dir="$DESTDIR/usr/lib/systemd/system"
  sysusers_dir="$DESTDIR/usr/lib/sysusers.d"
  tmpfiles_dir="$DESTDIR/usr/lib/tmpfiles.d"
  sudoers_dir="$DESTDIR/etc/sudoers.d"
  run install -d -m 0755 "$unit_dir" "$sysusers_dir" "$tmpfiles_dir" "$sudoers_dir"
  if $DRY_RUN; then
    temporary=/tmp/vectorwarp-install-dry-run
  else
    temporary=$(mktemp -d)
    trap 'rm -rf "$temporary"' EXIT
  fi
  render "$ARTIFACT/systemd/vectorwarp-api.service.in" "$temporary/vectorwarp-api.service"
  render "$ARTIFACT/systemd/vectorwarp-processor.service.in" "$temporary/vectorwarp-processor.service"
  render "$ARTIFACT/systemd/vectorwarp-restart.service.in" "$temporary/vectorwarp-restart.service"
  render "$ARTIFACT/systemd/vectorwarp.sudoers.in" "$temporary/vectorwarp"
  run install -m 0644 "$temporary/vectorwarp-api.service" "$unit_dir/vectorwarp-api.service"
  run install -m 0644 "$temporary/vectorwarp-processor.service" "$unit_dir/vectorwarp-processor.service"
  run install -m 0644 "$temporary/vectorwarp-restart.service" "$unit_dir/vectorwarp-restart.service"
  run install -m 0644 "$ARTIFACT/systemd/vectorwarp.sysusers" "$sysusers_dir/vectorwarp.conf"
  run install -m 0644 "$ARTIFACT/systemd/vectorwarp.tmpfiles" "$tmpfiles_dir/vectorwarp.conf"
  if command -v visudo >/dev/null 2>&1 && ! $DRY_RUN; then visudo -cf "$temporary/vectorwarp"; fi
  run install -m 0440 "$temporary/vectorwarp" "$sudoers_dir/vectorwarp"
  run install -d -m 0755 "$target_prefix/libexec"
  render "$ARTIFACT/libexec/vectorwarp-restart" "$temporary/vectorwarp-restart"
  run install -m 0755 "$temporary/vectorwarp-restart" "$target_prefix/libexec/vectorwarp-restart"
  run install -m 0755 "$ARTIFACT/libexec/vectorwarp-wait-api.js" "$target_prefix/libexec/vectorwarp-wait-api.js"
  if [[ -z $DESTDIR && $EUID -eq 0 ]]; then run chown -R root:root "$target_prefix/libexec"; fi
  if [[ -z $DESTDIR ]]; then
    run systemd-sysusers /usr/lib/sysusers.d/vectorwarp.conf
    run systemd-tmpfiles --create /usr/lib/tmpfiles.d/vectorwarp.conf
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
say 'installation complete; no service was enabled or started'
say "after review, an administrator may run: systemctl enable --now vectorwarp-api.service vectorwarp-processor.service"
