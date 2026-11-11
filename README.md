# grom

grom is an Android multiboot project. The current combined `/init` build is an
integration scaffold, **not a bootable multiboot implementation**. Its preboot
hook starts a supervised LVGL display test and then waits indefinitely because
credential input, charger handling, slot authentication, storage mapping and
safe handoff are still unimplemented. It cannot boot an Android OS in this
state. The test screen does not accept input.

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
- `motogfast/`: supplied physical device reference artifacts. They are not
  evidence of a working combined boot path.

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

Python 3.10+, CMake, a host C compiler, Git, Make and binutils are needed.
The patched image enters a display-only preboot scaffold and cannot hand off
to Android. Do not flash it to a physical device. For direct control of the
native build, set `ANDROID_HOME` to the SDK containing ONDK and run
`scripts/build-init.sh`.

To check an SDK x86_64 ramdisk in the emulator, point the runner at the
builder's output and the corresponding binary. The runner stages the SDK's
matching virtio-gpu module in another temporary ramdisk copy and checks that
the embedded `/init` matches the built binary:

```sh
GROMUI_RAMDISK_IMAGE=/path/to/out/grom-images/ramdisk-grom.img \
GROMUI_BINARY="$PWD/out/grom-init/x86_64/init" \
gromui/scripts/run_emulator.sh test_device --headless -- -grpc 8556
```

The sidecar JSON records the exact image output path and hashes. The Moto G
Fast `boot.img` reference can be inspected and repacked by the builder, but
that is not a physical boot test.
