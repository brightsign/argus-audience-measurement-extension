#!/usr/bin/env bash
set -euo pipefail

# Copies the compiled RKNN models from the shared cache (CACHE_DIR/models/<SOC>)
# into this repo's install/<SOC>/model tree, where the package script expects
# them. Models are compiled once into the cache (see scripts/build-models.sh /
# `make build-models`) and shared across sibling repos; this is the per-repo
# copy step at package time.

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck disable=SC1091
source "$(dirname "${BASH_SOURCE[0]}")/lib/cache.sh"   # sets MODELS_DIR

SOCS=(RK3588 RK3576 RK3568)
REQUIRED="RetinaFace.rknn"

missing=0
for soc in "${SOCS[@]}"; do
    src="${MODELS_DIR}/${soc}"
    if [ ! -f "${src}/${REQUIRED}" ]; then
        echo "WARNING: no compiled models for ${soc} in ${src}" >&2
        missing=1
        continue
    fi
    dest="${REPO_ROOT}/install/${soc}/model"
    mkdir -p "${dest}"
    cp -a "${src}/." "${dest}/"
    echo "  synced ${soc}: ${src} -> install/${soc}/model"
done

if [ "${missing}" = "1" ]; then
    cat >&2 <<EOF

Some SoC models are missing from the cache. Compile them with:
    make build-models
(needs the cached RKNN toolkit + the rknn_tk2 image from ../brightsign-sdk-builder).
EOF
fi

echo "==> Model sync complete"
