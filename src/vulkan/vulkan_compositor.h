/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2026 KWin Developers

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include "core/colorspace.h"
#include "core/region.h"
#include "kwin_export.h"
#include "utils/filedescriptor.h"

#include <QColor>
#include <QObject>
#include <QRectF>
#include <QSize>
#include <QTransform>
#include <QVector4D>
#include <array>
#include <memory>
#include <optional>
#include <span>
#include <vector>
#include <vulkan/vulkan_raii.hpp>

namespace KWin
{

class VulkanDevice;
class VulkanRenderTimeQuery;
class VulkanTexture;
class ColorPipeline;

struct KWIN_EXPORT VulkanSolidLayer
{
    QRectF rect;
    QColor color;
};

enum class VulkanBlendMode : uint32_t {
    SourceOver,
    DestinationOut,
    /** Replace the destination with a filtered backdrop, respecting shape coverage and filterParameters.y modulation. */
    BackdropReplace,
    Additive,
};

enum class VulkanColorFilter : uint32_t {
    None,
    Invert,
    Colorize,
    ColorBlindnessCorrection,
    BlurDownsample,
    BlurUpsample,
    BlurUpsampleAndColorize,
    Noise,
    /** Transparent red/blue overlay for fractional texture samples/vertices. */
    FractionalDebug,
    /** Nearest-neighbor sampling with a black grid between enlarged pixels. */
    ZoomPixelGrid,
    /** Pattern-aware xBRZ sampling for enlarged desktop content. */
    ZoomPatternUpscale,
    /** Cross-fades the primary texture from auxiliaryTexture. */
    ScreenTransformCrossFade,
};

/**
 * A layer consumed by the tiled compute compositor. If texture is null, color
 * is treated as a premultiplied solid source. Otherwise color modulates the
 * sampled premultiplied texture and sourceRect is expressed in texture pixels;
 * an empty sourceRect selects the complete texture.
 */
struct KWIN_EXPORT VulkanCompositorLayer
{
    QRectF rect;
    QColor color = Qt::white;
    VulkanTexture *texture = nullptr;
    std::array<VulkanTexture *, 3> texturePlanes = {};
    uint32_t texturePlaneCount = 0;
    /** Optional second sampled image used by filters such as screen cross-fade. */
    VulkanTexture *auxiliaryTexture = nullptr;
    QRectF sourceRect = {};
    qreal opacity = 1.0;
    /** Maps rect from layer-local coordinates into output pixel coordinates. */
    QTransform transform = {};
    /** Optional axis-aligned clip in output pixel coordinates. */
    std::optional<QRectF> clipRect = std::nullopt;
    /** Maps normalized destination coordinates to normalized source coordinates. */
    QTransform textureTransform = {};
    qreal brightness = 1.0;
    qreal saturation = 1.0;
    /**
     * Optional rounded rectangle in layer-local coordinates. The corner
     * radii are ordered top-left, top-right, bottom-left, bottom-right.
     */
    std::optional<QRectF> roundedRect = std::nullopt;
    QVector4D cornerRadii = {};
    /**
     * If non-zero, draw an outline around roundedRect instead of clipping the
     * layer to it. roundedRect describes the inner edge and cornerRadii the
     * inner radii.
     */
    qreal outlineThickness = 0.0;
    /**
     * Optional convex quadrilateral geometry in layer-local coordinates. The
     * vertices are ordered clockwise from top-left, matching WindowQuad. When
     * present, this replaces rect for coverage and interpolates the associated
     * normalized texture coordinates across two triangles.
     */
    std::optional<std::array<QPointF, 4>> quadVertices = std::nullopt;
    std::array<QPointF, 4> quadTextureCoordinates = {
        QPointF(0, 0),
        QPointF(1, 0),
        QPointF(1, 1),
        QPointF(0, 1),
    };
    VulkanBlendMode blendMode = VulkanBlendMode::SourceOver;
    VulkanColorFilter colorFilter = VulkanColorFilter::None;
    QMatrix4x4 colorFilterMatrix = {};
    QVector4D colorFilterParameters = {};
    /** Encoded color space of this layer. Null keeps the legacy pass-through path. */
    std::shared_ptr<ColorDescription> colorDescription;
    RenderingIntent renderingIntent = RenderingIntent::Perceptual;
    /** The texture is guaranteed opaque throughout rect before opacity modulation. */
    bool opaque = false;
};

struct KWIN_EXPORT VulkanCompositorRenderResult
{
    VulkanCompositorRenderResult();
    VulkanCompositorRenderResult(VulkanCompositorRenderResult &&other) noexcept;
    VulkanCompositorRenderResult &operator=(VulkanCompositorRenderResult &&other) noexcept;
    ~VulkanCompositorRenderResult();

    VulkanCompositorRenderResult(const VulkanCompositorRenderResult &) = delete;
    VulkanCompositorRenderResult &operator=(const VulkanCompositorRenderResult &) = delete;

    VulkanTexture *texture = nullptr;
    FileDescriptor completionFence;
    std::unique_ptr<VulkanRenderTimeQuery> preprocessTime;
    std::unique_ptr<VulkanRenderTimeQuery> compositeTime;
};

/**
 * Tiled compute compositor. Three preprocessing passes use a hierarchical 2D
 * prefix scan over rectangle-corner XOR events to emit per-tile layer masks.
 * Composition consumes those masks and splits scenes into descriptor batches
 * when more textures are present than fit in one descriptor set.
 */
class KWIN_EXPORT VulkanCompositor : public QObject
{
public:
    static constexpr uint32_t TileSize = 16;
    /** Maximum number of distinct sampled images in one descriptor batch. */
    static constexpr uint32_t MaximumTextureCount = 16;

    static std::unique_ptr<VulkanCompositor> create(VulkanDevice *device);
    ~VulkanCompositor() override;

    std::optional<VulkanCompositorRenderResult> render(const QSize &size,
                                                       std::span<const VulkanSolidLayer> layers,
                                                       const QColor &background = Qt::transparent,
                                                       const Region &damage = Region::infinite());
    std::optional<VulkanCompositorRenderResult> render(const QSize &size,
                                                       std::span<const VulkanCompositorLayer> layers,
                                                       const QColor &background = Qt::transparent,
                                                       const Region &damage = Region::infinite(),
                                                       const std::shared_ptr<ColorDescription> &targetColorDescription = nullptr,
                                                       const ColorPipeline *outputColorPipeline = nullptr,
                                                       FileDescriptor &&acquireFence = {});
    /**
     * Composites directly into an existing compute-writable texture. The
     * caller is responsible for including the complete target in @a damage
     * when its previous contents are not valid (for example, swapchain age
     * zero). @a acquireFence is consumed by the compute submission.
     */
    std::optional<VulkanCompositorRenderResult> renderTo(VulkanTexture *target,
                                                         std::span<const VulkanCompositorLayer> layers,
                                                         const QColor &background,
                                                         const Region &damage,
                                                         FileDescriptor &&acquireFence = {},
                                                         const std::shared_ptr<ColorDescription> &targetColorDescription = nullptr,
                                                         const ColorPipeline *outputColorPipeline = nullptr);

    VulkanTexture *texture() const;
    QSize size() const;

private:
    static constexpr uint32_t FrameResourceCount = 3;
    struct FrameResources
    {
        FrameResources();
        ~FrameResources();

        vk::raii::DescriptorPool descriptorPool;
        std::vector<vk::raii::DescriptorSet> descriptorSets;
        uint32_t descriptorCapacity = 0;
        vk::raii::Buffer layerBuffer;
        vk::raii::DeviceMemory layerMemory;
        vk::raii::Buffer hotLayerBuffer;
        vk::raii::DeviceMemory hotLayerMemory;
        vk::raii::Buffer tileBuffer;
        vk::raii::DeviceMemory tileMemory;
        vk::raii::Buffer prefixBuffer;
        vk::raii::DeviceMemory prefixMemory;
        vk::raii::Buffer carryBuffer;
        vk::raii::DeviceMemory carryMemory;
        vk::raii::Buffer dirtyTileBuffer;
        vk::raii::DeviceMemory dirtyTileMemory;
        vk::raii::Buffer outputLutBuffer;
        vk::raii::DeviceMemory outputLutMemory;
        std::unique_ptr<ColorPipeline> outputColorPipeline;
        vk::raii::ImageView targetImageView;
        std::vector<vk::raii::ImageView> inputImageViews;
        FileDescriptor completionFence;
    };

    struct TextureBatch
    {
        uint32_t firstLayer = 0;
        uint32_t lastLayer = 0;
        std::vector<VulkanTexture *> textures;
    };

    explicit VulkanCompositor(VulkanDevice *device);

    bool initialize();
    bool acquireFrameResources();
    bool ensureTarget(const QSize &size);
    bool ensureResources(const QSize &size, size_t layerCount);
    bool ensureDescriptorSets(uint32_t batchCount);
    bool updateBufferDescriptors(uint32_t batchCount);
    bool updateTargetDescriptor(VulkanTexture *target, uint32_t batchCount);
    std::optional<VulkanCompositorRenderResult> renderTo(VulkanTexture *target,
                                                         std::span<const VulkanCompositorLayer> layers,
                                                         const QColor &background,
                                                         const Region &damage,
                                                         bool forceFullDamage,
                                                         FileDescriptor &&acquireFence,
                                                         const std::shared_ptr<ColorDescription> &targetColorDescription,
                                                         const ColorPipeline *outputColorPipeline);
    bool updateTextureDescriptors(std::span<const TextureBatch> batches,
                                  std::vector<vk::ImageMemoryBarrier2> &acquireBarriers,
                                  std::vector<vk::ImageMemoryBarrier2> &releaseBarriers);
    bool updateOutputLut(const ColorPipeline &pipeline);
    bool createPipelines();
    void releaseResources();

    VulkanDevice *m_device;
    vk::raii::DescriptorSetLayout m_descriptorSetLayout;
    vk::raii::PipelineLayout m_pipelineLayout;
    vk::raii::Pipeline m_preprocessPipeline;
    vk::raii::Pipeline m_prefixPropagatePipeline;
    vk::raii::Pipeline m_prefixFinalizePipeline;
    vk::raii::Pipeline m_compositePipeline;
    vk::raii::Pipeline m_colorCompositePipeline;
    vk::raii::Pipeline m_simpleCompositePipeline;
    vk::raii::Pipeline m_shallowCompositePipeline;
    vk::raii::Sampler m_sampler;
    std::array<FrameResources, FrameResourceCount> m_frames;
    FrameResources *m_currentFrame = nullptr;
    uint32_t m_nextFrame = 0;
    std::unique_ptr<VulkanTexture> m_texture;
    std::unique_ptr<VulkanTexture> m_fallbackTexture;
    vk::raii::ImageView m_fallbackImageView;
    QSize m_size;
    size_t m_layerCapacity = 0;
};

} // namespace KWin

Q_DECLARE_METATYPE(KWin::VulkanSolidLayer)
Q_DECLARE_METATYPE(KWin::VulkanCompositorLayer)
