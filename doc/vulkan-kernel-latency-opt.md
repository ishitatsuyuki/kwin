### Relatively simple changes

- Put layer data in device-local memory. The layer buffer requests only host-visible/coherent memory ([vulkan_compositor.cpp](/home/ishitatsuyuki/Documents/kwin/src/vulkan/vulkan_compositor.cpp:480)), and the allocator takes the first matching type ([vulkan_device.cpp](/home/ishitatsuyuki/Documents/kwin/src/vulkan/vulkan_device.cpp:402)). On this Navi 10, that is likely system memory rather than host-visible VRAM. Prefer `DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT`, falling back to a staging upload into device-local memory.

- Precompose the texture-coordinate transform on the CPU. The current critical path is output position → inverse transform → local normalization → texture transform → source-rectangle interpolation → sample ([tile_composite.comp](/home/ishitatsuyuki/Documents/kwin/src/vulkan/shaders/tile_composite.comp:276)). Collapse that to a direct output-pixel-to-UV 2×3 matrix.

- Add an axis-aligned common path. Most windows need neither the general inverse-transform bounds test nor rounded coverage. A type flag could select:

  - axis-aligned RGBA
  - general affine RGBA
  - solid
  - rounded/outline
  - YUV
  - color-managed

- Split common and cold layer data. Even with perfect load sinking, the 464-byte stride and scattered common fields waste cache lines. Keep bounds, direct UV transform, modulation, texture indices, and flags together; move YUV and color-management parameters to separate buffers.

- Move target-global color data out of every layer. Destination transfer parameters, luminance coefficients, destination-to-LMS, and LMS-to-destination are identical for all layers targeting the same output, but are currently repeated in every record ([vulkan_compositor.cpp](/home/ishitatsuyuki/Documents/kwin/src/vulkan/vulkan_compositor.cpp:827)). Put them in a frame UBO/push-constant buffer.

- Separate common pipeline variants. In particular:

  - source-over without color management
  - source-over with color management
  - destination-out

  This lets the compiler remove large inactive paths and may reduce VGPR pressure and code size. Currently one destination-out layer selects bottom-to-top processing for the entire scene.

- Test local sizes rather than assuming 16×16 is optimal. Try 8×8, 16×8, and 16×16, plus wave32/wave64 variants where subgroup-size control is available. A 256-thread group can reduce scheduling flexibility when VGPR use is high.

- Explicitly exploit subgroup-uniform data if ISA shows vector loads or waterfalls. Every active lane in a subgroup reads the same tile-list entry and layer index. `subgroupBroadcastFirst()` or subgroup-leader loading may help, but only if ACO is not already generating scalar loads.

- Store `(tileX, tileY)` in the dirty-tile buffer. This removes the dynamic integer division and modulo at shader entry ([tile_composite.comp](/home/ishitatsuyuki/Documents/kwin/src/vulkan/shaders/tile_composite.comp:336)). Small, but nearly free.

- Add an integer-aligned `texelFetch` path for unscaled, untranslated buffers. It avoids filtering and much of the floating-point coordinate work.

- Try sampled inputs in `SHADER_READ_ONLY_OPTIMAL` rather than `GENERAL`, where external-image constraints permit it. Also consider typed storage-image variants for supported output formats.

- Cull known no-op layers before binning: zero opacity, zero-area geometry, empty clips, and destination-out with zero opacity.

- Feed known opaque regions into preprocessing. A tile completely covered by a guaranteed-opaque layer can omit every lower layer, without first sampling the opaque layer per pixel.

- As an intermediate step before bindless, raise the 16-texture batch limit according to device limits. Multiple batches repeat the tile-list scan, dispatch, destination load/store, and image barrier ([vulkan_compositor.cpp](/home/ishitatsuyuki/Documents/kwin/src/vulkan/vulkan_compositor.cpp:1086)).

### Software pipelining

The most promising form is blocked source-over evaluation. Layer evaluation is independent until the blend, while the current `remaining` accumulator creates a serial loop-carried chain.

For two front-to-back layers `A` and `B`:

```glsl
vec4 pair = A + B * (1.0 - A.a);
```

Evaluate and sample `A` and `B` independently, combine them, then apply the pair to the running result. Four layers can be reduced as two pairs and then one ordered composite. This provides:

- multiple independent texture requests in flight;
- overlap between texture latency and another layer’s coordinate/color work;
- a shorter blend dependency chain;
- block-level opaque termination.

Start with two layers. Four may cost too many VGPRs. It speculatively evaluates a lower layer that an opaque upper layer might hide, so it is best for known-translucent spans or the overdraw-heavy case. Destination-out should remain on a separate ordered path.

A lighter version is two-stage hand unrolling:

1. Prepare predicates, coverage, UV, and metadata for layer N.
2. Issue its texture fetch.
3. Prepare layer N−1 while that fetch is outstanding.
4. Finish color work and blend N.
5. Rotate the two slots.

Whether this improves on ACO’s scheduler needs confirmation from final ISA.

### Larger structural options

- Have each invocation process two or four pixels. This shares uniform layer metadata and exposes independent texture requests within a thread. An 8×8 group processing a 2×2 pixel quad still covers a 16×16 tile. The tradeoff is multiple accumulators and higher VGPR use.

- Stage chunks of hot layer records in LDS. Cooperatively load perhaps 4–8 compact records, synchronize once, then process the chunk. This amortizes global loads across the workgroup. It becomes attractive only after the hot record is compact; staging the current 464-byte structure is excessive.

- Produce richer per-tile entries during preprocessing: layer index plus “tile fully inside layer,” axis-aligned, opaque, or edge-coverage flags. Interior tiles could skip clip, local-bounds, and SDF work entirely.

- Generate coverage masks for rounded/transformed edge tiles. Preprocessing writes a 256-bit mask, and composition performs a bit test instead of recomputing the SDF for every overdraw layer.

- Implement bindless descriptor indexing. This removes descriptor batches, intermediate destination traffic, barriers, repeated list scans, and the loss of global front-to-back occlusion.

- Use compact prefix-summed tile lists. Besides reducing allocation size, compact neighboring lists should improve cache/TLB behavior in composition.

- Composite color-managed content into a linear high-precision intermediate and encode to the destination transfer function once per pixel. Currently destination encoding and related target-side work can happen for every color-managed layer. This is a much larger semantic and precision change, but potentially substantial.

- Use LUTs or approximations for transfer functions and tone mapping if SFU latency is significant. PQ currently contains several dependent `pow` chains. Accuracy requirements should drive whether this is acceptable.

My experiment order would be: device-local layer buffer, direct UV transform and compact hot record, pipeline/workgroup variants, two-layer blocked composition, then multi-pixel invocations or LDS staging. Bindless is likely the largest whole-pipeline improvement for real scenes with more than 16 unique images.
