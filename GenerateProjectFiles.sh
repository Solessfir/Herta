#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
action="${1:-gmake}"

if ! premake_path="$("${root_dir}/Scripts/Linux/Setup.sh" --print-premake-path 2>/dev/null)"; then
    echo 'Could not find project-local Premake. Run Setup.sh first.' >&2
    exit 1
fi

if ! vulkan_sdk_path="$("${root_dir}/Scripts/Linux/Setup.sh" --print-vulkan-sdk-path 2>/dev/null)"; then
    echo 'Could not find the project-local Vulkan SDK. Run Setup.sh first.' >&2
    exit 1
fi

HERTA_VULKAN_SDK="${vulkan_sdk_path}" "${premake_path}" --file="${root_dir}/premake5.lua" "${action}"
