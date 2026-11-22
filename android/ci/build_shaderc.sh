#!/bin/bash
# Builds the NDK's shaderc once, so the APK's Vulkan renderer can compile GLSL on the device.
#
# The NDK does not ship shaderc prebuilt. Google's guide (Vulkan shader compilers on Android) is to
# run ndk-build inside sources/third_party/shaderc, which leaves libshaderc.a for every ABI under
# sources/third_party/shaderc/libs/<stl>/<abi>/. android/app/src/main/cpp/CMakeLists.txt links that
# file. NDK r27 removed the sources, which is why the NDK is pinned to r26.
#
#   ANDROID_NDK         the NDK to use (default: ANDROID_NDK_HOME, ANDROID_NDK_ROOT, or the highest
#                       NDK under ANDROID_HOME/ndk)
#   SCR_SHADERC_ABIS    ABIs to build (default: arm64-v8a, the only ABI the runtime ships)
#   SCR_SHADERC_STL     STL variant directory to fill (default: c++_static)
#
# Usage: bash android/ci/build_shaderc.sh [--force]
set -eu

REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
ABIS=${SCR_SHADERC_ABIS:-arm64-v8a}
STL=${SCR_SHADERC_STL:-c++_static}
FORCE=false
[ "${1:-}" = "--force" ] && FORCE=true

ndk=${ANDROID_NDK:-${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}}
if [ -z "$ndk" ] || [ ! -d "$ndk" ]; then
    # Gradle normally resolves the NDK by version; match it here so the two cannot disagree.
    wanted=$(sed -n "s/.*ndkVersion '\(.*\)'.*/\1/p" "$REPO_ROOT/android/app/build.gradle" || true)
    if [ -n "$wanted" ] && [ -d "${ANDROID_HOME:-}/ndk/$wanted" ]; then
        ndk="${ANDROID_HOME}/ndk/$wanted"
    elif [ -d "${ANDROID_HOME:-}/ndk" ]; then
        ndk=$(ls -d "${ANDROID_HOME}"/ndk/* 2>/dev/null | sort -V | tail -1)
    fi
fi
if [ -z "$ndk" ] || [ ! -d "$ndk" ]; then
    echo "build_shaderc: no NDK found (set ANDROID_NDK)" >&2
    exit 1
fi

src="$ndk/sources/third_party/shaderc"
if [ ! -d "$src" ]; then
    # || true: a missing property file must not trip set -e before the explanation is printed.
    version=$(sed -n 's/^Pkg.Revision = //p' "$ndk/source.properties" 2>/dev/null || true)
    echo "build_shaderc: $ndk (${version:-unknown}) has no sources/third_party/shaderc." >&2
    echo "build_shaderc: NDK r27 and newer removed shaderc; install r26 (see android/README.md)." >&2
    exit 1
fi

libs="$src/libs/$STL"
missing=false
for abi in $ABIS; do
    [ -n "$(ls "$libs/$abi/libshaderc"*.a 2>/dev/null || true)" ] || missing=true
done
if [ "$missing" = false ] && [ "$FORCE" = false ]; then
    echo "build_shaderc: already built for $ABIS under $libs"
    exit 0
fi

# ndk-build needs the NDK's own make; the package ships prebuilt/{host}/bin/make for the host.
ndk_build="$ndk/ndk-build"
if [ ! -x "$ndk_build" ]; then
    echo "build_shaderc: $ndk_build is missing or not executable" >&2
    exit 1
fi

echo "build_shaderc: building shaderc for $ABIS ($STL) with $ndk"
# These are the arguments from Google's guide, unchanged. shaderc's own Android.mk decides where
# the .a files land, which is why the check above looks in libs/<stl>/<abi>.
(
    cd "$src"
    "$ndk_build" NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=Android.mk APP_STL:="$STL" \
        APP_ABI:="$(echo "$ABIS" | tr ' ' ',')" NDK_TOOLCHAIN_VERSION:=clang libshaderc_combined
)

for abi in $ABIS; do
    if [ -z "$(ls "$libs/$abi/libshaderc"*.a 2>/dev/null || true)" ]; then
        echo "build_shaderc: libshaderc.a was not produced for $abi (looked in $libs/$abi)" >&2
        exit 1
    fi
done
echo "build_shaderc: done"
