# Android build

Two things live here, and they are worth keeping apart:

* the **launcher** (Java): document picker, install progress, control layout, and the lifecycle that
  hands a Surface to the runtime;
* the **native runtime** (`libscr_runtime.so`, C++): the game, built from the shared sources in
  `../runtime` plus the Android platform layer.

The APK is still a preview and a disc image of your own is required. Nothing game-derived is
shipped: the installer reads the image the player selects, on the device.

## Building

JDK 17, Gradle 8.9, Android SDK platform 35, NDK 27.2.12479018 and Android SDK CMake 3.22.1:

```sh
cd android
gradle assembleDebug
```

The `Android launcher preview APK` GitHub Actions workflow builds the debug APK and uploads it as an
artifact; a separate emulator job runs the launcher UI and the shared PPC CPU primitive smoke tests.
It does not build or ship the game. Run it from **Actions → Android launcher preview APK → Run
workflow**. A successful build or smoke test confirms the launcher and those primitives, not game
compatibility.

### One run from a disc link to an installable APK

**Actions → Build playable Android APK from a disc link → Run workflow** is the end-to-end path:

1. give a direct `https://` link to your ISO or to a ZIP holding one (`disc_url`), or leave it empty
   to use the `DISC_URL` repository secret;
2. the run installs the toolchain, checks the installer and codec, downloads and validates the
   disc, and extracts it;
3. it builds the debug APK and uploads it as **`sonic-colors-launcher-apk`**.

That APK is the one to test with: install it, open it, tap **Choose your disc image…**, pick your
own ISO or ZIP on the phone, and it extracts the game there. It contains no game data, so it can be
built from any repository and is not limited to one disc.

`disc_sha256` pins the download (empty falls back to `DISC_SHA256`, and if neither is set the run
reports the digest it saw so the next run can be pinned). A link typed into the input is part of the
run's stored event payload, so a signed or private URL belongs in the secret instead.

Setting `include_game_code` additionally recompiles the disc into a playable APK
(`sonic-colors-playable-apk`). That half produces game-derived code, so it is refused unless the
repository is private, and it is removed from the runner on success and on failure alike. Only APKs
are ever uploaded.

### Native targets

`src/main/cpp/CMakeLists.txt` produces:

| library | contents |
| --- | --- |
| `libscr_runtime.so` | the game: guest CPU, IOS/HLE, audio, Vulkan renderer |
| `libscr_android_probe.so` | a tiny JNI metadata library, so an APK with a broken runtime still starts and explains itself |
| `libscr_recomp_code.so` | only with `-PcompileGenerated=true`; see below |

`libscr_runtime.so` is arm64-v8a only (its guest-thread fiber switch is AArch64 assembly) and needs
`minSdk 26` (Vulkan is 24, AAudio 26, and there is no fallback for either). Other ABIs build just
the probe library, with a warning rather than an assembler error.

The source list, and the reason for everything excluded from it, is documented inline in
`src/main/cpp/CMakeLists.txt`. In short: nothing that needs Dear ImGui, nlohmann/json, libarchive,
nod, or Direct3D 11 is built, because the APK has none of them — the Activity owns the UI.

### Checking a change without a device

`tools/android_target_check/check.py` reads the source lists out of the CMake file and can compile
and link them with any host clang that targets aarch64 (zig is enough). It catches a renamed file, a
new file that was never added to a list, and a source that regressed into an undefined symbol:

```sh
bash tools/fetch_external.sh                    # Vulkan/SPIR-V headers, not checked in
python3 tools/android_target_check/check.py --link
```

The link check needs stand-ins for the NDK's own headers (`jni.h`, `aaudio/AAudio.h`, `android/*.h`,
`shaderc/shaderc.h`, `sys/system_properties.h`), which are host-only test fixtures and not checked
in. What it does not prove: that those match the real NDK headers, that shaderc compiles the
generated GLSL, and anything about a real GPU — those still need a device.

## Optional owned-disc recompiled-code APK

For a manually triggered workflow, set repository Actions secrets `DISC_URL` to
an HTTPS direct-download link for your legally owned PAL `SNCP8P` ISO or a ZIP
containing exactly one ISO, and `DISC_SHA256` to the SHA-256 of the downloaded
ISO or ZIP. Then select **prepare_game** in Run workflow. Do not put a private
URL in a public workflow input. The download is limited, its digest is checked,
and a ZIP member is streamed to a fixed filename rather than extracted by path.
The job builds the existing host installer, validates the disc, extracts it,
generates recompilation sources, then compiles and links them into the debug APK.
The native libraries are arm64-v8a only, which is what the APK ships and what a
phone runs. The workflow uploads this
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

## What the runtime expects at run time

* The installer (`runtime/platform/android/iso_install.cpp`) reads the selected plain ISO, or a ZIP
  holding exactly one (stored or deflated), and writes the layout the shared runtime already uses: `sys/{boot,bi2,apploader,main.dol,fst}.bin`,
  `ticket/tmd/cert/h3.bin`, `disc/{header,region,unencrypted}.bin`, `files/…`. A player-supplied
  `keys.txt` (their own console's common key) is needed for an encrypted retail image; an already
  decrypted image needs none. No key material is in the APK: **Provide keys.txt** in the launcher
  copies the file the player owns into the app's files directory, which is where the native side
  looks for it.
* A ZIP is checked before it is used: an archive with two disc images, no disc image, an encrypted
  entry, an unsupported compression method or a stream that fails its CRC-32 is refused with a
  message saying what to do, and nothing is written.
* RVZ/WIA/WBFS/CISO/GCZ are **not** supported on device: those decoders are in nod, which is a Rust
  library the APK does not ship. The picker says so before copying anything. Convert to an ISO (or
  a ZIP of one) first.
* Guest memory uses a 4 GiB reservation with `memfd_create` (`runtime/platform/android/memory.cpp`),
  falling back to shared anonymous mappings when the kernel refuses.

## Still unverified

`android/ci/installer/CMakeLists.txt` proves the installer logic on a host; the on-device path, the
Vulkan renderer, AAudio output and the large-stack runtime thread have only been compiled, not run
on hardware. Those need a device.
