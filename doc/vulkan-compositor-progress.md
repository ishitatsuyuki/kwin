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
- [x] Decorations and sprite atlases
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
- [x] Overdraw-heavy GPU-time comparison with the OpenGL backend (translucent worst case remains slower on Navi 10; opaque front-to-back termination is substantially faster before scene occlusion)
- [x] Async-compute latency benchmark under graphics contention, with a forced-graphics-queue comparison mode

### Async-compute contention result

`vulkanCompositorBenchmark::benchmarkComputeLatencyUnderGraphicsContention`
queues OpenGL raster overdraw without cross-API synchronization, then measures
Vulkan compositor submission-to-fence latency. Graphics completion is excluded
from the measurement; Vulkan GPU timestamps are reported separately so queueing
delay is not mistaken for shader execution time. Set
`KWIN_VULKAN_FORCE_GRAPHICS_QUEUE=1` to select the graphics family before device,
command-pool, and compositor resource creation.

On the Radeon RX 5700 XT (RADV NAVI10), the second 20-sample run measured:

| Queued raster layers | Compute-only median / p95 | Graphics median / p95 |
| --- | --- | --- |
| 0 | 0.949 / 1.010 ms | 0.937 / 1.006 ms |
| 16 | 0.884 / 0.940 ms | 0.959 / 0.997 ms |
| 64 | 1.569 / 1.629 ms | 1.793 / 1.819 ms |

The idle difference is noise-sized. Under the heavier contention case, the
compute-only queue reduced median completion latency by 12.5% and p95 by 10.4%.
Its shader execution itself became slower under shared GPU pressure (1.294 ms
versus 0.573 ms in the graphics-queue timestamp), but avoided enough queueing
delay to finish sooner overall. The diagnostic graphics-queue path passes the
full `testVulkan` suite with Vulkan validation enabled.

## Planned optimization passes

Hierarchical binning and prefix-summed tile compaction are intentionally
deferred until profiling demonstrates that their complexity is justified.

- [x] Dirty-tile preprocessing and composition
- [x] Front-to-back source-over composition with opaque-layer early termination
- [x] Fixed-stride layer-index lists beyond 64 layers
- [ ] Compact prefix-summed tile-list allocation (deferred)
- [ ] Hierarchical AABB binning (deferred)
- [ ] Prefix-sum tile-list construction (deferred)
- [x] Texture descriptor batching without a fixed per-scene texture limit
- [ ] Texture descriptor indexing/bindless sampling
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
capture. The current full runs pass 41 compositor tests and 14 live integration
tests with no validation messages or current-boot kernel GPU-reset report.
