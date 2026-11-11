# Magisk source and integration

The source under `vendor/magisk` is vendored from
[topjohnwu/Magisk v30.4](https://github.com/topjohnwu/Magisk/tree/c3db2e368d47731e625dd0ed499f66bb57ed325b),
commit `c3db2e368d47731e625dd0ed499f66bb57ed325b`, including the submodule
commits recorded in `vendor/magisk/UPSTREAM.lock`. Upstream license
texts remain in the vendor tree. The source is copied into this repository so
the native build does not fetch source submodules. Its ONDK and Rust dependency
toolchains still need setup.

Fork changes:

1. `native/src/init/init.rs` calls `grom_preboot_run()` after basic `/proc` and
   `/sys` setup but before `BootConfig::init()` and Magisk's first-stage path.
   It bypasses the hook for the `selinux_setup` second-stage re-entry.
2. `native/src/Android.mk` builds `grom/`, the `gromui/` display backend, its
   LVGL adapter and pinned LVGL source as static libraries linked into the
   existing `magiskinit` executable. No wrapper executable is used.

The current `grom_preboot_run()` starts a supervised LVGL display test and
intentionally cannot return success. A renderer failure leaves PID 1 in
preboot. This prevents a raw encrypted pool from reaching stock init before
slot selection and mapping exist. MagiskInit's own init replacement, root
switching, policy handling and handoff remain upstream code. The hook position
is based on the pinned Magisk source, not yet verified against the emulator's or Moto
G Fast's actual boot-stage fstab consumers. Charger behavior and a safe boot
handoff require target-specific validation before this binary can be used.

## Verification (2026-11-11)

- `scripts/build-init.sh` built static x86_64 and arm64-v8a ELF executables.
  `llvm-nm` found both `grom_preboot_run` and `gromui_open` in each output.
- The documented gromui CMake build passed all three host tests: formats,
  failures and LVGL adapter.
- A disposable Android SDK `test_device` emulator ramdisk ran the x86_64
  output as `/init`. The kernel log showed `Run /init as init process`, followed
  by `magiskinit: grom preboot: waiting for slot selection`. The emulator was
  stopped after 15 seconds. This proves entry into the hook, not a stock Android
  handoff or any encrypted slot behavior.
- No physical Moto G Fast boot or storage test was performed.

## Preboot display test (2026-11-11)

The combined x86_64 binary rendered [the LVGL test screen](preboot-lvgl.png)
through the SDK emulator's DRM device at 800×600. The screenshot came from
the emulator gRPC display endpoint. Pixel checks confirmed the accent bar,
background, card and password label; the screen states that input is inactive.
`llvm-nm` found `grom_preboot_run`, `gromui_open`, `gromui_lvgl_create` and
`lv_init` in both x86_64 and arm64-v8a outputs.

A second disposable emulator run supplied an empty virtio-gpu module. After
display discovery failed, MagiskInit logged `grom preboot: test renderer
unavailable; staying in preboot`; the emulator log contained no kernel panic.
Neither test exercises credential entry, charger mode, slot mapping or Android
handoff. The arm64 output was cross-compiled and was not tested on the Moto G
Fast.

The preboot display path now lists device and sysfs directories rather than
opening every possible card and framebuffer pathname on every retry. It
refreshes sysfs nodes once per second when devtmpfs is unavailable. After the
first display present, PID 1 blocks waiting for renderer exit instead of
polling the child each second. Kernel interfaces still require `ioctl`,
`finit_module`, `unshare`, `pipe2`, `kill` and `wait4` calls; the static Magisk
runtime does not provide libc wrappers for several of these. The combined
x86_64 binary rendered the same 800×600 screen after this change. Both ABI
outputs and the host gromui executables have no ELF dynamic section; the
build script rejects a dynamically linked combined init. A fresh no-GPU
emulator run reached `test renderer unavailable; staying in preboot` at
11.18 seconds without a kernel panic. The updated standalone gromui init also
rendered its DRM color pattern at 800×600 in the SDK emulator.

To reproduce the screen capture, build the combined init, then run the
existing gromui emulator harness with that binary:

```sh
ANDROID_HOME="$HOME/Android/Sdk" scripts/build-init.sh
GROMUI_BINARY="$PWD/out/grom-init/x86_64/init" \
  ANDROID_SDK_ROOT="$HOME/Android/Sdk" \
  gromui/scripts/run_emulator.sh test_device --headless -- -grpc 8556
```

In a second terminal, use the Python capture environment described in
`gromui/README.md` and run `gromui/scripts/capture.py docs/preboot-lvgl.png`.
