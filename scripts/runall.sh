#!/usr/bin/env bash
set -euo pipefail

# Thin orchestrator over the Makefile. The heavy, box-level provisioning (the
# cross-compile SDK, the RKNN toolkit, the rknn_tk2 / bsoe-build images) now lives
# in the sibling brightsign-sdk-builder repo, which fills a shared build cache
# once per build box. This repo is a consumer: it detects those assets and builds
# against them. Everything here delegates to `make` -- `make package` does the
# same thing as running this with no flags.
#
# Prerequisite (once per build box):
#     cd ../brightsign-sdk-builder && make build
#
# Usage:
#     scripts/runall.sh            # fetch-sdk -> build-models -> build -> package
#     scripts/runall.sh --auto     # accepted for backwards compat (make is non-interactive)
#     scripts/runall.sh --clean    # remove this repo's build artifacts + optional cache/images
#     scripts/runall.sh --help
#
# Honors DEMO_MODE / FORCE_UPDATE / ARGUS_CACHE_DIR from the environment, which
# are passed through to make.

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; NC='\033[0m'
info()  { echo -e "${GREEN}[INFO]${NC} $*"; }
warn()  { echo -e "${YELLOW}[WARNING]${NC} $*"; }
err()   { echo -e "${RED}[ERROR]${NC} $*"; }
header(){ echo -e "\n${BLUE}=== $* ===${NC}\n"; }

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}"

DO_CLEAN=false
for arg in "$@"; do
    case "${arg}" in
        --auto) ;;                      # make is already non-interactive
        --clean) DO_CLEAN=true ;;
        -h|--help)
            cat <<'EOF'
Thin orchestrator over the Makefile. The SDK, RKNN toolkit, and container images
are provisioned once per build box by the sibling brightsign-sdk-builder repo into
a shared cache; this repo builds against it. `make package` does the same as a
no-flag run here.

Prerequisite (once per build box):
    cd ../brightsign-sdk-builder && make build

Usage:
    scripts/runall.sh            # fetch-sdk -> build-models -> build -> package
    scripts/runall.sh --auto     # accepted for backwards compat (make is non-interactive)
    scripts/runall.sh --clean    # remove this repo's build artifacts + optional cache/images
    scripts/runall.sh --help

Honors DEMO_MODE / FORCE_UPDATE / ARGUS_CACHE_DIR from the environment.
EOF
            exit 0 ;;
        *) err "Unknown option: ${arg}"; exit 1 ;;
    esac
done

if [ "${DO_CLEAN}" = true ]; then
    header "Clean"
    make clean-all
    read -r -p "Also remove the SHARED build cache (affects every project)? (y/N): " reply
    if [[ "${reply}" =~ ^[Yy] ]]; then
        make cache-clean
    fi
    read -r -p "Also remove the bsoe-build / rknn_tk2 Docker images? (y/N): " reply
    if [[ "${reply}" =~ ^[Yy] ]]; then
        docker rmi bsoe-build rknn_tk2 2>/dev/null || true
    fi
    info "Clean complete."
    exit 0
fi

header "BrightSign NPU Argus Extension - Build"
info "Delegating to make (SDK/toolkit/models from the shared cache)."

make fetch-sdk
make build-models
make build
# MP4 support is optional and needs the builder's OE tree present; never fatal.
make build-gst-plugins || warn "GStreamer MP4 plugins not built (optional) -- continuing."
make sync-models
./package

header "Done"
info "Development package: argus-dev-*.zip"
info "Production extension: argus-ext-*.zip"
ls -lh argus-*.zip 2>/dev/null || warn "No argus-*.zip files found!"
