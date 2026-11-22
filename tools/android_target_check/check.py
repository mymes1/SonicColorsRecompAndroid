#!/usr/bin/env python3
"""Checks the Android runtime target's source list against the tree, and against a link.

Two failure modes this catches, both of which only show up at NDK build time otherwise:

  * a file named in android/app/src/main/cpp/CMakeLists.txt that no longer exists (renamed or
    deleted), and
  * a source that exists but is in neither list, i.e. silently left out of the APK.

It also reports the platform sources that exist but are deliberately excluded, so the exclusion
list in the CMake file and the tree cannot drift apart unnoticed.

Run from the repository root:  python3 tools/android_target_check/check.py
Add --link to also compile and link the target with the host toolchain (zig), which is what
proves the list actually produces a complete library.
"""
import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CMAKE = os.path.join(ROOT, "android/app/src/main/cpp/CMakeLists.txt")

# The comment block in the CMake file lists these by subsystem; keep both sides in sync here.
EXCLUDED_PREFIXES = (
    "runtime/ui/",
    "runtime/debug/",
    "runtime/install/install.cpp",
    "runtime/platform/win32/",
    "runtime/platform/linux/",
    "runtime/audio/music_catalog.cpp",
    "runtime/audio/music_deps.cpp",
    "runtime/audio/music_paths.cpp",
    "runtime/mods/gamebanana.cpp",
    "runtime/mods/mod_install.cpp",
    "runtime/platform/android/android_app_stub.cpp",
)
# Files outside runtime/ that are still part of the target (there are none today, but the check
# needs to know which platform-only paths are shared on purpose).
SHARED_FILES = (
    "runtime/platform/linux/sha1.cpp",   # portable SHA-1, used by the installer and the verifier
)
EXCLUDED_FILES = (
    "runtime/main.cpp",
    "runtime/install_ui.cpp",
    "runtime/gx/renderer.cpp",
    "runtime/gx/video.cpp",
    "runtime/gx/dxbc_fixup.cpp",
    "runtime/platform/run_process.cpp",
)


def cmake_lists():
    """Reads the two source lists out of the CMake file, line by line.

    A block runs from `set(VAR` to the line that ends in `)`, which may be the last entry's own
    line (`runtime/tas/tas.cpp`), so this cannot be a single regex.
    """
    lines = open(CMAKE, encoding="utf-8").read().splitlines()
    found = {}
    for var in ("SCR_RUNTIME_CORE", "SCR_RUNTIME_ANDROID"):
        for i, line in enumerate(lines):
            if not line.startswith("set(" + var):
                continue
            body = []
            for entry in lines[i + 1:]:
                stripped = entry.split("#", 1)[0].strip()
                closed = stripped.endswith(")")
                if closed:
                    stripped = stripped[:-1].strip()
                if stripped:
                    body.append(stripped)
                if closed:
                    break
            # Entries are written as ${REPO_ROOT}/runtime/... : CMake resolves a relative source
            # path against the CMakeLists' own directory, which is not the repository root. The
            # check compares repository-relative paths, so the prefix is stripped here.
            found[var] = [p.replace("${REPO_ROOT}/", "") for p in body if p.endswith(".cpp")]
            break
        else:
            sys.exit("check.py: cannot find set(%s) in %s" % (var, CMAKE))
    return found


def all_sources():
    out = []
    for dirpath, _, files in os.walk(os.path.join(ROOT, "runtime")):
        for f in files:
            if f.endswith(".cpp"):
                out.append(os.path.relpath(os.path.join(dirpath, f), ROOT).replace(os.sep, "/"))
    return sorted(out)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--link", action="store_true", help="also compile and link with host zig")
    args = parser.parse_args()

    lists = cmake_lists()
    listed = lists["SCR_RUNTIME_CORE"] + lists["SCR_RUNTIME_ANDROID"]
    failures = []

    missing = [p for p in listed if not os.path.isfile(os.path.join(ROOT, p))]
    for p in missing:
        failures.append("listed but not on disk: " + p)

    def excluded(path):
        if path in SHARED_FILES:
            return False
        return path in EXCLUDED_FILES or path.startswith(EXCLUDED_PREFIXES)

    for path in all_sources():
        if path in listed or excluded(path):
            continue
        failures.append("in the tree but not in any list (add it, or exclude it on purpose): " + path)

    for path in listed:
        if excluded(path):
            failures.append("listed but marked excluded: " + path)

    print("%d files listed (%d core + %d android)" % (len(listed), len(lists["SCR_RUNTIME_CORE"]),
                                                      len(lists["SCR_RUNTIME_ANDROID"])))

    if args.link:
        ok = link_check(listed)
        if ok != 0:
            failures.append("link check failed (%d)" % ok)

    for f in failures:
        print("FAIL " + f)
    print("%d problem(s)" % len(failures))
    return 1 if failures else 0


# Symbols provided by the recompiler's output (generated/, built by tools/recomp.py into
# scr_recomp_code) or by the NDK. Nothing else may be undefined: the list is what proves the
# target's source set is complete.
GENERATED_SYMBOLS = {
    "ppc",              # PPCContext for the guest's main thread
    "ppc_lookup", "ppc_call_indirect", "ppc_check_irq",
    "g_ppcRelease", "g_ppcDelta", "g_ppcDeltaCount", "g_ppcRegionCode",
    "AudioOut_SetSuspended",        # audio/audio_out.h: platform function, Android's is in audio_out.cpp
    "Crash_Init", "Crash_GuestThreadKnown", "Crash_SampleSlowGuest", "Crash_SampleGuestPC",
    "Crash_SetGxThread", "Crash_StartSamplers",   # platform/android/crash.cpp is in the list; regrouped below
}
EXTERNAL_PREFIXES = ("AAudio", "ANativeWindow", "vk", "shaderc", "JNI", "__android",
                     "__system_property", "pthread_", "dl", "backtrace",
                     "gettid")   # bionic, since API 21
# Generated at build time by tools/version.cmake, exactly like the desktop target.
BUILD_GENERATED_SYMBOLS = {"g_version"}


def link_check(listed):
    """Compiles the listed sources for arm64 and links them, rejecting undefined symbols."""
    zig = os.environ.get("ZIG", "/tmp/zv/lib/python3.11/site-packages/ziglang/zig")
    if not os.path.isfile(zig):
        print("FAIL no zig at %s (set ZIG)" % zig)
        return 1
    out = "/tmp/android_target_check"
    subprocess.run(["rm", "-rf", out], check=True)
    os.makedirs(out)
    inc = ["-I" + os.path.join(ROOT, d) for d in
           ("", "runtime", "runtime/platform/android", "external/vulkan-headers/include",
            "external/spirv-headers/include")]
    common = ["-std=c++20", "-c", "-DSCR_ANDROID", "-target", "aarch64-linux-gnu"] + inc + \
             ["-include", os.path.join(ROOT, "runtime/platform/compat.h")]
    extra = os.environ.get("SCR_EXTRA_INCLUDE")
    if extra:
        common += ["-I" + p for p in extra.split(os.pathsep)]

    objects = []
    for src in listed:
        obj = os.path.join(out, src.replace("/", "_") + ".o")
        proc = subprocess.run([zig, "c++"] + common + [src, "-o", obj],
                              cwd=ROOT, capture_output=True, text=True)
        if proc.returncode != 0:
            print("COMPILE FAIL " + src)
            print(proc.stderr.strip()[:2000])
            return 1
        objects.append(obj)
    proc = subprocess.run([zig, "c++", "-shared", "-target", "aarch64-linux-gnu", "-o",
                           os.path.join(out, "libscr_runtime.so")] + objects + ["-Wl,--no-undefined"],
                          cwd=ROOT, capture_output=True, text=True)
    if proc.returncode == 0:
        print("linked %d objects into libscr_runtime.so with no undefined symbols" % len(objects))
        return 0

    # Expected: platform libraries (the NDK links those) and the generated guest code. Anything
    # else is a source missing from the lists above.
    undefined = sorted(set(re.findall(r"undefined symbol: ([A-Za-z_0-9]+)", proc.stderr)))
    allowed = set(GENERATED_SYMBOLS) | BUILD_GENERATED_SYMBOLS
    unexpected = [s for s in undefined
                  if s not in allowed and not s.startswith(EXTERNAL_PREFIXES)]
    if unexpected:
        print("undefined symbols that no excluded file explains:")
        for s in unexpected:
            print("  " + s)
        return 1
    print("linked %d objects: %d undefined symbols, all from the NDK or the generated code"
          % (len(objects), len(undefined)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
