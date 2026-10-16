#!/usr/bin/env bash
# Runs make inside the PS5 SDK Docker image. Defaults to `make ps5`.
#   tools/ps5-build.sh                 build build/ps5/ps5-adblock.elf
#   tools/ps5-build.sh fuzz            run the fuzzer with the image's clang
#   tools/ps5-build.sh deploy PS5_HOST=192.168.1.20
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
image=ps5-adblock-sdk

docker build -q -t "$image" "$root/platform/ps5/docker" >/dev/null
# As the calling user, so build/ is not left owned by root on Linux hosts.
docker run --rm --user "$(id -u):$(id -g)" -v "$root":/src -w /src "$image" make "${@:-ps5}"
