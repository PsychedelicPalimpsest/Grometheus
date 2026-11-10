# Performance audit

Measured 2026-11-10, x86_64, Intel i7-12700H, GCC, MinSizeRel (`-Os`).
These are single-run wall-clock samples under normal host scheduling, not hardware
throughput guarantees. No CPU affinity or frequency locking was used.

## Fix: native fbdev copies

The old fbdev loop scaled three channels and copied each pixel separately even
when the framebuffer already used native XRGB8888. The native path now copies
rows with `memcpy`. Native RGB with transparency uses a simple mask/alpha fill;
other validated layouts retain the general conversion. Source stride, framebuffer
pitch, visible offsets, and untouched padding remain respected. No dependencies
were added. The unused X byte is now copied as-is; it has no scanout meaning.

CPU/RAM benchmark, 10 warmups followed by 200 copies per format:

| Format / size | Before median | After median | Speedup |
|---|---:|---:|---:|
| XRGB8888, 800×600 | 3.656 ms | 0.100 ms | 36.5× |
| ARGB8888, 800×600 | 3.768 ms | 0.203 ms | 18.5× |
| XRGB8888, 1920×1080 | 16.088 ms | 0.332 ms | 48.5× |
| ARGB8888, 1920×1080 | 16.502 ms | 0.922 ms | 17.9× |

Raw results: `performance-copy-before.txt` and `performance-copy-after.txt`.
RAM results exclude actual device mapping, scanout, and driver costs. Reproduce:

```sh
cmake -S miniui -B miniui/build -DMUI_BENCHMARKS=ON -DCMAKE_BUILD_TYPE=MinSizeRel
cmake --build miniui/build
miniui/build/bench-fb-copy
```

## Android SDK emulator

800×600, KVM, virtio-vga. Presentation timings wrap only `mui_present`, exclude
pattern generation and the 16 ms inter-frame sleep, and include the first frame.
120 samples per run. The stock SDK Android 13 kernel supplies DRM. It lacks
CONFIG_FB; fbdev runs in the same SDK emulator with `/boot/vmlinuz-linux` and
its virtio-gpu modules, using the real fbdev mapping.

| Presentation | Median | p95 | Maximum |
|---|---:|---:|---:|
| fbdev before | 5.873 ms | 7.851 ms | 9.685 ms |
| fbdev after | 0.253 ms | 2.071 ms | 2.237 ms |
| DRM (unchanged algorithm) | 0.287 ms | 0.398 ms | 29.993 ms |

The fbdev median improves 23.3× in this run. DRM has an observed long tail;
this measurement does not distinguish driver latency from guest/host scheduling.
These call latencies are **not** panel refresh rates or input-to-display latency.

LVGL handler samples during the two-second animation (includes calls with no
refresh due, rendering, and presentation):

| Backend | Calls | Median | p95 | Maximum |
|---|---:|---:|---:|---:|
| DRM | 194 | 0.0013 ms | 0.645 ms | 3.899 ms |
| fbdev | 191 | 0.0013 ms | 2.198 ms | 2.312 ms |

The low median reflects no-refresh calls, **not** rendered frame latency. These
samples are useful for handler occupancy only; they do not isolate LVGL rendering
from backend presentation. No before/after LVGL speedup is claimed.

## Remaining findings

1. **Full-frame traffic for small updates.** LVGL renders partial rectangles into
   the persistent surface, then presents the entire frame once per refresh.
   At 800×600 this copies 1.92 MB per presentation; at 1080p, 8.29 MB.
   At 60 presentations/s the destination writes alone are 115.2 / 497.7 MB/s,
   with matching source reads, plus rendering and driver traffic. A damage API
   would help small animations. DRM alternates buffers, so damage must include
   changes missing from each individual buffer; blindly copying the latest
   rectangle would resurrect stale pixels.
2. **DRM modesets every frame.** Each presentation copies the surface, calls
   DIRTYFB, then synchronously SETCRTC. This is compatible but can be expensive
   on real display drivers. Event-driven page flips with bounded waits and
   explicit timeout/recovery ownership are the next architectural optimization.
   No page-flip implementation was introduced during this audit.
3. **Other fbdev formats still convert per pixel.** RGB565, 24-bit, and unusual
   channel layouts retain the general loop. Their physical-device performance
   has not been measured. fbdev remains unsynchronized and can tear.
4. **Example wakeups and pacing.** The LVGL example wakes every 10 ms even after
   animation ends. The plain demo redraws the entire pattern and sleeps 16 ms
   after presentation, so work time adds to its frame period. Production callers
   should use LVGL's next timer deadline and input events to sleep when idle.
5. **Memory footprint.** DRM requires the shadow surface plus two driver buffers
   (at least 5.76 MB at 800×600 or 24.88 MB at 1080p, excluding pitch padding).
   fbdev holds the shadow plus saved visible pixels and a device mapping. LVGL
   additionally allocates a 32-row draw buffer (102.4 KB at width 800), objects,
   fonts, and renderer allocations. These are calculated sizes, not RSS readings.

The hot paths perform no per-frame heap allocation in miniui itself. Adapter
flushes present only after the final LVGL chunk. Allocation and format validation
remain outside the presentation loop. ARM64 was cross-built, not benchmarked on
physical hardware.

## Verification

Normal and ASan/UBSan suites pass (formats, resource/error handling, LVGL adapter).
New format checks cover native copies with offsets and padded source rows, untouched
destination padding, and alpha replacement. ARM64 cross-build passes.
Real SDK gRPC screenshots, rather than QEMU's unrelated display surface, verify
colors and retained content. The runner generates local `perf-*.json` records with binary and screenshot
hashes and logs with timings. These run-specific files and screenshots are ignored
by Git; the raw copy benchmark output is retained.
Screenshots were also inspected visually. Timing instrumentation is confined to examples and the optional benchmark.
