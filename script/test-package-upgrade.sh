#!/usr/bin/env bash
# Upgrade a real published package to the candidate inside a disposable,
# systemd-booted OS. This is intentionally separate from the clean-install
# acceptance test so neither path can mask the other.
set -euo pipefail

die() { printf 'test-package-upgrade: %s\n' "$*" >&2; exit 1; }
previous= candidate= image= evidence= dragonos_fixture=false
while (($#)); do
  case "$1" in
    --previous|--candidate|--image|--evidence)
      (($# >= 2)) || die "missing value for $1"
      case "$1" in
        --previous) previous=$2;; --candidate) candidate=$2;;
        --image) image=$2;; --evidence) evidence=$2;;
      esac
      shift 2 ;;
    --dragonos-uhd-fixture) dragonos_fixture=true; shift ;;
    *) die "unknown argument: $1" ;;
  esac
done
[[ $EUID == 0 ]] || die 'run explicitly with sudo on an ephemeral runner'
[[ -f $previous && -f $candidate && -n $image && -n $evidence ]] || die 'previous, candidate, image and evidence are required'
case "$candidate" in *.deb) format=deb;; *.rpm) format=rpm;; *) die 'unknown candidate format';; esac
case "$previous" in *.$format) ;; *) die 'previous and candidate formats differ';; esac
command -v podman >/dev/null || die 'Podman is required'
apparmor_profile=${VECTORWARP_TEST_APPARMOR_PROFILE:-unconfined}
[[ $apparmor_profile == unconfined || $apparmor_profile =~ ^vectorwarp-fedora-ci-[0-9a-f]{12,32}$ ]] ||
  die 'invalid disposable AppArmor profile name'
apparmor_labels=()
if [[ $apparmor_profile != unconfined ]]; then
  apparmor_labels=(--label="vectorwarp-ci-apparmor=$apparmor_profile")
fi
previous=$(realpath "$previous"); candidate=$(realpath "$candidate")
mkdir -p "$evidence"; evidence=$(realpath "$evidence")
source_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
name="vectorwarp-upgrade-test-$(date +%s)-$$"
test_image="localhost/$name"
container=
cleanup() {
  status=$?; trap - EXIT
  if [[ -n $container ]]; then
    podman logs "$container" >"$evidence/container-boot.log" 2>&1 || true
    podman exec "$container" journalctl --no-pager -u vectorwarp-api -u vectorwarp-receiver -u vectorwarp-processor >"$evidence/journal.log" 2>&1 || true
    podman inspect "$container" >"$evidence/container.json" || true
    podman rm --force "$container" >/dev/null 2>&1 || status=1
  fi
  podman rmi "$test_image" >/dev/null 2>&1 || status=1
  exit "$status"
}
trap cleanup EXIT
sha256sum "$previous" "$candidate" >"$evidence/packages.sha256"
podman build --jobs=1 --memory=2g --memory-swap=2g --cpus=2 --build-arg "BASE_IMAGE=$image" \
  -t "$test_image" -f "$source_root/test/packaging/Containerfile.service-$format" \
  "$source_root/test/packaging" >"$evidence/base-image.log" 2>&1
container=$(podman run -d --name "$name" --systemd=always --cgroupns=private \
  --cap-add=SYS_ADMIN --security-opt=label=disable --security-opt="apparmor=$apparmor_profile" \
  "${apparmor_labels[@]}" \
  --ulimit core=-1:-1 --cpus=2 --memory=2g --memory-swap=2g --pids-limit=512 "$test_image")
deadline=$((SECONDS + 60)); state=
while ((SECONDS < deadline)); do
  state=$(podman exec "$container" timeout 5 systemctl is-system-running 2>/dev/null || true)
  [[ $state == running || $state == degraded ]] && break
  sleep .25
done
[[ $state == running || $state == degraded ]] || die "test systemd did not boot: $state"
podman cp "$previous" "$container:/tmp/previous.$format"
podman cp "$candidate" "$container:/tmp/candidate.$format"
if [[ $format == deb ]]; then
  install_cmd=(env DEBIAN_FRONTEND=noninteractive apt-get --yes install)
  query_cmd=(dpkg-query -W -f '${Version}')
  ecosystem_cmd='dpkg-query -W -f="${binary:Package}\t${Version}\n" 2>/dev/null | grep -Ei "^(gnuradio|uhd-host|libuhd|sdrplay)" | sort || true'
else
  install_cmd=(dnf --assumeyes --setopt=install_weak_deps=False install)
  query_cmd=(rpm -q --qf '%{VERSION}-%{RELEASE}')
  ecosystem_cmd='rpm -qa --qf "%{NAME}\t%{EVR}\n" | grep -Ei "^(gnuradio|uhd|sdrplay)" | sort || true'
fi
podman exec "$container" "${install_cmd[@]}" "/tmp/previous.$format" >"$evidence/previous-install.log" 2>&1
before=$(podman exec "$container" "${query_cmd[@]}" vectorwarp)
podman exec "$container" sh -c "$ecosystem_cmd" >"$evidence/ecosystem-before.tsv"
# Make the conffile byte-distinct without exposing its contents in evidence.
# A comment is valid YAML and models an operator-owned pre-journal config.
podman exec "$container" sh -c "printf '\n# upgrade-matrix-user-state\n' >>/etc/vectorwarp/config.yml"
config_before=$(podman exec "$container" sha256sum /etc/vectorwarp/config.yml | awk '{print $1}')
podman exec "$container" test ! -e /etc/vectorwarp/config.yml.receiver-state.json || die 'published-package fixture unexpectedly has a receiver journal'
podman exec "$container" systemctl enable --now vectorwarp-api.service vectorwarp-receiver.socket
podman exec "$container" systemctl stop vectorwarp-processor.service vectorwarp-restart.service
podman exec "$container" "${install_cmd[@]}" "/tmp/candidate.$format" >"$evidence/candidate-upgrade.log" 2>&1
after=$(podman exec "$container" "${query_cmd[@]}" vectorwarp)
[[ $after != "$before" ]] || die 'candidate did not replace the published package'
if [[ $format == deb ]]; then
  podman exec "$container" dpkg --compare-versions "$after" gt "$before" || die 'candidate is not newer than the published DEB'
else
  podman exec -i "$container" python3 - "$before" "$after" <<'PY' || die 'candidate is not newer than the published RPM'
import rpm, sys
def evr(value):
    version, release = value.split('-', 1)
    return ('0', version, release)
raise SystemExit(0 if rpm.labelCompare(evr(sys.argv[2]), evr(sys.argv[1])) > 0 else 1)
PY
fi
config_after=$(podman exec "$container" sha256sum /etc/vectorwarp/config.yml | awk '{print $1}')
[[ $config_after == "$config_before" ]] || die 'operator config changed across package upgrade'
printf 'before_sha256=%s\nafter_sha256=%s\n' "$config_before" "$config_after" >"$evidence/config-preservation.txt"
podman exec "$container" systemctl is-active --quiet vectorwarp-api.service || die 'active API was not restored after upgrade'
podman exec "$container" systemctl is-active --quiet vectorwarp-receiver.socket || die 'active receiver socket was not restored after upgrade'
! podman exec "$container" systemctl is-active --quiet vectorwarp-processor.service || die 'stopped processor was incorrectly activated'
! podman exec "$container" systemctl is-active --quiet vectorwarp-restart.service || die 'stopped restart service was incorrectly activated'
# A pre-journal RSPduo config must fail closed with a public recovery action.
# Persist only the reason/action/recovery fields; never copy config contents.
podman exec "$container" systemctl start vectorwarp-config-guard.service
podman exec "$container" test -f /var/lib/vectorwarp-api/config-guard.json || die 'missing reconciliation diagnostic'
podman exec "$container" journalctl --no-pager -u vectorwarp-config-guard.service >"$evidence/config-guard.log"
grep -Fq 'processor stopped pending exact receiver reconciliation' "$evidence/config-guard.log" ||
  die 'config guard did not emit the clear reconciliation message'
podman exec -i "$container" python3 - <<'PY' >"$evidence/reconciliation.json"
import json
path = '/var/lib/vectorwarp-api/config-guard.json'
value = json.load(open(path, encoding='utf-8'))
assert value['reason'] == 'receiver-journal-missing'
assert value['action'] == 'processor-stopped'
assert 'Settings Apply' in value['recovery']
print(json.dumps({key: value[key] for key in ('reason', 'action', 'recovery')}, sort_keys=True))
PY
podman exec "$container" sh -c "$ecosystem_cmd" >"$evidence/ecosystem-after.tsv"
cmp "$evidence/ecosystem-before.tsv" "$evidence/ecosystem-after.tsv" || die 'upgrade changed the host GNU Radio/UHD/SDRplay ecosystem'
printf 'previous=%s\ncandidate=%s\n' "$before" "$after" >"$evidence/versions.txt"

if $dragonos_fixture; then
  [[ $format == deb ]] || die 'DragonOS UHD fixture requires a DEB candidate'
  # Use a second clean userspace: the real upgrade container intentionally has
  # stock UHD installed by the published package and must not be mutated into a
  # synthetic DragonOS system.
  podman run --rm --security-opt=label=disable \
    -v "$candidate:/tmp/candidate.deb:ro" \
    -v "$source_root/test/packaging/dragonos-custom-uhd-fixture.sh:/tmp/fixture.sh:ro" \
    "$test_image" bash /tmp/fixture.sh /tmp/candidate.deb \
    >"$evidence/dragonos-custom-uhd.log" 2>&1
fi
printf 'Published-package upgrade passed: %s -> %s\n' "$before" "$after"
