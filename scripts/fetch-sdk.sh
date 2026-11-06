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
    cat <<EOF
No cross-compile SDK found in the shared cache, and no toolchain installer on hand.

The SDK is provisioned once per build box by the brightsign-sdk-builder repo,
which downloads the BrightSign OS source, builds the custom SDK, and installs it
into a shared cache. Do this once per build box:

    git clone https://github.com/brightsign/brightsign-sdk-builder.git
    cd brightsign-sdk-builder && make build

That populates ${SDK_DIR}. The cache lives outside any single repo, so after that
one (multi-hour) build this and our other example extensions all cross-compile
against the same cached SDK -- no per-repo SDK rebuild. Then re-run this build.

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
