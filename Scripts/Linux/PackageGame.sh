#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repository_root="$(cd -- "${script_dir}/../.." && pwd)"
project=Games/Sandbox/Sandbox.hertaproject
configuration=Shipping
output_directory=""
verify=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --verify) verify=1; shift ;;
        --project|--configuration|--repository-root|--output-directory)
            if [[ $# -lt 2 || -z "$2" ]]; then
                echo "Missing value for $1." >&2
                exit 2
            fi

            case "$1" in
                --project) project="$2" ;;
                --configuration) configuration="$2" ;;
                --repository-root) repository_root="$2" ;;
                --output-directory) output_directory="$2" ;;
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

for program in HertaGame HertaEditorCmd HertaAssetWorker; do
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

# Staging cooks the starting level's dependency closure through the asset worker and copies the project and level verbatim.
"${binary_directory}/HertaEditorCmd" game.stage "${project_path}" "${package_root}"
cp -- "${binary_directory}/HertaGame" "${package_root}/${executable_name}"
mkdir -p -- "${package_root}/Shaders"
cp -- "${shaders[@]}" "${package_root}/Shaders/"

cp -- "${repository_root}/LICENSE" "${package_root}/LICENSE"
# These dependencies embed their notices in source files rather than separate license files.
license_files=(External/spdlog/include/spdlog/fmt/bundled/format.h External/freetype/src/gzip/zlib.h)
while IFS= read -r tracked_file; do
    name="$(basename -- "${tracked_file}")"
    if [[ "${tracked_file}" == External/* && "${name,,}" =~ (license|copying|notice|^ofl\.|^ftl\.) ]]; then
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
fi

tar -C "${output_directory}" -czf "${archive_path}" "${package_name}"
echo "Packaged game: ${archive_path}"
