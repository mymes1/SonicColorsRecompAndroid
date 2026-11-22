cmake_minimum_required(VERSION 3.22.1)
project(scr_android LANGUAGES CXX)

# Three libraries come out of this file, all loaded from Java by NativeRunner, never by the
# framework (see the note in AndroidManifest.xml about not declaring a NativeActivity):
#   libscr_runtime.so      the game: guest CPU, IOS/HLE, audio, and the Vulkan renderer
#   libscr_android_probe.so  a small metadata library, so an APK whose runtime failed to build
#                          still starts and reports why instead of dying in System.loadLibrary
#   libscr_recomp_code.so  the recompiled PPC code, only when SCR_COMPILE_GENERATED=ON

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../.." ABSOLUTE)
set(ANDROID_DIR "${REPO_ROOT}/runtime/platform/android")

# Same standard the desktop target uses. Exceptions stay on: the installer and the ISO reader use
# them for error reporting, and -fno-exceptions would need every call site rewritten.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------- architecture
if(ANDROID_ABI STREQUAL "arm64-v8a")
    set(SCR_RUNTIME_ARCH_OK TRUE)
else()
    set(SCR_RUNTIME_ARCH_OK FALSE)
    # The guest-thread fiber switch (runtime/platform/android/fiber.cpp) is AArch64 assembly and the
    # runtime is only ever shipped for arm64-v8a, so the target below simply is not defined for any
    # other ABI. The probe library still builds, which keeps a mismatched Gradle configuration from
    # failing at the native step with an assembler error about `stp x19, x20`.
    message(WARNING "libscr_runtime.so is arm64-v8a only (AArch64 fiber switch); "
                    "not building it for ${ANDROID_ABI}")
endif()

# ---------------------------------------------------------------------------- probe library
# Deliberately tiny, and the only target built for every ABI. It shares the input bridge with the
# runtime, and it is what the Activity falls back to when libscr_runtime.so is missing, so a
# runtime build that failed still produces an installable APK that can show why.
add_library(scr_android_probe SHARED
    probe.cpp
    input_bridge.cpp
    probe_bridge.cpp
    "${ANDROID_DIR}/input.cpp"
    "${ANDROID_DIR}/memory.cpp")
target_compile_features(scr_android_probe PRIVATE cxx_std_20)
target_compile_definitions(scr_android_probe PRIVATE SCR_ANDROID)
target_compile_options(scr_android_probe PRIVATE
    -include ${REPO_ROOT}/runtime/platform/compat.h)   # shared sources; see scr_runtime below
target_link_libraries(scr_android_probe PRIVATE android log)
target_include_directories(scr_android_probe PRIVATE
    "${REPO_ROOT}/runtime/cpu"
    "${REPO_ROOT}/runtime"
    "${ANDROID_DIR}")

# ---------------------------------------------------------------- the runtime's source set
# Explicit lists rather than a GLOB: the Android build excludes whole subsystems, and which
# files are in and out is a deliberate decision (see the notes under each list).
#
# Every entry is written as ${REPO_ROOT}/... because this file lives in android/app/src/main/cpp:
# CMake resolves a relative source path against this file's directory, not the repository root, so
# a bare `runtime/cpu/cpu.cpp` is looked for at src/main/cpp/runtime/cpu/cpu.cpp and the configure
# step fails with "Cannot find source file".
set(SCR_RUNTIME_CORE
    # CPU, memory, HLE
    ${REPO_ROOT}/runtime/cpu/cpu.cpp
    ${REPO_ROOT}/runtime/cpu/interpreter.cpp
    ${REPO_ROOT}/runtime/kernel/boot.cpp
    ${REPO_ROOT}/runtime/kernel/crash.cpp
    ${REPO_ROOT}/runtime/kernel/hle_os.cpp
    ${REPO_ROOT}/runtime/kernel/scheduler.cpp
    ${REPO_ROOT}/runtime/kernel/threads.cpp
    ${REPO_ROOT}/runtime/hw/dsp.cpp
    ${REPO_ROOT}/runtime/hw/exi.cpp
    ${REPO_ROOT}/runtime/hw/hollywood.cpp
    ${REPO_ROOT}/runtime/hw/mmio.cpp
    ${REPO_ROOT}/runtime/hw/si.cpp
    ${REPO_ROOT}/runtime/hw/vi.cpp
    # IOS and the disc/filesystem view
    ${REPO_ROOT}/runtime/ios/dev_di.cpp
    ${REPO_ROOT}/runtime/ios/dev_es.cpp
    ${REPO_ROOT}/runtime/ios/dev_fs.cpp
    ${REPO_ROOT}/runtime/ios/disc.cpp
    ${REPO_ROOT}/runtime/ios/fs_atomic.cpp
    ${REPO_ROOT}/runtime/ios/ios.cpp
    ${REPO_ROOT}/runtime/ios/nand_autosave.cpp
    # graphics, shared with the desktop backends
    ${REPO_ROOT}/runtime/gx/gx_fifo.cpp
    ${REPO_ROOT}/runtime/gx/hires.cpp
    ${REPO_ROOT}/runtime/gx/shadergen.cpp
    ${REPO_ROOT}/runtime/gx/shadergen_glsl.cpp
    ${REPO_ROOT}/runtime/gx/texture_decode.cpp
    ${REPO_ROOT}/runtime/gx/vertex_loader.cpp
    # audio
    ${REPO_ROOT}/runtime/audio/ax.cpp
    ${REPO_ROOT}/runtime/audio/dsp_coef.cpp
    ${REPO_ROOT}/runtime/audio/music_manifest.cpp
    ${REPO_ROOT}/runtime/audio/music_mod.cpp
    ${REPO_ROOT}/runtime/audio/music_sprintf_hle.cpp
    ${REPO_ROOT}/runtime/audio/music_tool.cpp
    ${REPO_ROOT}/runtime/audio/wav_file.cpp
    # input
    ${REPO_ROOT}/runtime/input/gamepad.cpp
    ${REPO_ROOT}/runtime/input/input_debug.cpp
    ${REPO_ROOT}/runtime/input/motion.cpp
    ${REPO_ROOT}/runtime/input/pad_hle.cpp
    ${REPO_ROOT}/runtime/input/wpad_hle.cpp
    # everything else
    ${REPO_ROOT}/runtime/config.cpp
    ${REPO_ROOT}/runtime/log.cpp
    ${REPO_ROOT}/runtime/mods/mods.cpp
    ${REPO_ROOT}/runtime/net/http.cpp
    ${REPO_ROOT}/runtime/perf.cpp
    ${REPO_ROOT}/runtime/platform/aes128.cpp
    # Release table (SNCP8P) + SHA-1 gate for an installed copy, and the SHA-1 itself. These
    # three are nod-free; runtime/install/install.cpp (the desktop installer) is not built.
    ${REPO_ROOT}/runtime/install/releases.cpp
    ${REPO_ROOT}/runtime/install/verify.cpp
    ${REPO_ROOT}/runtime/platform/linux/sha1.cpp
    ${REPO_ROOT}/runtime/region.cpp
    ${REPO_ROOT}/runtime/tas/tas.cpp)


# Android implementations of the platform interfaces. These replace runtime/platform/{win32,linux}
# and are the only ones the runtime links against.
set(SCR_RUNTIME_ANDROID
    ${REPO_ROOT}/runtime/platform/android/android_app.cpp
    ${REPO_ROOT}/runtime/platform/android/app.cpp
    ${REPO_ROOT}/runtime/platform/android/audio_out.cpp
    ${REPO_ROOT}/runtime/platform/android/crash.cpp
    ${REPO_ROOT}/runtime/platform/android/deflate.cpp
    ${REPO_ROOT}/runtime/platform/android/desktop_stubs.cpp
    ${REPO_ROOT}/runtime/platform/android/fiber.cpp
    ${REPO_ROOT}/runtime/platform/android/image.cpp
    ${REPO_ROOT}/runtime/platform/android/input.cpp
    ${REPO_ROOT}/runtime/platform/android/iso_install.cpp
    ${REPO_ROOT}/runtime/platform/android/memory.cpp
    ${REPO_ROOT}/runtime/platform/android/platform.cpp
    ${REPO_ROOT}/runtime/platform/android/renderer_vk.cpp
    ${REPO_ROOT}/runtime/platform/android/video.cpp
    ${REPO_ROOT}/runtime/platform/android/video_swapchain.cpp
    ${REPO_ROOT}/runtime/platform/android/vk_context.cpp)


# Deliberately not built, with the reason each one is out:
#   runtime/gx/renderer.cpp, runtime/gx/video.cpp, runtime/gx/dxbc_fixup.cpp
#       The Direct3D 11 renderer, its swapchain, and the DXBC container fixups for the shaders
#       d3dcompiler produces; renderer_vk.cpp implements the same gx_state.h contract with Vulkan
#       and compiles GLSL through shaderc, so no DXBC container ever exists here.
#   runtime/debug/*
#       Replay/verification harnesses for the desktop (FIFO archives, pose captures). They
#       provide the same A1*/A2*/A3* symbols renderer_vk.cpp stubs, so they cannot be linked
#       alongside it, and they need file formats only the desktop tooling produces.
#   runtime/ui/*, runtime/install_ui.cpp, runtime/mods/{gamebanana,mod_install}.cpp
#       Dear ImGui launcher, mod browser and updater: need imgui, nlohmann/json and libarchive,
#       and the Activity owns that UI on Android.
#   runtime/audio/music_{catalog,paths,deps}.cpp
#       Music Library tab and its VGMStream tool discovery (nlohmann/json + subprocesses).
#   runtime/install/install.cpp
#       The desktop/CI installer, which links nod (a Rust library) and reads RVZ/WIA/WBFS/CISO/GCZ
#       images. iso_install.cpp is the on-device installer and handles a plain ISO, or a ZIP
#       holding one (deflate.cpp is its inflater). The rest of the
#       directory (releases, verify) is built: the release table and the SHA-1 check of the
#       installed main.dol are what AndroidMain uses to decide whether the data is usable.
#   runtime/main.cpp, runtime/platform/run_process.cpp
#       Desktop entry point and subprocess helper.
#   runtime/platform/android/android_app_stub.cpp
#       Host fallback for the Activity bridge, only for linking the runtime on a CI Linux host.
#   runtime/platform/android/main.cpp
#       Removed: an ANativeActivity_onCreate entry point. The framework never loads this
#       library itself, so starting the runtime there would run it before a disc is installed.

if(SCR_RUNTIME_ARCH_OK)
    add_library(scr_runtime SHARED
        ${SCR_RUNTIME_CORE}
        ${SCR_RUNTIME_ANDROID}
        input_bridge.cpp)

    # Vulkan: the NDK ships vulkan/vulkan.h and vulkan/vulkan_android.h for the API level being
    # built, so the APK does not depend on external/vulkan-headers (which is fetched by
    # tools/fetch_external.sh for the desktop build and the host checks). The runtime uses only the
    # core 1.1 API and VK_EXT_debug_utils, none of which needs a newer header than the NDK's.
    target_include_directories(scr_runtime PRIVATE
        "${REPO_ROOT}"
        "${REPO_ROOT}/runtime"
        "${ANDROID_DIR}")

    # g_version, generated the same way as the desktop build so the log line names the build the
    # player is running (tools/version.cmake reads SCR_VERSION, else git describe).
    set(SCR_VERSION_CPP "${CMAKE_CURRENT_BINARY_DIR}/scr_version.cpp")
    add_custom_target(scr_version
        COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${REPO_ROOT} -DVERSION_OUT=${SCR_VERSION_CPP}
                -P ${REPO_ROOT}/tools/version.cmake
        BYPRODUCTS ${SCR_VERSION_CPP} VERBATIM)
    target_sources(scr_runtime PRIVATE ${SCR_VERSION_CPP})
    add_dependencies(scr_runtime scr_version)

    target_compile_definitions(scr_runtime PRIVATE SCR_ANDROID)
    # runtime/platform/compat.h is force-included into every shared source, exactly as the desktop
    # build and the host tests do: the shared code uses MSVC's _byteswap_*, _rotl and the *_s
    # functions, which it defines for other compilers. Without it the NDK reports every one of
    # them as "use of undeclared identifier".
    target_compile_options(scr_runtime PRIVATE
        -include ${REPO_ROOT}/runtime/platform/compat.h
        -Wall -Wextra -Wno-unused-parameter
        -fno-strict-aliasing)   # guest memory is aliased through several views; TBAA breaks that

    # Platform libraries. Vulkan and AAudio are NDK stub libraries that resolve against the
    # device's own copies at load time, so nothing is packaged; the requirement is declared through
    # the APK's minSdk (26). find_library rather than a bare name, so an NDK without them fails the
    # configure step with a clear message instead of the link.
    find_library(SCR_VULKAN_LIB vulkan REQUIRED)
    find_library(SCR_AAUDIO_LIB aaudio REQUIRED)
    # shaderc: renderer_vk.cpp compiles the GLSL it generates into SPIR-V on the device, and that
    # is the renderer's only shader compiler (the desktop renderer goes through HLSL and
    # d3dcompiler). The NDK does not ship a prebuilt shaderc; Google's instructions are to build it
    # once per NDK with ndk-build, which puts .a files under sources/third_party/shaderc/libs.
    # android/ci/build_shaderc.sh does exactly that. NDK r27 deleted the sources entirely, which is
    # why the NDK is pinned to r26.
    set(SCR_NDK_SHADERC "${ANDROID_NDK}/sources/third_party/shaderc")
    find_library(SCR_SHADERC_LIB shaderc PATHS "${SCR_NDK_SHADERC}/libs/${ANDROID_STL}/${ANDROID_ABI}"
                 NO_DEFAULT_PATH)
    if(NOT SCR_SHADERC_LIB)
        find_library(SCR_SHADERC_LIB shaderc)
    endif()
    if(NOT SCR_SHADERC_LIB)
        find_path(SCR_SHADERC_INCLUDE shaderc/shaderc.h HINTS "${SCR_NDK_SHADERC}/include")
    endif()
    if(NOT SCR_SHADERC_LIB AND NOT SCR_SHADERC_INCLUDE)
        message(FATAL_ERROR
            "shaderc is missing for ${ANDROID_ABI}. The Vulkan renderer compiles its generated "
            "GLSL with shaderc at run time, and the NDK does not ship it prebuilt: build it once "
            "with\n"
            "  bash android/ci/build_shaderc.sh\n"
            "or point -DSCR_SHADERC_LIB=... / -DSCR_SHADERC_INCLUDE=... at an existing build. "
            "NDK r27 and newer no longer contain the shaderc sources at all.")
    endif()
    if(SCR_SHADERC_LIB)
        target_link_libraries(scr_runtime PRIVATE ${SCR_SHADERC_LIB})
    endif()
    if(SCR_SHADERC_INCLUDE)
        target_include_directories(scr_runtime PRIVATE ${SCR_SHADERC_INCLUDE})
    endif()
    target_link_libraries(scr_runtime PRIVATE
        android log jnigraphics ${SCR_VULKAN_LIB} ${SCR_AAUDIO_LIB})
endif()   # SCR_RUNTIME_ARCH_OK

# ---------------------------------------------------------------- recompiled guest code
option(SCR_COMPILE_GENERATED "Package generated PPC code with fail-fast runtime placeholders" OFF)
if(SCR_COMPILE_GENERATED)
    if(NOT EXISTS "${REPO_ROOT}/generated/sources.cmake")
        message(FATAL_ERROR "Generated PPC sources missing; run tools/build_generated.py")
    endif()
    include("${REPO_ROOT}/generated/sources.cmake")
    add_library(scr_generated_check OBJECT ${PPC_RECOMP_SOURCES})
    set_target_properties(scr_generated_check PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_compile_features(scr_generated_check PRIVATE cxx_std_20)
    target_compile_options(scr_generated_check PRIVATE -O2 -fno-exceptions -fno-strict-aliasing -w)
    target_include_directories(scr_generated_check PRIVATE "${REPO_ROOT}/runtime/cpu" "${REPO_ROOT}/generated")
    # The optional build packages the compiled guest code, but deliberately does
    # not provide a playable runtime. Missing hooks terminate instead of faking
    # success. Export only the JNI metadata entry point.
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(OVERRIDE_STUBS "${CMAKE_CURRENT_BINARY_DIR}/recomp_override_stubs.cpp")
    add_custom_command(OUTPUT "${OVERRIDE_STUBS}"
        COMMAND "${Python3_EXECUTABLE}" "${REPO_ROOT}/android/ci/gen_override_stubs.py"
                "${REPO_ROOT}/generated/ppc_recomp.h" "${OVERRIDE_STUBS}"
        DEPENDS "${REPO_ROOT}/android/ci/gen_override_stubs.py" "${REPO_ROOT}/generated/ppc_recomp.h"
        VERBATIM)
    add_library(scr_recomp_code SHARED recomp_bridge.cpp recomp_unavailable.cpp
                "${OVERRIDE_STUBS}" $<TARGET_OBJECTS:scr_generated_check>)
    target_compile_features(scr_recomp_code PRIVATE cxx_std_20)
    target_include_directories(scr_recomp_code PRIVATE "${REPO_ROOT}/runtime/cpu")
    target_link_options(scr_recomp_code PRIVATE "-Wl,--no-undefined")
endif()
