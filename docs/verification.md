# Verification — 2026-11-10

The test binaries replaced `/init` in temporary copies of an Android SDK AVD
ramdisk. Tests used the installed `test_device` x86_64 AVD, SDK Android 13 image,
virtio-vga at 800×600, and the SDK gRPC screenshot API. No Android framework,
SurfaceFlinger, ADB daemon or graphics service ran in the guest.

| Case | Kernel/interface | Screenshot |
|---|---|---|
| Core, automatic DRM selection | SDK `kernel-ranchu`, real `/dev/dri/card0` | [DRM pattern](drm.png) |
| Core, explicit fbdev | Linux 7.2.6-arch2-1, real `/dev/fb0` | [fbdev pattern](fbdev.png) |
| LVGL, automatic DRM selection | SDK `kernel-ranchu` | [LVGL DRM](lvgl-drm.png) |
| LVGL, explicit fbdev | Linux 7.2.6-arch2-1 | [LVGL fbdev](lvgl-fbdev.png) |
| LVGL, automatic fallback | Linux 7.2.6-arch2-1, test init hides DRM nodes | [Automatic fbdev](auto-fallback.png) |

The stock SDK kernel configuration explicitly says `# CONFIG_FB is not set`.
The fbdev tests therefore use a separate kernel **inside the Android SDK
emulator**, with its matching virtio-gpu and virtio_dma_buf modules. This is real
kernel fbdev emulation on virtio-gpu, not a userspace mock. It does not validate
a vendor's native fbdev implementation or a physical Android panel.

Screenshots were opened and visually inspected for channel order, borders,
checkerboard geometry, text, widget layout, progress completion and preserved
unchanged areas. LVGL before/after captures also check that animation advances
without changing the previously drawn top area. The runner checks exact colors
in the captured image. The runner also generates local `.json` records with binary
and screenshot hashes, plus kernel `.log` files. Those run-specific artifacts are
ignored by Git; the selected screenshots are committed.

QEMU monitor `screendump` returned a separate black surface even when the SDK
screen was displaying the image. All retained verification images use the SDK
`EmulatorController.getScreenshot` endpoint instead. A capture helper generates
its protobuf bindings from the installed SDK, in a temporary host directory.

## Reproduce

Run from the `miniui` directory after building. No existing emulator should be
using TCP 8556. The verification runner always uses temporary, read-only AVD
storage, and terminates the process group it started.

```sh
# First obtain the SDK module with the interactive runner; it prints its temp dir.
scripts/run_emulator.sh test_device --headless -- -grpc 8556
# Stop that runner after noting /tmp/miniui-emulator.XXXXXXXX/virtio-gpu.ko.

build/capture-env/bin/python scripts/verify_emulator.py lvgl-drm \
  --binary build/miniui-lvgl-init \
  --gpu-module /tmp/miniui-emulator.XXXXXXXX/virtio-gpu.ko

# For the tested Arch host kernel (paths vary with installed kernel version):
mkdir -p build/host-modules
zstd -dc /lib/modules/7.2.6-arch2-1/kernel/drivers/virtio/virtio_dma_buf.ko.zst \
  > build/host-modules/virtio_dma_buf.ko
zstd -dc /lib/modules/7.2.6-arch2-1/kernel/drivers/gpu/drm/virtio/virtio-gpu.ko.zst \
  > build/host-modules/virtio-gpu.ko
build/capture-env/bin/python scripts/verify_emulator.py lvgl-fbdev \
  --backend fbdev --binary build/miniui-lvgl-init \
  --kernel /boot/vmlinuz-linux --modules build/host-modules \
  --gpu-module build/host-modules/virtio-gpu.ko
# Replace --backend fbdev with --backend auto-fbdev to test discovery fallback.
```

`auto-fbdev` is only a harness setting. The library receives `MUI_AUTO`; it is
not told to select fbdev. The harness removes DRM nodes from its private guest
`/dev` so discovery proceeds to the real framebuffer. Fault injection additionally
checks fallback when a DRM node exists but cannot provide dumb buffers.

## Host checks

`formats`, `failures`, and `lvgl-adapter` pass both normally and with
`-fsanitize=address,undefined -fno-omit-frame-pointer -g`. Assertions remain
active in release test builds. Fault injection is supplemental to emulator tests.

Static ELF x86_64 and aarch64 builds have no dynamic interpreter. ARM64 is
cross-build verification only. Approximate stripped executable sizes using the
installed glibc toolchains: x86_64 pattern init 951 KiB; x86_64 LVGL init 1.3 MiB;
aarch64 LVGL init 968 KiB. Core backend object text totals 5,573 bytes in the
x86_64 MinSizeRel build. A musl build may reduce libc overhead but was not tested.
