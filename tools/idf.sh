#!/usr/bin/env bash
#
# Run an idf.py command inside the official Espressif ESP-IDF container.
#
#   ./tools/idf.sh set-target esp32c6     # once
#   ./tools/idf.sh build
#   ./tools/idf.sh menuconfig
#
# On Linux and macOS a serial device can be passed into the container, so
# flash and monitor work here too:
#
#   BATMON_PORT=/dev/ttyACM0 ./tools/idf.sh flash monitor
#
# On Windows (Git Bash) they cannot -- Docker Desktop has no COM passthrough.
# Build here, then flash from PowerShell with tools/flash.ps1.

set -euo pipefail

IMAGE="${BATMON_IDF_IMAGE:-espressif/idf:release-v5.3}"
PROJ="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

command -v docker >/dev/null 2>&1 || {
    echo "docker not found on PATH" >&2
    exit 1
}

DOCKER_ARGS=(--rm -i)
[ -t 0 ] && DOCKER_ARGS+=(-t)

case "$(uname -s)" in
MINGW* | MSYS* | CYGWIN*)
    # MSYS rewrites anything that looks like a Unix path in an argument, so
    # /project becomes C:/Program Files/Git/project and the mount lands in the
    # wrong place. Disabling the rewrite is the only reliable fix.
    export MSYS_NO_PATHCONV=1
    export MSYS2_ARG_CONV_EXCL='*'
    HOST_PROJ="$(cygpath -w "$PROJ")"
    ;;
*)
    HOST_PROJ="$PROJ"
    # Without this the container writes root-owned files into build/ and the
    # next host-side operation on them fails with permission denied.
    DOCKER_ARGS+=(-u "$(id -u):$(id -g)" -e HOME=/tmp)
    ;;
esac

# Pass a serial device through when one is named and exists (Linux/macOS only).
if [ -n "${BATMON_PORT:-}" ] && [ -e "${BATMON_PORT}" ]; then
    DOCKER_ARGS+=(--device "${BATMON_PORT}:${BATMON_PORT}")
    DOCKER_ARGS+=(-e "ESPPORT=${BATMON_PORT}")
fi

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "==> pulling $IMAGE (first run, ~2.5 GB)"
    docker pull "$IMAGE"
fi

echo "==> $IMAGE : idf.py ${*:-build}"

exec docker run "${DOCKER_ARGS[@]}" \
    -v "${HOST_PROJ}:/project" \
    -w /project \
    -e IDF_CCACHE_ENABLE=1 \
    "$IMAGE" \
    idf.py "${@:-build}"
