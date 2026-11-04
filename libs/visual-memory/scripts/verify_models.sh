#!/usr/bin/env bash
# Verify the Visual Memory model binaries against the pinned hashes in models/README.md.
# usage: verify_models.sh [models_dir]   (default: ../models, or $VM_MODELS_DIR)
# Note: .rknn files are not byte-reproducible; the hashes identify the SHIPPED files, not a rebuild.
set -u
DIR="${1:-${VM_MODELS_DIR:-$(cd "$(dirname "$0")/../models" && pwd)}}"
rc=0
check() {
  if [ ! -f "$DIR/$1" ]; then echo "MISSING  $1 (fetch it from the artifact store)"; rc=1; return; fi
  got=$(sha256sum "$DIR/$1" | cut -d' ' -f1)
  if [ "$got" = "$2" ]; then echo "OK       $1"; else echo "MISMATCH $1 (got $got)"; rc=1; fi
}
check mobilenet_v2_embedder.onnx     ab4ad1b40609324a3bcda0442637416a89f7bb445be2c806c05550dd9dd844b2
check mobilenet_embedder_fp16.rknn   d5f3e467b0206a73fa57d6790b76fe415dbd87b821222c0f37282e83d7644e36
check mobilenet_embedder_int8.rknn   b19606d53c7bd10a096de2773087535d6dab23042ba1afea73d638329400ba49
exit $rc
