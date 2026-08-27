#!/usr/bin/env bash

# Remove only artifacts recorded by this build's generated Simint install,
# then prove no standalone Simint development/runtime surface remains.
gtf_cleanup_simint() {
    : "${PREFIX:?PREFIX is required}"
    : "${GTF_BUILD_ROOT:?GTF_BUILD_ROOT is required}"

    local simint_manifest="$GTF_BUILD_ROOT/simint/install_manifest.txt"
    local path directory simint_survivors
    local -a simint_artifacts=()
    local -a simint_directories=()

    if [[ ! -s $simint_manifest ]]; then
        echo "No Simint install manifest at $simint_manifest: cannot prove which" \
             "installed files CF-SIMINT-006 must remove" >&2
        return 1
    fi

    # CMake may omit the final newline and may emit redundant path separators.
    # Process the final record and normalize every manifest-owned path.
    while IFS= read -r path || [[ -n $path ]]; do
        [[ -n $path ]] || continue
        path=$(realpath -m -- "$path")
        if [[ $path != "$PREFIX"/* ]]; then
            echo "Simint install manifest lists $path outside \$PREFIX" >&2
            return 1
        fi
        simint_artifacts+=("$path")
    done <"$simint_manifest"

    if ((${#simint_artifacts[@]} == 0)); then
        echo "No installed Simint artifacts found: the build no longer installs" \
             "the generated Simint that CF-SIMINT-006 removes" >&2
        return 1
    fi
    if [[ ! -f "$PREFIX/lib/libsimint.a" ]]; then
        echo "Generated Simint did not install lib/libsimint.a; refusing to" \
             "package an unverified Simint layout" >&2
        return 1
    fi
    if ! printf '%s\n' "${simint_artifacts[@]}" |
         grep -qxF "$PREFIX/lib/libsimint.a"; then
        echo "lib/libsimint.a is not owned by the Simint install manifest;" \
             "refusing to delete a file this build does not own" >&2
        return 1
    fi

    rm -f -- "${simint_artifacts[@]}"

    # Prune emptied manifest-owned directory trees deepest first. The prefix
    # itself is never a candidate, and non-empty dependency directories remain.
    for path in "${simint_artifacts[@]}"; do
        directory=$(dirname -- "$path")
        while [[ $directory == "$PREFIX"/* ]]; do
            simint_directories+=("$directory")
            directory=$(dirname -- "$directory")
        done
    done
    while IFS= read -r directory; do
        rmdir --ignore-fail-on-non-empty -- "$directory" 2>/dev/null || true
    done < <(
        printf '%s\n' "${simint_directories[@]}" |
            awk '{ print length($0), $0 }' |
            sort -k1,1nr -k2,2r |
            cut -d' ' -f2- |
            awk '!seen[$0]++'
    )

    for path in "${simint_artifacts[@]}"; do
        if [[ -e $path ]]; then
            echo "Simint artifact survived removal: $path" >&2
            return 1
        fi
    done

    simint_survivors=$(find "$PREFIX" -iname '*simint*' \
        -not -path "$PREFIX/conda-meta/*" -print)
    if [[ -n $simint_survivors ]]; then
        echo "Simint-named files remain under \$PREFIX but are not owned by this" \
             "build's Simint install manifest; resolve before packaging:" >&2
        printf '%s\n' "$simint_survivors" >&2
        return 1
    fi
}
