# Android launcher preview

This APK is **not a playable port**. It provides a document picker for a user's
ISO or ZIP and a persistent draggable control-layout mockup. Selecting a file
saves its document permission and performs a shallow, asynchronous disc-header
check (including the first ISO in a ZIP). It does not fully validate, extract,
or install the image. Play is disabled. Touch controls and gamepads are not
connected to a game backend. Use only legally owned game images.

Build with JDK 17, Gradle 8.9, Android SDK platform 35, NDK 27.2.12479018
and Android SDK CMake 3.22.1:

```sh
cd android
gradle assembleDebug
```

The `Android launcher preview APK` GitHub Actions workflow builds a debug APK
and uploads it as an artifact. The workflow installs those tools. A separate emulator job runs launcher UI and
shared PPC CPU primitive smoke tests
and uploads the test reports. It does not build or ship the game.
Run it from **Actions → Android launcher preview APK → Run workflow**.
A successful build or smoke test confirms only the preview launcher, not game compatibility.

## Optional owned-disc recompiled-code APK

For a manually triggered workflow, set repository Actions secrets `DISC_URL` to
an HTTPS direct-download link for your legally owned PAL `SNCP8P` ISO or a ZIP
containing exactly one ISO, and `DISC_SHA256` to the SHA-256 of the downloaded
ISO or ZIP. Then select **prepare_game** in Run workflow. Do not put a private
URL in a public workflow input. The download is limited, its digest is checked,
and a ZIP member is streamed to a fixed filename rather than extracted by path.
The job builds the existing host installer, validates the disc, extracts it,
generates recompilation sources, then compiles and links them into native Android
libraries for arm64-v8a and x86_64 inside a debug APK. The workflow uploads this
APK as `sonic-colors-android-recompiled-code-preview` for one day after an
x86_64 emulator metadata and launcher smoke test. It requires
a private repository because the APK contains game-derived code; it does not
upload the disc, extracted game data, or generated source files. Use trusted
workflow code: anyone able
to change that code and run it with repository secrets could exfiltrate them.
Runner disk space and processing time are substantial; cleanup runs even after
failure. The launcher can display the compiled game ID, but the native library
has fail-fast placeholders for runtime hooks and must not execute guest code.
The APK cannot play the game and still needs a real Android runtime.

The regular preview APK artifact comes from the separate build job and remains
independent of the disc job. Successfully compiling generated source objects
and linking generated code is not equivalent to running the game on Android.

## Remaining port work

The PC project currently generates recompiled sources from a locally owned
Wii game executable (`tools/build_generated.py`); those generated sources are
not in this checkout. Android also needs an ARM64-compatible native build,
Android lifecycle/storage integration, a supported graphics and audio backend,
disc extraction and validation (including safe ZIP handling), input mapping
for touch and controllers, and device testing. The current desktop CMake uses
host-specific prebuilt dependencies and desktop graphics components. A debug
launcher APK alone does not solve these requirements or establish full-game
compatibility.

The APK includes `scr_android_probe`, a tiny JNI library compiling a few existing
CPU primitives for arm64-v8a and x86_64. The emulator test runs it on x86_64;
it is not a native game runtime and does not prove ARM device compatibility.

A single-controller Android input adapter and native `Pad_Read` state are under
`runtime/platform/android` and the launcher Java sources. The launcher forwards controller events to that state without consuming UI events,
but no game runtime uses them yet. Vibration, multiple controllers and touch input remain unwired.

Android guest-memory mapping is implemented in `runtime/platform/android/memory.cpp`.
It uses `memfd_create` and a 4 GiB virtual reservation on 64-bit devices; it
fails cleanly when the kernel or sandbox does not support those operations.
Its aliasing is host-tested; it is not yet connected to a running game.
