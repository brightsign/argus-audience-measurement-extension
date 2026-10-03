#!/usr/bin/env bash
set -euo pipefail

# Cross-compiles the extension for every supported SoC against the shared-cache
# SDK. The SDK is provisioned once per build box by the brightsign-sdk-builder
# repo and resolved here through scripts/lib/cache.sh -- this script never builds
# the SDK, it only consumes it. Run `make fetch-sdk` first (the Makefile does).
#
# Honors DEMO_MODE: unset/1 builds the demo binary (expiration enforcement),
# DEMO_MODE=0 compiles it out for production. This mirrors the historical
# runall.sh step 3 exactly; only the SDK environment source moved to the cache.

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck disable=SC1091
source "$(dirname "${BASH_SOURCE[0]}")/lib/cache.sh"   # sets SDK_ENV

if [ ! -f "${SDK_ENV}" ]; then
    echo "ERROR: SDK environment not found at ${SDK_ENV}" >&2
    echo "Run 'make fetch-sdk' (provisioned by ../brightsign-sdk-builder)." >&2
    exit 1
fi

# shellcheck disable=SC1090
source "${SDK_ENV}"

# Demo mode defaults to ON. Production builds must set DEMO_MODE=0 to compile out
# expiration enforcement.
DEMO_CMAKE_FLAG=""
if [ "${DEMO_MODE:-1}" != "1" ]; then
    DEMO_CMAKE_FLAG="-DENABLE_DEMO_MODE=OFF"
    echo "Demo mode DISABLED: building production binary without expiration enforcement"
else
    echo "Demo mode ENABLED: building with expiration date enforcement"
fi

# SoC -> build directory. Names are the historical ones the package script keys
# off; do not rename without updating ./package.
build_dir_for() {
    case "$1" in
        rk3588) echo "build_xt5" ;;
        rk3576) echo "build_rk3576" ;;
        rk3568) echo "build_ls5" ;;
        *) echo "ERROR: unknown SoC '$1' (expected rk3588|rk3576|rk3568)" >&2; return 1 ;;
    esac
}

build_soc() {
    local soc="$1" build_dir soc_dir install_dir
    build_dir="$(build_dir_for "${soc}")" || exit 1
    soc_dir="${soc^^}"                       # rk3588 -> RK3588
    install_dir="${REPO_ROOT}/install/${soc_dir}"
    echo "==> Building for ${soc} (${build_dir})"
    rm -rf "${REPO_ROOT}/${build_dir}"
    mkdir -p "${REPO_ROOT}/${build_dir}"
    # Reset the install output so `make install` never fails re-copying read-only
    # SDK libs (e.g. libperl.so) left by a previous run. Preserve model/, which
    # sync-models populated from the cache and the CMake post-build step needs.
    if [ -d "${install_dir}" ]; then
        find "${install_dir}" -mindepth 1 -maxdepth 1 ! -name model -exec rm -rf {} +
    fi
    ( cd "${REPO_ROOT}/${build_dir}" && \
        cmake .. -DOECORE_TARGET_SYSROOT="${OECORE_TARGET_SYSROOT}" \
                 -DTARGET_SOC="${soc}" -DBUILD_TESTS=OFF ${DEMO_CMAKE_FLAG} && \
        make && \
        make install )
}

# Build the SoCs given as arguments, or all three by default. Lets callers do a
# faster single-SoC iteration, e.g. `make build SOCS=rk3588`.
SOCS=("$@")
if [ "${#SOCS[@]}" -eq 0 ]; then
    SOCS=(rk3588 rk3576 rk3568)
fi
for soc in "${SOCS[@]}"; do
    build_soc "${soc}"
done

echo "==> SoC builds complete: ${SOCS[*]}"
