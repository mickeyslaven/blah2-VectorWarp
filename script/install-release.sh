#!/usr/bin/env bash
set -euo pipefail

REPOSITORY_URL=https://mickeyslaven.github.io/blah2-VectorWarp
EXPECTED_FINGERPRINT=@SIGNING_FINGERPRINT@
KEY_FILE=
START_WEB=false
DRY_RUN=false
PREFLIGHT_ONLY=false

usage() {
  cat <<'EOF'
Usage: install-release.sh [options]

Add the signed VectorWarp package repository and install VectorWarp.

  --start-web             Explicitly enable and start only the web API
  --repo-url HTTPS_URL    Override the repository base (maintainer/testing)
  --fingerprint HEX       Expected 40-hex signing-key fingerprint
  --key-file PATH         Verify this public key instead of downloading it
  --preflight             Verify platform, tools and signing key; change nothing
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

while (($#)); do
  case "$1" in
    --start-web) START_WEB=true; shift ;;
    --repo-url) (($# >= 2)) || die '--repo-url needs a value'; REPOSITORY_URL=${2%/}; shift 2 ;;
    --fingerprint) (($# >= 2)) || die '--fingerprint needs a value'; EXPECTED_FINGERPRINT=$2; shift 2 ;;
    --key-file) (($# >= 2)) || die '--key-file needs a value'; KEY_FILE=$2; shift 2 ;;
    --preflight) PREFLIGHT_ONLY=true; shift ;;
    --dry-run) DRY_RUN=true; shift ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown option: $1" ;;
  esac
done

[[ $REPOSITORY_URL == https://* && $REPOSITORY_URL != *[[:space:]]* ]] ||
  die 'repository URL must use HTTPS and contain no whitespace'
EXPECTED_FINGERPRINT=${EXPECTED_FINGERPRINT//[[:space:]]/}
EXPECTED_FINGERPRINT=${EXPECTED_FINGERPRINT^^}
[[ $EXPECTED_FINGERPRINT =~ ^[0-9A-F]{40}$ ]] ||
  die 'the repository signing fingerprint has not been configured (40 hex characters required)'

[[ -r /etc/os-release ]] || die 'cannot identify this distribution'
# shellcheck disable=SC1091
. /etc/os-release
machine=$(uname -m)
case "$machine" in
  x86_64) deb_arch=amd64; rpm_arch=x86_64 ;;
  aarch64|arm64) deb_arch=arm64; rpm_arch=aarch64 ;;
  *) die "unsupported architecture: $machine" ;;
esac

case "${ID:-}:${VERSION_ID:-}" in
  ubuntu:22.04) manager=apt; codename=jammy; package_arch=$deb_arch ;;
  ubuntu:24.04) manager=apt; codename=noble; package_arch=$deb_arch ;;
  fedora:44) manager=dnf; codename=; package_arch=$rpm_arch ;;
  *) die 'supported systems are Ubuntu 22.04/24.04 and Fedora 44' ;;
esac

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
say "platform: ${ID} ${VERSION_ID} ($package_arch)"

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
