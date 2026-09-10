#!/usr/bin/env bash
set -euo pipefail

REPOSITORY_URL=https://mickeyslaven.github.io/blah2-VectorWarp
EXPECTED_FINGERPRINT=@SIGNING_FINGERPRINT@
KEY_FILE=
START_WEB=false
DRY_RUN=false
PREFLIGHT_ONLY=false
DETECT_PLATFORM_ONLY=false

usage() {
  cat <<'EOF'
Usage: install-release.sh [options]

Add the signed VectorWarp package repository and install VectorWarp.

  --start-web             Explicitly enable and start only the web API
  --repo-url HTTPS_URL    Override the repository base (maintainer/testing)
  --fingerprint HEX       Expected 40-hex signing-key fingerprint
  --key-file PATH         Verify this public key instead of downloading it
  --preflight             Verify platform, tools and signing key; change nothing
  --detect-platform       Show the matching distribution package; change nothing
  --dry-run               Print package/repository operations; change nothing
  -h, --help              Show this help

The radar processor is never enabled or started by this script. Review
/etc/vectorwarp/config.yml before starting vectorwarp-processor.service.
EOF
}

die() { printf 'install-release: %s\n' "$*" >&2; exit 1; }
say() { printf 'install-release: %s\n' "$*"; }
quote_command() { printf ' %q' "$@"; printf '\n'; }
run() { if $DRY_RUN; then printf '+'; quote_command "$@"; else "$@"; fi; }
need_command() { command -v "$1" >/dev/null 2>&1 || die "missing command: $1"; }

# Pure platform selection, also exercised by the offline installer tests.
# DragonOS uses its Ubuntu base, never its independent ISO release number.
detect_platform() {
  local os_id=$1 os_version=$2 ubuntu_codename=$3 version_codename=$4 machine=$5 dpkg_arch=${6:-} variant_id=${7:-}
  local base_version= base_codename= dragon=false pi=false family=ubuntu
  case "$machine" in
    x86_64) deb_arch=amd64; rpm_arch=x86_64 ;;
    aarch64|arm64) deb_arch=arm64; rpm_arch=aarch64 ;;
    *) die "unsupported architecture: $machine" ;;
  esac
  case "$os_id" in
    fedora)
      [[ $os_version == 44 ]] || die "Fedora $os_version is not packaged; use Fedora 44 or build from source"
      manager=dnf; codename=; package_arch=$rpm_arch
      platform_description="Fedora 44 ($package_arch RPM)"; return ;;
    ubuntu) base_version=$os_version ;;
    debian)
      family=debian; base_version=$os_version
      case "$variant_id" in raspbian|raspios) pi=true ;; esac ;;
    raspbian)
      family=debian; pi=true
      [[ $os_version == 13 && $version_codename == trixie ]] ||
        die 'Raspberry Pi OS is packaged only for 64-bit Trixie (Debian 13)'
      base_version=13 ;;
    dragonos|dragonos-*)
      dragon=true
      base_codename=${ubuntu_codename:-$version_codename}
      case "$base_codename" in
        jammy) base_version=22.04 ;;
        noble) base_version=24.04 ;;
        resolute) base_version=26.04 ;;
        '')
          # Some images retain Ubuntu's VERSION_ID without a codename.
          case "$os_version" in
            22.04|24.04|26.04) base_version=$os_version ;;
            *) die 'cannot identify the DragonOS Ubuntu base; expected jammy, noble or resolute in /etc/os-release' ;;
          esac ;;
        *) die "DragonOS base '$base_codename' is not packaged; supported bases are Ubuntu 22.04, 24.04 and 26.04" ;;
      esac
      case "$os_version" in
        22.04|24.04|26.04)
          [[ $os_version == "$base_version" ]] || die 'conflicting DragonOS Ubuntu version and codename in /etc/os-release' ;;
      esac ;;
    *) die "distribution '$os_id' is not packaged; supported systems are Fedora 44, Debian 13, Ubuntu 22.04/24.04/26.04, matching DragonOS editions and 64-bit Raspberry Pi OS Trixie" ;;
  esac
  if $pi; then
    [[ $machine == aarch64 || $machine == arm64 ]] ||
      die 'Raspberry Pi OS packages require a 64-bit arm64 userspace'
  fi
  if [[ $family == ubuntu ]]; then
    case "$base_version" in
      22.04) codename=jammy ;;
      24.04) codename=noble ;;
      26.04) codename=resolute ;;
      *) die "Ubuntu $base_version is not packaged; supported versions are 22.04, 24.04 and 26.04" ;;
    esac
    [[ -z $ubuntu_codename || $ubuntu_codename == "$codename" ]] ||
      die 'conflicting Ubuntu version and UBUNTU_CODENAME in /etc/os-release'
    if $dragon && [[ -n $ubuntu_codename ]]; then
      # DragonOS may use its own VERSION_CODENAME. Only a second recognizable
      # Ubuntu base can contradict its authoritative UBUNTU_CODENAME.
      case "$version_codename" in
        jammy|noble|resolute|focal)
          [[ $version_codename == "$codename" ]] ||
            die 'conflicting Ubuntu base and VERSION_CODENAME in /etc/os-release' ;;
      esac
    else
      [[ -z $version_codename || $version_codename == "$codename" ]] ||
        die 'conflicting Ubuntu version and VERSION_CODENAME in /etc/os-release'
    fi
  else
    [[ $base_version == 13 && $version_codename == trixie && -z $ubuntu_codename ]] ||
      die 'Debian packages require unambiguous Debian 13 (Trixie) metadata'
    codename=trixie
  fi
  [[ $dpkg_arch == "$deb_arch" ]] ||
    die "package-manager architecture '$dpkg_arch' does not match supported $deb_arch userspace"
  manager=apt; package_arch=$deb_arch
  platform_description="Ubuntu $base_version / $codename ($package_arch DEB)"
  if [[ $family == debian ]]; then platform_description="Debian $base_version / $codename ($package_arch DEB)"; fi
  if $dragon; then platform_description="DragonOS using $platform_description"; fi
  if $pi; then platform_description="Raspberry Pi OS using $platform_description"; fi
}

# Sourcing exposes only the detector to bounded tests, never installation.
if [[ ${BASH_SOURCE[0]} != "$0" ]]; then return 0; fi

while (($#)); do
  case "$1" in
    --start-web) START_WEB=true; shift ;;
    --repo-url) (($# >= 2)) || die '--repo-url needs a value'; REPOSITORY_URL=${2%/}; shift 2 ;;
    --fingerprint) (($# >= 2)) || die '--fingerprint needs a value'; EXPECTED_FINGERPRINT=$2; shift 2 ;;
    --key-file) (($# >= 2)) || die '--key-file needs a value'; KEY_FILE=$2; shift 2 ;;
    --preflight) PREFLIGHT_ONLY=true; shift ;;
    --detect-platform) DETECT_PLATFORM_ONLY=true; shift ;;
    --dry-run) DRY_RUN=true; shift ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown option: $1" ;;
  esac
done

[[ $REPOSITORY_URL == https://* && $REPOSITORY_URL != *[[:space:]]* ]] ||
  die 'repository URL must use HTTPS and contain no whitespace'
[[ -r /etc/os-release ]] || die 'cannot identify this distribution'
# shellcheck disable=SC1091
. /etc/os-release
dpkg_arch=$(dpkg --print-architecture 2>/dev/null || true)
detect_platform "${ID:-}" "${VERSION_ID:-}" "${UBUNTU_CODENAME:-}" "${VERSION_CODENAME:-}" "$(uname -m)" "$dpkg_arch" "${VARIANT_ID:-}"
say "platform: $platform_description"
if $DETECT_PLATFORM_ONLY; then exit 0; fi

EXPECTED_FINGERPRINT=${EXPECTED_FINGERPRINT//[[:space:]]/}
EXPECTED_FINGERPRINT=${EXPECTED_FINGERPRINT^^}
[[ $EXPECTED_FINGERPRINT =~ ^[0-9A-F]{40}$ ]] ||
  die 'the repository signing fingerprint has not been configured (40 hex characters required)'

for command in gpg install mktemp; do need_command "$command"; done
if [[ -z $KEY_FILE ]]; then need_command curl; else [[ -r $KEY_FILE ]] || die "cannot read key: $KEY_FILE"; fi
if [[ $manager == apt ]]; then need_command apt-get; else need_command dnf; fi

TEMP_DIR=$(mktemp -d)
trap 'rm -rf "$TEMP_DIR"' EXIT
install -d -m 0700 "$TEMP_DIR/gnupg"
export GNUPGHOME="$TEMP_DIR/gnupg"
downloaded_key="$TEMP_DIR/vectorwarp.asc"
if [[ -n $KEY_FILE ]]; then
  cp "$KEY_FILE" "$downloaded_key"
else
  curl --proto '=https' --proto-redir '=https' --tlsv1.2 --fail --silent --show-error --location \
    "$REPOSITORY_URL/keys/vectorwarp.asc" --output "$downloaded_key"
fi

mapfile -t fingerprints < <(gpg --batch --show-keys --with-colons "$downloaded_key" 2>/dev/null |
  awk -F: '$1 == "pub" { primary = 1; next } primary && $1 == "fpr" { print toupper($10); primary = 0 }')
[[ ${#fingerprints[@]} -eq 1 && ${fingerprints[0]} == "$EXPECTED_FINGERPRINT" ]] ||
  die 'repository key fingerprint does not match the pinned maintainer key'
say "verified signing key $EXPECTED_FINGERPRINT"

if $PREFLIGHT_ONLY; then say 'preflight passed; no repository or package was changed'; exit 0; fi
if [[ $EUID -ne 0 && $DRY_RUN == false ]]; then die 'installation requires root; inspect this script, then run it with sudo'; fi

if [[ $manager == apt ]]; then
  keyring="$TEMP_DIR/vectorwarp.gpg"
  gpg --batch --yes --dearmor --output "$keyring" "$downloaded_key"
  source_file="$TEMP_DIR/vectorwarp.sources"
  {
    printf 'Types: deb\n'
    printf 'URIs: %s/apt\n' "$REPOSITORY_URL"
    printf 'Suites: %s\n' "$codename"
    printf 'Components: main\n'
    printf 'Architectures: %s\n' "$package_arch"
    printf 'Signed-By: /usr/share/keyrings/vectorwarp-archive-keyring.gpg\n'
  } >"$source_file"
  [[ ! -L /usr/share/keyrings/vectorwarp-archive-keyring.gpg ]] || die 'refusing symlink keyring destination'
  [[ ! -L /etc/apt/sources.list.d/vectorwarp.sources ]] || die 'refusing symlink repository destination'
  run install -m 0644 "$keyring" /usr/share/keyrings/vectorwarp-archive-keyring.gpg
  run install -m 0644 "$source_file" /etc/apt/sources.list.d/vectorwarp.sources
  run apt-get update
  run apt-get install vectorwarp
else
  repo_file="$TEMP_DIR/vectorwarp.repo"
  {
    printf '[vectorwarp]\n'
    printf 'name=VectorWarp signed packages\n'
    printf 'baseurl=%s/rpm/fedora/44/$basearch\n' "$REPOSITORY_URL"
    printf 'enabled=1\n'
    printf 'gpgcheck=1\n'
    printf 'repo_gpgcheck=1\n'
    printf 'gpgkey=%s/keys/vectorwarp.asc\n' "$REPOSITORY_URL"
  } >"$repo_file"
  [[ ! -L /etc/yum.repos.d/vectorwarp.repo ]] || die 'refusing symlink repository destination'
  run install -m 0644 "$repo_file" /etc/yum.repos.d/vectorwarp.repo
  run dnf install vectorwarp
fi

if $START_WEB; then
  need_command systemctl
  run systemctl enable --now vectorwarp-api.service
  say 'web API enabled and started; radar processor remains disabled and stopped'
else
  say 'package installed; both services remain disabled and stopped'
fi
say 'review /etc/vectorwarp/config.yml before explicitly starting radar processing'
