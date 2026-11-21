cmake_minimum_required(VERSION 3.22.1)
project(scr_android_probe LANGUAGES CXX)

# This is a probe of shared recomp primitives, not the game runtime.
add_library(scr_android_probe SHARED probe.cpp input_bridge.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../../../runtime/platform/android/input.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../../../runtime/platform/android/memory.cpp)
target_compile_features(scr_android_probe PRIVATE cxx_std_20)
target_include_directories(scr_android_probe PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../../../../runtime/cpu
    ${CMAKE_CURRENT_LIST_DIR}/../../../../../runtime
    ${CMAKE_CURRENT_LIST_DIR}/../../../../../runtime/platform/android)

option(SCR_COMPILE_GENERATED "Package generated PPC code with fail-fast runtime placeholders" OFF)
if(SCR_COMPILE_GENERATED)
    get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../.." ABSOLUTE)
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
    target_link_options(scr_recomp_code PRIVATE -Wl,--no-undefined)

endif()
