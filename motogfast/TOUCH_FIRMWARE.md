# Moto G Fast touchscreen firmware findings

The firmware images and extracted binary artifacts discussed here are local
inputs; they are not part of the source repository.

## Artifact inspected

The local stock firmware archive is
`/home/mitch/Downloads/old/Motorola_Moto_G_Fast_XT2045-3_RAV_RETUS_10_QPJ30.131-16_10_(by_motostockrom.com).zip`.
Its SHA-256 is `c5a80cdf6a41a0c0536dd0fff100bf8a0a7ae11642207db717a4b6710e0b570d`.
It identifies itself as Android 10, build `QPJ30.131-16`. Its `super` image
contains the `vendor_a` partition inspected below. This is an artifact finding,
not a test of the user's running phone.

## Observed touchscreen drivers and firmware

The stock `vendor_a` filesystem contains these modules under
`/vendor/lib/modules/`:

| Module | Candidate controller |
| --- | --- |
| `focaltech_0flash_mmi.ko` | FocalTech FT8009 |
| `nova_0flash_mmi.ko` | Novatek |
| `himax_v2_mmi_hx83112.ko` | Himax HX83112 variant |
| `himax_v2_mmi.ko` | Himax |

All four report module `vermagic` of
`4.14.117-perf+ SMP preempt mod_unload modversions aarch64`.
`/vendor/etc/init/hw/init.mmi.overlay.rc` runs, during `early-init`:

```text
exec u:r:vendor_modprobe:s0 -- /vendor/bin/modprobe -a -d /vendor/lib/modules focaltech_0flash_mmi nova_0flash_mmi himax_v2_mmi_hx83112 himax_v2_mmi
```

The same vendor filesystem has candidate firmware under `/vendor/firmware/`:
`FT8006U_Pramboot.bin`, three `focaltech-*-ft8009-*-rav.bin` files,
`inx_Himax_firmware.bin`, `inxr_Himax_firmware.bin`, and
`novatek_ts_fw.bin`. Other manufacturing/test firmware files are present.
The kernel driver may require one of these files after the module loads.

The supplied `motogfast/RAV_RETUS/dtbo.img` has panel-dependent nodes for
`focaltech,fts`, `novatek,NVT-ts-spi`, and `himax,hxcommon`, selected by
`/chosen/mmi,panel_name`. The actual panel fitted to this phone is unknown.

## Compatibility and next validation

The local `motogfast/RAV_RETUS/boot.img` describes Android 11 with a
`4.14.180-perf+` kernel, so it is a different build from the Android 10
archive. A local TWRP image with a `4.14.170-perf+` kernel nevertheless bundles
`4.14.117-perf+` touchscreen modules. Its `/sbin/load_ts_firmware.sh` script
attempts to load `focaltech_0flash_mmi_rav.ko` for `rav`/`rav_t`, then the
Novatek and Himax candidates, and triggers firmware flashing through
`/sys/class/touchscreen`. That script is evidence of a loading strategy, not
proof that they load
successfully on either newer kernel. Module symbols, version CRCs, and the
running device must be checked before reusing the archived modules.
The source profile `grom/devices/motogfast-android10.json` records these
Android 10 candidates. `grombake.py` rejects it when paired with the local
Android 11 boot image because the kernel releases differ.

The current grom touch implementation reads Linux evdev nodes; it does not
itself supply controller firmware. The preboot bundle loader inserts modules;
grombake can now stage profile-listed firmware in `/lib/firmware` for the kernel
firmware loader. The original four-module image did not respond to touch.

On the user's device running TWRP 16.1.0 with kernel `4.14.170-perf+`, read-only
ADB checks identified panel `inx_hx83104a_hd_vid` and touchscreen class
`HX83112A`. TWRP had loaded the four candidate modules and exposed
`himax-touchscreen` on `/dev/input/event3`. Its kernel log showed the Himax
input device registering, then `request_firmware` loading
`inx_Himax_firmware.bin` and writing it to the controller's SRAM. The TWRP
firmware file and the Android 10 ROM's copy have identical SHA-256
`3cc4c2911342cb9b4190cacb2cec4302af497c8fe278ae6c060d56df63de62b4`.
The kernel config reports `CONFIG_DEVTMPFS` disabled and firmware-loader
userspace fallback enabled. These observations support a missing firmware
payload as one cause of failure; they do not prove that the Android 10 boot
image can initialize touch with the phone's currently installed DTBO.

`motogfast-android10-inx.json` bundles the Himax module pair and observed
firmware. A `fastboot boot` test of that image still showed no touch response.
An opt-in `--touch-diagnostics` image is being used to distinguish module
insertion, driver probe, firmware load, and evdev handling. A matching image
of the phone's installed vendor partition remains useful for further checks.
