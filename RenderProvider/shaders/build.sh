#!/usr/bin/env bash
# Compiles every GLSL stage here to the SPIR-V the Vulkan backend loads
# from bin/shaders/ (VulkanPresenter.h, loadPipeline). The .spv beside each
# source is what the Makefile copies; regenerate them with this after any
# shader change. Needs glslangValidator (glslang-tools) or glslc.
set -euo pipefail
cd "$(dirname "$0")"

if command -v glslangValidator >/dev/null 2>&1; then
    for f in *.vert *.frag; do glslangValidator -V "$f" -o "$f.spv"; done
elif command -v glslc >/dev/null 2>&1; then
    for f in *.vert *.frag; do glslc "$f" -o "$f.spv"; done
else
    echo "shaders/build.sh: no glslangValidator or glslc on PATH" >&2
    exit 1
fi
