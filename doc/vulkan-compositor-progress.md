# Vulkan compute compositor progress

This file tracks the work needed to reach visual model parity with KWin's
OpenGL compositor. A checked item is implemented and covered by an automated
test unless the item explicitly says otherwise.

## Compute foundation

- [x] Vulkan instance and DRM-device matching
- [x] dma-buf import and synchronization-fd submission
- [x] Prefer a high-priority, compute-only queue and fall back safely
- [x] Separate graphics and compute command pools/submission paths
- [x] 16x16 tile grid
- [x] Adaptive GPU AABB-to-tile preprocessing (direct masks for shallow scenes; hierarchical 2D prefix masks for larger scenes)
- [x] Summarized per-tile layer bitsets with no fixed scene-layer limit
- [x] GPU timestamp queries for preprocessing and composition
- [x] Packaged shader descriptor-interface validation before pipeline creation
- [x] Validation-layer test execution
- [x] Device-loss recovery for compositor resources

## Visual model

- [x] Solid premultiplied-alpha layers
- [x] Single-plane RGBA textures
- [x] Buffer transforms, scaling, and clipping
- [x] Per-item opacity, brightness, and saturation
- [x] Image items and per-sprite atlases
- [x] Multi-plane YUV sampling and range/matrix conversion (NV12/P010 dma-buf plane import is compile-tested; planar shader path is GPU-tested)
- [x] Rounded corners and outlined borders
- [x] Decorations and sprite atlases, including sparse zero-sized parts used with separately rendered border outlines
- [x] Shadow nine-patch upload and quad rendering
- [x] Per-layer transfer functions, colorimetry conversion, linear effects, and perceptual HDR tone mapping
- [x] ICC/LUT output calibration (BToA, shaper-matrix, MHC2, and VCGT operation chains)
- [x] High-precision output intermediates (RGBA16F effect/blur scratch with RGBA8 fallback; 10-bit, 16-bit UNORM, and RGBA16F compositor targets; DRM format selection honors the accuracy/power tradeoff)
- [x] All eight output transforms and final output color-pipeline offload
- [x] Effect offscreens and render-target nesting
- [x] Cross-fade snapshots
- [x] Built-in offscreen color filters (Invert, Color Blindness Correction, and System Bell color/invert)
- [x] Backdrop blur (ordered dual-Kawase passes, region and rounded-window clipping, opacity/saturation matrix, additive noise, and triple-buffered scratch resources)
- [x] Arbitrary legacy effect fragment shaders through a fenced EGL/dma-buf compatibility bridge (see below)
- [x] Layer-bound debug overlay used by the Show Compositing effect
- [x] Fractional-coordinate debug visualizer (`KWIN_SCENE_VISUALIZE=fractional`; red texture-sampling and blue transformed-vertex overlays, shader- and renderer-tested)

## KWin integration

- [x] Vulkan render target with acquire/completion fences and GPU timing
- [x] Vulkan `ItemRenderer` scene traversal
- [x] Selectable `VulkanCompositing` mode (`KWIN_COMPOSE=V`)
- [x] Vulkan dma-buf output-layer swapchain and buffer-age tracking
- [x] DRM backend (compiled; virtual DRM coverage, physical KMS run still needed)
- [x] Wayland nested backend (live-presented to a KWin host under Vulkan validation)
- [x] Advertise renderer linux-dmabuf feedback so Wayland EGL and Xwayland GL clients stay GPU-accelerated in nested Vulkan sessions
- [x] X11 nested backend with DRI3 Present wait fences (compiled; live DRI3 host presentation still needed)
- [x] Virtual backend
- [x] Direct scanout and overlay-plane interaction (compiled; physical KMS run still needed)
- [x] Multi-GPU presentation through the existing fenced GPU-copy swapchain (compiled; multi-GPU run still needed)
- [x] Source buffer release points and output completion fences
- [x] Tile-aligned Vulkan repaint expansion before scene occlusion/layer collection (partial-damage move regression plus fractional-scale and all-output-transform coverage)
- [x] DRM presentation tests preserve Vulkan swapchain ages and multi-GPU copy damage history instead of aging untouched test buffers
- [x] DRM presentation tests retain the last rendered framebuffer so asynchronous cursor moves cannot present an unrendered stale cursor slot
- [x] Exact tile damage at fractional output scales using direct output-device/target transform mapping throughout scene collection and rasterization, including tile-footprint buffer-age and KMS/copy damage tracking
- [x] Wayland explicit host synchronization protocol (`linux-drm-syncobj-v1` acquire fences and per-commit release timelines for output and hardware-cursor dma-bufs; live host protocol trace under Vulkan validation)
- [x] Screenshot and screencast paths (output, region, and window capture; PipeWire memfd and directly rendered dma-buf buffers with syncobj fences)
- [x] Embedded and metadata cursor capture paths
- [x] Qt Quick OpenGL/dma-buf scene-graph integration, including internal windows, native Vulkan window-thumbnail targets, two-way Vulkan/EGL fences, compositor release fencing, and a software fallback when native sharing is unavailable

## Effect compatibility

- [x] Quick scene effects (Overview, Window View, and Tiles Editor) through the OpenGL/dma-buf scene graph, including zero-copy Vulkan window thumbnails and a software fallback
- [x] Show FPS and Show Compositing overlays
- [x] Image-item overlays (Screen Edge, Track Mouse, and Shake Cursor; Shake Cursor is compile-tested on Vulkan)
- [x] Color Picker 1x1 Vulkan scene capture (compile-tested; interactive D-Bus coverage pending)
- [x] Painter-style primitive overlays (Mouse Click, Touch Points, Show Paint, and Mouse Mark; renderer differential-tested and effects compile-tested)
- [x] Startup Feedback bouncing, blinking, and passive overlays (live-tested under validation)
- [x] Offscreen mesh deformation (Fall Apart live-tested under validation; Magic Lamp and Wobbly Windows compile-tested)
- [x] 3D animation transforms used by Glide and Sheet (both live-tested under validation)
- [x] Effect-specific color filters (Invert and Color Blindness Correction live-tested; System Bell compile-tested)
- [x] Blur (shader-level visual and live plugin validation coverage)
- [x] Zoom and Magnifier screen-sampling effects (triple-buffered Vulkan scene capture, blur-inclusive capture, Zoom xBRZ/pixel-grid modes, and live validation coverage)
- [x] Animated screen-transform transition (previous/current Vulkan snapshots, compute cross-fade, geometry interpolation/rotation, and live rotate/restore validation coverage)
- [x] Arbitrary legacy effect fragment shaders, including AnimationEffect shaders installed after snapshot capture (live-tested under validation)
- [x] Cross-fade snapshot effects (Blend Changes live-tested under validation)

## Verification and performance

- [x] Differential solid-layer scenes against a QPainter reference
- [x] Differential textured and item-tree scenes against QPainter
- [x] Differential textured item-tree scenes against the OpenGL renderer
- [x] Edge sizes (non-multiples of 16), empty scenes, and scenes beyond 64 layers
- [x] Descriptor-batched scenes beyond 16 unique textures, including destination-out across batches
- [x] Transforms, fractional scaling, clipping, damage-only scenes, and preservation of scene contents around partial-damage tile margins
- [x] GPU-vs-CPU transfer-function, gamut-conversion, and HDR tone-mapping scenes
- [x] ICC and output-calibration scene corpus (BToA, shaper-matrix, and MHC2 profiles)
- [x] Vulkan validation with zero errors
- [x] Live KWin virtual-output frame under Vulkan validation
- [x] Internal-window and offscreen Qt Quick scenes through a Vulkan virtual output (OpenGL/dma-buf path and native Vulkan-to-EGL window thumbnails validation-tested)
- [x] Legacy GL fragment-shader bridge with two-way Vulkan/EGL native-fence synchronization, orientation-sensitive pixels, and late shader installation
- [x] GPU preprocessing/composition microbenchmark
- [x] Overdraw-heavy GPU-time comparison with the OpenGL backend (optimized common compute is slightly faster at 16/64 translucent layers on Navi 10; opaque front-to-back termination is substantially faster before scene occlusion)
- [x] Async-compute latency benchmark under graphics contention, with a forced-graphics-queue comparison mode

### Pre-optimization baseline (2026-07-17)

`vulkanCompositorBenchmark::benchmarkComputeLatencyUnderGraphicsContention`
queues OpenGL raster overdraw without cross-API synchronization, then measures
Vulkan compositor submission-to-fence latency. Graphics completion is excluded
from the measurement; Vulkan GPU timestamps are reported separately so queueing
delay is not mistaken for shader execution time. Set
`KWIN_VULKAN_FORCE_GRAPHICS_QUEUE=1` to select the graphics family before device,
command-pool, and compositor resource creation.

The baseline is commit `fe5d243a82` in a Debug build on an AMD Radeon RX 5700
XT (RADV NAVI10), Mesa 26.1.4, Linux 7.1.3, and Qt 6.11.1. The dedicated
compute queue is family 1; the forced graphics queue is family 0. Global queue
priority was unavailable, so both runs report `highPriority=false`. The amdgpu
performance setting was `profile_standard`; the active DPM states before and
after the runs were 1300 MHz GFX and 875 MHz memory. No Vulkan validation layer
was enabled for performance measurement.

Both queue modes used the same command, with the environment override added for
the graphics-queue control:

```sh
build/bin/vulkanCompositorBenchmark -median 5 -minimumtotal 500
KWIN_VULKAN_FORCE_GRAPHICS_QUEUE=1 build/bin/vulkanCompositorBenchmark -median 5 -minimumtotal 500
```

The isolated results below report Qt's median synchronized wall time and the
median of the benchmark's GPU timestamp samples. GPU time includes AABB
preprocessing and tile composition; the two stages are also shown separately.

| 1920x1080 scene | Compute wall | Compute GPU (preprocess + composite) | Forced-gfx GPU | OpenGL wall | OpenGL GPU |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1 translucent layer | 0.51 ms | 0.210 ms (0.005 + 0.205) | 0.199 ms | 0.087 ms | 0.030 ms |
| 16 translucent layers | 1.59 ms | 1.254 ms (0.024 + 1.229) | 1.254 ms | 0.53 ms | 0.436 ms |
| 64 translucent layers | 5.50 ms | 5.099 ms (0.090 + 5.009) | 5.103 ms | 1.90 ms | 1.732 ms |
| 64 opaque layers | 0.65 ms | 0.304 ms (0.090 + 0.214) | 0.292 ms | 1.90 ms | 1.730 ms |

The translucent worst case is about 2.9x slower than OpenGL in isolated GPU
time. With opaque front-to-back termination, Vulkan is about 5.7x faster than
the unoccluded OpenGL draw loop. Selecting the graphics queue has little effect
on isolated shader time.

The contention benchmark takes 20 samples per test invocation. The table uses
Qt's median of five invocations after its warm-up; p95 and GPU execution are the
medians of the corresponding five reported values.

| Queued raster layers | Compute-only median / p95 / GPU | Graphics median / p95 / GPU | Compute latency reduction (median / p95) |
| --- | ---: | ---: | ---: |
| 0 | 1.540 / 1.560 / 1.254 ms | 1.543 / 1.562 / 1.249 ms | 0.2% / 0.1% |
| 16 | 1.701 / 1.725 / 1.399 ms | 1.801 / 1.822 / 1.249 ms | 5.6% / 5.3% |
| 64 | 2.606 / 2.644 / 2.311 ms | 3.122 / 3.143 / 1.249 ms | 16.5% / 15.9% |

The idle difference is noise-sized. Under the heavier contention case, compute
shader execution itself becomes slower because it shares GPU resources with the
raster workload, but independent scheduling avoids enough graphics-queue delay
to finish 16.5% sooner. Both complete benchmark runs passed 13 tests with no
current-boot kernel GPU-reset, timeout, fault, or device-loss report.

### First optimization pass (2026-07-17)

Commit `9ec59a82e4` integrates the first profiler-guided optimization pass on the
same Navi 10 system and Debug build as the baseline. It adds a 112-byte hot
layer record, prefers host-visible device-local memory for layer records,
precomposes output-pixel-to-UV transforms, packs dirty-tile coordinates, and
routes axis-aligned RGBA/solid source-over scenes without output LUTs through a
separate compact pipeline. General color-managed, YUV, filtered, rounded,
quad, and non-source-over scenes retain the full visual-model pipeline; their
axis-aligned geometry can still skip inverse-transform coverage work.

| 1920x1080 scene | Baseline Vulkan GPU | Optimized Vulkan GPU (preprocess + composite) | Change | OpenGL GPU |
| --- | ---: | ---: | ---: | ---: |
| 1 translucent layer | 0.210 ms | 0.086 ms (0.005 + 0.081) | -59.0% | 0.030 ms |
| 16 translucent layers | 1.254 ms | 0.426 ms (0.015 + 0.411) | -66.0% | 0.436 ms |
| 64 translucent layers | 5.099 ms | 1.625 ms (0.070 + 1.554) | -68.1% | 1.731 ms |
| 64 opaque layers | 0.304 ms | 0.154 ms (0.069 + 0.085) | -49.3% | 1.731 ms |

The optimized compute path is 2.3% faster than the OpenGL loop at 16
translucent layers and 6.1% faster at 64 layers. The one-layer case remains
slower because fixed dispatch, preprocessing, and synchronization costs
dominate. Opaque early termination is now about 11.2x faster than the
unoccluded OpenGL loop.

The contention medians use the same reporting method as the baseline table:

| Queued raster layers | Baseline median / p95 / GPU | Optimized median / p95 / GPU | Median change |
| --- | ---: | ---: | ---: |
| 0 | 1.540 / 1.560 / 1.254 ms | 0.879 / 0.907 / 0.426 ms | -42.9% |
| 16 | 1.701 / 1.725 / 1.399 ms | 0.946 / 0.962 / 0.495 ms | -44.4% |
| 64 | 2.606 / 2.644 / 2.311 ms | 2.120 / 2.139 / 1.665 ms | -18.6% |

`RADV_DEBUG=nocache,shaders` was used for consistent NIR and ISA dumps. The
general shader still reaches `v83`/`s105`, while the separate common pipeline
reaches only `v21`/`s46` and has 18 basic blocks instead of roughly 2,400.
This register/code-size isolation accounts for most of the improvement.

Experiments are preserved on `experiment/vulkan-*` branches. Results so far:

- Accepted: device-local host-visible layer records, direct UV as a prerequisite
  to the axis-aligned path, compact hot records, the separate simple pipeline,
  and packed dirty-tile coordinates.
- Rejected: wave32 (about 20% slower), 16x8 and 8x8 one-pixel workgroups
  (noise-sized isolated changes but 6-20% worse under heavy graphics
  contention), and hot-field reordering without reducing the 592-byte stride
  (about 3% slower).
- Rejected: two-layer blocked source-over. It increased the simple shader from
  `v21` to `v31` and regressed 16/64-layer composition by about 18-19%.
- Rejected: integer-aligned `texelFetch`. It was neutral for one unscaled
  layer, but regressed 16 layers by 5.4% and 64 layers by 8.9% versus RADV's
  filtered sample path. The experiment and benchmark are preserved on
  `experiment/vulkan-integer-texel-fetch`.
- Rejected: a specialization-constant sRGB-to-Display-P3 matrix. It removed
  two buffer loads and improved conversion by about 1%, but left register use
  unchanged at `v23`/`s79`; the exact-matrix pipeline was not worth retaining.
  The experiment is preserved on `experiment/vulkan-constant-color-matrix`.
- Accepted selectively: an 8x8 workgroup whose invocations each composite a
  2x2 pixel quad. The extra independent texture requests raise register use to
  `v47`/`s62`, so it is selected only for one- and two-layer simple scenes;
  those composition times fall from 81.2 to 58.3 us (-28%) and from 96.6 to
  81.0 us (-16%), respectively. Four or more layers retain the 16x16 serial
  kernel and its `v21`/`s46` footprint.
- Neutral alone: direct UV precomposition; coverage kept the inverse-transform
  work live until the axis-aligned path was introduced.

### Color-managed common paths (2026-07-17)

The color-managed overdraw benchmark separately measures identical source and
target descriptions and actual sRGB-to-Display-P3 conversion. Identical
descriptions with neutral brightness, saturation, and textured RGB modulation
are now treated as a semantic no-op. Color filters remain on the full path
because they consume destination transfer metadata. A focused axis-aligned
source-over pipeline handles conversions while preserving the full transfer,
gamut conversion, linear effects, and tone-mapping equations.

| 1920x1080 scene | General shader composite | Specialized composite | Change |
| --- | ---: | ---: | ---: |
| 1 same-space SDR layer | 0.226 ms | 0.058 ms | -74.2% |
| 16 same-space SDR layers | 1.955 ms | 0.411 ms | -79.0% |
| 64 same-space SDR layers | 7.800 ms | 1.555 ms | -80.1% |
| 1 sRGB-to-P3 layer | 0.225 ms | 0.126 ms | -44.2% |
| 16 sRGB-to-P3 layers | 1.867 ms | 1.584 ms | -15.2% |
| 64 sRGB-to-P3 layers | 7.447 ms | 6.264 ms | -15.9% |

The focused conversion shader reaches `v23`/`s79`, compared with
`v83`/`s105` for the general shader. Remaining deep color-conversion time is
mostly transfer-function work; approximations or encode-once linear
composition require a separate accuracy and semantic evaluation.

### Tile-level opaque occlusion (2026-07-17)

Source-over composition now runs bottom-to-top. GPU preprocessing resets a
tile's layer list whenever a later, guaranteed-opaque axis-aligned layer fully
covers that tile. Surface opaque regions supply the guarantee only at full
opacity and without rounded clipping; transformed/window edge tiles retain all
lower layers conservatively. The output boundary is clamped to its real size,
so a partial final output tile can still be culled.

Bottom-to-top evaluation shortens live state even without occlusion: the
compact shader drops from `v21` to `v16` and the shallow 2x2 shader from `v47`
to `v27`. Preprocessing rises from `s8` to `s15` but remains a small part of
total time.

| 1920x1080 scene | Front-to-back total GPU | Tile-cull/back-to-front total GPU | Change |
| --- | ---: | ---: | ---: |
| 1 translucent layer | 0.063 ms | 0.055 ms | -12.9% |
| 16 translucent layers | 0.427 ms | 0.399 ms | -6.4% |
| 64 translucent layers | 1.624 ms | 1.537 ms | -5.3% |
| 64 known-opaque layers | 0.154 ms | 0.143 ms | -7.1% |
| Tile-aligned large opaque window over 64 layers | 0.814 ms | 0.770 ms | -5.4% |
| Unaligned large opaque window over 64 layers | 0.824 ms | 0.797 ms | -3.2% |
| Unaligned small opaque window over 64 layers | 1.448 ms | 1.378 ms | -4.9% |

### Hierarchical 2D prefix masks (2026-07-17)

The layer-mask monoid is a fixed-width bitset under XOR. Each axis-aligned
layer AABB contributes its bit at the four corners of a 2D difference grid;
an inclusive 2D XOR prefix reconstructs exact tile membership. A second bitset
uses the same operation for guaranteed-opaque full-tile coverage, allowing the
final pass to retain the existing conservative lower-layer cutoff.

A single scalar total per 16x16-tile bin is not enough for a 2D prefix because
partially consumed rows and columns cross bin boundaries. The implementation
therefore uses row and column boundary vectors:

1. One 16x16 workgroup per bin and 32-layer word generates corner events in
   shared memory and performs parallel horizontal and vertical inclusive scans.
2. Independent jobs propagate the right-edge state across every tile row and
   the bottom-edge state across every tile column. The column job also folds in
   complete northwest-bin totals.
3. One invocation per tile XORs its local prefix, left carry, and top carry,
   applies the highest opaque-layer cutoff, and emits layer masks plus a compact
   nonempty-word summary for composition.

The direct and prefix paths share that summarized-mask format. Scenes with at
most 32 layers retain a one-dispatch direct AABB pass; larger scenes use the
three-pass prefix path. This avoids the fixed prefix latency on normal shallow
desktops while bounding preprocessing growth for deep scenes. The experiment
is preserved on `experiment/vulkan-prefix-mask`; its direct-comparison parent
is `experiment/vulkan-prefix-mask-baseline`.

The preprocessing-focused benchmark keeps all layers outside a 1920x1080
target so raster work stays constant. Values below are final GPU timestamp
samples from the same Debug build and validation-enabled run on Navi 10.

| Offscreen layers | Direct-list preprocess / total | Adaptive-mask preprocess / total | Total change |
| ---: | ---: | ---: | ---: |
| 1 | 0.0048 / 0.0490 ms | 0.0064 / 0.0510 ms | +3.9% |
| 16 | 0.0140 / 0.0771 ms | 0.0130 / 0.0790 ms | +2.5% |
| 64 | 0.0367 / 0.0992 ms | 0.0201 / 0.0858 ms | -13.5% |
| 256 | 0.1226 / 0.1870 ms | 0.0370 / 0.1030 ms | -44.9% |
| 1024 | 0.4662 / 0.5303 ms | 0.1692 / 0.2348 ms | -55.7% |

Full-screen overdraw confirms that the scan overhead is amortized around the
same point. The shallow direct-mask path keeps the 1-4-layer penalty below
three percent, while 64-layer and opaque/mixed scenes benefit:

| 1920x1080 scene | Direct-list total GPU | Adaptive-mask total GPU | Change |
| --- | ---: | ---: | ---: |
| 1 translucent layer | 0.0560 ms | 0.0576 ms | +2.9% |
| 4 translucent layers | 0.1336 ms | 0.1370 ms | +2.6% |
| 8 translucent layers | 0.2166 ms | 0.2126 ms | -1.8% |
| 16 translucent layers | 0.3994 ms | 0.3929 ms | -1.6% |
| 64 translucent layers | 1.5349 ms | 1.4694 ms | -4.3% |
| 64 known-opaque layers | 0.1432 ms | 0.1041 ms | -27.3% |
| Tile-aligned opaque window over 64 layers | 0.7640 ms | 0.7154 ms | -6.4% |
| Unaligned large opaque window over 64 layers | 0.7970 ms | 0.7460 ms | -6.4% |
| Unaligned small opaque window over 64 layers | 1.3728 ms | 1.3235 ms | -3.6% |

Unconditionally using the prefix path was rejected: its three dispatches made
1, 2, and 4-layer scenes 27%, 22%, and 12% slower. The adaptive cutoff retains
the scalable path only where it wins. `RADV_DEBUG=nocache,shaders` dumps were
also checked; no new scratch spills appeared in the three preprocessing
kernels.

## Resolved correctness review findings (2026-07-18)

An automated lifetime and failure-path review found eight issues in the Vulkan
and nested presentation paths:

- Painter-overlay resize and blur-noise strength changes could destroy an image
  still referenced by an in-flight descriptor. Replacements are now prepared
  without disturbing the current image, and the old image is destroyed only
  after the compute queue is idle.
- Composite dispatch used one X workgroup per dirty tile and could exceed the
  guaranteed 65,535 workgroups for outputs just larger than 4096x4096. Dirty
  tiles are now dispatched as a bounded two-dimensional grid and all composite
  shaders linearize the X/Y workgroup coordinates. A full-damage 4096x4112
  regression verifies the last tile under Vulkan validation.
- A partial frame-ring allocation failure could be mistaken for a valid cached
  allocation on retry. Cached dimensions are now invalidated before allocation,
  and the fast path validates every buffer in every frame slot.
- DRM swapchain recreation kept the old swapchain installed if every new format
  candidate failed. Recreation now clears it before trying candidates, so the
  failure propagates instead of acquiring an old-size or non-importable buffer.
- Once a nested host surface has a linux-drm-syncobj surface, every buffer
  commit requires acquire and release points. Output-layer and cursor commits
  now retain their buffer and fence and defer the commit when point creation
  fails; implicit synchronization remains the fallback only before explicit
  synchronization has been activated.
- The nested host cursor retained an unowned `GraphicsBuffer *` while disabled.
  It now holds a `GraphicsBufferRef`, keeping the last cursor buffer valid across
  cursor swapchain recreation and re-enable.
- Nested Vulkan layers without host color management repeatedly cleared their
  damage journals because the last color was never recorded. Layer commits now
  record the effective sRGB color even when no color-management surface exists.

## Resolved bug: DRM presentation tests corrupted buffer age

Observed on 2026-07-17 as 16x16 black regions and flickering between the current
desktop and surfaces from much older frames. The corruption was specific to the
physical DRM path and looked like missing damage repair.

`DrmVulkanLayer::preparePresentationTest()` acquired a Vulkan swapchain slot,
imported it for an atomic test, and called the same release operation used after
a completed render. That marked the untouched test slot as age 1 and advanced
all other slots, but no frame or matching entry was added to the damage journal.
The next partial render could therefore acquire old or uninitialized contents
with a falsely recent age and omit the repair needed to bring them current.

Presentation tests now leave render-slot ages unchanged, and the rendered-frame
operation is named `releaseRendered()` to make that invariant explicit. The
multi-GPU copy journal is reset after copying a test buffer for the same reason.
The regression covers untouched presentation-test imports, real rendered age
advancement, and a second slot held as if scanned out.

## Resolved bug: cursor tests leaked stale framebuffers

Observed on 2026-07-17 as the hardware cursor alternating between its current
and previous images while the cursor shape changed. The cursor plane itself
remained enabled.

Each atomic presentation test imported an arbitrary free Vulkan swapchain slot
and stored it as `DrmVulkanLayer::currentBuffer()`, even though the slot was not
rendered. Cursor position updates are committed asynchronously between normal
frames and use `currentBuffer()`, so a movement after the test could present the
stale test slot. The next rendered cursor commit restored the current image,
producing the back-and-forth flicker.

Presentation tests now reuse the last rendered framebuffer when one exists. A
test-only slot is imported only while initially bringing up a layer, before any
rendered framebuffer is available.

## Resolved bug: window animations used the output origin

Observed on 2026-07-17 as the Squash minimize and unminimize trajectories
feeling different from the OpenGL compositor. The effect script was unchanged,
but the Vulkan item traversal applied `WindowPaintData` after the root window
item's global position. A size animation therefore scaled the position as well
as the window-local geometry, making the effective animation origin the output
origin. OpenGL keeps the root position outside the effect transform.

The Vulkan renderer now cancels the traversal's root translation, applies the
effect transform in window-local coordinates, and then restores the root
position with the same device-pixel snapping used by OpenGL. The differential
regression renders a directly positioned root item with simultaneous scale and
translation at 1.25x output scale and compares Vulkan with both an explicit
reference and the OpenGL renderer.

## Resolved bug: minimized window thumbnails were transparent

Observed on 2026-07-18 as window previews becoming blank after their source
window was minimized. Minimization hides the root `WindowItem` from the normal
workspace scene, but `WindowThumbnailSource` explicitly renders that root into
an offscreen target. The OpenGL and QPainter item renderers intentionally render
the requested root regardless of its workspace visibility and apply visibility
checks only to descendants. The Vulkan traversal instead rejected the invisible
root, leaving the freshly cleared thumbnail target transparent.

The Vulkan renderer now bypasses explicit visibility only for the root passed to
`renderItem()` while continuing to exclude invisible descendants. The native
Vulkan-to-Qt Quick thumbnail regression minimizes its source window, damages it,
and verifies that the updated two-color contents remain visible under Vulkan
validation.

## Resolved bug: opaque blurred surfaces exposed stale backdrops while fading

Observed on 2026-07-18 as purple or wallpaper-colored panel and logout-screen
transitions, most reliably when a fullscreen window made Plasma's adaptive
panel opaque. The surface contents faded through the Wayland alpha modifier,
but scene occlusion still used the surface's intrinsic opaque region without
the item opacity. The fullscreen window underneath could therefore be omitted
from the repaint while the separately rendered backdrop blur sampled preserved
wallpaper pixels. Independently modulating the blur and surface also cannot
represent group opacity: it can reveal a filtered backdrop that the opaque
surface fully hides before the fade.

Opaque-region collection now accounts for inherited item opacity, keeping the
real backdrop renderable during surface fades. For Vulkan, the blur effect
brackets the window layers associated with each backdrop blur. The renderer
first removes regions the surface guarantees are intrinsically opaque, since
those pixels hide the blur even inside a faded group. It composes any remaining
full-strength blur and intrinsic surface contents together, then interpolates
that completed group against the untouched scene exactly once using the
combined window, paint-effect, and surface opacity. A failed blur pass falls
back to ordinary unblurred composition with the same group opacity. The
Vulkan integration regression changes a checkerboard backdrop while an opaque
blurred layer-shell surface covers it, applies `wp_alpha_modifier_v1`, and
compares the result with the blur-free reference under validation both with and
without a declared Wayland opaque region.

## Resolved performance bug: Vulkan readback used uncached host memory

Observed on 2026-07-17 as very slow Vulkan-rendered `WindowThumbnail` updates.
The software Qt Quick fallback renders the window into a Vulkan texture and
then uses `VulkanTexture::download()` to copy it into a `QImage`. The readback
buffer requested only host-visible, coherent memory, so the first-match
allocator selected uncached system memory on RADV/Navi 10. Reading the copied
pixels through that mapping made the final CPU `memcpy()` dominate the update.

Readback buffers now require host-visible memory and prefer a host-cached type,
with an explicit mapped-memory invalidation so non-coherent cached types are
also valid. Devices without host-cached memory retain the prior compatible
fallback. A 2048x1152 RGBA blocking-download benchmark was added to preserve
the workload. In the RelWithDebInfo build on the Navi 10 test machine, without
validation, the median fell from 52.1 ms to 3.6 ms per download (14.5x faster)
with:

```sh
build/bin/vulkanCompositorBenchmark -median 5 -iterations 10 benchmarkDownload
```

## Resolved performance bug: Vulkan thumbnails round-tripped through host memory

The OpenGL Qt Quick scene graph previously received Vulkan-rendered window
thumbnails through `VulkanTexture::download()` followed by
`QQuickWindow::createTextureFromImage()`. Even with cached readback memory, this
serialized the Vulkan producer with a full image readback and then uploaded the
same pixels through Mesa's `glTexSubImage2D` path on every thumbnail update.

Offscreen Quick views now allocate KWin-owned ABGR8888 dma-buf slots whose
format and modifier are supported as both Vulkan storage images and ordinary
EGL textures. Vulkan renders the thumbnail directly into the slot, Qt Quick
samples the imported GL texture, and native sync-file fences provide both
Vulkan-to-EGL producer ordering and EGL-to-Vulkan slot-reuse ordering. There is
no host image copy or upload in this path. The source client/WSI buffers retain
their original Vulkan render-completion release points; the later Qt Quick GL
fence is attached only to the KWin-owned thumbnail target, so native handoff
does not extend client-buffer lifetime. Software Qt Quick and systems without a
jointly supported native format retain the `QImage` fallback.

The Vulkan integration regression uses an orientation-sensitive two-color
thumbnail, asserts that Qt Quick receives a native texture rather than a
`QImage`, then damages the source and verifies a second frame. This covers the
producer fence, GL consumer fence, and safe target-slot reuse under validation.

## Resolved performance bug: shared-memory uploads blocked frame rendering

Observed on 2026-07-17 as missed render deadlines with samples rooted in
`QImage::convertToFormat_helper()` from `BufferTextureVulkan::attach()`. Common
little-endian `Format_ARGB32_Premultiplied` and `Format_RGB32` shared-memory
buffers were converted to RGBA8888 in full, copied into a newly allocated
full-image staging buffer even for small damage, submitted separately, and
synchronously waited before scene collection could continue.

These QImage formats now upload directly as Vulkan B8G8R8A8. RGB32 image views
swizzle alpha to one so the unused native X byte cannot make an opaque surface
transparent. Uploads pack only damaged rows into a persistently mapped,
three-slot staging ring and record their transfers at the start of the compute
composition command buffer. Pending-upload ownership follows the texture, so
nested/offscreen compositors that sample a newly updated texture also record
the right transfer. The standalone `VulkanTexture::update()` compatibility path
remains synchronous but now allocates and copies only the damaged rectangles.

ARGB partial-damage and RGB32 alpha-swizzle regressions cover the new native
formats. The existing legacy GL shader bridge test also exercises the
cross-renderer pending-upload handoff that caught a missing transfer during
development. A steady-state CPU enqueue benchmark and the synchronous
compatibility benchmark preserve both workloads.

The comparison used base commit `08eb76c50d`, the RelWithDebInfo build on the
Navi 10/RADV test machine, Qt 6.11.1, the dedicated compute queue at normal
priority, the `3D_FULL_SCREEN` amdgpu power profile, and no Vulkan validation.
At 2048x1152, the old ARGB-to-RGBA conversion alone took 1.49 ms. The old
synchronous upload took 2.0 ms for 64x64 damage and 2.3 ms for full damage.
After the change, synchronous updates take 0.11 ms and 2.5 ms respectively;
the frame-integrated path removes the separate submission/fence wait and its
median CPU enqueue cost is 1.904 us for 64x64 damage and 0.386 ms for full
damage. The full synchronous path is intentionally not the optimized frame
path and pays for packed-row staging before its blocking submit.

```sh
build/bin/vulkanCompositorBenchmark -median 5 \
    benchmarkUploadUpdate benchmarkDeferredUploadCpu
```

## Resolved performance bug: blur passes allocated unused timestamp pools

Every `VulkanCompositor::renderTo()` previously created separate two-slot
timestamp query pools for preprocessing and composition. Ordered dual-Kawase
blur invokes the compositor once for scene capture, for every downsample and
upsample level, for blur combination, and once more for final composition. The
intermediate results were retained until their triple-buffered blur-frame fence
signaled solely to keep those query pools alive, but their timestamps were
never read or forwarded to the output frame.

Compositor timing is now explicitly selectable. Intermediate blur submissions
disable it and can discard their result objects immediately; final output and
benchmark submissions retain the existing preprocessing and composition
queries. With the default four blur iterations and one blurred window, this
reduces query-pool creation from 20 pools to 2 per rendered frame. The Vulkan
regression suite covers both timing-enabled waits and timing-disabled results,
and the validation-enabled backdrop-blur regression covers the intermediate
submission and scratch-resource lifetimes.

## Resolved performance issue: steady-state Vulkan resource churn

Automated review identified several CPU-side operations that were repeated for
every DRM frame even though their inputs normally remain unchanged. DRM Vulkan
layers now retain their negotiated format choice until size, alpha, color-power,
multi-GPU import, or low-bandwidth requirements change, and cache the output
color-pipeline inputs so ICC LUT construction only runs after a color-state
change. Vulkan textures own a lazily created reusable image view instead of the
compositor creating target and sampled views for every frame-resource slot, and
the host-visible layer, hot-layer, dirty-tile, and output-LUT buffers remain
mapped for their allocation lifetime.

The same cleanup consolidated full-buffer and plane dma-buf import bookkeeping,
the renderer-independent CPU nine-patch stitcher, and screencast format
selection. The backend-specific nine-patch image formats and upload paths remain
separate. These are deterministic allocation and duplicate-work removals; no
GPU-time claim or new benchmark baseline is recorded for them.

## Planned optimization passes

Compact variable-length tile lists remain deferred; summarized fixed-width
bitsets avoid allocation scans and are faster for the measured deep scenes.

- [x] Dirty-tile preprocessing and composition
- [x] Packed dirty-tile coordinates without shader integer division/modulo
- [x] Back-to-front source-over composition with tile-level opaque occlusion
- [x] Precomposed output-pixel-to-UV transforms
- [x] Axis-aligned RGBA/solid source-over pipeline with compact hot records
- [x] Prefer host-visible device-local layer records with a compatible host-visible fallback
- [x] Workgroup and subgroup-size sweep on Navi 10 (16x16 wave64 retained)
- [x] Two-layer blocked source-over evaluation measured and rejected on Navi 10
- [x] Multi-pixel invocation variant measured and selected for simple scenes with at most two layers
- [x] Feed known surface opaque regions into conservative per-tile preprocessing culling
- [x] Summarized fixed-stride layer bitsets beyond 64 layers
- [ ] Compact prefix-summed tile-list allocation (deferred)
- [x] Hierarchical AABB binning with 16x16-bin 2D XOR prefix scans
- [x] Adaptive direct/prefix mask construction at the measured 32-layer cutoff
- [x] Texture descriptor batching without a fixed per-scene texture limit
- [ ] Texture descriptor indexing/bindless sampling
- [x] Simple source-over pipeline variant with color management
- [ ] Move target-global color data out of full per-layer records
- [ ] Replace the duplicated full-record fallback with a true cold-only buffer
- [ ] Pipeline and descriptor reuse across outputs
- [x] Frame overlap without per-frame queue-idle waits (three independently fenced frame-resource sets)

## Legacy effect shader compatibility bridge

The legacy effect API passes an already compiled `GLShader *` into
`OffscreenEffect`/`AnimationEffect`; it does not retain portable fragment source
or expose enough state to translate the program into a Vulkan pipeline. The
Vulkan backend therefore keeps the main composition path in compute but uses a
compatibility EGL context for these opaque programs.

The redirected window is rendered by Vulkan into an RGBA16F dma-buf when that
format is jointly renderable, with RGBA8 as fallback. EGL samples that buffer,
runs the unchanged legacy shader and its existing uniforms, and writes a second
shared dma-buf that the Vulkan compositor samples. Exported native fences cover
Vulkan-to-EGL source handoff, EGL-to-Vulkan result handoff, source reuse, and
final result reuse; no CPU wait or queue-idle operation is inserted. Cross-fade
snapshots are made shareable before an `AnimationEffect` can install its shader,
so late shader installation preserves the old snapshot instead of recapturing
new window contents.

This slow compatibility path requires a dma-buf format/modifier that is both a
Vulkan storage image and an EGL render target. EGL native fences keep the normal
bridge asynchronous; implementations without them fall back to blocking sync-fd
waits and `glFinish()` for correctness. Native in-tree effects continue to use
compute filters and do not pay the cross-API cost. The validation integration test uses an orientation-sensitive
two-color window and installs a custom channel-swapping shader after snapshot
capture. The current full runs pass 44 compositor tests and 16 live integration
tests with no validation messages or current-boot kernel GPU-reset report.
