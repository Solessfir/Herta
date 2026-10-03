#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repository_root="$(cd -- "${script_dir}/../.." && pwd -P)"
dry_run=false

for argument in "$@"; do
    case "${argument}" in
        --dry-run) dry_run=true ;;
        *) echo "Unknown Cleanup argument: ${argument}" >&2; exit 2 ;;
    esac
done

for marker in premake5.lua Config/Dependencies.lock; do
    if [[ ! -e "${repository_root}/${marker}" ]]; then
        echo "Cleanup could not validate the Herta repository root: ${repository_root}" >&2
        exit 1
    fi
done

remove_herta_path() {
    local target="$1"
    local relative_path

    case "${target}" in
        "${repository_root}"/*) ;;
        *) echo "Cleanup target is outside the Herta repository: ${target}" >&2; exit 1 ;;
    esac

    [[ -e "${target}" || -L "${target}" ]] || return 0
    relative_path="${target#"${repository_root}/"}"

    if [[ "${dry_run}" == true ]]; then
        printf 'Would remove %s\n' "${relative_path}"
        return 0
    fi

    rm -rf -- "${target}"
    printf 'Removed %s\n' "${relative_path}"
}

generated_directories=(
    Binaries
    DerivedDataCache
    Intermediate
    Saved
    SDK
    External/Premake
    TestResults
    .idea
    .vs
)

for directory in "${generated_directories[@]}"; do
    remove_herta_path "${repository_root}/${directory}"
done

for file in Makefile compile_commands.json .DS_Store Desktop.ini Thumbs.db; do
    remove_herta_path "${repository_root}/${file}"
done

shopt -s nullglob
for file in \
    "${repository_root}"/*.code-workspace \
    "${repository_root}"/*.make \
    "${repository_root}"/*.sln \
    "${repository_root}"/*.slnx \
    "${repository_root}"/*.suo \
    "${repository_root}"/*.user \
    "${repository_root}"/*.vcxproj \
    "${repository_root}"/*.vcxproj.filters \
    "${repository_root}"/*.vcxproj.user \
    "${repository_root}"/*.workspace; do
    remove_herta_path "${file}"
done

if [[ "${dry_run}" == true ]]; then
    echo 'Cleanup dry run completed.'
else
    echo 'Herta generated state is clean.'
fi
