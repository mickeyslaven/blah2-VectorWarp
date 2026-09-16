#!/usr/bin/env bash
# CI-only staging: never run candidate code with the runner's privileges.
set -euo pipefail

[[ $# == 4 && $EUID == 0 ]] || {
  echo 'Usage (root): prepare-receiver-build.sh CANDIDATE VECTORWARP BUILD_UID BUILD_GID' >&2
  exit 2
}
candidate=$1
vectorwarp=$2
build_uid=$3
build_gid=$4
[[ $build_uid =~ ^[1-9][0-9]*$ && $build_gid =~ ^[1-9][0-9]*$ ]] || {
  echo 'The build identity must be non-root.' >&2
  exit 2
}
[[ -f $candidate/host/CMakeLists.txt && -x $vectorwarp/script/build-native.sh ]] || {
  echo 'The checked-out candidate or VectorWarp source is incomplete.' >&2
  exit 2
}

# Do not use RUNNER_TEMP: it can itself be inside an inaccessible runner home.
# Keep the parent and trusted source root-owned, so candidate code cannot
# replace or modify the VectorWarp build script, source or Git metadata.
build_root=$(mktemp -d /tmp/vectorwarp-receiver-build.XXXXXXXX)
chmod 0755 "$build_root"
cp -a -- "$candidate" "$build_root/candidate"
cp -a -- "$vectorwarp" "$build_root/vectorwarp"
chown -R root:root "$build_root/vectorwarp"
chmod -R u+rwX,go+rX,go-w "$build_root/vectorwarp"
chown -R "$build_uid:$build_gid" "$build_root/candidate"
chmod -R u+rwX "$build_root/candidate"
for directory in home receiver-sdk candidate-build vectorwarp-build vectorwarp-deps; do
  install -d -m 0700 -o "$build_uid" -g "$build_gid" "$build_root/$directory"
done
printf '%s\n' "$build_root"
