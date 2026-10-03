#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
package_script="${script_dir}/../PackageEditor.sh"
temporary_root="$(mktemp -d)"
trap 'rm -rf -- "${temporary_root}"' EXIT
repository_root="${temporary_root}/repository with spaces"
binary_directory=Binaries/linux/x86_64/Shipping
package_name=Herta-linux-x86_64-Shipping
test_count=0

write_fixture_file() {
    local relative_path="$1"
    mkdir -p -- "$(dirname -- "${repository_root}/${relative_path}")"
    printf 'fixture %s\n' "${relative_path}" > "${repository_root}/${relative_path}"
}

assert_file() {
    ((test_count += 1))
    if [[ ! -f "$1" ]]; then
        echo "Missing packaged file: $1" >&2
        exit 1
    fi
}

assert_absent() {
    ((test_count += 1))
    if [[ -e "$1" ]]; then
        echo "Unexpected packaged file: $1" >&2
        exit 1
    fi
}

assert_fails() {
    local expected_message="$1"
    shift
    local output
    ((test_count += 1))
    if output="$(bash "${package_script}" --repository-root "${repository_root}" "$@" 2>&1)"; then
        echo 'Packaging unexpectedly succeeded.' >&2
        exit 1
    fi
    if [[ "${output}" != *"${expected_message}"* ]]; then
        echo "Unexpected packaging failure: ${output}" >&2
        exit 1
    fi
}

content_files=(
    Engine/Content/Editor/Fonts/Roboto/Roboto-Regular.ttf
    Engine/Content/Editor/Fonts/Roboto/Roboto-Medium.ttf
    Engine/Content/Editor/Fonts/Roboto/OFL.txt
    Engine/Content/Editor/Fonts/DroidSansMono/DroidSansMono.ttf
    Engine/Content/Editor/Fonts/DroidSansMono/LICENSE.txt
    Engine/Content/Shapes/Cube.gltf
    Engine/Content/Shapes/Cube.gltf.hmeta
    'Games/Sandbox/Content/fixture texture.png'
    'Games/Sandbox/Content/fixture texture.png.hmeta'
    Games/Sandbox/Scenes/Sandbox.hscene
)
license_files=(
    External/example/LICENSE
    External/example/COPYING.txt
    External/example/NOTICE
    External/example/OFL.txt
    External/example/FTL.TXT
    External/spdlog/include/spdlog/fmt/bundled/format.h
    External/freetype/src/gzip/zlib.h
    External/freetype/src/autofit/ft-hb-ft.c
    External/freetype/src/autofit/ft-hb-decls.h
    External/freetype/src/autofit/ft-hb-types.h
    External/freetype/src/autofit/hb-script-list.h
)
for relative_path in LICENSE "${content_files[@]}" "${license_files[@]}" External/example/Source.cpp; do
    write_fixture_file "${relative_path}"
done

git -C "${repository_root}" init --quiet
git -C "${repository_root}" add .
git -C "${repository_root}" -c user.name=Test -c user.email=test@example.com -c commit.gpgsign=false commit --quiet -m Fixture

# Exercise recursive tracked-file discovery without network access.
submodule_root="${temporary_root}/dependency"
mkdir -- "${submodule_root}"
printf 'submodule license\n' > "${submodule_root}/LICENSE.txt"
git -C "${submodule_root}" init --quiet
git -C "${submodule_root}" add .
git -C "${submodule_root}" -c user.name=Test -c user.email=test@example.com -c commit.gpgsign=false commit --quiet -m Fixture
git -C "${repository_root}" -c protocol.file.allow=always submodule add --quiet "${submodule_root}" External/Submodule
write_fixture_file External/Submodule/untracked-NOTICE.txt

for program in HertaEditor HertaEditorCmd HertaAssetWorker; do
    write_fixture_file "${binary_directory}/${program}"
    chmod 755 -- "${repository_root}/${binary_directory}/${program}"
done

for shader in TexturedMesh DebugDraw WorldGrid; do
    for stage in vert frag; do
        write_fixture_file "${binary_directory}/Shaders/${shader}.${stage}.hshader"
    done
done

excluded_files=(
    "${binary_directory}/HertaTests"
    "${binary_directory}/HertaShaderWorker"
    "${binary_directory}/libvulkan.so"
    "${binary_directory}/libslang-compiler.so"
    "${binary_directory}/msvcp140.dll"
    "${binary_directory}/Shaders/Extra.vert.hshader"
    Engine/Content/untracked.txt
    Games/Sandbox/Content/untracked.txt
    SDK/Linux/fixture.txt
    DerivedDataCache/fixture.txt
)
for relative_path in "${excluded_files[@]}"; do
    write_fixture_file "${relative_path}"
done

bash "${package_script}" --repository-root "${repository_root}"
package_root="${repository_root}/Intermediate/Packages/${package_name}"
archive_path="${package_root}.tar.gz"
assert_file "${archive_path}"
extract_root="${temporary_root}/extracted"
mkdir -- "${extract_root}"
tar -xzf "${archive_path}" -C "${extract_root}"
extracted_package="${extract_root}/${package_name}"
for relative_path in LICENSE README.txt "${content_files[@]}"; do
    assert_file "${extracted_package}/${relative_path}"
done

for relative_path in "${license_files[@]}" External/Submodule/LICENSE.txt; do
    assert_file "${extracted_package}/ThirdPartyLicenses/${relative_path}"
done
assert_absent "${extracted_package}/ThirdPartyLicenses/External/Submodule/untracked-NOTICE.txt"
assert_absent "${extracted_package}/ThirdPartyLicenses/External/example/Source.cpp"

for program in HertaEditor HertaEditorCmd HertaAssetWorker; do
    assert_file "${extracted_package}/${binary_directory}/${program}"
    ((test_count += 1))
    [[ "$(stat -c '%a' "${extracted_package}/${binary_directory}/${program}")" == 755 ]]
done

for shader in TexturedMesh DebugDraw WorldGrid; do
    for stage in vert frag; do
        assert_file "${extracted_package}/${binary_directory}/Shaders/${shader}.${stage}.hshader"
    done
done

for relative_path in "${excluded_files[@]}"; do
    assert_absent "${extracted_package}/${relative_path}"
done
((test_count += 1))
grep -Fq "Revision: $(git -C "${repository_root}" rev-parse HEAD)" "${extracted_package}/README.txt"
((test_count += 1))
grep -Fq 'experimental editor' "${extracted_package}/README.txt"
((test_count += 1))
grep -Fq 'FreeType Team' "${extracted_package}/README.txt"

archive_hash="$(sha256sum "${archive_path}")"
assert_fails 'Package output already exists'
((test_count += 1))
[[ "$(sha256sum "${archive_path}")" == "${archive_hash}" ]]

existing_root="${temporary_root}/existing-directory"
mkdir -p -- "${existing_root}/${package_name}"
printf 'preserve directory\n' > "${existing_root}/${package_name}/sentinel"
assert_fails 'Package output already exists' --output-directory "${existing_root}"
((test_count += 1))
[[ "$(< "${existing_root}/${package_name}/sentinel")" == 'preserve directory' ]]

existing_archive="${temporary_root}/existing-archive"
mkdir -- "${existing_archive}"
printf 'preserve archive\n' > "${existing_archive}/${package_name}.tar.gz"
assert_fails 'Package output already exists' --output-directory "${existing_archive}"
((test_count += 1))
[[ "$(< "${existing_archive}/${package_name}.tar.gz")" == 'preserve archive' ]]

missing_inputs=(
    LICENSE
    "${binary_directory}/HertaEditor"
    "${binary_directory}/HertaEditorCmd"
    "${binary_directory}/HertaAssetWorker"
    "${binary_directory}/Shaders/WorldGrid.frag.hshader"
    Engine/Content/Editor/Fonts/DroidSansMono/DroidSansMono.ttf
    Engine/Content/Shapes/Cube.gltf.hmeta
    'Games/Sandbox/Content/fixture texture.png'
    Games/Sandbox/Scenes/Sandbox.hscene
    External/freetype/src/gzip/zlib.h
    External/example/LICENSE
)
for relative_path in "${missing_inputs[@]}"; do
    mv -- "${repository_root}/${relative_path}" "${temporary_root}/missing-input"
    assert_fails "Required package file is missing: ${relative_path}" --output-directory "${temporary_root}/missing-output"
    assert_absent "${temporary_root}/missing-output"
    mv -- "${temporary_root}/missing-input" "${repository_root}/${relative_path}"
done

chmod 644 -- "${repository_root}/${binary_directory}/HertaEditor"
assert_fails 'Required package program is not executable' --output-directory "${temporary_root}/missing-output"
chmod 755 -- "${repository_root}/${binary_directory}/HertaEditor"
git -C "${repository_root}" rm --quiet --cached Engine/Content/Shapes/Cube.gltf
assert_fails 'Required package content is not tracked' --output-directory "${temporary_root}/missing-output"
git -C "${repository_root}" add Engine/Content/Shapes/Cube.gltf
assert_fails 'Configuration must be' --configuration Debug
assert_fails 'Missing value' --configuration
assert_fails 'Unknown PackageEditor argument' --unknown

# An empty tracked Game mount still needs a writable directory in the archive.
git -C "${repository_root}" rm --quiet --cached "${content_files[7]}" "${content_files[8]}"
cp -a -- "${repository_root}/${binary_directory}" "${repository_root}/Binaries/linux/x86_64/Development"
development_output="${temporary_root}/development-output"
bash "${package_script}" --repository-root "${repository_root}" --configuration Development --output-directory "${development_output}"
assert_file "${development_output}/Herta-linux-x86_64-Development.tar.gz"
((test_count += 1))
[[ -d "${development_output}/Herta-linux-x86_64-Development/Games/Sandbox/Content" ]]
((test_count += 1))
[[ -z "$(ls -A -- "${development_output}/Herta-linux-x86_64-Development/Games/Sandbox/Content")" ]]

printf 'Passed %d Linux editor packaging checks.\n' "${test_count}"
