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

# Visual Memory Stage 2 embedding model (MobileNetV2 feature extractor). Converted for RK3588 only
# (see libs/visual-memory/models/README.md); other SoCs need their own conversion, so they are skipped.
VM_MODELS="${VM_MODELS_DIR:-${REPO_ROOT}/libs/visual-memory/models}"   # binaries are not in git: see libs/visual-memory/models/README.md
if [ -f "${VM_MODELS}/mobilenet_embedder_fp16.rknn" ]; then
    dest="${REPO_ROOT}/install/RK3588/model/visual_memory"
    mkdir -p "${dest}"
    cp "${VM_MODELS}/mobilenet_embedder_fp16.rknn" "${dest}/mobilenet_embedder.rknn"   # default: FP16 reference
    [ -f "${VM_MODELS}/mobilenet_embedder_int8.rknn" ] && cp "${VM_MODELS}/mobilenet_embedder_int8.rknn" "${dest}/"
    cp "${VM_MODELS}/mobilenet_embedder.json" "${dest}/" 2>/dev/null || true
    echo "  synced Visual Memory embedding model -> install/RK3588/model/visual_memory"
fi

echo "==> Model sync complete"
