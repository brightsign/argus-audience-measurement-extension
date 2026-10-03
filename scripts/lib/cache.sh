#!/usr/bin/env bash
# Single source of truth for the shared build cache location and the paths
# derived from it. The SDK, the RKNN toolkit clones, and the compiled models are
# expensive to produce and are identical across projects, so they live in a
# cache OUTSIDE any one repo and are built once (by the brightsign-sdk-builder
# repo) and reused. This repo (argus-audience-measurement-extension) is a
# consumer: it reads the cache, it does not provision it.
#
# Override the location with ARGUS_CACHE_DIR (absolute recommended; a relative
# value is resolved against the repo root). The default sits parallel to the
# repo so it is "next to" the checkout, not inside it, and every sibling
# checkout agrees on it.
#
# Two ways to use it:
#   source scripts/lib/cache.sh                 # sets CACHE_DIR SDK_DIR SDK_ENV TOOLKIT_DIR MODELS_DIR
#   bash scripts/lib/cache.sh print <key>       # prints one path, for Makefile $(shell ...)
#       keys: cache | sdk | sdk-env | toolkit | models
#
# A caller that sets REPO_ROOT before sourcing wins; otherwise REPO_ROOT is
# derived from this file's location (repo/scripts/lib/cache.sh -> repo).

_cache_lib_dir="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
REPO_ROOT="${REPO_ROOT:-$(cd "${_cache_lib_dir}/../.." && pwd)}"

if [ -n "${ARGUS_CACHE_DIR:-}" ]; then
    case "${ARGUS_CACHE_DIR}" in
        /*) CACHE_DIR="${ARGUS_CACHE_DIR}" ;;
        *)  CACHE_DIR="${REPO_ROOT}/${ARGUS_CACHE_DIR}" ;;
    esac
else
    CACHE_DIR="${REPO_ROOT}/../argus-build-cache"
fi
# Normalize to a clean absolute path without requiring the dir to exist yet; the
# SDK installer bakes this path in, so it must not contain unresolved '..'.
CACHE_DIR="$(realpath -m "${CACHE_DIR}" 2>/dev/null || echo "${CACHE_DIR}")"

SDK_DIR="${CACHE_DIR}/sdk"
SDK_ENV="${SDK_DIR}/environment-setup-aarch64-oe-linux"
TOOLKIT_DIR="${CACHE_DIR}/toolkit"
MODELS_DIR="${CACHE_DIR}/models"

export CACHE_DIR SDK_DIR SDK_ENV TOOLKIT_DIR MODELS_DIR

if [ "${1:-}" = "print" ]; then
    case "${2:-cache}" in
        cache)   echo "${CACHE_DIR}" ;;
        sdk)     echo "${SDK_DIR}" ;;
        sdk-env) echo "${SDK_ENV}" ;;
        toolkit) echo "${TOOLKIT_DIR}" ;;
        models)  echo "${MODELS_DIR}" ;;
        *)       echo "unknown cache key: ${2}" >&2; exit 1 ;;
    esac
fi
