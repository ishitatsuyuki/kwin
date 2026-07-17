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
- [x] GPU AABB-to-tile preprocessing
- [x] Per-tile layer-index lists with no fixed scene-layer limit
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
- [x] Wayland explicit host synchronization protocol (`linux-drm-syncobj-v1` acquire fences and per-commit release timelines for output and hardware-cursor dma-bufs; live host protocol trace under Vulkan validation)
- [x] Screenshot and screencast paths (output, region, and window capture; PipeWire memfd and directly rendered dma-buf buffers with syncobj fences)
- [x] Embedded and metadata cursor capture paths
- [x] Qt Quick software scene-graph integration, including live Vulkan-rendered window thumbnails for Quick effects

## Effect compatibility

- [x] Quick scene effects (Overview, Window View, and Tiles Editor) through the software scene graph
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
- [x] Transforms, fractional scaling, clipping, and damage-only scenes
- [x] GPU-vs-CPU transfer-function, gamut-conversion, and HDR tone-mapping scenes
- [x] ICC and output-calibration scene corpus (BToA, shaper-matrix, and MHC2 profiles)
- [x] Vulkan validation with zero errors
- [x] Live KWin virtual-output frame under Vulkan validation
- [x] Live Qt Quick window-thumbnail scene through a Vulkan virtual output
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

## Planned optimization passes

Hierarchical binning and prefix-summed tile compaction are intentionally
deferred until profiling demonstrates that their complexity is justified.

- [x] Dirty-tile preprocessing and composition
- [x] Packed dirty-tile coordinates without shader integer division/modulo
- [x] Front-to-back source-over composition with opaque-layer early termination
- [x] Precomposed output-pixel-to-UV transforms
- [x] Axis-aligned RGBA/solid source-over pipeline with compact hot records
- [x] Prefer host-visible device-local layer records with a compatible host-visible fallback
- [x] Workgroup and subgroup-size sweep on Navi 10 (16x16 wave64 retained)
- [x] Two-layer blocked source-over evaluation measured and rejected on Navi 10
- [x] Multi-pixel invocation variant measured and selected for simple scenes with at most two layers
- [x] Fixed-stride layer-index lists beyond 64 layers
- [ ] Compact prefix-summed tile-list allocation (deferred)
- [ ] Hierarchical AABB binning (deferred)
- [ ] Prefix-sum tile-list construction (deferred)
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
capture. The current full runs pass 42 compositor tests and 15 live integration
tests with no validation messages or current-boot kernel GPU-reset report.
