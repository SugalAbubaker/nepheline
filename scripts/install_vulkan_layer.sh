#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Installs the Khronos validation layer from the pinned LunarG Linux SDK into
# a prefix (default ~/vulkan-sdk), for CI runners and Linux development
# machines whose distribution ships an older layer. Ubuntu 24.04's
# vulkan-validationlayers is 1.3.275: it crashes inside itself on lavapipe
# and reports synchronisation hazards the pinned 1.4.321 layer does not.
#
#     ./scripts/install_vulkan_layer.sh 1.4.321.1 [prefix]
#     export VK_LAYER_PATH=<prefix>/share/vulkan/explicit_layer.d
#
# Only the layer and its manifest are extracted from the ~300 MB SDK tarball,
# keeping the SDK's lib/ and share/ layout. VK_LAYER_PATH replaces the
# loader's default layer search paths, so the distribution's layer, if
# installed, is not seen.
set -euo pipefail

version=${1:?usage: install_vulkan_layer.sh <sdk-version> [prefix]}
prefix=${2:-$HOME/vulkan-sdk}
layer_so=$prefix/lib/libVkLayer_khronos_validation.so
manifest=$prefix/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json

if [ -f "$layer_so" ] && [ -f "$manifest" ] &&
    [ "$(cat "$prefix/VERSION" 2>/dev/null)" = "$version" ]; then
    echo "Vulkan validation layer $version is already installed in $prefix"
    exit 0
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
url="https://sdk.lunarg.com/sdk/download/$version/linux/vulkansdk-linux-x86_64-$version.tar.xz"
echo "Downloading $url"
curl -fsSL --retry 3 --retry-delay 5 -o "$tmp/sdk.tar.xz" "$url"

echo "Extracting the validation layer into $prefix"
rm -rf "$prefix"
mkdir -p "$prefix"
# The tarball's members are <version>/x86_64/...; two components are stripped.
tar -xJf "$tmp/sdk.tar.xz" -C "$prefix" --strip-components=2 --wildcards \
    '*/x86_64/lib/libVkLayer_khronos_validation.so' \
    '*/x86_64/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json'
for f in "$layer_so" "$manifest"; do
    if [ ! -f "$f" ]; then
        echo "install_vulkan_layer.sh: $f is missing after extraction" >&2
        exit 1
    fi
done

# The SDK manifest names the layer by file name alone, which the loader
# resolves through the dynamic linker's search path (LunarG's setup-env.sh
# puts lib/ on LD_LIBRARY_PATH). The loader also accepts a path relative to
# the manifest, which keeps the prefix self-contained.
sed -i 's|"library_path": "libVkLayer_khronos_validation.so"|"library_path": "../../../lib/libVkLayer_khronos_validation.so"|' \
    "$manifest"
if ! grep -q '"library_path": "../../../lib/libVkLayer_khronos_validation.so"' "$manifest"; then
    echo "install_vulkan_layer.sh: the manifest's library_path was not rewritten" >&2
    exit 1
fi

# The SDK ships the layer with its debug information (~520 MB). Stripping the
# symbols the dynamic linker does not need leaves a few tens of MB, which
# keeps the CI cache small; the layer's messages do not depend on them.
if [ "$(uname -s)" = Linux ] && command -v strip >/dev/null 2>&1; then
    strip --strip-unneeded "$layer_so"
fi

echo "$version" >"$prefix/VERSION"
echo "Vulkan validation layer $version installed in $prefix ($(du -sh "$prefix" | cut -f1))"
