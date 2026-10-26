#!/usr/bin/env bash
# Run inside one disposable native-architecture guest. Use separate clean and
# upgrade guests/overlays; this script deliberately never resets a VM or host.
set -euo pipefail
die() { printf 'guest-release-test: %s\n' "$*" >&2; exit 1; }
mode= source= candidate= previous= expected_id= expected_version= expected_arch=
while (($#)); do
  case "$1" in
    --mode|--source|--candidate|--previous|--expected-id|--expected-version|--expected-arch)
      (($# >= 2)) || die "missing value for $1"
      case "$1" in
        --mode) mode=$2;; --source) source=$2;; --candidate) candidate=$2;;
        --previous) previous=$2;; --expected-id) expected_id=$2;;
        --expected-version) expected_version=$2;; --expected-arch) expected_arch=$2;;
      esac
      shift 2 ;;
    *) die "unknown argument: $1" ;;
  esac
done
[[ $mode == clean || $mode == upgrade ]] || die 'mode must be clean or upgrade'
[[ -d $source && -f $candidate && -n $expected_id && -n $expected_version && -n $expected_arch ]] || die 'source, candidate and expected platform are required'
[[ $mode != upgrade || -f $previous ]] || die 'upgrade mode requires the previous published package'
. /etc/os-release
case "${ID:-}" in dragonos|dragonos-*) actual_id=dragonos;; ubuntu) [[ $expected_id == dragonos ]] && actual_id=dragonos || actual_id=ubuntu;; *) actual_id=${ID:-};; esac
[[ $actual_id == "$expected_id" && ${VERSION_ID:-} == "$expected_version" && $(uname -m) == "$expected_arch" ]] ||
  die "platform mismatch: ${ID:-}/${VERSION_ID:-}/$(uname -m)"

package_version() {
  if command -v dpkg-query >/dev/null; then dpkg-query -W -f='${Version}\n' vectorwarp
  else rpm -q --qf '%{VERSION}-%{RELEASE}\n' vectorwarp; fi
}
ecosystem() {
  if command -v dpkg-query >/dev/null; then
    dpkg-query -W -f='${binary:Package}\t${Version}\n' 2>/dev/null | grep -Ei '^(gnuradio|uhd-host|libuhd|sdrplay)' | sort || true
  else
    rpm -qa --qf '%{NAME}\t%{EVR}\n' | grep -Ei '^(gnuradio|uhd|sdrplay)' | sort || true
  fi
}
install_package() {
  phase=${2:-candidate}
  if [[ $1 == *.deb ]]; then
    if [[ $expected_id == dragonos && $phase == published ]]; then
      # v0.1.29 and older made stock uhd-host a hard dependency. DragonOS
      # users with its ABI-compatible vendor libuhd could only retain that
      # ecosystem by overriding this one metadata edge. The candidate must
      # remove the need for this exception (PR #92).
      sudo dpkg --ignore-depends=uhd-host --install "$1"
      return
    fi
    apt_options=(--yes)
    [[ $expected_id != dragonos ]] || apt_options+=(--no-install-recommends)
    sudo env DEBIAN_FRONTEND=noninteractive apt-get "${apt_options[@]}" install "$1"
  else sudo dnf --assumeyes --setopt=install_weak_deps=False install "$1"; fi
}

if [[ $mode == clean ]]; then
  [[ ! $(command -v dpkg-query >/dev/null && dpkg-query -W vectorwarp 2>/dev/null || rpm -q vectorwarp 2>/dev/null || true) ]] ||
    die 'clean guest already contains VectorWarp'
  [[ $expected_id != dragonos ]] || ecosystem >"${TMPDIR:-/tmp}/vectorwarp-ecosystem-before.tsv"
  "$source/script/build-native.sh" --backend all --gpu auto --build-dir "$source/build/vm-release" --jobs 2 --test
  install_package "$candidate"
  package_version
  sudo systemctl enable --now vectorwarp-api.service
  sudo systemctl is-active --quiet vectorwarp-api.service
  if [[ $expected_id == dragonos ]]; then
    ecosystem >"${TMPDIR:-/tmp}/vectorwarp-ecosystem-after.tsv"
    cmp "${TMPDIR:-/tmp}/vectorwarp-ecosystem-before.tsv" "${TMPDIR:-/tmp}/vectorwarp-ecosystem-after.tsv" ||
      die 'clean install changed the DragonOS GNU Radio/UHD/SDRplay ecosystem'
  fi
else
  install_package "$previous" published
  before=$(package_version)
  ecosystem >"${TMPDIR:-/tmp}/vectorwarp-ecosystem-before.tsv"
  printf '\n# upgrade-matrix-user-state\n' | sudo tee -a /etc/vectorwarp/config.yml >/dev/null
  config_before=$(sudo sha256sum /etc/vectorwarp/config.yml | awk '{print $1}')
  sudo test ! -e /etc/vectorwarp/config.yml.receiver-state.json || die 'published-package fixture unexpectedly has a receiver journal'
  sudo systemctl enable --now vectorwarp-api.service vectorwarp-receiver.socket
  sudo systemctl stop vectorwarp-processor.service vectorwarp-restart.service
  install_package "$candidate"
  after=$(package_version)
  [[ $after != "$before" ]] || die 'candidate did not replace the published package'
  if command -v dpkg >/dev/null; then
    dpkg --compare-versions "$after" gt "$before" || die 'candidate DEB is not newer than the published package'
  else
    python3 - "$before" "$after" <<'PY' || die 'candidate RPM is not newer than the published package'
import rpm, sys
def evr(value):
    version, release = value.split('-', 1)
    return ('0', version, release)
raise SystemExit(0 if rpm.labelCompare(evr(sys.argv[2]), evr(sys.argv[1])) > 0 else 1)
PY
  fi
  config_after=$(sudo sha256sum /etc/vectorwarp/config.yml | awk '{print $1}')
  [[ $config_after == "$config_before" ]] || die 'operator config changed across package upgrade'
  sudo systemctl is-active --quiet vectorwarp-api.service
  sudo systemctl is-active --quiet vectorwarp-receiver.socket
  ! sudo systemctl is-active --quiet vectorwarp-processor.service || die 'stopped processor was incorrectly activated'
  ! sudo systemctl is-active --quiet vectorwarp-restart.service || die 'stopped restart service was incorrectly activated'
  sudo systemctl start vectorwarp-config-guard.service
  sudo test -f /var/lib/vectorwarp-api/config-guard.json || die 'missing reconciliation diagnostic'
  sudo journalctl --no-pager -u vectorwarp-config-guard.service | grep -Fq \
    'processor stopped pending exact receiver reconciliation' || die 'missing clear reconciliation message'
  sudo python3 - <<'PY'
import json
value = json.load(open('/var/lib/vectorwarp-api/config-guard.json', encoding='utf-8'))
assert value['reason'] == 'receiver-journal-missing'
assert value['action'] == 'processor-stopped'
assert 'Settings Apply' in value['recovery']
print('legacy receiver state requires one explicit revision-guarded Settings Apply')
PY
  ecosystem >"${TMPDIR:-/tmp}/vectorwarp-ecosystem-after.tsv"
  cmp "${TMPDIR:-/tmp}/vectorwarp-ecosystem-before.tsv" "${TMPDIR:-/tmp}/vectorwarp-ecosystem-after.tsv" ||
    die 'upgrade changed the image GNU Radio/UHD/SDRplay ecosystem'
  printf 'upgraded %s -> %s without changing the SDR ecosystem\n' "$before" "$after"
fi
