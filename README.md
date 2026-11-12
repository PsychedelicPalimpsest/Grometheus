# grom

grom is an Android multiboot project. The current combined `/init` build is an
integration scaffold, **not a bootable multiboot implementation**. Its preboot
hook starts a supervised LVGL display test and then waits indefinitely because
credential processing, charger handling, slot authentication, storage mapping and
safe handoff are still unimplemented. It cannot boot an Android OS in this
state. The test screen accepts touch input, but its backend rejects unlock.

## Layout

- `grom/`: first-stage grom integration code, supervised test UI and its C ABI.
- `gromui/`: independent C11 display library, LVGL adapter and emulator test
  init. Its example executables are not production init. See
  [gromui/README.md](gromui/README.md) for its own build and test workflow.
- `vendor/magisk/`: pinned Magisk source with a small MagiskInit fork. See
  [docs/magisk-vendor.md](docs/magisk-vendor.md) for provenance and changes.
- `grombake.py`: detects the boot image architecture, builds the matching static
  combined init, patches an image copy, reopens it for verification, and records
  hashes. `scripts/build-init.sh` is its lower-level build step.
- `motogfast/`: device research notes and extracted text references. Firmware
  images stay local and are not evidence of a working combined boot path.

The current emulator display result is [shown here](docs/preboot-lvgl.png).
See [backend calls from the preboot UI](docs/backend-ui-flows.md) for the
password, charging, selection and provisioning call paths.

## Build or patch an image

From the project root:

```sh
python3 grombake.py                         # build both configured ABIs
python3 grombake.py /path/to/boot.img       # detect, build, patch, verify
python3 grombake.py /path/to/ramdisk.img    # Android SDK emulator ramdisk
```

The builder prefers an SDK with Magisk's pinned ONDK r29.2 already installed,
even if `ANDROID_HOME` points elsewhere. If ONDK is missing, it invokes the
vendored Magisk installer to download it. It builds x86_64 for the Android SDK
emulator and arm64-v8a for the Moto G Fast development target, then runs the
gromui tests and writes `out/grom-init/build.json` with hashes and static ELF
checks. Given an image or directory, it detects x86_64 or arm64-v8a from the
kernel and ramdisk init, builds that ABI, saves the original init as
`/init.orig`, and writes a patched copy under `out/grom-images/` with a JSON
sidecar. It accepts Android boot images and standalone ramdisks; on devices
with a separate `init_boot.img`, it selects that sibling when `boot.img` has no
ramdisk. It never flashes a device or modifies the input image.

```sh
python3 grombake.py --offline             # use cached ONDK and Cargo sources
python3 grombake.py --sdk /path/to/sdk    # choose an SDK explicitly
python3 grombake.py --release             # Magisk release build
python3 grombake.py boot.img -o /tmp/grom-boot.img
python3 grombake.py boot.img --arch arm64-v8a # fallback only if detection fails
python3 grombake.py --list-steps
```

Device driver profiles live in `grom/devices/`. To bundle early modules from a
downloaded ROM ZIP (or an unpacked ROM directory), pass it as the image source:

```sh
python3 grombake.py /path/to/stock-rom.zip --driver-profile motogfast-android10
```

The ROM extractor reads super metadata, copies only vendor partition ranges,
and extracts only files named by the profile. It handles Motorola
`super.img_sparsechunk.N`, split raw `super.N.img`, a whole `super.img`, and a
standalone `vendor.img`. Both populated vendor slots require `--vendor-slot a`
or `b`. The current filesystem extractor supports ext4 vendor partitions;
other filesystems fail with an explicit error. ROM extraction needs `lpdump`
for super metadata and `debugfs` for ext4 files. To bundle from an already
extracted vendor tree, use:

```sh
python3 grombake.py /path/to/boot.img \
  --driver-profile my-device --driver-source /path/to/extracted/vendor
```

When the boot image and ROM dump are separate, pass the boot image as the
positional argument and the archive with `--romdump /path/to/stock-rom.zip`.

For a USB shell in preboot, build with `--debug-adb`. The builder copies
`adbd`, `sh`, `toybox`, the Android linker, and their ELF dependencies from a
matching recovery image into the ramdisk. Supply that image with
`--debug-adb-source /path/to/recovery.img`; recovery images are not bundled
in this source tree.
The shell is available as `adb shell`, and `adb shell dmesg` includes grom's
stderr diagnostics. The debug command accepts display-pixel coordinates and
captures the renderer's current frame as a PPM image:

```sh
adb shell grom-debug tap 320 780
adb exec-out grom-debug screenshot > preboot.ppm
adb shell grom-debug bootloader
```

`/grom/ui-timing` reports physical touch events and renderer timing. This is a
root shell before authentication. Images built
with `--debug-adb` must be treated as debug artifacts; normal builds omit it.
The ADB gadget uses USB only and remains in preboot while this scaffold waits
for a slot. The shell, tap injection, renderer screenshots, and reboot to the
bootloader have been verified on the Moto G Fast.

The builder rejects missing files, wrong ELF architecture, and a known target
kernel release that differs from the profile. It copies the ordered modules
to `/grom/drivers/` and any profile-listed firmware to `/lib/firmware/` in the patched ramdisk,
then reopens the image and checks every copy against its source. At preboot,
grom attempts the listed modules in order and unlinks each staged `.ko` and
the list after use. Loaded kernel drivers remain available to Android.
Some touchscreen drivers separately request `.bin` controller firmware after
loading. The `motogfast-android10-inx` profile contains the Himax module pair
and firmware observed on the user's INX panel in TWRP. Firmware remains in the
preboot ramdisk so the driver can request it again while preboot is running.
This Android 10 profile rejects the repository's newer Android 11 boot kernel.
On a temporary Moto G Fast debug boot, the Himax touchscreen registered and
physical stationary keyboard taps reached LVGL after fixing the protocol-B
decoder to retain coordinates reported before a new tracking ID. The user
confirmed typing works. That verified boot used the `inx_Himax_firmware.bin`
extracted from the phone's vendor partition; it differs from the Android 10
ROM dump copy, so matching firmware to the phone remains important. The
opt-in ADB image provided a working root `adb shell`.

Python 3.10+, CMake, a host C compiler, Git, Make and binutils are needed.
The patched image enters a display-only preboot scaffold and cannot hand off
to Android. Do not flash it to a physical device. For direct control of the
native build, set `ANDROID_HOME` to the SDK containing ONDK and run
`scripts/build-init.sh`.

To check an SDK x86_64 ramdisk in the emulator, point the runner at the
builder's output and the corresponding binary. The runner stages the SDK's
matching virtio display and touch modules in another temporary ramdisk copy
and checks that the embedded `/init` matches the built binary:

```sh
GROMUI_RAMDISK_IMAGE=/path/to/out/grom-images/ramdisk-grom.img \
GROMUI_BINARY="$PWD/out/grom-init/x86_64/init" \
GROMUI_SCREEN_SIZE=720x1560 \
gromui/scripts/run_emulator.sh test_device --headless -- -grpc 8556
```

The sidecar JSON records the exact image output path and hashes. The Moto G
Fast `boot.img` reference can be inspected and repacked by the builder, but
that is not a physical boot test.
