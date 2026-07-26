#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repository_root="$(cd -- "${script_dir}/.." && pwd)"
lock_path="${repository_root}/Config/Dependencies.lock"
print_premake_path=false
validate_only=false

source "${script_dir}/DependencyLock.sh"

for argument in "$@"; do
    case "${argument}" in
        --print-premake-path) print_premake_path=true ;;
        --validate-only) validate_only=true ;;
        *) echo "Unknown Setup argument: ${argument}" >&2; exit 2 ;;
    esac
done

read_herta_dependency_lock "${lock_path}"
get_herta_premake_dependency "linux-x64"

if [[ "${validate_only}" == true ]]; then
    echo "Validated ${herta_dependency_count} dependency lock entries."
    exit 0
fi

if [[ "$(uname -s)" != "Linux" || "$(uname -m)" != "x86_64" ]]; then
    echo "Herta Setup currently supports Linux x86_64 only on this path." >&2
    exit 1
fi

install_directory="${repository_root}/SDK/Linux/Premake/${premake_version}"
premake_path="${install_directory}/${premake_entry}"
download_directory="${repository_root}/SDK/.Downloads"
archive_path="${download_directory}/$(basename -- "${premake_url}")"
temporary_archive=""
temporary_directory=""

cleanup() {
    [[ -z "${temporary_archive}" || ! -e "${temporary_archive}" ]] || rm -f -- "${temporary_archive}"
    [[ -z "${temporary_directory}" || ! -e "${temporary_directory}" ]] || rm -rf -- "${temporary_directory}"
}
trap cleanup EXIT

validate_premake() {
    local executable="$1"
    local version_output

    [[ -x "${executable}" ]] || return 1
    if ! version_output="$(cd /tmp && "${executable}" --version 2>&1)"; then
        echo "Premake at '${executable}' failed version validation: ${version_output}" >&2
        return 1
    fi
    if [[ "${version_output}" != *"${premake_version}"* ]]; then
        echo "Premake at '${executable}' did not report expected version '${premake_version}'. Output: ${version_output}" >&2
        return 1
    fi
}

if [[ "${print_premake_path}" == true ]]; then
    if ! validate_premake "${premake_path}"; then
        echo "Premake is not installed at '${premake_path}'. Run Setup.sh first." >&2
        exit 1
    fi

    printf '%s\n' "${premake_path}"
    exit 0
fi

for required_tool in git make sha256sum tar; do
    if ! command -v "${required_tool}" >/dev/null 2>&1; then
        echo "Setup requires '${required_tool}' on PATH. Install it with your distribution package manager." >&2
        exit 1
    fi
done

cxx="${CXX:-}"
if [[ -z "${cxx}" ]]; then
    for candidate in c++ g++ clang++; do
        if command -v "${candidate}" >/dev/null 2>&1; then
            cxx="${candidate}"
            break
        fi
    done
fi
if [[ -z "${cxx}" || ! -x "$(command -v "${cxx}" 2>/dev/null || true)" ]]; then
    echo "Setup requires a C++23-capable GCC or Clang compiler. Set CXX or install the compiler with your distribution package manager." >&2
    exit 1
fi

echo "C++ compiler: $(command -v "${cxx}")"
git -C "${repository_root}" submodule sync --recursive || {
    echo "Failed to synchronize Git submodule URLs." >&2
    exit 1
}
git -C "${repository_root}" submodule update --init --recursive || {
    echo "Failed to initialize Git submodules. Check network access and repository credentials." >&2
    exit 1
}

if ! validate_premake "${premake_path}"; then
    mkdir -p -- "${download_directory}" "$(dirname -- "${install_directory}")"

    if [[ -f "${archive_path}" ]] && ! echo "${premake_sha256}  ${archive_path}" | sha256sum --check --status; then
        rm -f -- "${archive_path}"
    fi

    if [[ ! -f "${archive_path}" ]]; then
        temporary_archive="${archive_path}.$$.tmp"
        echo "Downloading Premake ${premake_version}..."
        if command -v curl >/dev/null 2>&1; then
            curl --fail --location --retry 3 --output "${temporary_archive}" "${premake_url}"
        elif command -v wget >/dev/null 2>&1; then
            wget --output-document="${temporary_archive}" "${premake_url}"
        else
            echo "Setup requires curl or wget to download Premake." >&2
            exit 1
        fi

        echo "${premake_sha256}  ${temporary_archive}" | sha256sum --check --status || {
            echo "Premake archive SHA-256 mismatch." >&2
            exit 1
        }
        mv -- "${temporary_archive}" "${archive_path}"
        temporary_archive=""
    fi

    if [[ -e "${install_directory}" ]]; then
        echo "Premake install directory exists but is invalid: ${install_directory}" >&2
        exit 1
    fi

    temporary_directory="$(dirname -- "${install_directory}")/.${premake_version}.$$.tmp"
    mkdir -- "${temporary_directory}"
    tar -xzf "${archive_path}" -C "${temporary_directory}"
    chmod +x "${temporary_directory}/${premake_entry}"
    validate_premake "${temporary_directory}/${premake_entry}"
    mv -- "${temporary_directory}" "${install_directory}"
    temporary_directory=""
fi

echo "Premake ${premake_version}: ${premake_path}"
if command -v blender >/dev/null 2>&1; then
    echo "Optional Blender integration: $(blender --version 2>&1 | head -n 1)"
else
    echo "Optional Blender integration: not detected"
fi
echo "Herta setup completed."
