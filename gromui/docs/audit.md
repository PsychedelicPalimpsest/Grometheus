# Resilience audit

Performed after the initial DRM/fbdev emulator pattern checks, then extended
for the LVGL adapter. Tests are in `tests/`; screenshots are actual SDK output.

## Changes and checks

- Removed Android/libdrm/C++ dependencies and compile-time pixel-format choices.
  Vendored only DRM UAPI headers; retained upstream licenses and provenance.
- Checked dimensions, multiplication bounds, pitch, mapped size, offsets,
  channel widths/overlap, packed-pixel type, truecolor visual and unsupported
  grayscale/nonstandard formats before writes. Framebuffer mappings and individual
  buffers are capped at 256 MiB, dimensions at 16384. These are per-allocation
  limits, not an aggregate process memory budget.
- Tested RGB565, 24-bit swapped-channel layout and 32-bit ARGB, including row padding and
  nonzero offsets. Rejected overlapping channels and out-of-bounds offsets.
- DRM connector discovery forces the initial kernel probe, bounds resource lists
  to 128 entries, and clears unrequested property counts before the second query.
  Prefers embedded LVDS/eDP/DSI panels. A failure on a card proceeds to other cards
  and then fbdev. It does not exhaust every possible mode/CRTC combination on a
  failing card; unusual hardware may need additional selection policy.
- Used driver-returned dumb-buffer allocation size/pitch. All ioctl argument
  structures and unused fields are initialized. File descriptors use CLOEXEC.
- Capped repeated EINTR handling at 16 attempts; synchronous KMS eliminates
  unbounded page-flip event loops and pointers to expired callback state.
  A successful modeset is required before changing the tracked front buffer.
- Restored prior CRTC connectors/mode before removing framebuffer IDs and dumb
  buffers, then closed the device. Partial initialization uses the same cleanup.
  Restore failure is returned while resources are still released.
- fbdev keeps the existing mode and uses a persistent software surface. Saves and
  restores visible pixels, without overwriting padding or other virtual pages.
  Its best-effort unblank cannot restore unknown prior backlight/blank state.
- Fault injection traverses open, ioctl and mmap failures and checks that fds,
  mappings, framebuffer IDs and dumb handles are released. Also checks failed
  present keeps the old front-buffer index, failed restoration is reported, and
  a DRM card lacking dumb buffers falls back to fbdev.
- LVGL uses a persistent gromui surface plus 32-row partial buffers, validates
  flush rectangles, accounts for source stride, and presents only at the final
  flush. Sticky error reporting still calls `lv_display_flush_ready` on failure.
  The delete event frees adapter storage. Tests check partial-update preservation,
  color, presentation count, invalid creation, failed presentation and teardown.
- LVGL is pinned to 9.4.0 with a checked local archive hash. Its allocator uses
  libc; fatal internal assertions abort instead of silently spinning forever.
  The test init supervises the renderer in a child to protect PID 1.
- Standard and ASan/UBSan test suites pass. x86_64 and ARM64 static builds pass.
  Screenshots verify both kernel backends and automatic fallback in the SDK
  emulator. RGB565 and 24-bit formats have host tests, not physical panel tests.

## Remaining boundaries

This is Linux display portability, not portability to arbitrary operating
systems or every Android vendor driver. It requires legacy KMS plus dumb
buffers or packed truecolor fbdev. Atomic-only/vendor-specific drivers are
outside this implementation. Big-endian and 32-bit targets are not verified.

There is no input, rotation, hotplug manager, automatic live backend migration,
brightness policy, DRM leasing, multiseat or shared-compositor support. Use
exclusive display ownership; external mode changes can invalidate cached layout.
Framebuffer writes can tear. Synchronous DRM modesets trade throughput for a
small, predictable resource lifecycle and can still stall inside a kernel driver.

A production recovery must supply a rendering watchdog, controlled shutdown,
matching modules, device discovery and panel/backlight initialization. No library
can restore device state after SIGKILL or a kernel failure. The test `/init` is
not a replacement for that production boot policy.
