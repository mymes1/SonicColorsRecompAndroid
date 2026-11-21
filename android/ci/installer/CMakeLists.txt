cmake_minimum_required(VERSION 3.22)
project(scr_ci_installer LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(FetchContent)
FetchContent_Declare(nod_prebuilt
    URL https://github.com/encounter/nod/releases/download/v2.0.0-alpha.12/libnod-linux-x86_64.tar.gz
    URL_HASH SHA256=920adb1d2386c8728e0521f8d52809057c04bd46913433a0000ee0931d08d69f
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
FetchContent_MakeAvailable(nod_prebuilt)
find_package(nod REQUIRED CONFIG PATHS ${nod_prebuilt_SOURCE_DIR} NO_DEFAULT_PATH)
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
add_executable(scr_install
    ${REPO_ROOT}/tools/scr_install/main.cpp
    ${REPO_ROOT}/runtime/install/install.cpp
    ${REPO_ROOT}/runtime/install/verify.cpp
    ${REPO_ROOT}/runtime/install/releases.cpp
    ${REPO_ROOT}/runtime/platform/linux/sha1.cpp)
target_include_directories(scr_install PRIVATE ${REPO_ROOT}/runtime)
target_compile_options(scr_install PRIVATE -include ${REPO_ROOT}/runtime/platform/compat.h)
target_link_libraries(scr_install PRIVATE nod::nod_static pthread dl m)
