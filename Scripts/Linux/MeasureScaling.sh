#!/usr/bin/env bash
set -euo pipefail

configuration="${1:-Shipping}"
repository_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
editor="$repository_root/Binaries/linux/x86_64/$configuration/HertaEditor"
command="$repository_root/Binaries/linux/x86_64/$configuration/HertaEditorCmd"

if [[ ! -x "$editor" || ! -x "$command" ]]; then
    printf 'Build HertaEditor and HertaEditorCmd in %s first.\n' "$configuration" >&2
    exit 1
fi

if [[ ! -x /usr/bin/time ]]; then
    printf 'Install GNU time to record peak resident memory.\n' >&2
    exit 1
fi

output_directory="$repository_root/TestResults/Scaling/$(date +%Y%m%d-%H%M%S)-$$"
mkdir -p -- "$output_directory"
cd -- "$repository_root"

for workload in rendering dynamic; do
    for count in 1000 5000 10000; do
        name="$workload-$count"
        scene="$output_directory/$name.hscene"
        "$command" scene.generate-scaling "$workload" "$count" "$scene"
        arguments=("--scaling-test=$scene")
        if [[ "$workload" == dynamic ]]; then
            arguments+=(--scaling-simulate)
        fi

        /usr/bin/time -v -o "$output_directory/$name.memory.log" "$editor" "${arguments[@]}" >"$output_directory/$name.log" 2>"$output_directory/$name.errors.log"
        grep -E 'Scaling phase=|Scaling metric=|Scaling GPU' "$output_directory/$name.log"
        grep 'Maximum resident set size' "$output_directory/$name.memory.log"
    done
done

printf 'Scaling artifacts: %s\n' "$output_directory"
