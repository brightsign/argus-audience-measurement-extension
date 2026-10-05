#!/usr/bin/env bash
set -euo pipefail

# Bundle the transitive shared-library closure needed by attention_demo (chiefly
# OpenCV's image codecs: libtiff, libwebp, libjpeg, libpng and their deps) from
# the SDK sysroot into the extension's lib dir. BrightSignOS does not ship these,
# so they must travel with the extension or attention_demo fails at startup with
# "libtiff.so.5: cannot open shared object file".
#
# Only app libs under <sysroot>/usr/lib are bundled. Excluded, by design:
#   - core glibc/gcc runtime (libc, libstdc++, libgcc_s, ...): the player provides
#     them and overriding them would break everything.
#   - the gstreamer/glib stack: provided by the ext-bundle and the player; bundling
#     SDK copies would risk version conflicts.
#   - libs already bundled (opencv, rga, rknnrt, turbojpeg, mosquitto).
#
# Uses host readelf, which reads aarch64 ELF headers fine. Best-effort: a soname
# not found as an app lib is left for the player to provide.
#
# Usage: bundle-extra-deps.sh <sysroot> <dest_lib_dir> <root_binary> [root_binary...]

SYSROOT="${1:?usage: bundle-extra-deps.sh <sysroot> <dest_lib_dir> <root_bin...>}"
DEST="${2:?missing dest lib dir}"
shift 2
ROOTS=("$@")
USRLIB="${SYSROOT}/usr/lib"

READELF="$(command -v readelf || true)"
if [ -z "${READELF}" ]; then
    echo "bundle-extra-deps: readelf not found; skipping dependency bundling" >&2
    exit 0
fi

# sonames the player/ext-bundle provides, or that must never be overridden:
#   - core glibc + the C/C++ runtime (the matching BrightSignOS provides these;
#     overriding libc/libstdc++/ld-linux would break the process).
#   - the glib + gstreamer stack, which the ext-bundle ships as its runtime; a
#     second SDK copy would collide with it.
# Everything else reachable from attention_demo (image codecs, openssl for
# mosquitto, opencv_videoio's backends) is bundled.
EXCLUDE='^(ld-linux-aarch64\.|libc\.so|libc-[0-9]|libm\.so|libdl\.so|libpthread\.so|librt\.so|libresolv\.so|libanl\.so|libnss_|libutil\.so|libcrypt\.so|libstdc\+\+\.so|libgcc_s\.so|libgst[a-z]*-1\.0\.|libglib-2\.0\.|libgobject-2\.0\.|libgio-2\.0\.|libgmodule-2\.0\.|libgthread-2\.0\.)'

mkdir -p "${DEST}"
needed() { "${READELF}" -d "$1" 2>/dev/null | awk -F'[][]' '/NEEDED/{print $2}'; }

declare -A seen
queue=()
enqueue() { while IFS= read -r n; do [ -n "$n" ] && queue+=("$n"); done; }

# Process substitution (not a pipe) so enqueue runs in THIS shell and its
# appends to `queue` persist.
for r in "${ROOTS[@]}"; do [ -f "$r" ] && enqueue < <(needed "$r"); done
for f in "${DEST}"/*.so*; do [ -f "$f" ] && enqueue < <(needed "$f"); done

added=0
while [ "${#queue[@]}" -gt 0 ]; do
    soname="${queue[0]}"; queue=("${queue[@]:1}")
    [ -n "${seen[$soname]:-}" ] && continue
    seen[$soname]=1
    [[ "$soname" =~ $EXCLUDE ]] && continue
    if [ -e "${DEST}/${soname}" ]; then
        enqueue < <(needed "${DEST}/${soname}")   # already bundled; still follow its deps
        continue
    fi
    src="${USRLIB}/${soname}"
    [ -e "$src" ] || continue                     # not an app lib; player provides it
    cp -L "$src" "${DEST}/${soname}"
    chmod 0755 "${DEST}/${soname}"
    echo "bundle-extra-deps: + ${soname}"
    added=$((added + 1))
    enqueue < <(needed "$src")
done

echo "bundle-extra-deps: bundled ${added} extra lib(s) into ${DEST}"
