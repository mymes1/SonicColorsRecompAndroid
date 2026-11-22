#!/bin/bash
# Fetches the headers the runtime includes but does not vendor in the repository.
#
# external/ is deliberately not checked in (it is 5.4 MB of upstream headers, and the snapshot
# excludes it). Everything the Android build needs from it is here:
#
#   vulkan-headers/include   vulkan/vulkan.h and the Vk core headers
#   spirv-headers/include    spirv/unified1/spirv.h and the SPIR-V core grammar
#
# The Android NDK ships Vulkan headers too, but the desktop build and the host compile checks
# need these, so the Android target points at the same copy and the two cannot disagree about
# the API version.
set -eu

cd "$(dirname "$0")"
REPO_ROOT=$(cd .. && pwd)
cd "$REPO_ROOT"   # every path below is relative to the repository root
VULKAN_TAG=v1.4.369
SPIRV_TAG=vulkan-sdk-1.4.363.0

fetch() {
    local repo=$1 tag=$2 dest=$3
    local tmp
    tmp=$(mktemp -d)
    # Sparse + shallow: only include/ matters, and the C++ bindings are not wanted.
    git clone --quiet --filter=blob:none --sparse --depth 1 --branch "$tag" "$repo" "$tmp/src"
    git -C "$tmp/src" sparse-checkout set include
    rm -rf "$dest"
    mkdir -p "$dest"
    cp -R "$tmp/src/include" "$dest/include"
    # The C++ bindings (vulkan.hpp, vulkan_handles.cppm, ...) need vulkan-hpp's own validation and
    # add 4 MB; the runtime is C and does not use them.
    find "$dest/include" -name '*.hpp' -delete
    find "$dest/include" -name '*.cppm' -delete
    cp "$tmp/src"/LICENSE* "$dest/" 2>/dev/null || true
    rm -rf "$tmp"
    echo "$dest: $(du -sh "$dest" | cut -f1)"
}

mkdir -p external
fetch https://github.com/KhronosGroup/Vulkan-Headers "$VULKAN_TAG" external/vulkan-headers
fetch https://github.com/KhronosGroup/SPIRV-Headers "$SPIRV_TAG" external/spirv-headers

printf '%s\n' \
    "# Vendored headers" \
    "" \
    "Not checked in: run \`bash tools/fetch_external.sh\` after" \
    "a fresh checkout to populate this directory. Only the header trees are kept; the C++ bindings" \
    "are deleted because the runtime is C." \
    "" \
    "| directory | upstream | tag |" \
    "| --- | --- | --- |" \
    "| \`vulkan-headers/include\` | KhronosGroup/Vulkan-Headers | \`$VULKAN_TAG\` |" \
    "| \`spirv-headers/include\` | KhronosGroup/SPIRV-Headers | \`$SPIRV_TAG\` |" \
    "" \
    "Licences are copied next to each tree. To move to a newer Vulkan SDK, change the tags at the" \
    "top of the fetch script and re-run it; nothing in the runtime depends on the exact revision." \
    > external/README.md

echo "wrote external/README.md"
