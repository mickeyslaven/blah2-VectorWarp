#!/usr/bin/env bash
set -euo pipefail

REPOSITORY_URL=https://mickeyslaven.github.io/blah2-VectorWarp
EXPECTED_FINGERPRINT=@SIGNING_FINGERPRINT@
KEY_FILE=
START_WEB=false
REPO_ONLY=false
SETUP_PI_GPU=false
DRY_RUN=false
PREFLIGHT_ONLY=false
DETECT_PLATFORM_ONLY=false

usage() {
  cat <<'EOF'
Usage: install-release.sh [options]

Add the signed VectorWarp package repository and install VectorWarp.

  --start-web             Explicitly enable and start only the web API
  --repo-only             Add the signed repository without installing packages
  --setup-pi-gpu          After install, offer a signed native Pi Mesa transaction
  --repo-url HTTPS_URL    Override the repository base (maintainer/testing)
  --fingerprint HEX       Expected 40-hex signing-key fingerprint
  --key-file PATH         Verify this public key instead of downloading it
  --preflight             Verify platform, tools and signing key; change nothing
  --detect-platform       Show the matching distribution package; change nothing
  --dry-run               Print package/repository operations; change nothing
  -h, --help              Show this help

On a fresh install, radar stays stopped. On upgrade, package hooks may restore
previously running services. Run vectorwarp, configure the receiver in Settings,
then choose Save & Restart to start radar. Later, vectorwarp start uses saved
settings. Run vectorwarp help for all commands (0.1.7+).
EOF
}

die() { printf 'install-release: %s\n' "$*" >&2; exit 1; }
say() { printf 'install-release: %s\n' "$*"; }
quote_command() { printf ' %q' "$@"; printf '\n'; }
run() { if $DRY_RUN; then printf '+'; quote_command "$@"; else "$@"; fi; }
need_command() { command -v "$1" >/dev/null 2>&1 || die "missing command: $1"; }

validate_repository_url() {
  local url=$1 remainder authority port=
  [[ $url == https://* && $url != *[[:space:][:cntrl:]]* &&
    $url != *'?'* && $url != *'#'* && $url != *'\'* ]] ||
    die 'repository URL must be a plain HTTPS origin/path without credentials, query, fragment or whitespace'
  remainder=${url#https://}
  authority=${remainder%%/*}
  [[ -n $authority && $authority != *@* ]] ||
    die 'repository URL must contain a host and must not embed credentials'
  [[ $authority =~ ^([A-Za-z0-9]([A-Za-z0-9.-]*[A-Za-z0-9])?)(:([0-9]{1,5}))?$ ]] ||
    die 'repository URL has an invalid host or port'
  port=${BASH_REMATCH[4]:-}
  if [[ -n $port ]] && ((10#$port < 1 || 10#$port > 65535)); then
    die 'repository URL port must be between 1 and 65535'
  fi
}

validate_public_key() {
  local key_file=$1 expected=$2 records type validity expires field10 capabilities rest
  local now primary_count=0 primary_fingerprint= primary_usable=false signing_subkey=false last_key=
  records=$(gpg --batch --show-keys --with-colons "$key_file" 2>/dev/null) ||
    die 'repository key is not valid OpenPGP data'
  [[ -n $records ]] || die 'repository key is empty'
  now=$(date +%s)
  while IFS=: read -r type validity _ _ _ _ expires _ _ field10 _ capabilities rest; do
    case "$type" in
      sec|ssb) die 'repository public key must not contain secret key material' ;;
      pub)
        ((primary_count += 1))
        last_key=pub
        if [[ $validity != r && $validity != e && $validity != d && $validity != i &&
          (-z $expires || 10#$expires -gt now) ]]; then
          primary_usable=true
        fi ;;
      sub)
        last_key=sub
        if [[ $validity != r && $validity != e && $validity != d && $validity != i &&
          $capabilities == *s* &&
          (-z $expires || 10#$expires -gt now) ]]; then
          signing_subkey=true
        fi ;;
      fpr)
        if [[ $last_key == pub ]]; then primary_fingerprint=$field10; fi
        last_key= ;;
      *) last_key= ;;
    esac
  done <<<"$records"
  [[ $primary_count -eq 1 && $primary_fingerprint == "$expected" ]] ||
    die 'repository key fingerprint does not match the single pinned maintainer key'
  $primary_usable || die 'repository primary key is expired, revoked, disabled or invalid'
  $signing_subkey || die 'repository key has no unexpired, non-revoked signing subkey'
}

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
      case "$os_version/$version_codename" in
        12/bookworm) base_version=12 ;;
        13/trixie) base_version=13 ;;
        *) die 'Raspberry Pi OS is packaged only for 64-bit Bookworm (Debian 12) or Trixie (Debian 13)' ;;
      esac ;;
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
    *) die "distribution '$os_id' is not packaged; supported systems are Fedora 44, Debian 12 ARM64, Debian 13, Ubuntu 22.04/24.04/26.04, matching DragonOS editions and 64-bit Raspberry Pi OS Bookworm or Trixie" ;;
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
    case "$base_version/$version_codename" in
      12/bookworm)
        [[ $deb_arch == arm64 ]] || die 'Debian 12 Bookworm is packaged only for arm64'
        codename=bookworm ;;
      13/trixie) codename=trixie ;;
      *) die 'Debian packages require unambiguous Debian 12 (Bookworm) or Debian 13 (Trixie) metadata' ;;
    esac
    [[ -z $ubuntu_codename ]] || die 'Debian packages must not define UBUNTU_CODENAME'
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
    --repo-only) REPO_ONLY=true; shift ;;
    --setup-pi-gpu) SETUP_PI_GPU=true; shift ;;
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

if $REPO_ONLY && { $START_WEB || $SETUP_PI_GPU; }; then
  die '--repo-only cannot be combined with --start-web or --setup-pi-gpu'
fi

validate_repository_url "$REPOSITORY_URL"
[[ -r /etc/os-release ]] || die 'cannot identify this distribution'
# shellcheck disable=SC1091
. /etc/os-release
dpkg_arch=
case "${ID:-}" in
  ubuntu|debian|raspbian|dragonos|dragonos-*)
    need_command dpkg
    dpkg_arch=$(dpkg --print-architecture) ;;
esac
detect_platform "${ID:-}" "${VERSION_ID:-}" "${UBUNTU_CODENAME:-}" "${VERSION_CODENAME:-}" "$(uname -m)" "$dpkg_arch" "${VARIANT_ID:-}"
say "platform: $platform_description"
if $DETECT_PLATFORM_ONLY; then exit 0; fi

EXPECTED_FINGERPRINT=${EXPECTED_FINGERPRINT//[[:space:]]/}
EXPECTED_FINGERPRINT=${EXPECTED_FINGERPRINT^^}
[[ $EXPECTED_FINGERPRINT =~ ^[0-9A-F]{40}$ ]] ||
  die 'the repository signing fingerprint has not been configured (40 hex characters required)'

for command in date gpg install mktemp stat; do need_command "$command"; done
if [[ -z $KEY_FILE ]]; then
  need_command curl
else
  [[ -r $KEY_FILE && -f $KEY_FILE && ! -L $KEY_FILE ]] || die "cannot read a regular key file: $KEY_FILE"
  key_size=$(stat -c %s "$KEY_FILE")
  [[ $key_size -gt 0 && $key_size -le 1048576 ]] || die 'repository key must be between 1 byte and 1 MiB'
fi
if [[ $manager == apt ]]; then
  need_command apt-get
else
  need_command dnf
fi
if $START_WEB; then need_command systemctl; fi

TEMP_DIR=$(mktemp -d)
trap 'rm -rf "$TEMP_DIR"' EXIT
install -d -m 0700 "$TEMP_DIR/gnupg"
export GNUPGHOME="$TEMP_DIR/gnupg"
downloaded_key="$TEMP_DIR/vectorwarp.asc"
if [[ -n $KEY_FILE ]]; then
  cp "$KEY_FILE" "$downloaded_key"
else
  curl --proto '=https' --proto-redir '=https' --tlsv1.2 --fail --silent --show-error --location \
    --connect-timeout 10 --max-time 60 --max-filesize 1048576 \
    "$REPOSITORY_URL/keys/vectorwarp.asc" --output "$downloaded_key" ||
    die 'could not download the bounded repository public key; no system configuration was changed'
fi
key_size=$(stat -c %s "$downloaded_key")
[[ $key_size -gt 0 && $key_size -le 1048576 ]] || die 'repository key must be between 1 byte and 1 MiB'
validate_public_key "$downloaded_key" "$EXPECTED_FINGERPRINT"
say "verified signing key $EXPECTED_FINGERPRINT"

if $PREFLIGHT_ONLY; then say 'preflight passed; no repository or package was changed'; exit 0; fi
if [[ $EUID -ne 0 && $DRY_RUN == false ]]; then die 'installation requires root; run it with sudo'; fi

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
  run install -m 0644 "$keyring" /usr/share/keyrings/vectorwarp-archive-keyring.gpg ||
    die 'could not install the APT keyring; no package manager was run'
  run install -m 0644 "$source_file" /etc/apt/sources.list.d/vectorwarp.sources ||
    die 'could not install the APT source; the keyring may have been updated and a retry is safe'
  if ! $REPO_ONLY; then
    run apt-get update ||
      die 'APT metadata refresh failed; repository configuration was retained for a safe retry'
    run apt-get install vectorwarp ||
      die 'APT package installation failed; repository configuration was retained for a safe retry'
  fi
else
  repo_file="$TEMP_DIR/vectorwarp.repo"
  installed_key=/etc/pki/rpm-gpg/RPM-GPG-KEY-vectorwarp
  {
    printf '[vectorwarp]\n'
    printf 'name=VectorWarp signed packages\n'
    printf 'baseurl=%s/rpm/fedora/44/$basearch\n' "$REPOSITORY_URL"
    printf 'enabled=1\n'
    printf 'gpgcheck=1\n'
    printf 'repo_gpgcheck=1\n'
    printf 'gpgkey=file://%s\n' "$installed_key"
  } >"$repo_file"
  [[ ! -L $installed_key ]] || die 'refusing symlink signing-key destination'
  [[ ! -L /etc/yum.repos.d/vectorwarp.repo ]] || die 'refusing symlink repository destination'
  # DNF must use the exact key verified above, not download it again later.
  run install -D -m 0644 "$downloaded_key" "$installed_key" ||
    die 'could not install the DNF signing key; no package manager was run'
  run install -m 0644 "$repo_file" /etc/yum.repos.d/vectorwarp.repo ||
    die 'could not install the DNF repository file; the signing key may have been updated and a retry is safe'
  if ! $REPO_ONLY; then
    run dnf install vectorwarp ||
      die 'DNF package installation failed; repository configuration was retained for a safe retry'
  fi
fi

if $REPO_ONLY; then
  if $DRY_RUN; then
    say 'repository setup dry run; no system configuration was changed'
  else
    say 'repository configured; no package manager or service was started'
  fi
  if [[ $manager == apt ]]; then
    say 'install: sudo apt update && sudo apt install vectorwarp'
  else
    say 'install: sudo dnf install vectorwarp'
  fi
  exit 0
fi

if $SETUP_PI_GPU; then
  run /opt/vectorwarp/libexec/vectorwarp-gpu-setup --install-driver ||
    die 'VectorWarp installed; Pi driver setup was cancelled or unavailable; rerun the local setup command after review'
else
  say 'Pi GPU check: /opt/vectorwarp/libexec/vectorwarp-gpu-setup --status; explicit driver setup: sudo /opt/vectorwarp/libexec/vectorwarp-gpu-setup --install-driver'
fi

if $START_WEB; then
  run systemctl enable --now vectorwarp-api.service ||
    die 'package installation succeeded but enabling or starting the web API failed; installer did not start radar; existing service state was not verified'
  say 'web API enabled; configure the receiver in Settings before starting radar'
else
  say 'package installed; configure the receiver in Settings before starting radar'
fi
say 'installer did not start radar directly; package upgrade hooks may restore previously running services'
say 'VectorWarp 0.1.7+: run vectorwarp to open Settings; vectorwarp help lists all commands'
