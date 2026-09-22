#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repository_root="$(cd -- "${script_dir}/.." && pwd)"
lock_path="${repository_root}/Config/Dependencies.lock"
print_premake_path=false
print_vulkan_sdk_path=false
validate_only=false

source "${script_dir}/DependencyLock.sh"

for argument in "$@"; do
    case "${argument}" in
        --print-premake-path) print_premake_path=true ;;
        --print-vulkan-sdk-path) print_vulkan_sdk_path=true ;;
        --validate-only) validate_only=true ;;
        *) echo "Unknown Setup argument: ${argument}" >&2; exit 2 ;;
    esac
done

read_herta_dependency_lock "${lock_path}"
get_herta_premake_dependency "linux-x64"
get_herta_vulkan_dependency "linux-x64"

if [[ "${validate_only}" == true ]]; then
    echo "Validated ${herta_dependency_count} dependency lock entries."
    exit 0
fi

if [[ "$(uname -s)" != "Linux" || "$(uname -m)" != "x86_64" ]]; then
    echo "Herta Setup currently supports Linux x86_64 only on this path." >&2
    exit 1
fi

premake_install_directory="${repository_root}/External/Premake/Linux/${premake_version}"
premake_path="${premake_install_directory}/${premake_entry}"
premake_download_directory="${repository_root}/External/Premake/.Downloads"
premake_archive_path="${premake_download_directory}/$(basename -- "${premake_url}")"
vulkan_install_directory="${repository_root}/SDK/Linux/Vulkan/${vulkan_version}"
vulkan_header_path="${vulkan_install_directory}/${vulkan_entry}"
vulkan_loader_path="${vulkan_install_directory}/x86_64/lib/VulkanLoader/lib/libvulkan.so"
vulkan_info_path="${vulkan_install_directory}/x86_64/bin/vulkaninfo"
vulkan_download_directory="${repository_root}/SDK/Linux/Vulkan/.Downloads"
vulkan_archive_path="${vulkan_download_directory}/$(basename -- "${vulkan_url}")"
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

validate_vulkan_sdk() {
    local header_path="$1"
    local loader_path="$2"
    local vulkan_info_path="$3"
    local sdk_root
    sdk_root="$(dirname -- "$(dirname -- "${vulkan_info_path}")")"

    [[ -s "${header_path}" && -s "${loader_path}" && -x "${vulkan_info_path}" &&
       -s "${sdk_root}/include/slang/slang.h" && -s "${sdk_root}/include/slang/slang-com-ptr.h" &&
       -s "${sdk_root}/lib/libslang-compiler.so" && -x "${sdk_root}/bin/slangc" ]]
}

download_verified_archive() {
    local dependency_name="$1"
    local dependency_version="$2"
    local dependency_url="$3"
    local dependency_sha256="$4"
    local destination_path="$5"
    local destination_directory

    destination_directory="$(dirname -- "${destination_path}")"
    mkdir -p -- "${destination_directory}"

    if [[ -f "${destination_path}" ]] && ! echo "${dependency_sha256}  ${destination_path}" | sha256sum --check --status; then
        rm -f -- "${destination_path}"
    fi

    if [[ -f "${destination_path}" ]]; then
        return
    fi

    temporary_archive="${destination_path}.$$.tmp"
    echo "Downloading ${dependency_name} ${dependency_version}..."
    if command -v curl >/dev/null 2>&1; then
        curl --fail --location --retry 3 --output "${temporary_archive}" "${dependency_url}"
    elif command -v wget >/dev/null 2>&1; then
        wget --output-document="${temporary_archive}" "${dependency_url}"
    else
        echo "Setup requires curl or wget to download ${dependency_name}." >&2
        exit 1
    fi

    echo "${dependency_sha256}  ${temporary_archive}" | sha256sum --check --status || {
        echo "${dependency_name} download SHA-256 mismatch." >&2
        exit 1
    }
    mv -- "${temporary_archive}" "${destination_path}"
    temporary_archive=""
}

print_prerequisite_command() {
    if [[ -f /etc/os-release ]]; then
        source /etc/os-release
    fi
    case "${ID:-}:${ID_LIKE:-}" in
        *ubuntu*)
            if [[ "${CXX:-}" == *clang* ]]; then
                echo 'Install them with: sudo apt-get update && sudo apt-get install -y clang-19 gcc-14 g++-14 git make coreutils tar curl pkg-config xorg-dev libwayland-dev libwayland-bin libxkbcommon-dev' >&2
                echo 'Then select Clang 19 with: export CC=clang-19 CXX=clang++-19' >&2
            else
                echo 'Install them with: sudo apt-get update && sudo apt-get install -y gcc-14 g++-14 git make coreutils tar curl pkg-config xorg-dev libwayland-dev libwayland-bin libxkbcommon-dev' >&2
                echo 'Then select GCC 14 with: export CC=gcc-14 CXX=g++-14' >&2
            fi
            ;;
        *debian*) echo 'Install GCC 14 or newer, Git, Make, coreutils, tar, curl, pkg-config, X11 development packages, Wayland development tools, and libxkbcommon development headers.' >&2 ;;
        *fedora*|*rhel*) echo 'Install them with: sudo dnf install -y git make gcc-c++ coreutils tar curl pkgconf-pkg-config libXcursor-devel libXi-devel libXinerama-devel libXrandr-devel wayland-devel libxkbcommon-devel' >&2 ;;
        *arch*) echo 'Install them with: sudo pacman -S --needed git make gcc coreutils tar curl pkgconf libx11 libxrandr libxinerama libxcursor libxi wayland libxkbcommon' >&2 ;;
        *) echo 'Install Git, Make, a C++ compiler, coreutils, tar, curl, pkg-config, X11 development packages, Wayland development tools, and libxkbcommon development headers.' >&2 ;;
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

if [[ "${print_vulkan_sdk_path}" == true ]]; then
    if ! validate_vulkan_sdk "${vulkan_header_path}" "${vulkan_loader_path}" "${vulkan_info_path}"; then
        echo "Vulkan SDK is not installed at '${vulkan_install_directory}'. Run Setup.sh first." >&2
        exit 1
    fi

    printf '%s\n' "${vulkan_install_directory}"
    exit 0
fi

missing_tools=()
for required_tool in git make sha256sum tar pkg-config wayland-scanner; do
    if ! command -v "${required_tool}" >/dev/null 2>&1; then
        missing_tools+=("${required_tool}")
    fi
done
if ! command -v curl >/dev/null 2>&1 && ! command -v wget >/dev/null 2>&1; then
    missing_tools+=("curl or wget")
fi

required_packages=(x11 xrandr xinerama xcursor xi wayland-client wayland-cursor xkbcommon)
missing_packages=()
if command -v pkg-config >/dev/null 2>&1; then
    for required_package in "${required_packages[@]}"; do
        if ! pkg-config --exists "${required_package}"; then
            missing_packages+=("${required_package}")
        fi
    done
fi
if [[ ${#missing_packages[@]} -gt 0 ]]; then
    echo "Setup is missing Linux window-system development packages: ${missing_packages[*]}" >&2
    print_prerequisite_command
    exit 1
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
compiler_probe_flags=()
if [[ -n "${CXXFLAGS:-}" ]]; then
    read -r -a compiler_probe_flags <<< "${CXXFLAGS}"
fi
cat > "${compiler_probe_directory}/Probe.cpp" <<'EOF'
#include <expected>
#include <functional>
#include <print>

int main()
{
    const std::expected<int, int> value = 42;
    std::move_only_function<int()> callback = [] { return 42; };
    std::println("Herta C++23 probe");
    return value.value() == callback() ? 0 : 1;
}
EOF
if ! "${cxx}" "${compiler_probe_flags[@]}" -std=c++23 -Wall -Wextra -Werror "${compiler_probe_directory}/Probe.cpp" -o "${compiler_probe_directory}/Probe" ||
   ! "${compiler_probe_directory}/Probe" >/dev/null; then
    echo "Compiler '${cxx}' failed the required C++23 library probe for <expected>, <functional>, and <print>." >&2
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
    download_verified_archive "Premake" "${premake_version}" "${premake_url}" "${premake_sha256}" "${premake_archive_path}"
    mkdir -p -- "$(dirname -- "${premake_install_directory}")"

    if [[ -e "${premake_install_directory}" ]]; then
        echo "Premake install directory exists but is invalid: ${premake_install_directory}" >&2
        exit 1
    fi

    temporary_directory="$(dirname -- "${premake_install_directory}")/.${premake_version}.$$.tmp"
    mkdir -- "${temporary_directory}"
    tar -xzf "${premake_archive_path}" -C "${temporary_directory}"
    chmod +x "${temporary_directory}/${premake_entry}"
    validate_premake "${temporary_directory}/${premake_entry}"
    mv -- "${temporary_directory}" "${premake_install_directory}"
    temporary_directory=""
fi

rm -f -- "${premake_archive_path}"
rmdir -- "${premake_download_directory}" 2>/dev/null || true

if ! validate_vulkan_sdk "${vulkan_header_path}" "${vulkan_loader_path}" "${vulkan_info_path}"; then
    download_verified_archive "Vulkan SDK" "${vulkan_version}" "${vulkan_url}" "${vulkan_sha256}" "${vulkan_archive_path}"
    mkdir -p -- "$(dirname -- "${vulkan_install_directory}")"

    if [[ -e "${vulkan_install_directory}" ]]; then
        echo "Vulkan SDK install directory exists but is invalid: ${vulkan_install_directory}" >&2
        exit 1
    fi

    temporary_directory="$(dirname -- "${vulkan_install_directory}")/.${vulkan_version}.$$.tmp"
    mkdir -- "${temporary_directory}"
    tar -xJf "${vulkan_archive_path}" -C "${temporary_directory}"

    staged_vulkan_directory="${temporary_directory}"
    if [[ -d "${temporary_directory}/${vulkan_version}" ]]; then
        staged_vulkan_directory="${temporary_directory}/${vulkan_version}"
    fi
    if ! validate_vulkan_sdk "${staged_vulkan_directory}/${vulkan_entry}" "${staged_vulkan_directory}/x86_64/lib/VulkanLoader/lib/libvulkan.so" "${staged_vulkan_directory}/x86_64/bin/vulkaninfo"; then
        echo 'Vulkan SDK archive did not contain the required headers and tools.' >&2
        exit 1
    fi

    mv -- "${staged_vulkan_directory}" "${vulkan_install_directory}"
    if [[ "${staged_vulkan_directory}" == "${temporary_directory}" ]]; then
        temporary_directory=""
    else
        rm -rf -- "${temporary_directory}"
        temporary_directory=""
    fi
fi

rm -f -- "${vulkan_archive_path}"
rmdir -- "${vulkan_download_directory}" 2>/dev/null || true

echo "Premake ${premake_version}: ${premake_path}"
echo "Vulkan SDK ${vulkan_version}: ${vulkan_install_directory}"
if command -v blender >/dev/null 2>&1; then
    echo "Optional Blender integration: $(blender --version 2>&1 | head -n 1)"
else
    echo "Optional Blender integration: not detected"
fi
echo "Herta setup completed."
