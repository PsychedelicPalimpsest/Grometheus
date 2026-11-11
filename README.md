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
- `scripts/build-init.sh`: builds one combined `magiskinit` binary per ABI and
  copies it to `out/grom-init/<abi>/init`.
- `motogfast/`: supplied physical device reference artifacts. They are not
  evidence of a working combined boot path.

The current emulator display result is [shown here](docs/preboot-lvgl.png).

## Build the combined binary

Magisk v30.4 requires its pinned ONDK r29.2 toolchain. From the project root:

```sh
export ANDROID_HOME="$HOME/Android/Sdk"
cd vendor/magisk
python3 build.py ndk
cd ../..
scripts/build-init.sh
```

The wrapper builds x86_64 for the Android SDK emulator and arm64-v8a for the
Moto G Fast development target. It verifies the pinned LVGL archive while
preparing the native build. Compilation alone does not establish boot support;
do not install the output as a boot image until the preboot flow and target
boot sequence have been implemented and validated.
