#!/bin/sh
set -eu

if [ "$#" -eq 0 ]; then
  echo "usage: $0 PACKAGE..." >&2
  exit 2
fi

attempt=1
while [ "$attempt" -le 3 ]; do
  # Refresh from scratch on every attempt. Ubuntu mirrors can briefly publish
  # an index before all referenced packages have reached the same endpoint.
  rm -rf /var/lib/apt/lists/*
  if apt-get -o Acquire::Retries=3 -o APT::Update::Error-Mode=any update && \
     DEBIAN_FRONTEND=noninteractive apt-get -o Acquire::Retries=3 install \
       --yes --no-install-recommends "$@"; then
    apt-get clean
    exit 0
  fi

  if [ "$attempt" -eq 3 ]; then
    echo "apt installation failed after $attempt refreshed attempts" >&2
    exit 1
  fi

  delay=$((attempt * 15))
  echo "apt mirror was inconsistent; refreshing again in ${delay}s" >&2
  sleep "$delay"
  attempt=$((attempt + 1))
done
