# gromui

Small Linux display backend extracted and rewritten from TWRP/AOSP minui.
C11, libc and Linux UAPI only. No libdrm, Android libraries, C++ runtime,
font files, PNG, FreeType, pixelflinger, input handling or recovery UI.
The original TWRP checkout and the supplied grom-init reference were not used
as production init code.

`gromui_open(..., NULL)` lists existing DRM cards first, then
`/dev/graphics/fbN` and `/dev/fbN`, and probes those nodes in numeric order.
If a directory denies listing, it falls back to bounded numeric probing.
A backend is accepted only after usable buffers are established.
Explicit backend/path options are available for diagnostics. A failed DRM
initialization releases its resources before trying another device or fbdev.

## Build

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=MinSizeRel \
  -DGROMUI_LVGL=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Set `GROMUI_LVGL=OFF` for just the display library and color-pattern example.
Builds are offline; LVGL 9.4.0 source is pinned in `third_party/`. CMake 3.19+
and a Linux C toolchain are required. All executables link statically.
Use `add_subdirectory(gromui)` and link
`gromui`, or `gromui-lvgl` for the adapter. LVGL configuration lives in
`include/lv_conf.h`; external codecs and platform drivers are disabled.

ARM64 static build:

```sh
cmake -S . -B build-aarch64 -G Ninja \
  -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc -DCMAKE_BUILD_TYPE=MinSizeRel \
  -DGROMUI_LVGL=ON -DBUILD_TESTING=OFF
cmake --build build-aarch64
```

## API

```c
#include <gromui.h>
gromui_display *display;
if (gromui_open(&display, NULL) < 0) return 1; /* inspect errno */
const gromui_surface *frame = gromui_buffer(display);
/* Each pixel is a native-endian uint32_t 0x00RRGGBB. Stride is in bytes.
   This buffer persists across presents, including unchanged pixels. */
frame->pixels[0] = 0x00ff0000;
if (gromui_present(display) < 0) { /* handle display loss */ }
gromui_close(display);
```

DRM uses two mapped dumb buffers, kernel pitches and synchronous legacy KMS
presentation. Dirty notifications support devices needing explicit updates.
fbdev converts to the driver's reported 16/24/32-bit truecolor layout and
copies into the visible mapped region, respecting offsets and row padding.
It deliberately does not depend on panning or double-buffer support. This can
tear; DRM modesets are also slower than asynchronous page flips. These are
recovery-oriented CPU display paths, not a GPU renderer.

For LVGL, initialize LVGL, supply a monotonic tick callback, call
`gromui_lvgl_create(display)`, construct widgets, and run `lv_timer_handler()`.
The adapter uses 32-row XRGB8888 partial buffers and presents once at the last
flush of a frame. Check `gromui_lvgl_error()` after timer processing: it retains
presentation failures while still completing LVGL flushes. Delete the LVGL
display **before** closing gromui. Calls must be serialized. See
`examples/lvgl_demo.c` and `tests/lvgl_adapter.c`.

## Early init responsibilities

The library opens existing device nodes; the executable owns mounts, matching
kernel modules, uevents/device nodes, console ownership and backlight policy.
Retry discovery for asynchronously appearing devices. You need KMS dumb
buffers with legacy modesetting, or a packed truecolor fbdev device.
Atomic-only DRM, vendor overlays, indexed/directcolor fbdev, GPU-only buffers,
and displays requiring proprietary initialization are not supported.

`examples/init.c` demonstrates `/init` mounts and sysfs-based node discovery,
loads the emulator's virtio prerequisites, switches tty0 to graphics mode,
and supervises rendering in a child so a renderer crash does not kill PID 1.
It is a **test init**, not a complete recovery: it intentionally stays alive,
has no storage actions, input, chainloading or production shutdown policy.
The LVGL screen's button is a rendering sample, not an implemented action.

The combined MagiskInit binary uses the same display backend and LVGL adapter
for a separate grom preboot test screen. grom supervises that renderer in a
child and keeps its device mounts private. The displayed password field is a
visual placeholder; no input or unlock action is implemented.

Keep exclusive ownership of the display while open. On close, gromui restores
the previous DRM CRTC routing/mode or saved visible fbdev pixels and frees its
buffers before closing the fd. fbdev mode is not changed. Unblank is best effort;
prior backlight/blank state cannot be queried portably and is not restored.
`gromui_close` reports a DRM restore failure, but still frees resources. Kernel
hotplug/mode changes require close/reopen; there is no automatic live migration.
Kernel ioctl hangs cannot be bounded by a userspace retry counter: production
PID 1 should supervise rendering and implement its own timeout/fallback policy.

## Emulator screenshots

See [verification](docs/verification.md), [audit](docs/audit.md), and
[performance measurements](docs/performance.md).
Both DRM and fbdev were run through **real kernel devices** in the Android SDK
emulator, with the binary replacing `/init`. The SDK Android kernel has
`CONFIG_FB` disabled, so fbdev verification uses the same SDK emulator with a
separate fbdev-enabled Linux kernel. ARM64 was cross-built, not device-tested.

```sh
# Python environment for SDK screenshot capture (host tooling only)
python3 -m venv build/capture-env
build/capture-env/bin/pip install grpcio grpcio-tools pillow
# Runner extracts the matching virtio-gpu module from the SDK vendor image.
scripts/run_emulator.sh test_device --headless -- -grpc 8556
# In another terminal, before terminating that owned emulator:
build/capture-env/bin/python scripts/capture.py docs/screenshot.png
```

Use the SDK gRPC screenshot endpoint. QEMU monitor `screendump` captures a
separate black surface with this emulator's gfxstream integration. ADB and
SurfaceFlinger are unavailable in this initramfs; logs are supplemental evidence.
Use a free gRPC port and stop other test emulators before starting a run.
`verify_emulator.py` boots, captures and terminates only its own process group.
The runner keeps GPT and concatenated cpio parsing in `scripts/ramdisk_tools.py`.
