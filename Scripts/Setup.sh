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

install_directory="${repository_root}/External/Premake/Linux/${premake_version}"
premake_path="${install_directory}/${premake_entry}"
download_directory="${repository_root}/External/Premake/.Downloads"
archive_path="${download_directory}/$(basename -- "${premake_url}")"
temporary_archive=""
temporary_directory=""
compiler_probe_directory=""

cleanup() {
    [[ -z "${temporary_archive}" || ! -e "${temporary_archive}" ]] || rm -f -- "${temporary_archive}"
    [[ -z "${temporary_directory}" || ! -e "${temporary_directory}" ]] || rm -rf -- "${temporary_directory}"
    [[ -z "${compiler_probe_directory}" || ! -e "${compiler_probe_directory}" ]] || rm -rf -- "${compiler_probe_directory}"
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

print_prerequisite_command() {
    if [[ -f /etc/os-release ]]; then
        source /etc/os-release
    fi
    case "${ID:-}:${ID_LIKE:-}" in
        *ubuntu*)
            echo 'Install them with: sudo apt-get update && sudo apt-get install -y gcc-14 g++-14 git make coreutils tar curl' >&2
            echo 'Then select GCC 14 with: export CC=gcc-14 CXX=g++-14' >&2
            ;;
        *debian*) echo 'Install Git, Make, coreutils, tar, curl, and GCC 14 or newer with your configured Debian repositories.' >&2 ;;
        *fedora*|*rhel*) echo 'Install them with: sudo dnf install -y git make gcc-c++ coreutils tar curl' >&2 ;;
        *arch*) echo 'Install them with: sudo pacman -S --needed git make gcc coreutils tar curl' >&2 ;;
        *) echo 'Install Git, Make, a C++ compiler, coreutils, tar, and curl with your distribution package manager.' >&2 ;;
    esac
}

if [[ "${print_premake_path}" == true ]]; then
    if ! validate_premake "${premake_path}"; then
        echo "Premake is not installed at '${premake_path}'. Run Setup.sh first." >&2
        exit 1
    fi

    printf '%s\n' "${premake_path}"
    exit 0
fi

missing_tools=()
for required_tool in git make sha256sum tar; do
    if ! command -v "${required_tool}" >/dev/null 2>&1; then
        missing_tools+=("${required_tool}")
    fi
done
if ! command -v curl >/dev/null 2>&1 && ! command -v wget >/dev/null 2>&1; then
    missing_tools+=("curl or wget")
fi
if [[ ${#missing_tools[@]} -gt 0 ]]; then
    echo "Setup is missing: ${missing_tools[*]}" >&2
    print_prerequisite_command
    exit 1
fi

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
    echo 'Setup requires a C++23-capable GCC or Clang compiler.' >&2
    print_prerequisite_command
    exit 1
fi

compiler_probe_directory="$(mktemp -d)"
cat > "${compiler_probe_directory}/Probe.cpp" <<'EOF'
#include <expected>
#include <print>

int main()
{
    const std::expected<int, int> value = 42;
    std::println("Herta C++23 probe");
    return value.value() == 42 ? 0 : 1;
}
EOF
if ! "${cxx}" -std=c++23 -Wall -Wextra -Werror "${compiler_probe_directory}/Probe.cpp" -o "${compiler_probe_directory}/Probe" ||
   ! "${compiler_probe_directory}/Probe" >/dev/null; then
    echo "Compiler '${cxx}' failed the required C++23 library probe for <expected> and <print>." >&2
    print_prerequisite_command
    exit 1
fi
rm -rf -- "${compiler_probe_directory}"
compiler_probe_directory=""

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

rm -f -- "${archive_path}"
rmdir -- "${download_directory}" 2>/dev/null || true

echo "Premake ${premake_version}: ${premake_path}"
if command -v blender >/dev/null 2>&1; then
    echo "Optional Blender integration: $(blender --version 2>&1 | head -n 1)"
else
    echo "Optional Blender integration: not detected"
fi
echo "Herta setup completed."
