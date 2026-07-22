/*
    SPDX-FileCopyrightText: 2026 KWin Developers

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include "scene/itemrenderer.h"
#include "vulkan/vulkan_compositor.h"

#include <QImage>
#include <QMetaObject>
#include <QPointer>
#include <unordered_set>

namespace KWin
{

class DecorationItem;
class ImageItem;
class OutlinedBorderItem;
class RenderView;
class ShadowItem;
class SurfaceItem;
class TextureVulkan;
class VulkanDevice;
class VulkanRenderTarget;
class VulkanUploadManager;
class SyncReleasePoint;

/**
 * Scene traversal for the tiled Vulkan compositor. Native Vulkan output
 * targets are still being integrated; a QImage render target is supported as
 * a differential-test bridge and performs a blocking readback in endFrame().
 */
class KWIN_EXPORT ItemRendererVulkan : public ItemRenderer
{
public:
    explicit ItemRendererVulkan(VulkanDevice *device);
    ~ItemRendererVulkan() override;

    bool isValid() const;
    VulkanDevice *device() const;

    /**
     * Expands device-space damage so that its render-target projection covers
     * complete compositor tiles. This must happen before scene occlusion and
     * layer collection.
     */
    static Region expandDamageToTileBoundaries(const RenderTarget &renderTarget,
                                               const Region &deviceDamage);

    QPainter *painter() const override;

    std::unique_ptr<Texture> createTexture(GraphicsBuffer *buffer, const std::shared_ptr<SyncReleasePoint> &releasePoint) override;
    std::unique_ptr<Texture> createTexture(const QImage &image) override;

    std::unique_ptr<NinePatch> createNinePatch(const QImage &image) override;
    std::unique_ptr<NinePatch> createNinePatch(const QImage &topLeftPatch,
                                               const QImage &topPatch,
                                               const QImage &topRightPatch,
                                               const QImage &rightPatch,
                                               const QImage &bottomRightPatch,
                                               const QImage &bottomPatch,
                                               const QImage &bottomLeftPatch,
                                               const QImage &leftPatch) override;

    std::unique_ptr<Atlas> createAtlas(const QList<QImage> &sprites) override;

    void beginFrame(const RenderTarget &renderTarget, const RenderViewport &viewport) override;
    void endFrame() override;
    void renderBackground(const RenderTarget &renderTarget, const RenderViewport &viewport, const Region &deviceRegion) override;
    void renderItem(const RenderTarget &renderTarget,
                    const RenderViewport &viewport,
                    Item *item,
                    int mask,
                    const Region &deviceRegion,
                    const WindowPaintData &data,
                    const std::function<bool(Item *)> &filter,
                    const std::function<bool(Item *)> &holeFilter) override;
    void setLayerDebugging(bool enable) override;

    /** Appends an already projected textured quad to the active frame. */
    void renderTextureQuad(VulkanTexture *texture,
                           const std::array<QPointF, 4> &vertices,
                           const std::array<QPointF, 4> &textureCoordinates,
                           const QRectF &clipRect,
                           qreal opacity,
                           qreal brightness,
                           qreal saturation,
                           const std::shared_ptr<ColorDescription> &colorDescription = ColorDescription::sRGB,
                           VulkanColorFilter colorFilter = VulkanColorFilter::None,
                           const QMatrix4x4 &colorFilterMatrix = {},
                           const QVector4D &colorFilterParameters = {},
                           VulkanTexture *auxiliaryTexture = nullptr,
                           FileDescriptor &&acquireFence = {},
                           const std::shared_ptr<SyncReleasePoint> &releasePoint = nullptr);

    /** Clears an output-pixel rectangle while preserving layer ordering. */
    void renderClearRect(const QRectF &rect);

    /** Captures all layers collected so far into an effect-owned texture. */
    std::optional<VulkanCompositorRenderResult> renderCurrentLayersTo(
        VulkanCompositor *compositor,
        VulkanTexture *target,
        const std::shared_ptr<ColorDescription> &colorDescription);

    /** Records an ordered dual-Kawase blur of the layers collected so far. */
    void renderBackdropBlur(const QList<QRectF> &shape,
                            uint32_t iterationCount,
                            qreal offset,
                            qreal modulation,
                            qreal noiseOpacity,
                            int noiseStrength,
                            const QMatrix4x4 &colorMatrix,
                            const std::optional<QRectF> &roundedRect = std::nullopt,
                            const QVector4D &cornerRadii = {},
                            qreal groupOpacity = 1.0,
                            Item *groupRoot = nullptr,
                            SurfaceItem *groupSurface = nullptr,
                            RenderView *cacheView = nullptr);
    void finishBackdropBlur();

private:
    struct RoundedClip
    {
        QRectF rect;
        QVector4D radii;
    };

    struct BackdropBlur
    {
        size_t layerIndex = 0;
        std::optional<size_t> groupEndLayerIndex;
        qreal groupOpacity = 1.0;
        QList<QRectF> shape;
        uint32_t iterationCount = 1;
        qreal offset = 1.0;
        qreal modulation = 1.0;
        qreal noiseOpacity = 1.0;
        int noiseStrength = 0;
        QMatrix4x4 colorMatrix;
        std::optional<QRectF> roundedRect;
        QVector4D cornerRadii;
        QPointer<RenderView> cacheView;
    };

    struct BackdropCache
    {
        QPointer<RenderView> view;
        bool hasView = false;
        std::unique_ptr<VulkanTexture> scene;
        FileDescriptor completionFence;
        QSize size;
        std::shared_ptr<ColorDescription> colorDescription;
        bool initialized = false;
    };

    struct BlurFrameResources
    {
        std::unique_ptr<VulkanTexture> sceneA;
        std::unique_ptr<VulkanTexture> sceneB;
        std::unique_ptr<VulkanTexture> sceneC;
        std::vector<std::unique_ptr<VulkanTexture>> levels;
        std::unique_ptr<VulkanCompositor> fullSizeCompositor;
        std::vector<std::unique_ptr<VulkanCompositor>> levelCompositors;
        FileDescriptor completionFence;
        QSize size;
    };

    struct BlurTargetResources
    {
        QPointer<RenderView> view;
        bool hasView = false;
        BlurFrameResources frame;
    };

    struct ActiveBackdropBlurGroup
    {
        size_t blurIndex;
        Item *root;
        SurfaceItem *surface;
    };

    void collectItem(Item *item,
                     const QTransform &parentTransform,
                     qreal parentOpacity,
                     qreal brightness,
                     qreal saturation,
                     const QRectF &clipRect,
                     const std::optional<RoundedClip> &roundedClip,
                     const std::function<bool(Item *)> &filter,
                     const std::function<bool(Item *)> &holeFilter,
                     bool isRoot);
    void collectSurface(SurfaceItem *item,
                        const QTransform &transform,
                        qreal opacity,
                        qreal brightness,
                        qreal saturation,
                        const QRectF &clipRect,
                        const std::optional<RoundedClip> &roundedClip);
    void collectImage(ImageItem *item, const QTransform &transform, qreal opacity, qreal brightness, qreal saturation, const QRectF &clipRect);
    void collectDecoration(DecorationItem *item, const QTransform &transform, qreal opacity, qreal brightness, qreal saturation, const QRectF &clipRect);
    void collectShadow(ShadowItem *item, const QTransform &transform, qreal opacity, qreal brightness, qreal saturation, const QRectF &clipRect);
    void collectOutlinedBorder(OutlinedBorderItem *item, const QTransform &transform, qreal opacity, qreal brightness, qreal saturation, const QRectF &clipRect);
    void appendTextureLayer(const QRectF &rect,
                            TextureVulkan *texture,
                            const QRectF &sourceRect,
                            const QTransform &transform,
                            qreal opacity,
                            qreal brightness,
                            qreal saturation,
                            const QRectF &clipRect,
                            const std::shared_ptr<ColorDescription> &colorDescription,
                            RenderingIntent renderingIntent,
                            const QTransform &textureTransform = {},
                            const std::optional<RoundedClip> &roundedClip = std::nullopt,
                            bool opaque = false);
    void appendFractionalDebugLayer(const VulkanCompositorLayer &layer);
    BlurFrameResources *acquireBlurFrame(RenderView *view, uint32_t maximumIterationCount);
    bool ensureBlurFrameResources(BlurFrameResources &frame, uint32_t maximumIterationCount);
    std::unique_ptr<VulkanTexture> allocateBlurIntermediate(const QSize &size) const;
    BackdropCache *backdropCache(RenderView *view);
    bool ensureBackdropCache(BackdropCache &cache);
    void markBackdropCachesSubmitted(const FileDescriptor &completionFence);
    std::optional<VulkanCompositorRenderResult> renderFrameWithBackdropBlur(
        BlurFrameResources &frame,
        VulkanCompositor *captureCompositor = nullptr,
        VulkanTexture *captureTarget = nullptr,
        const std::shared_ptr<ColorDescription> &captureColorDescription = nullptr);
    FileDescriptor takeAcquireFence();

    VulkanDevice *const m_device;
    std::unique_ptr<VulkanUploadManager> m_uploadManager;
    std::unique_ptr<VulkanCompositor> m_compositor;
    std::optional<VulkanCompositorRenderResult> m_lastResult;
    QList<VulkanCompositorLayer> m_layers;
    QList<BackdropBlur> m_backdropBlurs;
    std::optional<ActiveBackdropBlurGroup> m_activeBackdropBlurGroup;
    std::vector<std::unique_ptr<BlurTargetResources>> m_blurTargets;
    std::vector<std::unique_ptr<BackdropCache>> m_backdropCaches;
    std::unordered_set<BackdropCache *> m_usedBackdropCaches;
    mutable QImage m_painterOverlay;
    mutable std::unique_ptr<QPainter> m_painter;
    std::unique_ptr<VulkanTexture> m_painterTexture;
    std::unique_ptr<VulkanTexture> m_blurNoiseTexture;
    int m_blurNoiseStrength = 0;
    QTransform m_painterTransform;
    mutable bool m_painterOverlayUsed = false;
    QImage *m_imageTarget = nullptr;
    VulkanRenderTarget *m_vulkanTarget = nullptr;
    QSize m_targetSize;
    qreal m_renderTargetScale = 1.0;
    std::shared_ptr<ColorDescription> m_targetColorDescription;
    Region m_damage;
    std::unordered_set<std::shared_ptr<SyncReleasePoint>> m_releasePoints;
    FileDescriptor m_acquireFence;
    bool m_layerDebugging = false;
    bool m_fractionalDebugging = false;
    bool m_deviceLost = false;
    QMetaObject::Connection m_deviceLostConnection;
};

} // namespace KWin
