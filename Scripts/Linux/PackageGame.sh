#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repository_root="$(cd -- "${script_dir}/../.." && pwd)"
project=Games/Sandbox/Sandbox.hertaproject
configuration=Shipping
output_directory=""
content_mode=package
signing_key=""
verify=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --verify) verify=1; shift ;;
        --project|--configuration|--repository-root|--output-directory|--content-mode|--signing-key)
            if [[ $# -lt 2 || -z "$2" ]]; then
                echo "Missing value for $1." >&2
                exit 2
            fi

            case "$1" in
                --project) project="$2" ;;
                --configuration) configuration="$2" ;;
                --repository-root) repository_root="$2" ;;
                --output-directory) output_directory="$2" ;;
                --content-mode) content_mode="$2" ;;
                --signing-key) signing_key="$(realpath -- "$2")" ;;
            esac
            shift 2
            ;;
        *) echo "Unknown PackageGame argument: $1" >&2; exit 2 ;;
    esac
done

case "${configuration}" in
    Shipping|Development) ;;
    *) echo 'Configuration must be Shipping or Development.' >&2; exit 2 ;;
esac

# Loose stages the content as files, package puts it in <Project>.hpak beside the executables, and embedded builds that package into them.
case "${content_mode}" in
    loose|package|embedded) ;;
    *) echo 'Content mode must be loose, package, or embedded.' >&2; exit 2 ;;
esac

repository_root="$(cd -- "${repository_root}" && pwd -P)"
output_directory="$(realpath -m -- "${output_directory:-${repository_root}/Intermediate/Packages}")"
project_path="$(realpath -- "${repository_root}/${project}")"
project_name="$(basename -- "${project_path}" .hertaproject)"
# Staging puts the project in a <Project> folder, so the executable needs a suffix to sit beside it.
executable_name="${project_name}.x86_64"
package_name="${project_name}-linux-x86_64-${configuration}"
package_root="${output_directory}/${package_name}"
archive_path="${output_directory}/${package_name}.tar.gz"
binary_directory="${repository_root}/Binaries/linux/x86_64/${configuration}"

if [[ -e "${package_root}" || -L "${package_root}" || -e "${archive_path}" || -L "${archive_path}" ]]; then
    echo "Package output already exists. Choose an empty output directory: ${output_directory}" >&2
    exit 1
fi

for program in HertaGame HertaServer HertaCooker HertaAssetWorker HertaShaderWorker; do
    if [[ ! -x "${binary_directory}/${program}" ]]; then
        echo "Build ${configuration} before packaging; missing ${program} in ${binary_directory}" >&2
        exit 1
    fi
done

shopt -s nullglob
shaders=("${binary_directory}"/Shaders/*.hshader)
if [[ ${#shaders[@]} -eq 0 ]]; then
    echo "Cooked shaders are missing from ${binary_directory}/Shaders" >&2
    exit 1
fi

signing_arguments=()
if [[ -n "${signing_key}" ]]; then
    signing_arguments=("--signing-key=${signing_key}")
fi

# Cooking stages the starting level's dependency closure through the asset worker, then writes it loose, as a package with the engine
# shaders, or as that package embedded in both executables.
cook() {
    "${binary_directory}/HertaCooker" cook "--project=${project_path}" "--output=$1" "--mode=$2" "--name=${project_name}" "--shaders=${binary_directory}/Shaders" "${@:3}" "${signing_arguments[@]}"
}

mkdir -p -- "${package_root}"
cp -- "${binary_directory}/HertaGame" "${package_root}/${executable_name}"
cp -- "${binary_directory}/HertaServer" "${package_root}/${executable_name}-server"
case "${content_mode}" in
    loose)
        cook "${package_root}" loose
        mkdir -p -- "${package_root}/Shaders"
        cp -- "${shaders[@]}" "${package_root}/Shaders/"
        ;;
    package) cook "${package_root}" package ;;
    embedded) cook "${package_root}" embedded "--embed-into=${package_root}/${executable_name},${package_root}/${executable_name}-server" ;;
esac

cp -- "${repository_root}/LICENSE" "${package_root}/LICENSE"
# These dependencies embed their notices in source files rather than separate license files.
license_files=(External/spdlog/include/spdlog/fmt/bundled/format.h External/freetype/src/gzip/zlib.h)
while IFS= read -r tracked_file; do
    name="$(basename -- "${tracked_file}")"
    if [[ "${tracked_file}" == External/* && "${name,,}" =~ (license|licence|copying|notice|^ofl\.|^ftl\.) ]]; then
        license_files+=("${tracked_file}")
    fi
done < <(git -c core.quotepath=false -C "${repository_root}" ls-files --recurse-submodules)

for license_file in "${license_files[@]}"; do
    mkdir -p -- "${package_root}/ThirdPartyLicenses/$(dirname -- "${license_file}")"
    cp -- "${repository_root}/${license_file}" "${package_root}/ThirdPartyLicenses/${license_file}"
done

revision="$(git -C "${repository_root}" rev-parse HEAD)"
cat > "${package_root}/README.txt" <<README
${project_name} - Herta game, Linux x86_64 ${configuration}
Revision: ${revision}

Run ./${executable_name}. Settings and logs are kept per user in \${XDG_CONFIG_HOME:-~/.config}/Herta/${project_name}.
A Vulkan-capable GPU, driver, and system Vulkan loader are required, with X11 or Wayland.

WASD moves, Space jumps and wall jumps, Shift sprints, C or Ctrl crouches and slides, R restarts the course, Esc opens the menu, and F11 or Alt+Enter toggles fullscreen.

Multiplayer: ./${executable_name} --host[=port] hosts a game and plays it; others join with ./${executable_name} --connect=address:port and an optional --name=Name.
./${executable_name}-server runs a dedicated server without a window, GPU, or audio device; it needs only OpenSSL's libcrypto and takes --port=, which defaults to 27015, and --max-players=.

Herta is licensed under MIT. See LICENSE and ThirdPartyLicenses; the HUD font license is in Engine/Content/Fonts.
This software is based in part on the work of the FreeType Team (https://freetype.org).
README

if [[ ${verify} -eq 1 ]]; then
    # Running a copy outside the checkout proves the package needs nothing from the source tree.
    relocated="$(mktemp -d)"
    trap 'rm -rf -- "${relocated}"' EXIT
    cp -r -- "${package_root}/." "${relocated}/"
    if ! (cd -- "${relocated}" && "./${executable_name}" --smoke-test=600); then
        echo "The relocated game failed its scripted run." >&2
        exit 1
    fi

    echo "Relocated game passed its scripted run from ${relocated}"

    # The dedicated server must start from the package alone, and a hosted game must play through its own loopback client.
    if ! (cd -- "${relocated}" && "./${executable_name}-server" --port=0 --ticks=120); then
        echo "The relocated dedicated server failed; see ${relocated}/Saved/Logs/Server.log." >&2
        exit 1
    fi

    if ! (cd -- "${relocated}" && "./${executable_name}" --smoke-test=300 --host=27315); then
        echo "The relocated game failed its hosted run." >&2
        exit 1
    fi

    echo "Relocated dedicated server started and a hosted game played from ${relocated}"

    # The same sources must package to the same bytes, which signed packages keep too: Ed25519 signatures are deterministic.
    if [[ "${content_mode}" == package ]]; then
        cook "${relocated}/Rebuild" package
        first="$(sha256sum -- "${package_root}/${project_name}.hpak" | cut -d' ' -f1)"
        second="$(sha256sum -- "${relocated}/Rebuild/${project_name}.hpak" | cut -d' ' -f1)"
        if [[ "${first}" != "${second}" ]]; then
            echo "Packaging the same content twice gave different bytes: ${first} and ${second}" >&2
            exit 1
        fi

        echo "The package rebuilt byte for byte: ${first}"
    fi
fi

tar -C "${output_directory}" -czf "${archive_path}" "${package_name}"
echo "Packaged game: ${archive_path}"
