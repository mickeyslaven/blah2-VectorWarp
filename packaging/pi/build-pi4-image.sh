#!/usr/bin/env bash
# Assemble a distributable Pi 4 image from a verified Raspberry Pi OS Lite base.
#
# This intentionally has no downloader and never accepts a block device.  It
# mutates only a temporary image file attached through a newly-created loop
# device.  Run it as root on native arm64 Linux; chrooted package maintainer
# scripts must execute the target architecture's binaries.
set -euo pipefail
IFS=$'\n\t'

IMAGE_BYTES=$((8 * 1024 * 1024 * 1024))
RESERVE_BYTES=$((1024 * 1024 * 1024))
SOURCE_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)
PI_PROFILE_SOURCE=$SOURCE_DIR/contrib/systemd/pi4-rspduo-performance.conf

die() { printf 'build-pi4-image: %s\n' "$*" >&2; exit 1; }
usage() {
  cat <<'EOF'
Usage: packaging/pi/build-pi4-image.sh \
  --base BASE.img.xz --base-sha256 SHA256 --deb vectorwarp_arm64.deb \
  --output vectorwarp-pi4.img.xz --source-revision REVISION --allow-network

Builds an 8 GiB raw image from a pinned, official Raspberry Pi OS Lite 64-bit
Bookworm .img.xz. BASE and DEB are local inputs. This script never flashes or
writes a host disk. --allow-network permits the target image's signed OS APT
repositories to download and install DEB dependencies. OUTPUT and
its provenance JSON must not exist.
EOF
}

base= base_sha256= deb= output= source_revision= allow_network=false
while (($#)); do
  case "$1" in
    --base) (($# >= 2)) || die '--base needs a value'; base=$2; shift 2 ;;
    --base-sha256) (($# >= 2)) || die '--base-sha256 needs a value'; base_sha256=$2; shift 2 ;;
    --deb) (($# >= 2)) || die '--deb needs a value'; deb=$2; shift 2 ;;
    --output) (($# >= 2)) || die '--output needs a value'; output=$2; shift 2 ;;
    --source-revision) (($# >= 2)) || die '--source-revision needs a value'; source_revision=$2; shift 2 ;;
    --allow-network) allow_network=true; shift ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown option: $1" ;;
  esac
done

[[ -n $base && -n $base_sha256 && -n $deb && -n $output && -n $source_revision && $allow_network == true ]] || {
  usage >&2; exit 64;
}
[[ $base_sha256 =~ ^[[:xdigit:]]{64}$ ]] || die '--base-sha256 must be a SHA-256 hex digest'
[[ $source_revision =~ ^[[:xdigit:]]{7,64}$ ]] || die '--source-revision must be a Git revision hex digest'
[[ $output == *.img.xz ]] || die '--output must end in .img.xz'

base=$(realpath -e "$base")
deb=$(realpath -e "$deb")
output=$(realpath -m "$output")
[[ $base =~ ^[-A-Za-z0-9_./+]+$ && $deb =~ ^[-A-Za-z0-9_./+]+$ && $output =~ ^[-A-Za-z0-9_./+]+$ ]] ||
  die 'input and output paths may contain only letters, digits, slash, dot, underscore, plus, and hyphen'
[[ -f $base && $base == *.img.xz ]] || die '--base must be an existing .img.xz file'
[[ -f $deb && $deb == *.deb ]] || die '--deb must be an existing .deb file'
[[ -f $PI_PROFILE_SOURCE ]] || die 'missing reviewed Pi 4 RSPduo profile source'
[[ $(grep -c '^Environment=' "$PI_PROFILE_SOURCE") == 7 ]] || die 'Pi performance profile must contain seven environment flags'
[[ ! -e $output && ! -L $output ]] || die 'refusing to replace --output'
[[ ! -e "$output.provenance.json" && ! -L "$output.provenance.json" ]] || die 'refusing to replace output provenance'
mkdir -p "$(dirname "$output")"

[[ $(id -u) == 0 ]] || die 'must run as root (loop setup and image mounts require it)'
case $(uname -m) in aarch64|arm64) ;; *) die 'must run on native arm64 Linux' ;; esac
[[ $(dpkg --print-architecture) == arm64 ]] || die 'host dpkg architecture must be arm64'
for command in apt-get awk chroot cmp cp df dpkg dpkg-deb e2fsck findmnt grep install losetup \
  mkdir mknod mount mountpoint parted partprobe readlink resize2fs rm sha256sum sleep sort stat \
  truncate umount xz; do
  command -v "$command" >/dev/null 2>&1 || die "required command is unavailable: $command"
done

actual_base_sha256=$(sha256sum "$base" | awk '{print $1}')
profile_sha256=$(sha256sum "$PI_PROFILE_SOURCE" | awk '{print $1}')
[[ ${actual_base_sha256,,} == ${base_sha256,,} ]] || die 'base image SHA-256 does not match --base-sha256'
xz --test "$base" || die 'base image is not a valid XZ stream'
[[ $(dpkg-deb -f "$deb" Package) == vectorwarp ]] || die 'input package is not named vectorwarp'
[[ $(dpkg-deb -f "$deb" Architecture) == arm64 ]] || die 'input package architecture is not arm64'

# Leave enough room for the 8 GiB staging image, the compressed result, and
# metadata.  The check is intentionally conservative because XZ compression
# may provide little saving for a nearly-full image.
available_bytes=$(df -PB1 "$(dirname "$output")" | awk 'NR == 2 {print $4}')
(( available_bytes >= IMAGE_BYTES * 2 + RESERVE_BYTES )) ||
  die 'output filesystem needs at least 17 GiB free for safe image assembly'

work=$(mktemp -d "$(dirname "$output")/.vectorwarp-pi4-image.XXXXXX")
raw_image=$work/vectorwarp-pi4.img
root_mount=$work/root
loop=
mounted=()
cleanup() {
  local status=$?
  local retained=false
  set +e
  trap - EXIT INT TERM
  local target
  for ((i=${#mounted[@]} - 1; i >= 0; i--)); do
    target=${mounted[i]}
    if mountpoint -q "$target" && ! umount "$target"; then
      retained=true
      printf 'build-pi4-image: could not unmount %s; retaining %s for recovery\n' "$target" "$work" >&2
    fi
  done
  if [[ -n $loop ]] && ! losetup -d "$loop" 2>/dev/null; then
    retained=true
    printf 'build-pi4-image: could not detach %s; retaining %s for recovery\n' "$loop" "$work" >&2
  fi
  if ! $retained; then
    rm -rf "$work"
  fi
  exit "$status"
}
trap cleanup EXIT INT TERM

# Copy the verified base stream into a regular file and make the root image a
# fixed, predictable 8 GiB.  No host partition or non-loop /dev node is named.
xz -dc -- "$base" >"$raw_image"
[[ $(stat -c %s "$raw_image") -lt $IMAGE_BYTES ]] || die 'base image is already larger than 8 GiB'
truncate -s "$IMAGE_BYTES" "$raw_image"
loop=$(losetup --find --show --partscan "$raw_image")
[[ $loop =~ ^/dev/loop[0-9]+$ ]] || die 'losetup did not return a loop device'
[[ $(realpath "$(cat "/sys/class/block/${loop##*/}/loop/backing_file")") == "$raw_image" ]] ||
  die 'refusing loop device whose backing file is not the staging image'

parted -s "$loop" unit s print | grep -Eq '^ 1[[:space:]].*fat' || die 'base boot partition is not FAT partition 1'
parted -s "$loop" unit s print | grep -Eq '^ 2[[:space:]]' || die 'base root partition is not partition 2'
parted -s "$loop" resizepart 2 100%
partprobe "$loop"
root_partition=${loop}p2
for _ in $(seq 1 20); do [[ -b $root_partition ]] && break; sleep 1; done
[[ -b $root_partition ]] || die 'root loop partition did not appear'
e2fsck -pf "$root_partition" || [[ $? == 1 ]] || die 'root filesystem check failed'
resize2fs "$root_partition"

mkdir -p "$root_mount"
mount -o rw "$root_partition" "$root_mount"; mounted+=("$root_mount")
[[ -f $root_mount/etc/os-release ]] || die 'base root has no /etc/os-release'
grep -qx 'ID=debian' "$root_mount/etc/os-release" || die 'base is not Debian-branded Raspberry Pi OS'
grep -qx 'VERSION_CODENAME=bookworm' "$root_mount/etc/os-release" || die 'base is not Bookworm'
[[ $(dpkg --root="$root_mount" --print-architecture) == arm64 ]] || die 'base image dpkg architecture is not arm64'
dpkg --root="$root_mount" --status raspi-firmware | grep -qx 'Status: install ok installed' ||
  die 'base does not contain Raspberry Pi firmware'
dpkg --root="$root_mount" --status raspberrypi-sys-mods | grep -qx 'Status: install ok installed' ||
  die 'base does not contain Raspberry Pi first-boot integration'
[[ -d $root_mount/etc/NetworkManager ]] || die 'base lacks NetworkManager configuration needed by Imager Wi-Fi customisation'

# Preserve the official boot/first-boot layout: partition 1 is never mounted
# or edited. The chroot gets virtual filesystems and only harmless character
# devices; it never receives the host's full /dev or writable /sys.
mkdir -p "$root_mount"/{dev,dev/pts,proc,sys,tmp}
mount -t tmpfs -o mode=0755,nosuid,nodev tmpfs "$root_mount/dev"; mounted+=("$root_mount/dev")
mkdir -p "$root_mount/dev/pts"
for device in null zero random urandom; do
  : >"$root_mount/dev/$device"
  mount --bind "/dev/$device" "$root_mount/dev/$device"; mounted+=("$root_mount/dev/$device")
done
mount -t devpts devpts "$root_mount/dev/pts"; mounted+=("$root_mount/dev/pts")
ln -s pts/ptmx "$root_mount/dev/ptmx"
mount -t proc -o nosuid,nodev,noexec proc "$root_mount/proc"; mounted+=("$root_mount/proc")
mount -t sysfs -o ro,nosuid,nodev,noexec sysfs "$root_mount/sys"; mounted+=("$root_mount/sys")
printf '%s\n' '#!/bin/sh' '# image assembly: maintainer scripts may not start services' 'exit 101' >"$root_mount/usr/sbin/policy-rc.d"
chmod 0755 "$root_mount/usr/sbin/policy-rc.d"
if [[ -e $root_mount/etc/resolv.conf || -L $root_mount/etc/resolv.conf ]]; then
  cp -a "$root_mount/etc/resolv.conf" "$work/resolv.conf.original"
fi
rm -f "$root_mount/etc/resolv.conf"
cp -- /etc/resolv.conf "$root_mount/etc/resolv.conf"
cp -- "$deb" "$root_mount/tmp/vectorwarp.deb"
# Apt uses only the signed repositories and keyrings already in the official
# target image. No repository, key, or source list is added by this builder.
chroot "$root_mount" /usr/bin/apt-get update
chroot "$root_mount" /usr/bin/apt-get --yes --no-install-recommends install /tmp/vectorwarp.deb libopenblas0-pthread mesa-vulkan-drivers vulkan-tools
chroot "$root_mount" /usr/bin/apt-get clean
rm -rf "$root_mount/var/lib/apt/lists"/* "$root_mount/tmp/vectorwarp.deb" "$root_mount/usr/sbin/policy-rc.d"
rm -f "$root_mount/etc/resolv.conf"
[[ -e $work/resolv.conf.original || -L $work/resolv.conf.original ]] &&
  cp -a "$work/resolv.conf.original" "$root_mount/etc/resolv.conf" || true

# The package intentionally does not activate systemd in this chroot.  Enable
# only the web API for boot, leave radar processing disabled until the operator
# configures a receiver, and apply the packaged service GPU-access policy.
mkdir -p "$root_mount/etc/systemd/system/multi-user.target.wants"
ln -sfn /usr/lib/systemd/system/vectorwarp-api.service \
  "$root_mount/etc/systemd/system/multi-user.target.wants/vectorwarp-api.service"
find "$root_mount/etc/systemd/system" -type l \( -name vectorwarp-processor.service -o -name vectorwarp-restart.service \) -delete
chroot "$root_mount" /usr/bin/python3 -I /opt/vectorwarp/libexec/vectorwarp-gpu-setup --configure-service-access
# Image assembly has no DRM device nodes, so the helper cannot discover the
# existing Pi groups. Grant only the known kernel render groups when present.
gpu_groups=()
for group in render video; do chroot "$root_mount" getent group "$group" >/dev/null && gpu_groups+=("$group"); done
(( ${#gpu_groups[@]} == 2 )) || die 'Pi OS image lacks expected render and video groups'
chroot "$root_mount" /usr/sbin/usermod --append --groups "$(IFS=,; printf '%s' "${gpu_groups[*]}")" vectorwarp

# Ship the reviewed Pi 4 RSPduo baseline at 2 MS/s / 500 ms, replacing only
# the example RF centre frequency with a neutral placeholder.  The processor
# is disabled above, so no capture begins until a person completes setup.
neutral_config=$root_mount/opt/vectorwarp/current/config-examples/config-pi4-rspduo.yml
[[ -f $neutral_config ]] || die 'package lacks the Pi 4 RSPduo configuration example'
install -m 0660 "$neutral_config" "$root_mount/etc/vectorwarp/config.yml"
chroot "$root_mount" /bin/chown vectorwarp-api:vectorwarp-config /etc/vectorwarp/config.yml
sed -i 's/^  fc: 551000000$/  fc: 100000000/' "$root_mount/etc/vectorwarp/config.yml"
chroot "$root_mount" /opt/vectorwarp/runtime/node/bin/node -e '
const fs = require("fs"), yaml = require("/opt/vectorwarp/current/api/node_modules/js-yaml");
const result = require("/opt/vectorwarp/current/api/config-manager").validateConfig(yaml.load(fs.readFileSync("/etc/vectorwarp/config.yml", "utf8")));
if (!result.valid) throw new Error(result.errors.join("; "));
'

# Bake the seven reviewed RSPduo runtime flags as an image profile. Generic
# receivers ignore these names. The profile remains removable before choosing
# another RSP sample mode.
install -d -m 0755 "$root_mount/etc/systemd/system/vectorwarp-processor.service.d"
install -m 0644 "$PI_PROFILE_SOURCE" \
  "$root_mount/etc/systemd/system/vectorwarp-processor.service.d/pi4-rspduo-performance.conf"

# This target-only unit selects the stock performance governor only on a Pi 4
# BCM2711. It never runs during assembly and does not overclock the hardware.
cat >"$root_mount/etc/systemd/system/vectorwarp-pi4-performance-governor.service" <<'EOF'
[Unit]
Description=VectorWarp Pi 4 stock CPU performance governor
ConditionPathExists=/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
Before=vectorwarp-api.service vectorwarp-processor.service

[Service]
Type=oneshot
ExecStart=/bin/sh -ec 'grep -aq "brcm,bcm2711" /proc/device-tree/compatible || exit 0; for governor in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do [ -w "$governor" ] && printf performance >"$governor"; done'

[Install]
WantedBy=multi-user.target
EOF
ln -sfn /etc/systemd/system/vectorwarp-pi4-performance-governor.service \
  "$root_mount/etc/systemd/system/multi-user.target.wants/vectorwarp-pi4-performance-governor.service"

# A public image contains no preselected Pi identity, credential material,
# device keys, recordings, or test-machine state.  Raspberry Pi Imager creates
# the user, SSH setting, and NetworkManager Wi-Fi connection during flashing.
rm -f "$root_mount"/etc/ssh/ssh_host_*_key "$root_mount"/etc/ssh/ssh_host_*_key.pub \
  "$root_mount/var/lib/systemd/random-seed" "$root_mount/var/lib/urandom/random-seed"
: >"$root_mount/etc/machine-id"
[[ -e $root_mount/var/lib/dbus/machine-id && ! -L $root_mount/var/lib/dbus/machine-id ]] && : >"$root_mount/var/lib/dbus/machine-id" || true
rm -rf "$root_mount/var/lib/vectorwarp/recordings"/* "$root_mount/var/lib/vectorwarp-adapters/rspduo"/*

chroot "$root_mount" /bin/bash -eu -c '
  test "$(dpkg-query -W -f="\${db:Status-Status}" vectorwarp)" = installed
  test "$(dpkg-query -W -f="\${db:Status-Status}" libopenblas0-pthread)" = installed
  test "$(dpkg-query -W -f="\${db:Status-Status}" mesa-vulkan-drivers)" = installed
  test "$(dpkg-query -W -f="\${db:Status-Status}" vulkan-tools)" = installed
  id -nG vectorwarp | tr " " "\n" | grep -qx render
  id -nG vectorwarp | tr " " "\n" | grep -qx video
  find /usr/share/vulkan/icd.d -type f -name "*.json" -print -quit | grep -q .
  test -L /etc/systemd/system/multi-user.target.wants/vectorwarp-api.service
  test "$(readlink /etc/systemd/system/multi-user.target.wants/vectorwarp-api.service)" = /usr/lib/systemd/system/vectorwarp-api.service
  ! find /etc/systemd/system -type l \( -name vectorwarp-processor.service -o -name vectorwarp-restart.service \) -print -quit | grep -q .
  test "$(getent passwd 1000 | cut -d: -f1,3,6,7)" = "pi:1000:/home/pi:/bin/bash"
  ! getent passwd testPi >/dev/null
  awk -F: "\$3 >= 1000 && \$3 != 65534 && \$1 != \"pi\" { bad=1 } END { exit bad ? 1 : 0 }" /etc/passwd
  awk -F: "\$1 == \"root\" || \$1 == \"pi\" { if (\$2 ~ /^[!*]/) locked[\$1]=1; else bad=1 } END { exit locked[\"root\"] && locked[\"pi\"] && !bad ? 0 : 1 }" /etc/shadow
  ! find /home/pi -xdev -type f \( -name authorized_keys -o -name id_rsa -o -name id_ed25519 -o -name "*.pem" -o -name ".bash_history" -o -name ".zsh_history" -o -name ".python_history" \) -print -quit | grep -q .
  ! find / -xdev -iname "*testpi*" -print -quit | grep -q .
  ! find / -xdev -type f -name "libsdrplay_api.so*" -print -quit | grep -q .
  grep -qx "    latitude: 0" /etc/vectorwarp/config.yml
  grep -qx "    longitude: 0" /etc/vectorwarp/config.yml
  grep -qx "    name: \"Set receiver site\"" /etc/vectorwarp/config.yml
  grep -qx "  fc: 100000000" /etc/vectorwarp/config.yml
  grep -qx "    type: \"RspDuo\"" /etc/vectorwarp/config.yml
  grep -qx "    cpi: 0.5" /etc/vectorwarp/config.yml
  grep -qx "    surveillance_workers: 1" /etc/vectorwarp/config.yml
  grep -qx "    fft_threads: 4" /etc/vectorwarp/config.yml
  test "$(grep -c "^Environment=" /etc/systemd/system/vectorwarp-processor.service.d/pi4-rspduo-performance.conf)" = 7
  test -L /etc/systemd/system/multi-user.target.wants/vectorwarp-pi4-performance-governor.service
  grep -q "brcm,bcm2711" /etc/systemd/system/vectorwarp-pi4-performance-governor.service
  ! find /var/lib/vectorwarp -type f -print -quit | grep -q .
'
chroot "$root_mount" /usr/bin/dpkg-query -W -f '${binary:Package}\t${Version}\n' | sort >"$work/installed-packages.tsv"

# Never compress an image while its filesystem is mounted.  Teardown happens
# before producing the final artifact and its immutable provenance record.
for ((i=${#mounted[@]} - 1; i >= 0; i--)); do
  mountpoint -q "${mounted[i]}" && umount "${mounted[i]}"
done
mounted=()
losetup -d "$loop"; loop=
xz -T2 -6 --memlimit-compress=512MiB --stdout "$raw_image" >"$work/image.img.xz"

python3 - "$base" "$deb" "$work/image.img.xz" "$output" "$work/installed-packages.tsv" "$source_revision" "$actual_base_sha256" "$profile_sha256" <<'PY' >"$work/image.img.xz.provenance.json"
import hashlib, json, os, subprocess, sys
base, deb, compressed, final_output, package_list, revision, base_sha, profile_sha = sys.argv[1:]
def digest(path):
    h = hashlib.sha256()
    with open(path, 'rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()
def field(name):
    return subprocess.check_output(['dpkg-deb', '-f', deb, name], text=True).strip()
data = {
    'format': 1, 'source_revision': revision,
    'base': {'filename': os.path.basename(base), 'sha256': base_sha, 'bytes': os.path.getsize(base)},
    'vectorwarp_deb': {'filename': os.path.basename(deb), 'sha256': digest(deb), 'bytes': os.path.getsize(deb),
                       'package': field('Package'), 'version': field('Version'), 'architecture': field('Architecture')},
    'image': {'filename': os.path.basename(final_output), 'sha256': digest(compressed), 'bytes': os.path.getsize(compressed),
              'raw_bytes': 8 * 1024 * 1024 * 1024},
    'installed_packages': dict(line.rstrip('\n').split('\t', 1) for line in open(package_list, encoding='utf-8') if '\t' in line),
    'services': {'api_enabled': True, 'processor_enabled': False},
    'pi4_rspduo_profile': {'source': 'contrib/systemd/pi4-rspduo-performance.conf', 'sha256': profile_sha,
                            'environment_flags': 7, 'counter_scale': 3,
                            'stock_bcm2711_performance_governor_enabled': True},
    'first_boot': {'raspberry_pi_imager_user_ssh_wifi': True},
}
print(json.dumps(data, indent=2, sort_keys=True))
PY
chmod 0644 "$work/image.img.xz.provenance.json"
mv "$work/image.img.xz" "$output"
mv "$work/image.img.xz.provenance.json" "$output.provenance.json"
printf 'created %s\nprovenance %s.provenance.json\n' "$output" "$output"
