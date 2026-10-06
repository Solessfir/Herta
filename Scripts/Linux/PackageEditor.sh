#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repository_root="$(cd -- "${script_dir}/../.." && pwd)"
configuration=Shipping
output_directory=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --configuration|--repository-root|--output-directory)
            if [[ $# -lt 2 || -z "$2" ]]; then
                echo "Missing value for $1." >&2
                exit 2
            fi

            case "$1" in
                --configuration) configuration="$2" ;;
                --repository-root) repository_root="$2" ;;
                --output-directory) output_directory="$2" ;;
            esac
            shift 2
            ;;
        *) echo "Unknown PackageEditor argument: $1" >&2; exit 2 ;;
    esac
done

case "${configuration}" in
    Shipping|Development) ;;
    *) echo 'Configuration must be Shipping or Development.' >&2; exit 2 ;;
esac

repository_root="$(cd -- "${repository_root}" && pwd -P)"
output_directory="$(realpath -m -- "${output_directory:-${repository_root}/Intermediate/Packages}")"
package_name="Herta-linux-x86_64-${configuration}"
package_root="${output_directory}/${package_name}"
archive_path="${output_directory}/${package_name}.tar.gz"
binary_directory="Binaries/linux/x86_64/${configuration}"

if [[ -e "${package_root}" || -L "${package_root}" || -e "${archive_path}" || -L "${archive_path}" ]]; then
    echo "Package output already exists. Choose an empty output directory: ${output_directory}" >&2
    exit 1
fi

required_content=(
    Engine/Content/Editor/Fonts/Roboto/Roboto-Regular.ttf
    Engine/Content/Editor/Fonts/Roboto/Roboto-Medium.ttf
    Engine/Content/Editor/Fonts/Roboto/OFL.txt
    Engine/Content/Editor/Fonts/DroidSansMono/DroidSansMono.ttf
    Engine/Content/Editor/Fonts/DroidSansMono/LICENSE.txt
    Engine/Content/Shapes/Cube.gltf
    Engine/Content/Shapes/Cube.gltf.hmeta
)
package_files=(LICENSE Games/Sandbox/Sandbox.hertaproject)
while IFS= read -r -d '' template_file; do
    package_files+=("${template_file#"${repository_root}/"}")
done < <(find "${repository_root}/Templates/Projects/Game" -type f -print0)
for program in HertaEditor HertaEditorCmd HertaAssetWorker; do
    relative_path="${binary_directory}/${program}"
    package_files+=("${relative_path}")
    if [[ -f "${repository_root}/${relative_path}" && ! -x "${repository_root}/${relative_path}" ]]; then
        echo "Required package program is not executable: ${relative_path}" >&2
        exit 1
    fi
done

for shader in TexturedMesh DebugDraw WorldGrid; do
    for stage in vert frag; do
        package_files+=("${binary_directory}/Shaders/${shader}.${stage}.hshader")
    done
done
package_files+=("${binary_directory}/Shaders/TexturedMesh.instanced.vert.hshader")

# These dependencies embed their notices in source files rather than separate license files.
license_files=(
    External/spdlog/include/spdlog/fmt/bundled/format.h
    External/freetype/src/gzip/zlib.h
    External/freetype/src/autofit/ft-hb-ft.c
    External/freetype/src/autofit/ft-hb-decls.h
    External/freetype/src/autofit/ft-hb-types.h
    External/freetype/src/autofit/hb-script-list.h
)

temporary_root="$(mktemp -d)"
trap 'rm -rf -- "${temporary_root}"' EXIT
git -C "${repository_root}" ls-files --recurse-submodules -z > "${temporary_root}/tracked-files"
declare -A tracked_files=()
while IFS= read -r -d '' relative_path; do
    tracked_files["${relative_path}"]=1
    case "${relative_path}" in
        Engine/Content/*|Games/Sandbox/Content/*|Games/Sandbox/Scenes/*) package_files+=("${relative_path}") ;;
        External/*)
            filename="${relative_path##*/}"
            if [[ "${filename,,}" =~ (license|copying|notice|^ofl\.|^ftl\.) ]]; then
                license_files+=("${relative_path}")
            fi
            ;;
    esac
done < "${temporary_root}/tracked-files"

for relative_path in "${required_content[@]}"; do
    if [[ -z "${tracked_files[${relative_path}]+present}" ]]; then
        echo "Required package content is not tracked: ${relative_path}" >&2
        exit 1
    fi
done

for relative_path in "${package_files[@]}" "${required_content[@]}" "${license_files[@]}"; do
    if [[ ! -f "${repository_root}/${relative_path}" ]]; then
        echo "Required package file is missing: ${relative_path}" >&2
        exit 1
    fi
done
revision="$(git -C "${repository_root}" rev-parse HEAD)"

staging_root="${temporary_root}/${package_name}"
copy_package_file() {
    local relative_path="$1"
    local destination="$2"
    mkdir -p -- "$(dirname -- "${destination}")"
    cp -p -- "${repository_root}/${relative_path}" "${destination}"
}

for relative_path in "${package_files[@]}"; do
    copy_package_file "${relative_path}" "${staging_root}/${relative_path}"
done

for relative_path in "${license_files[@]}"; do
    copy_package_file "${relative_path}" "${staging_root}/ThirdPartyLicenses/${relative_path}"
done
mkdir -p -- "${staging_root}/Games/Sandbox/Content"

cat > "${staging_root}/README.txt" <<EOF
Herta experimental editor - linux x86_64 ${configuration}
Revision: ${revision}

Extract the complete archive into a writable directory.
Run ${binary_directory}/HertaEditor.
The game runtime is not implemented. Save scene edits with Ctrl+S.
A Vulkan-capable GPU, driver, and system Vulkan loader are required.
Linux builds target Ubuntu 24.04 with the GCC 14 libstdc++ runtime and X11/Wayland libraries.
Blender is optional and only required to import .blend files.

Herta is licensed under MIT. See LICENSE and ThirdPartyLicenses.
Font licenses are included beside the fonts in Engine/Content.
This software is based in part on the work of the FreeType Team (https://freetype.org).
EOF

tar -czf "${temporary_root}/${package_name}.tar.gz" -C "${temporary_root}" "${package_name}"
mkdir -p -- "${output_directory}"
for source_path in "${staging_root}" "${temporary_root}/${package_name}.tar.gz"; do
    # -T prevents an existing destination directory from absorbing the staged package.
    mv -T -n -- "${source_path}" "${output_directory}/$(basename -- "${source_path}")"
    if [[ -e "${source_path}" ]]; then
        echo "Package output already exists: ${output_directory}" >&2
        exit 1
    fi
done

printf 'Packaged experimental editor: %s\n' "${archive_path}"
