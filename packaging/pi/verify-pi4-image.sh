#!/usr/bin/env bash
# Read-only structural verification plus a private-namespace API smoke test.
set -euo pipefail
IFS=$'\n\t'
die() { printf 'verify-pi4-image: %s\n' "$*" >&2; exit 1; }
usage() { cat <<'EOF'
Usage: packaging/pi/verify-pi4-image.sh --image IMAGE.img.xz --workdir DIR
       packaging/pi/verify-pi4-image.sh --root MOUNTED_ROOT --workdir DIR

--image is decompressed to a regular temporary file and attached only through a
new loop device. --root is an already-mounted image root and is never changed.
The API smoke test copies that root to WORKDIR, then runs only in a new mount,
network, and PID namespace. This verifies software, not physical boot, Wi-Fi,
GPU, receiver, or SDR hardware.
EOF
}
image= root= workdir=
while (($#)); do case "$1" in
  --image) image=${2:?--image needs a value}; shift 2;;
  --root) root=${2:?--root needs a value}; shift 2;;
  --workdir) workdir=${2:?--workdir needs a value}; shift 2;;
  -h|--help) usage; exit 0;; *) die "unknown option: $1";; esac; done
[[ -n $workdir ]] && { [[ -n $image && -z $root ]] || [[ -z $image && -n $root ]]; } || { usage >&2; exit 64; }
[[ $(id -u) == 0 ]] || die 'must run as root for loop and namespace isolation'
for c in awk chroot cp df dpkg find findmnt grep ip losetup mount mountpoint parted readelf realpath rm rsync sha256sum sort stat timeout umount unshare xz; do command -v "$c" >/dev/null || die "missing $c"; done
workdir=$(realpath -m "$workdir")
[[ $workdir =~ ^[-A-Za-z0-9_./+]+$ ]] || die '--workdir has unsafe characters'
mkdir -p "$workdir"; [[ -d $workdir && ! -L $workdir ]] || die '--workdir must be a real directory'
scratch=$(mktemp -d "$workdir/.verify-pi4.XXXXXX")
loop= mounted=()
cleanup() { local rc=$? fail=false p; trap - EXIT INT TERM; set +e
  for ((i=${#mounted[@]}-1;i>=0;i--)); do p=${mounted[i]}; mountpoint -q "$p" && ! umount "$p" && fail=true; done
  [[ -n $loop ]] && ! losetup -d "$loop" && fail=true
  if $fail; then printf 'verify-pi4-image: retained %s after cleanup failure\n' "$scratch" >&2; else rm -rf "$scratch"; fi
  exit "$rc"; }
trap cleanup EXIT INT TERM

if [[ -n $image ]]; then
  image=$(realpath -e "$image"); [[ $image == *.img.xz && -f $image ]] || die '--image must be a .img.xz file'
  xz --test "$image"
  raw=$scratch/image.img; xz -dc -- "$image" >"$raw"
  loop=$(losetup --find --show --partscan "$raw"); [[ $loop =~ ^/dev/loop[0-9]+$ ]] || die 'unexpected loop device'
  [[ $(realpath "$(cat "/sys/class/block/${loop##*/}/loop/backing_file")") == "$raw" ]] || die 'loop backing mismatch'
  parted -s "$loop" unit s print | grep -Eq '^ 1[[:space:]].*fat' || die 'partition 1 is not FAT boot'
  parted -s "$loop" unit s print | grep -Eq '^ 2[[:space:]].*ext4' || die 'partition 2 is not ext4 root'
  root=$scratch/root; mkdir "$root"; mount -o ro,nosuid,nodev "$loop"p2 "$root"; mounted+=("$root")
else
  root=$(realpath -e "$root"); [[ -d $root && ! -L $root ]] || die '--root must be a real mounted root directory'
  mounted_source=$(findmnt -n -o SOURCE --target "$root")
  mounted_options=$(findmnt -n -o OPTIONS --target "$root")
  [[ $mounted_source =~ ^/dev/loop[0-9]+p[0-9]+$ ]] || die '--root must be mounted from a loop partition'
  [[ ,$mounted_options, == *,ro,* ]] || die '--root must be mounted read-only'
fi

test_root() {
  local r=$1
  grep -qx 'ID=debian' "$r/etc/os-release"; grep -qx 'VERSION_CODENAME=bookworm' "$r/etc/os-release"
  [[ $(dpkg --root="$r" --print-architecture) == arm64 ]]
  for pkg in raspi-firmware raspberrypi-sys-mods network-manager vectorwarp libopenblas0-pthread mesa-vulkan-drivers vulkan-tools; do
    dpkg --root="$r" --status "$pkg" | grep -qx 'Status: install ok installed'
  done
  test -d "$r/etc/NetworkManager"; test -f "$r/usr/lib/systemd/system/raspberrypi-sys-mods.service" || test -d "$r/usr/lib/raspberrypi-sys-mods"
  test -L "$r/etc/systemd/system/multi-user.target.wants/vectorwarp-api.service"
  ! find "$r/etc/systemd/system" -type l -name vectorwarp-processor.service -print -quit | grep -q .
  test "$(chroot "$r" getent passwd 1000 | cut -d: -f1,3,6,7)" = 'pi:1000:/home/pi:/bin/bash'
  chroot "$r" /bin/sh -ec 'awk -F: "\$1 == \"root\" || \$1 == \"pi\" { if (\$2 ~ /^[!*]/) ok[\$1]=1; else bad=1 } END { exit ok[\"root\"] && ok[\"pi\"] && !bad ? 0 : 1 }" /etc/shadow'
  chroot "$r" /bin/sh -ec 'awk -F: "\$3 >= 1000 && \$3 != 65534 && \$1 != \"pi\" { bad=1 } END { exit bad ? 1 : 0 }" /etc/passwd'
  ! find "$r/home/pi" -xdev -type f \( -name authorized_keys -o -name id_rsa -o -name id_ed25519 -o -name '*.pem' -o -name '.*history' \) -print -quit | grep -q .
  ! find "$r" -xdev -iname '*testpi*' -print -quit | grep -q .
  ! find "$r" -xdev -type f -name 'libsdrplay_api.so*' -print -quit | grep -q .
  test -f "$r/opt/vectorwarp/current/api/server.js"; test -f "$r/opt/vectorwarp/current/api/config-manager.js"
  test -f "$r/opt/vectorwarp/current/html/index.html"
  test -x "$r/usr/lib/raspberrypi-sys-mods/firstboot"
  test -x "$r/usr/lib/raspberrypi-sys-mods/imager_custom"
  test -L "$r/usr/lib/systemd/system/vectorwarp-api.service.wants/vectorwarp-receiver.socket"
  test -x "$r/opt/vectorwarp/runtime/node/bin/node"; test -f "$r/opt/vectorwarp/current/receiver-source/rspduo/kit.json"
  test -f "$r/etc/vectorwarp/config.yml"; grep -qx '  fc: 100000000' "$r/etc/vectorwarp/config.yml"
  grep -qx '    type: "RspDuo"' "$r/etc/vectorwarp/config.yml"
  chroot "$r" /bin/sh -ec 'id -nG vectorwarp | tr " " "\n" | grep -qx render; id -nG vectorwarp | tr " " "\n" | grep -qx video; find /usr/share/vulkan/icd.d -type f -name "*.json" -print -quit | grep -q .'
  required_elfs=("$r/opt/vectorwarp/runtime/node/bin/node" "$r/opt/vectorwarp/current/bin/blah2" "$r/opt/vectorwarp/current/bin/blah2-gpu-worker" "$r/opt/vectorwarp/current/bin/blah2-mixed-worker")
  for elf in "${required_elfs[@]}"; do
    [[ -x $elf ]] || die "missing required native worker: ${elf#$r}"
    readelf -h "$elf" | grep -q 'Machine:.*AArch64'
    if ! ldd_output=$(chroot "$r" /usr/bin/ldd "${elf#$r}"); then die "ldd failed for ${elf#$r}"; fi
    [[ $ldd_output != *'not found'* ]] || die "unresolved native dependency: ${elf#$r}"
  done
  for elf in "$r"/opt/vectorwarp/current/bin/*.so*; do
    [[ -f $elf ]] || continue
    readelf -h "$elf" | grep -q 'Machine:.*AArch64'
    if ! ldd_output=$(chroot "$r" /usr/bin/ldd "${elf#$r}"); then die "ldd failed for ${elf#$r}"; fi
    [[ $ldd_output != *'not found'* ]] || die "unresolved native dependency: ${elf#$r}"
  done
}
test_root "$root"

# The target root is read-only above. Copy it for the smoke test, then give the
# copy a private mount/network/PID namespace and an unshared loopback port.
smoke=$scratch/smoke-root
rsync -aHAX --numeric-ids "$root/" "$smoke/"
timeout 30s unshare --mount --net --pid --fork /bin/bash -eu -s -- "$smoke" <<'NS'
r=$1
mount --make-rprivate /
mount -t proc proc "$r/proc"
mount -t tmpfs -o mode=0755,nosuid,nodev tmpfs "$r/dev"
mkdir -p "$r/dev/pts"; mount -t devpts devpts "$r/dev/pts"; ln -s pts/ptmx "$r/dev/ptmx"
for d in null zero random urandom; do : >"$r/dev/$d"; mount --bind "/dev/$d" "$r/dev/$d"; done
ip link set lo up
chroot --userspec=vectorwarp-api:vectorwarp-api --groups=vectorwarp-config "$r" /bin/sh -ec '
  test -r /etc/vectorwarp/config.yml; test -w /var/lib/vectorwarp-api
  BLAH2_SETUP_PORT=39081 /opt/vectorwarp/runtime/node/bin/node /opt/vectorwarp/current/api/server.js /etc/vectorwarp/config.yml >/tmp/vectorwarp-api-smoke.log 2>&1 &
  api=$!; trap "kill \$api 2>/dev/null || true; wait \$api 2>/dev/null || true" EXIT INT TERM
  /opt/vectorwarp/runtime/node/bin/node -e "const http=require(\"http\"), paths=[\"/\",\"/api/config\",\"/api/system/status\"], deadline=Date.now()+10000; function one(path){return new Promise((ok,bad)=>{let q=http.get({host:\"127.0.0.1\",port:39081,path,timeout:2000},r=>{let s=\"\";r.on(\"data\",x=>s+=x);r.on(\"end\",()=>{try { r.statusCode===200&&(path===\"/\"?/<html/i.test(s):JSON.parse(s))?ok():bad(new Error(path+\": \"+r.statusCode)) } catch(e) { bad(e) }}).on(\"error\",bad)});q.on(\"timeout\",()=>q.destroy(new Error(\"HTTP timeout\")));q.on(\"error\",bad)})}; (function check(){Promise.all(paths.map(one)).then(()=>process.exit(0),e=>Date.now()<deadline?setTimeout(check,100):(()=>{console.error(e);process.exit(1)})())})()"
'
NS
printf 'verified Pi image root and isolated API smoke test\n'
