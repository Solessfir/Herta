#!/usr/bin/env bash

herta_dependency_lock_schema="HERTA_DEPENDENCIES_V1"

read_herta_dependency_lock() {
    local lock_path="$1"

    if [[ ! -f "${lock_path}" ]]; then
        echo "Dependency lock file not found: ${lock_path}" >&2
        return 1
    fi

    herta_dependency_names=()
    herta_dependency_kinds=()
    herta_dependency_platforms=()
    herta_dependency_versions=()
    herta_dependency_licenses=()
    herta_dependency_urls=()
    herta_dependency_sha256s=()
    herta_dependency_installed_entries=()
    herta_dependency_count=0

    local -A dependency_keys=()
    local schema_found=false
    local line_number=0
    local raw_line
    local line
    local pipe_characters
    local name
    local kind
    local platform
    local version
    local license
    local url
    local sha256
    local installed_entry
    local field
    local key

    while IFS= read -r raw_line || [[ -n "${raw_line}" ]]; do
        ((line_number += 1))
        line="${raw_line%$'\r'}"
        if [[ ${line_number} -eq 1 ]]; then
            line="${line#$'\xEF\xBB\xBF'}"
        fi

        if [[ -z "${line}" || "${line}" == \#* ]]; then
            continue
        fi

        if [[ "${schema_found}" == false ]]; then
            if [[ "${line}" != "${herta_dependency_lock_schema}" ]]; then
                echo "Unsupported dependency lock schema '${line}' at line ${line_number}." >&2
                return 1
            fi

            schema_found=true
            continue
        fi

        pipe_characters="${line//[^|]/}"
        if [[ ${#pipe_characters} -ne 7 ]]; then
            echo "Dependency line ${line_number} must contain exactly eight pipe-delimited fields." >&2
            return 1
        fi

        IFS='|' read -r name kind platform version license url sha256 installed_entry <<< "${line}"
        for field in "${name}" "${kind}" "${platform}" "${version}" "${license}" "${url}" "${sha256}" "${installed_entry}"; do
            if [[ -z "${field}" ]]; then
                echo "Dependency line ${line_number} contains an empty field." >&2
                return 1
            fi
        done

        if [[ "${kind}" != "tool" && "${kind}" != "sdk" ]]; then
            echo "Dependency line ${line_number} has unknown kind '${kind}'." >&2
            return 1
        fi
        if [[ ! "${sha256}" =~ ^[0-9a-fA-F]{64}$ ]]; then
            echo "Dependency line ${line_number} has an invalid SHA-256 value." >&2
            return 1
        fi
        if [[ ! "${url}" =~ ^https://[^/[:space:]]+(/.*)?$ || "${url}" =~ [[:space:]] ]]; then
            echo "Dependency line ${line_number} must use an absolute HTTPS URL." >&2
            return 1
        fi
        if [[ "${installed_entry}" == /* || "${installed_entry}" == \\* ||
              "${installed_entry}" =~ ^[[:alpha:]]: ||
              "${installed_entry}" =~ (^|[\\/])\.\.([\\/]|$) ]]; then
            echo "Dependency line ${line_number} has an unsafe installed entry path." >&2
            return 1
        fi

        key="${name}|${platform}"
        if [[ -n "${dependency_keys[${key}]+present}" ]]; then
            echo "Dependency line ${line_number} duplicates '${key}'." >&2
            return 1
        fi
        dependency_keys["${key}"]=1

        herta_dependency_names+=("${name}")
        herta_dependency_kinds+=("${kind}")
        herta_dependency_platforms+=("${platform}")
        herta_dependency_versions+=("${version}")
        herta_dependency_licenses+=("${license}")
        herta_dependency_urls+=("${url}")
        herta_dependency_sha256s+=("${sha256,,}")
        herta_dependency_installed_entries+=("${installed_entry}")
        ((herta_dependency_count += 1))
    done < "${lock_path}"

    if [[ "${schema_found}" == false ]]; then
        echo "Dependency lock file does not contain schema '${herta_dependency_lock_schema}'." >&2
        return 1
    fi
}

get_herta_premake_dependency() {
    local platform="$1"
    local match_count=0
    local index

    premake_version=""
    premake_url=""
    premake_sha256=""
    premake_entry=""

    for ((index = 0; index < herta_dependency_count; index += 1)); do
        if [[ "${herta_dependency_names[index]}" == "premake" &&
              "${herta_dependency_platforms[index]}" == "${platform}" ]]; then
            ((match_count += 1))
            premake_version="${herta_dependency_versions[index]}"
            premake_url="${herta_dependency_urls[index]}"
            premake_sha256="${herta_dependency_sha256s[index]}"
            premake_entry="${herta_dependency_installed_entries[index]}"
        fi
    done

    if [[ ${match_count} -ne 1 ]]; then
        echo "Dependencies.lock must contain exactly one premake entry for ${platform}." >&2
        return 1
    fi
}
