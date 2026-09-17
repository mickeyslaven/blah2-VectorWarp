#!/usr/bin/env bash
# CI-only fresh userspace/systemd/browser acceptance. No host devices, sockets,
# services, or credentials enter the container. Not a VectorWarp dependency.
set -euo pipefail
die() { printf 'test-installed-package: %s\n' "$*" >&2; exit 1; }
package= image= evidence= browser_modules=
while (($#)); do
  case "$1" in
    --package|--image|--evidence|--browser-modules)
      (($# >= 2)) || die "missing value for $1"
      case "$1" in
        --package) package=$2;; --image) image=$2;;
        --evidence) evidence=$2;; --browser-modules) browser_modules=$2;;
      esac
      shift 2;;
    *) die "unknown argument: $1";;
  esac
done
[[ $EUID == 0 ]] || die 'Run explicitly with sudo on an ephemeral CI runner'
[[ -f $package && -n $image && -n $evidence && -d $browser_modules/playwright ]] || die 'package, image, evidence and browser modules are required'
source_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
package=$(realpath "$package")
browser_modules=$(realpath "$browser_modules")
mkdir -p "$evidence"
evidence=$(realpath "$evidence")
case "$package" in *.deb) format=deb;; *.rpm) format=rpm;; *) die 'unknown package format';; esac
command -v podman >/dev/null || die 'Podman is required only on the test runner'
command -v node >/dev/null || die 'Node is required only on the browser test runner'
test_cpus=${VECTORWARP_TEST_CPUS:-2}
[[ $test_cpus =~ ^[0-9]*\.?[0-9]+$ ]] || die 'invalid test CPU limit'
quota=$(awk -v cpus="$test_cpus" 'BEGIN { printf "%.0f", cpus * 100000 }')
((quota >= 10000 && quota <= 400000)) || die 'test CPU limit must be between 0.1 and 4'
cpu_set=()
if [[ -n ${VECTORWARP_TEST_CPUSET:-} ]]; then cpu_set=(--cpuset-cpus "$VECTORWARP_TEST_CPUSET"); fi
name="vectorwarp-service-test-$(date +%s)-$$"
test_image="localhost/$name"
container=
cleanup() {
  local status=$?
  trap - EXIT
  if [[ -n $container ]]; then
    podman logs "$container" >"$evidence/container-boot.log" 2>&1 || true
    podman exec "$container" journalctl --no-pager -u vectorwarp-api -u vectorwarp-receiver -u vectorwarp-restart >"$evidence/journal.log" 2>&1 || true
    podman inspect "$container" >"$evidence/container.json" || true
    podman stop --time 10 "$container" >/dev/null 2>&1 || true
    podman rm "$container" >/dev/null 2>&1 || true
  fi
  podman rmi "$test_image" >/dev/null 2>&1 || true
  exit "$status"
}
trap cleanup EXIT
sha256sum "$package" >"$evidence/package.sha256"
podman info --format json >"$evidence/test-runtime.json"
podman build --jobs=1 --memory=2g --memory-swap=2g --cpu-period=100000 --cpu-quota="$quota" "${cpu_set[@]}" \
  --build-arg "BASE_IMAGE=$image" -t "$test_image" \
  -f "$source_root/test/packaging/Containerfile.service-$format" "$source_root/test/packaging" \
  >"$evidence/base-image.log" 2>&1
# SYS_ADMIN permits systemd's own mount namespaces inside the private container.
# The outer container's generic AppArmor/SELinux profile cannot permit these
# nested mounts on every runner. This exemption is for this disposable test OS,
# not VectorWarp's installed units: their own sandbox remains enabled and tested.
# No --privileged, host namespace, host filesystem mount, or device passthrough.
container=$(podman run -d --name "$name" --systemd=always --cgroupns=private \
  --cap-add=SYS_ADMIN --security-opt=label=disable --security-opt=apparmor=unconfined \
  --ulimit core=-1:-1 \
  --cpus="$test_cpus" "${cpu_set[@]}" --memory=2g --memory-swap=2g \
  --pids-limit=512 "$test_image")
# Wait for boot mounts/tmpfiles before copying fixtures into /tmp. A container
# can be running while systemd has not mounted its final temporary filesystem.
# Newer systemd needs an unlimited core hard limit at PID 1 startup; hosted
# runners may otherwise inherit zero. Set it through the container runtime,
# without adding SYS_RESOURCE to VectorWarp or changing any host limit.
# systemctl --wait cannot wait for a bus that does not exist yet. Poll through
# that initial race as well as systemd's subsequent boot states.
boot_state=
boot_deadline=$((SECONDS + 60))
while ((SECONDS < boot_deadline)); do
  boot_state=$(podman exec "$container" timeout 5 systemctl is-system-running 2>/dev/null || true)
  if [[ $boot_state == running || $boot_state == degraded ]]; then break; fi
  [[ $(podman inspect --format '{{.State.Running}}' "$container") == true ]] || break
  sleep .25
done
if [[ $boot_state != running && $boot_state != degraded ]]; then
  podman logs "$container" >&2 || true
  die "test systemd did not finish boot: $boot_state"
fi
podman cp "$package" "$container:/tmp/package.$format"
if [[ $format == deb ]]; then
  podman exec "$container" env DEBIAN_FRONTEND=noninteractive apt-get --yes install /tmp/package.deb >"$evidence/install.log" 2>&1
else
  podman exec "$container" dnf --assumeyes --setopt=install_weak_deps=False install /tmp/package.rpm >"$evidence/install.log" 2>&1
fi
podman cp "$source_root/test/packaging/installed_reinstall_test.py" "$container:/tmp/installed_reinstall_test.py"
podman exec "$container" python3 /tmp/installed_reinstall_test.py "/tmp/package.$format" >"$evidence/reinstall.log" 2>&1
podman cp "$source_root/test/packaging/installed_service_test.py" "$container:/tmp/installed_service_test.py"
podman exec "$container" python3 /tmp/installed_service_test.py >"$evidence/services.log" 2>&1
address=$(podman inspect --format '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' "$container")
[[ $address =~ ^10\.[0-9]+\.[0-9]+\.[0-9]+$ ]] || die 'expected a private test container address'
NODE_PATH="$browser_modules" node "$source_root/test/browser/installed-settings.cjs" \
  "http://$address:3000" "$evidence" >"$evidence/browser.log" 2>&1
podman cp "$source_root/test/recording/processor_replay_test.py" "$container:/tmp/processor_replay_test.py"
# This tests the actual installed processor and shared libraries with synthetic
# IQ and loopback sinks. It is not a live receiver or detection-accuracy claim.
podman exec "$container" runuser -u vectorwarp -- python3 /tmp/processor_replay_test.py \
  --binary /opt/vectorwarp/current/bin/blah2 >"$evidence/processor-replay.log" 2>&1
printf 'Installed service, browser, and synthetic processor tests passed: %s\n' "$package"
