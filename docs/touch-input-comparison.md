# Touch input source comparison

This is a source comparison, not a claim that a particular device is supported.
The checked-out sources are:

- TWRP recovery `android-10.0` at `bb4ee71`, cloned to
  `/tmp/grom-touch-twrp-android10`.
- TWRP recovery `android-14.1` at `426b747`, cloned to
  `/tmp/grom-touch-twrp-android14`.
- AOSP recovery `android10-release` at `5a6cde8`, cloned to
  `/tmp/grom-touch-aosp-recovery`.
- AOSP frameworks/native `android10-release` at `2173449`, cloned to
  `/tmp/grom-touch-aosp-native` (sparse checkout of `services/inputflinger`).

## Where TWRP reads touch

TWRP's `minuitwrp/events.cpp` enumerates `/dev/input/event*` in `ev_init`,
polls the opened descriptors and reads `struct input_event` in `ev_get`, and
converts raw touch frames to screen coordinates and press/release events in
`vk_modify`. `gui/gui.cpp` then converts those synthetic events into
`TOUCH_START`, `TOUCH_DRAG`, and `TOUCH_RELEASE`. Its `ABS_MT_SLOT` handling
does not maintain a separate contact state per slot. AOSP recovery's
`minui/events.cpp` provides event-device discovery and reading; AOSP Android's
full touch decoding is in frameworks/native `services/inputflinger/InputReader.cpp`.

## Protocol coverage

| Input report | TWRP | AOSP Android 10 | grom |
| --- | --- | --- | --- |
| `ABS_X/Y` with `BTN_TOUCH` | Yes | Yes | Yes |
| MT type A, `ABS_MT_POSITION_X/Y` and `SYN_MT_REPORT` | Yes | Yes | Yes |
| MT type B, `ABS_MT_SLOT` and `ABS_MT_TRACKING_ID` | Basic single-pointer translation | Per-slot state | Per-slot state, single-pointer preboot UI |
| Packed legacy `ABS_MT_POSITION` (`0x2a`) | Yes | No path found | No |
| Relative mouse and mouse buttons | Yes | Yes | No; separate from touchscreen support |

The Linux multitouch specification defines type A and type B. Grom covers both
standard MT protocols. The packed `0x2a` event is a TWRP legacy extension,
not a third standard Linux MT protocol. Modern Linux input headers do not
define it. Only implement it as an explicitly selected compatibility path
after seeing that event in a target device trace; its axis geometry and
release behavior require device evidence.

TWRP also treats zero `ABS_MT_TOUCH_MAJOR` or `ABS_MT_PRESSURE`, negative
`ABS_MT_TRACKING_ID`, and one packed-position sentinel as possible release
signals. It has build-time switches to ignore some of these signals, swap or
flip axes, blacklist input devices, or use a key code as a touch sync signal.
These are device-specific workarounds. In particular, pressure zero can mean
hover rather than release, and a type B tracking ID of -1 only releases its
slot. Grom should not apply TWRP's release guesses globally.

## Compatibility work worth doing

1. Keep the evdev frame decoder generic. Continue to select an input device
   by capabilities and use `SYN_REPORT` for frame boundaries. The Moto G Fast
   Himax trace and a physical tap have already exercised grom's X/Y-before-ID
   handling; no TWRP-specific parser is needed for that device.
2. Add optional touch transforms (swap X/Y, flip X/Y, and calibration) to a
   device profile only when a real device trace and screen test show the need.
   Axis ranges alone cannot determine rotation or mirroring.
3. For type B, grom now initializes the current slot from
   `EVIOCGABS(ABS_MT_SLOT)` on attach and refreshes it after a dropped-event
   resync. This matters if the device omits `ABS_MT_SLOT` because the current
   kernel slot has not changed.
4. Keep a trace-based compatibility path for nonstandard report formats and
   release signals. Capture `getevent -lp` and raw event sequences from the
   working recovery before adding one, and test press, stationary hold, lift,
   second finger, dropped frame, and reconnection.

Source links: [TWRP events](https://github.com/TeamWin/android_bootable_recovery/blob/bb4ee714/minuitwrp/events.cpp),
[TWRP GUI](https://github.com/TeamWin/android_bootable_recovery/blob/bb4ee714/gui/gui.cpp),
[AOSP recovery events](https://android.googlesource.com/platform/bootable/recovery/+/5a6cde8b02d860932a7e415990d3ec00539bdd98/minui/events.cpp),
[AOSP InputReader](https://android.googlesource.com/platform/frameworks/native/+/2173449d9dccb15b87c9df5d373dc32e1dcabed6/services/inputflinger/InputReader.cpp),
[Linux MT protocol](https://cdn.kernel.org/doc/html/latest/input/multi-touch-protocol.html),
[Android touch devices](https://source.android.com/docs/core/interaction/input/touch-devices).
