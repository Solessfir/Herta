#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
action="${1:-gmake}"

premake_path="$("${root_dir}/Scripts/Setup.sh" --print-premake-path)"
"${premake_path}" --file="${root_dir}/premake5.lua" "${action}"
