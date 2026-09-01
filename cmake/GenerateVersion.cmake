# GenerateVersion.cmake
#
# Run in CMake -P script mode from a custom build step. Computes the current
# git commit hash, branch, dirty state, and UTC build time, then configures
# ${IN} -> ${OUT}. Re-run on every build so the stamp always reflects the
# actual source state that produced the binary.
#
# Expected -D variables: SRC_DIR (repo root), IN (template), OUT (header)

# --- git commit hash (short) ---
execute_process(
    COMMAND git -C "${SRC_DIR}" rev-parse --short=9 HEAD
    OUTPUT_VARIABLE ARGUS_GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT ARGUS_GIT_HASH)
    set(ARGUS_GIT_HASH "unknown")
endif()

# --- git branch ---
execute_process(
    COMMAND git -C "${SRC_DIR}" rev-parse --abbrev-ref HEAD
    OUTPUT_VARIABLE ARGUS_GIT_BRANCH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT ARGUS_GIT_BRANCH)
    set(ARGUS_GIT_BRANCH "unknown")
endif()

# --- dirty flag: "+" if there are uncommitted tracked changes ---
execute_process(
    COMMAND git -C "${SRC_DIR}" status --porcelain --untracked-files=no
    OUTPUT_VARIABLE _git_status
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(_git_status STREQUAL "")
    set(ARGUS_GIT_DIRTY "")
else()
    set(ARGUS_GIT_DIRTY "+")
endif()

# --- UTC build timestamp ---
string(TIMESTAMP ARGUS_BUILD_UTC "%Y-%m-%dT%H:%M:%SZ" UTC)

# Only rewrite OUT if content changed, to avoid needless recompiles.
configure_file("${IN}" "${OUT}.tmp" @ONLY)
if(EXISTS "${OUT}")
    file(READ "${OUT}"     _old)
    file(READ "${OUT}.tmp" _new)
    if(NOT _old STREQUAL _new)
        file(RENAME "${OUT}.tmp" "${OUT}")
    else()
        file(REMOVE "${OUT}.tmp")
    endif()
else()
    file(RENAME "${OUT}.tmp" "${OUT}")
endif()
