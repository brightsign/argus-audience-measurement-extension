#!/usr/bin/env bash
set -euo pipefail

# Ensures the custom BrightSign 9.1 aarch64 cross-compile SDK is available in the
# shared build cache (CACHE_DIR/sdk -- see scripts/lib/cache.sh). The SDK is
# provisioned once per build box by the `brightsign-sdk-builder` repo, which
# installs it straight into the cache; this script simply detects it there. As a
# convenience it will also install from a toolchain installer you have on hand.
#
# "Self-contained path" matters: OE SDK installers bake absolute paths
# (SDKTARGETSYSROOT, PATH, CC/CXX --sysroot=..., and the ELF INTERP/RPATH of the
# cross-toolchain binaries themselves) into the tree at install time, relocated to
# wherever `-d <dir>` points. Because every project resolves the SAME absolute
# CACHE_DIR/sdk path, one correct install serves them all; copying in an SDK
# relocated for a different path silently breaks the build.
#
# The extension's CMake build cross-compiles for aarch64 using the toolchain +
# sysroots this provides:
#   sdk/environment-setup-aarch64-oe-linux   (CC/CXX/OECORE_TARGET_SYSROOT/...)
#   sdk/sysroots/aarch64-oe-linux            (OpenCV, GStreamer, RGA, RKNN, ...)
#   sdk/sysroots/x86_64-oesdk-linux          (the cross toolchain)

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck disable=SC1091
source "$(dirname "${BASH_SOURCE[0]}")/lib/cache.sh"   # sets SDK_DIR, SDK_ENV, CACHE_DIR
ENV_SCRIPT="${SDK_ENV}"
INSTALLER_GLOB="brightsign-x86_64-cobra-toolchain-*.sh"

# An SDK is usable only if present AND baked for THIS cache's absolute path. A
# stale-path SDK fails with "cannot execute: required file not found" when the
# cross compiler is invoked, since its ELF INTERP points elsewhere.
sdk_is_self_consistent() {
    [ -d "${SDK_DIR}/sysroots/aarch64-oe-linux" ] || return 1
    [ -f "${ENV_SCRIPT}" ] || return 1
    grep -q "SDKTARGETSYSROOT=${SDK_DIR}/sysroots/aarch64-oe-linux" "${ENV_SCRIPT}"
}

# Locate the brightsign-sdk-builder checkout that provisions this cache, so a
# missing SDK can be built automatically. Tries, in order: an explicit
# SDK_BUILDER_DIR override, a sibling of this repo, and a sibling of the cache
# (the builder commonly lives next to the cache it fills, not next to this repo).
# On success sets SDK_BUILDER_DIR to the resolved absolute path.
locate_sdk_builder() {
    local candidates=(
        "${SDK_BUILDER_DIR:-}"
        "${REPO_ROOT}/../brightsign-sdk-builder"
        "$(dirname "${CACHE_DIR}")/brightsign-sdk-builder"
    )
    local d
    for d in "${candidates[@]}"; do
        [ -n "${d}" ] || continue
        if [ -f "${d}/Makefile" ]; then
            SDK_BUILDER_DIR="$(cd "${d}" && pwd)"
            return 0
        fi
    done
    return 1
}

if sdk_is_self_consistent; then
    echo "SDK already installed and self-consistent at ${SDK_DIR}"
    exit 0
fi

if [ -e "${SDK_DIR}" ]; then
    echo "ERROR: ${SDK_DIR} exists but is not a self-consistent SDK for this cache" >&2
    echo "(its environment-setup-aarch64-oe-linux does not reference ${SDK_DIR})." >&2
    echo "Remove it and re-run to install fresh: rm -rf ${SDK_DIR}" >&2
    exit 1
fi

# Install from a toolchain installer if one is on hand: $1, else the shared cache
# dir, else the repo root, else CWD.
INSTALLER="${1:-}"
if [ -z "${INSTALLER}" ]; then
    # shellcheck disable=SC2086
    INSTALLER="$(ls ${CACHE_DIR}/${INSTALLER_GLOB} 2>/dev/null | head -n1 || true)"
fi
if [ -z "${INSTALLER}" ]; then
    # shellcheck disable=SC2086
    INSTALLER="$(ls ${REPO_ROOT}/${INSTALLER_GLOB} 2>/dev/null | head -n1 || true)"
fi
if [ -z "${INSTALLER}" ]; then
    # shellcheck disable=SC2086
    INSTALLER="$(ls ./${INSTALLER_GLOB} 2>/dev/null | head -n1 || true)"
fi

if [ -z "${INSTALLER}" ] || [ ! -f "${INSTALLER}" ]; then
    # No prebuilt installer on hand. Unless disabled, build the SDK from source via
    # the brightsign-sdk-builder repo straight into THIS cache, so `make build`
    # auto-provisions a missing SDK instead of just stopping. This is a one-time,
    # Docker-based BrightSign OS build: Docker + network + ~25GB, and it takes
    # HOURS. Set SDK_AUTO_BUILD=0 to get the manual instructions instead.
    if [ "${SDK_AUTO_BUILD:-1}" != "0" ] && locate_sdk_builder; then
        cat <<EOF
================================================================================
No SDK in the cache and no installer on hand -- building it from source.
    builder : ${SDK_BUILDER_DIR}
    cache   : ${CACHE_DIR}
This is a one-time BrightSign OS SDK build (Docker + network + ~25GB; HOURS).
Set SDK_AUTO_BUILD=0 to skip auto-build and get manual instructions instead.
Press Ctrl-C now to abort.
================================================================================
EOF
        # The builder resolves its cache from ARGUS_CACHE_DIR (its own variable,
        # separate repo); pass the absolute cache path this repo resolved -- and
        # BS_CACHE_DIR too, for forward-compatibility -- so it fills THIS cache.
        if [ "${SDK_BUILD_DRY_RUN:-0}" = "1" ]; then
            echo "[dry-run] make -C ${SDK_BUILDER_DIR} build ARGUS_CACHE_DIR=${CACHE_DIR} BS_CACHE_DIR=${CACHE_DIR}"
            exit 0
        fi
        make -C "${SDK_BUILDER_DIR}" build \
            ARGUS_CACHE_DIR="${CACHE_DIR}" BS_CACHE_DIR="${CACHE_DIR}"
        if ! sdk_is_self_consistent; then
            echo "ERROR: builder ran but ${SDK_DIR} is still not a self-consistent SDK." >&2
            echo "Review the brightsign-sdk-builder output above for the failure." >&2
            exit 1
        fi
        echo "SDK built and self-consistent at ${SDK_DIR}"
        exit 0
    fi

    cat <<EOF
No cross-compile SDK found in the shared cache, and no toolchain installer on hand.

The SDK is provisioned once per build box by the brightsign-sdk-builder repo,
which downloads the BrightSign OS source, builds the custom SDK, and installs it
into the shared cache this repo reads. Normally this build auto-runs it; it was
skipped because SDK_AUTO_BUILD=0 or the builder repo was not found.

Build it yourself from the builder checkout:

    make -C /path/to/brightsign-sdk-builder build ARGUS_CACHE_DIR=${CACHE_DIR}

That populates ${SDK_DIR}. Then re-run this build. Point auto-build at a
non-standard builder location with SDK_BUILDER_DIR=/path/to/brightsign-sdk-builder.

Alternatively, if you already have a brightsign-x86_64-cobra-toolchain-*.sh
installer, install from it directly:
    - pass its path:  make fetch-sdk SDK_INSTALLER=/path/to/installer.sh
    - or drop it in the shared cache dir (see 'make cache-info') or the repo root.

Do NOT symlink or copy in an already-extracted sdk/ directory relocated for a
different path -- its baked paths will fail. Always install with -d pointed at the
shared cache path so OE relocation bakes the right path in.
EOF
    exit 1
fi

echo "Installing SDK from ${INSTALLER} into ${SDK_DIR} (self-relocating)..."
chmod +x "${INSTALLER}"
# -y: non-interactive. -d: install (and relocate all baked absolute paths,
# including the cross-toolchain binaries' ELF INTERP/RPATH) into SDK_DIR.
"${INSTALLER}" -y -d "${SDK_DIR}"

if ! sdk_is_self_consistent; then
    echo "ERROR: SDK installed but environment-setup-aarch64-oe-linux does not" >&2
    echo "reference ${SDK_DIR} -- relocation did not take effect as expected." >&2
    exit 1
fi

# The RKNN runtime library ships separately from the BrightSign SDK installer.
# Only fetched here on the installer fallback path; when the brightsign-sdk-builder
# repo provisions the cache it installs librknnrt.so itself. Pinned to the RKNN
# version this repo targets so the runtime, API headers, and compiled models stay
# compatible.
RKNN_TAG="${RKNN_TAG:-v2.3.2}"
RKNN_LIB="${SDK_DIR}/sysroots/aarch64-oe-linux/usr/lib/librknnrt.so"
if [ ! -f "${RKNN_LIB}" ]; then
    echo "Fetching librknnrt.so (Rockchip NPU runtime, ${RKNN_TAG})..."
    wget -O "${RKNN_LIB}" \
        "https://github.com/airockchip/rknn-toolkit2/raw/${RKNN_TAG}/rknpu2/runtime/Linux/librknn_api/aarch64/librknnrt.so"
fi

echo "SDK installed and self-consistent at ${SDK_DIR}"
echo "Source it before building: source ${SDK_DIR}/environment-setup-aarch64-oe-linux"
