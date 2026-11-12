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
for the outline preboot UI. grom supervises that renderer in a child and keeps
its device mounts private. The UI is implemented in `grom/ui/outline.c` and
depends on the LVGL-free `grom/include/grom_backend.h`. That header describes
backend operations for authentication, matching-destination selection,
provisioning sessions, charge status and power. It defines no screens. The
frontend has only password and charging entry calls; later screens are its own
presentation state. Output labels are limited to authenticated matches or the
current provisioning session. Credential buffers are held outside LVGL text
areas and erased after a backend call. Calls are synchronous and direct; this
refactor adds no IPC for authentication. The native SDL runner in
`design/preview.c` and combined preboot test renderer use a backend scaffold
that rejects unlock and reports storage, provisioning and charger operations
unavailable. The combined renderer sends power requests to its supervising
PID 1; the SDL runner has no power endpoint. They do not authenticate,
provision, charge or boot anything. The combined renderer accepts touch from
an already available Linux `/dev/input/event*` touchscreen. Its input layer
lives in `grom/src/touch.c`,
outside the gromui display library. It discovers devices by capabilities, reads
single-touch and both multitouch protocols, maps absolute axes to the display,
and cancels a press after dropped events or device removal. Under a tmpfs `/dev`
it creates input nodes from sysfs. It retries discovery as devices appear.
Touch requires the kernel driver and any firmware to be ready. The combined
preboot executable and emulator harness use `grom/src/driver_bundle.c` to load
the ordered modules staged by a profile under `/grom/drivers/`, then unlink
the staged `.ko` files. Device profiles live under `grom/devices/`; grombake
can bundle the selected drivers from a matching firmware tree. The Moto G Fast
profile loaded the Himax driver on the physical Moto G Fast. The protocol-B
decoder now preserves coordinates reported before a new tracking ID; a
temporary debug boot with firmware extracted from the phone's vendor
partition delivered stationary keyboard taps, and the user confirmed typing
works.
The type-B reader initializes its current slot from the kernel and refreshes
it after dropped-event resynchronization, so it can handle a first touch that
does not repeat `ABS_MT_SLOT`.
Device-specific coordinate transforms remain future work.
Touch input reaches the outline UI, but the scaffold backend still rejects unlock
and there is no production handoff. `GROMUI_SCREEN_SIZE` can test a portrait
viewport in the SDK emulator.
The combined renderer also scans evdev devices for `KEY_POWER`. A complete
physical power-button press and release requests power-off through PID 1.
Key repeats, a release without a preceding press, and dropped input frames
cannot trigger shutdown. The listener rescans for hotplugged input devices.
See [the backend call map](../docs/backend-ui-flows.md) for each flow.

Build the native runner with `cmake -S design -B build-design -G Ninja`, then
`cmake --build build-design` and run `build-design/grom-design`. Pass a width and
height, for example `build-design/grom-design 1080 2400`, to preview scaling.
SDL2 development files are required. Run
`ctest --test-dir build-design --output-on-failure`
for the frontend interaction test. Its deterministic backend functions exist
only in that test executable. The runner is a test host for the frontend, not
grom logic.

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
# Runner extracts matching virtio display and touch modules from the SDK vendor image.
GROMUI_SCREEN_SIZE=720x1560 scripts/run_emulator.sh test_device --headless -- -grpc 8556
# In another terminal, before terminating that owned emulator:
build/capture-env/bin/python scripts/capture.py docs/screenshot.png
```

For the combined grom build, set `GROMUI_RAMDISK_IMAGE` to a standalone
ramdisk patched by root `grombake.py` and `GROMUI_BINARY` to its x86_64 init.
The runner preserves that image's `/init` and `/init.orig`. If the image has no
driver bundle, it stages the SDK emulator's ordered virtio modules in its
temporary copy using the same profile format as grombake.

Use the SDK gRPC screenshot endpoint. QEMU monitor `screendump` captures a
separate black surface with this emulator's gfxstream integration. ADB and
SurfaceFlinger are unavailable in this initramfs; logs are supplemental evidence.
Use a free gRPC port and stop other test emulators before starting a run.
`verify_emulator.py` boots, captures and terminates only its own process group.
The runner keeps GPT and concatenated cpio parsing in `scripts/ramdisk_tools.py`.
