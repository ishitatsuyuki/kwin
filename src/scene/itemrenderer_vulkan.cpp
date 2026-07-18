/*
    SPDX-FileCopyrightText: 2026 KWin Developers

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "itemrenderer_vulkan.h"

#include "core/rendertarget.h"
#include "core/renderviewport.h"
#include "core/syncobjtimeline.h"
#include "effect/effect.h"
#include "scene/decorationitem.h"
#include "scene/imageitem.h"
#include "scene/ninepatch.h"
#include "scene/outlinedborderitem.h"
#include "scene/scene.h"
#include "scene/shadowitem.h"
#include "scene/surfaceitem.h"
#include "scene/vulkan/atlas.h"
#include "scene/vulkan/ninepatch.h"
#include "scene/vulkan/texture.h"
#include "vulkan/vulkan_device.h"
#include "vulkan/vulkan_logging.h"
#include "vulkan/vulkan_render_time_query.h"
#include "vulkan/vulkan_rendertarget.h"
#include "vulkan/vulkan_texture.h"
#include "window.h"

#include <QPainter>
#include <QRandomGenerator>

#include <algorithm>
#include <cmath>

namespace KWin
{

namespace
{

QTransform transformFromMapping(const std::function<QPointF(const QPointF &)> &map)
{
    const QPointF origin = map(QPointF(0, 0));
    const QPointF x = map(QPointF(1, 0));
    const QPointF y = map(QPointF(0, 1));
    return QTransform(x.x() - origin.x(), x.y() - origin.y(),
                      y.x() - origin.x(), y.y() - origin.y(),
                      origin.x(), origin.y());
}

QTransform outputTransform(const OutputTransform &transform)
{
    return transformFromMapping([&transform](const QPointF &point) {
        return transform.map(point, QSizeF(1, 1));
    });
}

} // namespace

ItemRendererVulkan::ItemRendererVulkan(VulkanDevice *device)
    : m_device(device)
    , m_uploadManager(std::make_unique<VulkanUploadManager>(device))
    , m_compositor(VulkanCompositor::create(device))
{
    const QStringList visualizeOptions = qEnvironmentVariable("KWIN_SCENE_VISUALIZE").split(u';', Qt::SkipEmptyParts);
    m_fractionalDebugging = visualizeOptions.contains(QLatin1StringView("fractional"));
    m_deviceLostConnection = QObject::connect(device, &VulkanDevice::deviceLost, device, [this]() {
        m_deviceLost = true;
        m_layers.clear();
        m_backdropBlurs.clear();
        m_activeBackdropBlurGroup.reset();
        m_releasePoints.clear();
    });
}

ItemRendererVulkan::~ItemRendererVulkan()
{
    QObject::disconnect(m_deviceLostConnection);
}

Region ItemRendererVulkan::expandDamageToTileBoundaries(const RenderTarget &renderTarget,
                                                        const Region &deviceDamage)
{
    if (deviceDamage.isEmpty()) {
        return deviceDamage;
    }

    // Device damage is already expressed in output pixels. Map it directly to
    // the render target instead of round-tripping through logical coordinates.
    // At fractional output scales that round trip is lossy: for example, a
    // target-space tile [16, 32) at 1.25x maps back to approximately [15, 33).
    // The compositor would then dispatch both neighboring tiles while scene
    // collection only provided layers for the one-pixel overlap, clearing the
    // rest of those tiles to transparent black.
    const Region targetDamage = renderTarget.transform().map(deviceDamage, renderTarget.transformedSize());
    Region tiledTargetDamage;
    for (const Rect &rect : targetDamage.rects()) {
        const int tileSize = int(VulkanCompositor::TileSize);
        const int left = rect.left() / tileSize * tileSize;
        const int top = rect.top() / tileSize * tileSize;
        const int right = (rect.right() + tileSize - 1) / tileSize * tileSize;
        const int bottom = (rect.bottom() + tileSize - 1) / tileSize * tileSize;
        tiledTargetDamage |= Rect(left, top, right - left, bottom - top);
    }
    tiledTargetDamage &= Rect(QPoint(), renderTarget.size());

    // Map the tile-aligned target damage back to the output-device coordinate
    // space consumed by Scene::paint(). Output transforms are lossless for
    // integer rectangles, so mapping it forward again produces exactly the
    // same tile boundaries.
    const Region untransformedDamage = renderTarget.transform().inverted().map(tiledTargetDamage, renderTarget.size());
    Region expandedDamage = deviceDamage | untransformedDamage;
    expandedDamage &= renderTarget.transformedRect();
    return expandedDamage;
}

bool ItemRendererVulkan::isValid() const
{
    return !m_deviceLost && bool(m_compositor);
}

VulkanDevice *ItemRendererVulkan::device() const
{
    return m_device;
}

QPainter *ItemRendererVulkan::painter() const
{
    if (m_deviceLost || m_targetSize.isEmpty()) {
        return nullptr;
    }
    if (!m_painter) {
        if (m_painterOverlay.size() != m_targetSize
            || m_painterOverlay.format() != QImage::Format_RGBA8888_Premultiplied) {
            m_painterOverlay = QImage(m_targetSize, QImage::Format_RGBA8888_Premultiplied);
        }
        m_painterOverlay.fill(Qt::transparent);
        m_painter = std::make_unique<QPainter>(&m_painterOverlay);
        m_painter->setTransform(m_painterTransform);
        m_painterOverlayUsed = true;
    }
    return m_painter.get();
}

std::unique_ptr<Texture> ItemRendererVulkan::createTexture(GraphicsBuffer *buffer, const std::shared_ptr<SyncReleasePoint> &releasePoint)
{
    if (m_deviceLost) {
        return nullptr;
    }
    return BufferTextureVulkan::create(m_device, buffer, releasePoint, m_uploadManager.get());
}

std::unique_ptr<Texture> ItemRendererVulkan::createTexture(const QImage &image)
{
    if (m_deviceLost) {
        return nullptr;
    }
    return ImageTextureVulkan::create(m_device, image, m_uploadManager.get());
}

std::unique_ptr<NinePatch> ItemRendererVulkan::createNinePatch(const QImage &image)
{
    if (m_deviceLost) {
        return nullptr;
    }
    return NinePatchVulkan::create(m_device, image);
}

std::unique_ptr<NinePatch> ItemRendererVulkan::createNinePatch(const QImage &topLeftPatch,
                                                               const QImage &topPatch,
                                                               const QImage &topRightPatch,
                                                               const QImage &rightPatch,
                                                               const QImage &bottomRightPatch,
                                                               const QImage &bottomPatch,
                                                               const QImage &bottomLeftPatch,
                                                               const QImage &leftPatch)
{
    if (m_deviceLost) {
        return nullptr;
    }
    return NinePatchVulkan::create(m_device,
                                   topLeftPatch,
                                   topPatch,
                                   topRightPatch,
                                   rightPatch,
                                   bottomRightPatch,
                                   bottomPatch,
                                   bottomLeftPatch,
                                   leftPatch);
}

std::unique_ptr<Atlas> ItemRendererVulkan::createAtlas(const QList<QImage> &sprites)
{
    if (m_deviceLost) {
        return nullptr;
    }
    return AtlasVulkan::create(m_device, sprites);
}

void ItemRendererVulkan::beginFrame(const RenderTarget &renderTarget, const RenderViewport &viewport)
{
    if (m_painter) {
        m_painter->end();
        m_painter.reset();
    }
    m_lastResult.reset();
    m_layers.clear();
    m_backdropBlurs.clear();
    m_activeBackdropBlurGroup.reset();
    m_usedBackdropCaches.clear();
    std::erase_if(m_backdropCaches, [](const std::unique_ptr<BackdropCache> &cache) {
        return cache->hasView && cache->view.isNull()
            && (!cache->completionFence.isValid() || cache->completionFence.isReadable());
    });
    m_imageTarget = renderTarget.image();
    m_vulkanTarget = renderTarget.vulkanTarget();
    m_targetSize = renderTarget.size();
    m_targetColorDescription = renderTarget.colorDescription();
    m_painterTransform = transformFromMapping([&viewport](const QPointF &point) {
        return viewport.mapToRenderTarget(point);
    });
    m_painterOverlayUsed = false;
    m_damage = {};
    m_releasePoints.clear();
    m_acquireFence = {};
}

void ItemRendererVulkan::endFrame()
{
    if (m_painter) {
        m_painter->end();
        m_painter.reset();
    }
    if (m_deviceLost || !m_compositor || (!m_imageTarget && !m_vulkanTarget) || m_targetSize.isEmpty()) {
        return;
    }
    finishBackdropBlur();
    if (m_painterOverlayUsed) {
        const auto usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled;
        bool uploaded = m_painterTexture && m_painterTexture->size() == m_painterOverlay.size()
            && m_uploadManager->upload(m_painterTexture.get(), m_painterOverlay, Region(0, 0, m_painterOverlay.width(), m_painterOverlay.height()));
        if (!uploaded) {
            std::unique_ptr<VulkanTexture> replacement;
            const auto format = VulkanTexture::qImageToVulkanFormat(m_painterOverlay.format());
            if (format) {
                replacement = VulkanTexture::allocate(m_device,
                                                      *format,
                                                      m_painterOverlay.size(),
                                                      usage,
                                                      VulkanQueueRole::Compute,
                                                      VulkanTexture::qImageToComponentMapping(m_painterOverlay.format()));
                uploaded = replacement
                    && m_uploadManager->upload(replacement.get(), m_painterOverlay, Region(0, 0, m_painterOverlay.width(), m_painterOverlay.height()));
            }
            if (!uploaded) {
                replacement = VulkanTexture::upload(m_device, m_painterOverlay, usage, VulkanQueueRole::Compute);
                uploaded = bool(replacement);
            }
            if (uploaded) {
                // Descriptor image views do not own their images. The previous
                // overlay may still be sampled by an older frame.
                if (m_painterTexture) {
                    m_device->waitComputeIdle();
                }
                m_painterTexture = std::move(replacement);
            }
        }
        if (uploaded) {
            m_layers.push_back(VulkanCompositorLayer{
                .rect = QRectF(QPointF(), QSizeF(m_targetSize)),
                .texture = m_painterTexture.get(),
                .colorDescription = ColorDescription::sRGB,
            });
        } else {
            qCWarning(KWIN_VULKAN) << "Failed to upload a painter effect overlay";
        }
    }
    std::optional<VulkanCompositorRenderResult> result;
    BlurFrameResources *blurFrame = nullptr;
    if (!m_backdropBlurs.isEmpty()) {
        const uint32_t maximumIterationCount = std::ranges::max(m_backdropBlurs, {}, &BackdropBlur::iterationCount).iterationCount;
        blurFrame = acquireBlurFrame(maximumIterationCount);
        if (blurFrame) {
            result = renderFrameWithBackdropBlur(*blurFrame);
        }
    }
    if (!result) {
        if (blurFrame) {
            qCWarning(KWIN_VULKAN) << "Vulkan backdrop blur failed; rendering the scene without blur";
        }
        QList<VulkanCompositorLayer> fallbackLayers = m_layers;
        for (const BackdropBlur &blur : std::as_const(m_backdropBlurs)) {
            if (!blur.groupEndLayerIndex) {
                continue;
            }
            const size_t end = std::min(*blur.groupEndLayerIndex, size_t(fallbackLayers.size()));
            for (size_t i = std::min(blur.layerIndex, end); i < end; ++i) {
                fallbackLayers[qsizetype(i)].opacity *= blur.groupOpacity;
            }
        }
        FileDescriptor acquireFence = takeAcquireFence();
        result = m_vulkanTarget
            ? m_compositor->renderTo(m_vulkanTarget->texture(), fallbackLayers, Qt::transparent, m_damage, std::move(acquireFence), m_targetColorDescription, m_vulkanTarget->outputColorPipeline(), m_uploadManager.get())
            : m_compositor->render(m_targetSize, fallbackLayers, Qt::transparent, m_damage, m_targetColorDescription, nullptr, std::move(acquireFence), m_uploadManager.get());
    }
    if (!result) {
        qCWarning(KWIN_VULKAN) << "Vulkan scene composition failed";
        m_releasePoints.clear();
        return;
    }
    markBackdropCachesSubmitted(result->completionFence);
    if (blurFrame) {
        blurFrame->completionFence = result->completionFence.duplicate();
    }
    for (const auto &releasePoint : m_releasePoints) {
        releasePoint->addReleaseFence(result->completionFence);
    }
    m_releasePoints.clear();
    if (m_vulkanTarget) {
        m_vulkanTarget->setCompletionFence(std::move(result->completionFence));
        m_vulkanTarget->addRenderTimeQuery(std::move(result->preprocessTime));
        m_vulkanTarget->addRenderTimeQuery(std::move(result->compositeTime));
        return;
    }
    m_lastResult = std::move(result);
    const QImage image = m_lastResult->texture->download();
    if (image.isNull()) {
        return;
    }
    QPainter painter(m_imageTarget);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.drawImage(QPoint(0, 0), image);
}

void ItemRendererVulkan::renderBackground(const RenderTarget &renderTarget, const RenderViewport &, const Region &deviceRegion)
{
    // Device damage is already in output pixels. Going through logical
    // coordinates grows exact tile boundaries at fractional output scales.
    m_damage |= renderTarget.transform().map(deviceRegion, renderTarget.transformedSize());
}

void ItemRendererVulkan::renderItem(const RenderTarget &renderTarget,
                                    const RenderViewport &viewport,
                                    Item *item,
                                    int mask,
                                    const Region &deviceRegion,
                                    const WindowPaintData &data,
                                    const std::function<bool(Item *)> &filter,
                                    const std::function<bool(Item *)> &holeFilter)
{
    if (!m_compositor || !item) {
        return;
    }
    const Region targetClip = renderTarget.transform().map(deviceRegion, renderTarget.transformedSize());
    if (targetClip.isEmpty()) {
        return;
    }
    m_damage |= targetClip;

    // Item traversal applies the root item's position before the parent
    // transform. WindowPaintData, however, transforms window-local geometry;
    // the root position must remain outside of it, as it does in the OpenGL
    // renderer. Cancel the traversal translation, apply the effect transform,
    // then restore the root position snapped to the device pixel grid.
    const QPointF rootPosition = item->position();
    const QPointF snappedRootPosition(std::round(rootPosition.x() * viewport.scale()) / viewport.scale(),
                                      std::round(rootPosition.y() * viewport.scale()) / viewport.scale());
    QTransform sceneTransform = QTransform::fromTranslate(-rootPosition.x(), -rootPosition.y());
    QTransform effectTransform;
    if (mask & Scene::PAINT_WINDOW_TRANSFORMED) {
        effectTransform.translate(data.xTranslation(), data.yTranslation());
        effectTransform.scale(data.xScale(), data.yScale());
    }
    sceneTransform *= effectTransform;
    sceneTransform *= QTransform::fromTranslate(snappedRootPosition.x(), snappedRootPosition.y());
    const QTransform viewportTransform = transformFromMapping([&viewport](const QPointF &point) {
        return viewport.mapToRenderTarget(point);
    });
    sceneTransform *= viewportTransform;
    const qreal parentOpacity = m_activeBackdropBlurGroup && m_activeBackdropBlurGroup->root == item
        ? 1.0
        : data.opacity();
    collectItem(item,
                sceneTransform,
                parentOpacity,
                data.brightness(),
                data.saturation(),
                QRectF(static_cast<QRect>(targetClip.boundingRect())),
                std::nullopt,
                filter,
                holeFilter,
                true);
}

void ItemRendererVulkan::setLayerDebugging(bool enable)
{
    m_layerDebugging = enable;
}

void ItemRendererVulkan::renderTextureQuad(VulkanTexture *texture,
                                           const std::array<QPointF, 4> &vertices,
                                           const std::array<QPointF, 4> &textureCoordinates,
                                           const QRectF &clipRect,
                                           qreal opacity,
                                           qreal brightness,
                                           qreal saturation,
                                           const std::shared_ptr<ColorDescription> &colorDescription,
                                           VulkanColorFilter colorFilter,
                                           const QMatrix4x4 &colorFilterMatrix,
                                           const QVector4D &colorFilterParameters,
                                           VulkanTexture *auxiliaryTexture,
                                           FileDescriptor &&acquireFence,
                                           const std::shared_ptr<SyncReleasePoint> &releasePoint)
{
    if (!texture || clipRect.isEmpty()) {
        return;
    }
    qreal minimumX = vertices.front().x();
    qreal minimumY = vertices.front().y();
    qreal maximumX = minimumX;
    qreal maximumY = minimumY;
    for (size_t i = 1; i < vertices.size(); ++i) {
        minimumX = std::min(minimumX, vertices[i].x());
        minimumY = std::min(minimumY, vertices[i].y());
        maximumX = std::max(maximumX, vertices[i].x());
        maximumY = std::max(maximumY, vertices[i].y());
    }
    const QRectF bounds(QPointF(minimumX, minimumY), QPointF(maximumX, maximumY));
    const QRectF clippedBounds = bounds.intersected(clipRect);
    if (clippedBounds.isEmpty()) {
        return;
    }
    m_damage |= Rect(clippedBounds.toAlignedRect());
    m_layers.push_back(VulkanCompositorLayer{
        .rect = bounds,
        .texture = texture,
        .auxiliaryTexture = auxiliaryTexture,
        .opacity = opacity,
        .clipRect = clipRect,
        .brightness = brightness,
        .saturation = saturation,
        .quadVertices = vertices,
        .quadTextureCoordinates = textureCoordinates,
        .colorFilter = colorFilter,
        .colorFilterMatrix = colorFilterMatrix,
        .colorFilterParameters = colorFilterParameters,
        .colorDescription = colorDescription,
    });
    if (acquireFence.isValid()) {
        m_acquireFence = m_acquireFence.isValid()
            ? SyncReleasePoint::mergeSyncFds(m_acquireFence, acquireFence)
            : std::move(acquireFence);
    }
    if (releasePoint) {
        m_releasePoints.insert(releasePoint);
    }
    appendFractionalDebugLayer(m_layers.back());
}

void ItemRendererVulkan::renderClearRect(const QRectF &rect)
{
    const QRectF clippedRect = rect.intersected(QRectF(QPointF(), QSizeF(m_targetSize)));
    if (clippedRect.isEmpty()) {
        return;
    }
    m_damage |= Rect(clippedRect.toAlignedRect());
    m_layers.push_back(VulkanCompositorLayer{
        .rect = clippedRect,
        .color = Qt::white,
        .blendMode = VulkanBlendMode::DestinationOut,
    });
}

std::optional<VulkanCompositorRenderResult> ItemRendererVulkan::renderCurrentLayersTo(
    VulkanCompositor *compositor,
    VulkanTexture *target,
    const std::shared_ptr<ColorDescription> &colorDescription)
{
    if (!compositor || !target || target->size() != m_targetSize) {
        return std::nullopt;
    }
    if (!m_backdropBlurs.isEmpty()) {
        const uint32_t maximumIterationCount = std::ranges::max(m_backdropBlurs, {}, &BackdropBlur::iterationCount).iterationCount;
        BlurFrameResources *frame = acquireBlurFrame(maximumIterationCount);
        if (!frame) {
            return std::nullopt;
        }
        auto result = renderFrameWithBackdropBlur(*frame, compositor, target, colorDescription);
        if (result) {
            frame->completionFence = result->completionFence.duplicate();
            markBackdropCachesSubmitted(result->completionFence);
        }
        return result;
    }
    return compositor->renderTo(target,
                                m_layers,
                                Qt::transparent,
                                Region(Rect(QPoint(), target->size())),
                                takeAcquireFence(),
                                colorDescription,
                                nullptr,
                                m_uploadManager.get());
}

void ItemRendererVulkan::renderBackdropBlur(const QList<QRectF> &shape,
                                            uint32_t iterationCount,
                                            qreal offset,
                                            qreal modulation,
                                            qreal noiseOpacity,
                                            int noiseStrength,
                                            const QMatrix4x4 &colorMatrix,
                                            const std::optional<QRectF> &roundedRect,
                                            const QVector4D &cornerRadii,
                                            qreal groupOpacity,
                                            Item *groupRoot,
                                            SurfaceItem *groupSurface,
                                            RenderView *cacheView)
{
    if (shape.isEmpty() || m_targetSize.isEmpty()) {
        return;
    }
    if (noiseStrength > 0 && (!m_blurNoiseTexture || m_blurNoiseStrength != noiseStrength)) {
        QImage noise(QSize(256, 256), QImage::Format_Grayscale8);
        for (int y = 0; y < noise.height(); ++y) {
            uint8_t *line = noise.scanLine(y);
            for (int x = 0; x < noise.width(); ++x) {
                line[x] = uint8_t(QRandomGenerator::global()->bounded(noiseStrength));
            }
        }
        auto replacement = VulkanTexture::upload(m_device,
                                                 noise,
                                                 vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
                                                 VulkanQueueRole::Compute);
        if (replacement) {
            // Blur frame resources retain only image views. Keep the sampled
            // image alive until all earlier compute submissions have finished.
            if (m_blurNoiseTexture) {
                m_device->waitComputeIdle();
            }
            m_blurNoiseTexture = std::move(replacement);
            m_blurNoiseStrength = noiseStrength;
        }
    }
    const bool hasRequestedNoise = m_blurNoiseTexture && m_blurNoiseStrength == noiseStrength;
    m_backdropBlurs.push_back(BackdropBlur{
        .layerIndex = size_t(m_layers.size()),
        .groupEndLayerIndex = std::nullopt,
        .groupOpacity = std::clamp(groupOpacity, 0.0, 1.0),
        .shape = shape,
        .iterationCount = std::max(iterationCount, 1u),
        .offset = std::max(offset, 0.0),
        .modulation = std::clamp(modulation, 0.0, 1.0),
        .noiseOpacity = std::clamp(noiseOpacity, 0.0, 1.0),
        .noiseStrength = hasRequestedNoise ? noiseStrength : 0,
        .colorMatrix = colorMatrix,
        .roundedRect = roundedRect,
        .cornerRadii = cornerRadii,
        .cacheView = cacheView,
    });
    if (groupRoot) {
        m_activeBackdropBlurGroup = ActiveBackdropBlurGroup{
            .blurIndex = size_t(m_backdropBlurs.size() - 1),
            .root = groupRoot,
            .surface = groupSurface,
        };
    }
}

void ItemRendererVulkan::finishBackdropBlur()
{
    if (!m_activeBackdropBlurGroup) {
        return;
    }
    if (m_activeBackdropBlurGroup->blurIndex < size_t(m_backdropBlurs.size())) {
        m_backdropBlurs[qsizetype(m_activeBackdropBlurGroup->blurIndex)].groupEndLayerIndex = size_t(m_layers.size());
    }
    m_activeBackdropBlurGroup.reset();
}

ItemRendererVulkan::BlurFrameResources *ItemRendererVulkan::acquireBlurFrame(uint32_t maximumIterationCount)
{
    for (uint32_t offset = 0; offset < m_blurFrames.size(); ++offset) {
        const uint32_t index = (m_nextBlurFrame + offset) % m_blurFrames.size();
        BlurFrameResources &frame = m_blurFrames[index];
        if (!frame.completionFence.isValid() || frame.completionFence.isReadable()) {
            frame.completionFence = FileDescriptor{};
            if (!ensureBlurFrameResources(frame, maximumIterationCount)) {
                return nullptr;
            }
            m_nextBlurFrame = (index + 1) % m_blurFrames.size();
            return &frame;
        }
    }

    // Match the compositor's frame-resource policy: only block if three blur
    // frames are still in flight and their scratch images cannot be reused.
    m_device->waitComputeIdle();
    for (BlurFrameResources &frame : m_blurFrames) {
        frame.completionFence = FileDescriptor{};
    }
    BlurFrameResources &frame = m_blurFrames[m_nextBlurFrame];
    m_nextBlurFrame = (m_nextBlurFrame + 1) % m_blurFrames.size();
    return ensureBlurFrameResources(frame, maximumIterationCount) ? &frame : nullptr;
}

bool ItemRendererVulkan::ensureBlurFrameResources(BlurFrameResources &frame, uint32_t maximumIterationCount)
{
    if (frame.size == m_targetSize
        && frame.sceneA && frame.sceneB && frame.sceneC && frame.fullSizeCompositor
        && frame.levels.size() >= maximumIterationCount
        && frame.levelCompositors.size() >= maximumIterationCount) {
        return true;
    }

    frame = BlurFrameResources{};
    frame.size = m_targetSize;
    frame.sceneA = allocateBlurIntermediate(m_targetSize);
    frame.sceneB = allocateBlurIntermediate(m_targetSize);
    frame.sceneC = allocateBlurIntermediate(m_targetSize);
    frame.fullSizeCompositor = VulkanCompositor::create(m_device);
    if (!frame.sceneA || !frame.sceneB || !frame.sceneC || !frame.fullSizeCompositor) {
        return false;
    }

    frame.levels.reserve(maximumIterationCount);
    frame.levelCompositors.reserve(maximumIterationCount);
    for (uint32_t level = 0; level < maximumIterationCount; ++level) {
        const int divisor = 1 << std::min(level + 1, 30u);
        const QSize size(std::max(m_targetSize.width() / divisor, 1),
                         std::max(m_targetSize.height() / divisor, 1));
        auto texture = allocateBlurIntermediate(size);
        auto compositor = VulkanCompositor::create(m_device);
        if (!texture || !compositor) {
            return false;
        }
        frame.levels.push_back(std::move(texture));
        frame.levelCompositors.push_back(std::move(compositor));
    }
    return true;
}

std::unique_ptr<VulkanTexture> ItemRendererVulkan::allocateBlurIntermediate(const QSize &size) const
{
    const auto usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled;
    auto texture = VulkanTexture::allocate(m_device, vk::Format::eR16G16B16A16Sfloat, size, usage, VulkanQueueRole::Compute);
    if (!texture) {
        texture = VulkanTexture::allocate(m_device, vk::Format::eR8G8B8A8Unorm, size, usage, VulkanQueueRole::Compute);
    }
    return texture;
}

ItemRendererVulkan::BackdropCache *ItemRendererVulkan::backdropCache(RenderView *view)
{
    const bool hasView = view != nullptr;
    for (const std::unique_ptr<BackdropCache> &cache : m_backdropCaches) {
        if (cache->hasView == hasView && cache->view.data() == view) {
            return cache.get();
        }
    }
    auto cache = std::make_unique<BackdropCache>();
    cache->view = view;
    cache->hasView = hasView;
    BackdropCache *const result = cache.get();
    m_backdropCaches.push_back(std::move(cache));
    return result;
}

bool ItemRendererVulkan::ensureBackdropCache(BackdropCache &cache)
{
    if (cache.scene && cache.size == m_targetSize && cache.colorDescription == m_targetColorDescription) {
        return true;
    }

    auto replacement = allocateBlurIntermediate(m_targetSize);
    if (!replacement) {
        return false;
    }
    if (cache.scene && cache.completionFence.isValid() && !cache.completionFence.isReadable()) {
        // Descriptor image views do not own their images. Size or color-state
        // changes are rare, so wait before replacing a cache still sampled by
        // an older frame instead of carrying a retired-image list.
        m_device->waitComputeIdle();
        for (const std::unique_ptr<BackdropCache> &entry : m_backdropCaches) {
            entry->completionFence = FileDescriptor{};
        }
    }
    cache.scene = std::move(replacement);
    cache.size = m_targetSize;
    cache.colorDescription = m_targetColorDescription;
    cache.initialized = false;
    return true;
}

void ItemRendererVulkan::markBackdropCachesSubmitted(const FileDescriptor &completionFence)
{
    for (BackdropCache *cache : std::as_const(m_usedBackdropCaches)) {
        cache->completionFence = completionFence.duplicate();
    }
}

std::optional<VulkanCompositorRenderResult> ItemRendererVulkan::renderFrameWithBackdropBlur(
    BlurFrameResources &frame,
    VulkanCompositor *captureCompositor,
    VulkanTexture *captureTarget,
    const std::shared_ptr<ColorDescription> &captureColorDescription)
{
    const Region fullDamage(0, 0, m_targetSize.width(), m_targetSize.height());
    const QRectF fullRect{QPointF(), QSizeF(m_targetSize)};
    const auto appendBase = [&fullRect](QList<VulkanCompositorLayer> &layers, VulkanTexture *texture) {
        if (texture) {
            layers.push_back(VulkanCompositorLayer{
                .rect = fullRect,
                .texture = texture,
            });
        }
    };
    const auto scratch = [&frame](VulkanTexture *first, VulkanTexture *second = nullptr) {
        for (VulkanTexture *candidate : {frame.sceneA.get(), frame.sceneB.get(), frame.sceneC.get()}) {
            if (candidate != first && candidate != second) {
                return candidate;
            }
        }
        return static_cast<VulkanTexture *>(nullptr);
    };
    FileDescriptor pendingAcquireFence = takeAcquireFence();
    // Intermediate passes are ordered on the compute queue and scratch reuse is
    // gated by the final fence. Their results are otherwise discarded, so do
    // not allocate timestamp query pools that no consumer will read.
    const auto submit = [this, &pendingAcquireFence](VulkanCompositor *compositor,
                                                     VulkanTexture *target,
                                                     const QList<VulkanCompositorLayer> &layers,
                                                     const Region &damage,
                                                     const std::shared_ptr<ColorDescription> &colorDescription) {
        auto result = compositor->renderTo(target,
                                           layers,
                                           Qt::transparent,
                                           damage,
                                           std::move(pendingAcquireFence),
                                           colorDescription,
                                           nullptr,
                                           m_uploadManager.get(),
                                           VulkanCompositor::Timing::Disabled);
        if (!result) {
            return false;
        }
        return true;
    };

    size_t firstLayer = 0;
    VulkanTexture *base = nullptr;
    for (const BackdropBlur &blur : std::as_const(m_backdropBlurs)) {
        if (blur.layerIndex < firstLayer || blur.layerIndex > size_t(m_layers.size())) {
            return std::nullopt;
        }

        BackdropCache *cache = nullptr;
        VulkanTexture *scene = nullptr;
        Region sceneDamage = fullDamage;
        // A per-view cache can represent the scene below the first ordered
        // blur. Later blur inputs include earlier blurred windows and keep
        // using frame-local scratch images.
        if (!base && firstLayer == 0) {
            cache = backdropCache(blur.cacheView.data());
            if (ensureBackdropCache(*cache)) {
                scene = cache->scene.get();
                // Scene collection already clipped layers to this damage, so
                // update the same tiles and preserve the remaining backdrop.
                sceneDamage = cache->initialized ? m_damage : fullDamage;
            } else {
                cache = nullptr;
            }
        }
        if (!scene) {
            scene = scratch(base);
        }
        QList<VulkanCompositorLayer> sceneLayers;
        appendBase(sceneLayers, base);
        for (size_t i = firstLayer; i < blur.layerIndex; ++i) {
            sceneLayers.push_back(m_layers[qsizetype(i)]);
        }
        if (!submit(frame.fullSizeCompositor.get(), scene, sceneLayers, sceneDamage, m_targetColorDescription)) {
            return std::nullopt;
        }
        if (cache) {
            cache->initialized = true;
            m_usedBackdropCaches.insert(cache);
        }

        VulkanTexture *read = scene;
        for (uint32_t level = 0; level < blur.iterationCount; ++level) {
            VulkanTexture *write = frame.levels[level].get();
            const QList<VulkanCompositorLayer> passLayers{VulkanCompositorLayer{
                .rect = QRectF(QPointF(), QSizeF(write->size())),
                .texture = read,
                .colorFilter = VulkanColorFilter::BlurDownsample,
                .colorFilterParameters = QVector4D(blur.offset, 1.0, 0.0, 0.0),
            }};
            if (!submit(frame.levelCompositors[level].get(), write, passLayers,
                        Region(0, 0, write->size().width(), write->size().height()), nullptr)) {
                return std::nullopt;
            }
            read = write;
        }
        for (uint32_t level = blur.iterationCount - 1; level > 0; --level) {
            VulkanTexture *write = frame.levels[level - 1].get();
            const QList<VulkanCompositorLayer> passLayers{VulkanCompositorLayer{
                .rect = QRectF(QPointF(), QSizeF(write->size())),
                .texture = read,
                .colorFilter = VulkanColorFilter::BlurUpsample,
                .colorFilterParameters = QVector4D(blur.offset, 1.0, 0.0, 0.0),
            }};
            if (!submit(frame.levelCompositors[level - 1].get(), write, passLayers,
                        Region(0, 0, write->size().width(), write->size().height()), nullptr)) {
                return std::nullopt;
            }
            read = write;
        }

        VulkanTexture *blurredScene = scratch(scene);
        QList<VulkanCompositorLayer> blurLayers;
        appendBase(blurLayers, scene);
        for (const QRectF &shapeRect : blur.shape) {
            const QRectF clip = shapeRect.intersected(fullRect);
            if (clip.isEmpty()) {
                continue;
            }
            blurLayers.push_back(VulkanCompositorLayer{
                .rect = fullRect,
                .texture = frame.levels.front().get(),
                .clipRect = clip,
                .roundedRect = blur.roundedRect,
                .cornerRadii = blur.cornerRadii,
                .blendMode = VulkanBlendMode::BackdropReplace,
                .colorFilter = VulkanColorFilter::BlurUpsampleAndColorize,
                .colorFilterMatrix = blur.colorMatrix,
                .colorFilterParameters = QVector4D(blur.offset, blur.modulation, 0.0, 0.0),
            });
            if (blur.noiseStrength > 0 && m_blurNoiseTexture) {
                blurLayers.push_back(VulkanCompositorLayer{
                    .rect = fullRect,
                    .texture = m_blurNoiseTexture.get(),
                    .clipRect = clip,
                    .blendMode = VulkanBlendMode::Additive,
                    .colorFilter = VulkanColorFilter::Noise,
                    .colorFilterParameters = QVector4D(0.0, blur.noiseOpacity, 0.0, 0.0),
                });
            }
        }
        if (!submit(frame.fullSizeCompositor.get(), blurredScene, blurLayers, fullDamage, nullptr)) {
            return std::nullopt;
        }
        if (!blur.groupEndLayerIndex) {
            base = blurredScene;
            firstLayer = blur.layerIndex;
            continue;
        }

        const size_t groupEnd = *blur.groupEndLayerIndex;
        if (groupEnd < blur.layerIndex || groupEnd > size_t(m_layers.size())) {
            return std::nullopt;
        }
        VulkanTexture *groupScene = scratch(scene, blurredScene);
        QList<VulkanCompositorLayer> groupLayers;
        appendBase(groupLayers, blurredScene);
        for (size_t i = blur.layerIndex; i < groupEnd; ++i) {
            groupLayers.push_back(m_layers[qsizetype(i)]);
        }
        if (!submit(frame.fullSizeCompositor.get(), groupScene, groupLayers, fullDamage, nullptr)) {
            return std::nullopt;
        }

        // The blur and the intrinsically rendered window are one visual group.
        // Interpolate that complete result against the untouched scene once;
        // fading the two independently would expose the blurred backdrop.
        QList<VulkanCompositorLayer> fadedGroupLayers;
        appendBase(fadedGroupLayers, scene);
        fadedGroupLayers.push_back(VulkanCompositorLayer{
            .rect = fullRect,
            .texture = groupScene,
            .blendMode = VulkanBlendMode::BackdropReplace,
            .colorFilterParameters = QVector4D(0.0, blur.groupOpacity, 0.0, 0.0),
        });
        if (!submit(frame.fullSizeCompositor.get(), blurredScene, fadedGroupLayers, fullDamage, nullptr)) {
            return std::nullopt;
        }
        base = blurredScene;
        firstLayer = groupEnd;
    }

    QList<VulkanCompositorLayer> finalLayers;
    appendBase(finalLayers, base);
    for (size_t i = firstLayer; i < size_t(m_layers.size()); ++i) {
        finalLayers.push_back(m_layers[qsizetype(i)]);
    }
    if (captureCompositor && captureTarget) {
        return captureCompositor->renderTo(captureTarget,
                                           finalLayers,
                                           Qt::transparent,
                                           fullDamage,
                                           std::move(pendingAcquireFence),
                                           captureColorDescription,
                                           nullptr,
                                           m_uploadManager.get());
    }
    return m_vulkanTarget
        ? m_compositor->renderTo(m_vulkanTarget->texture(), finalLayers, Qt::transparent, m_damage, std::move(pendingAcquireFence), m_targetColorDescription, m_vulkanTarget->outputColorPipeline(), m_uploadManager.get())
        : m_compositor->render(m_targetSize, finalLayers, Qt::transparent, m_damage, m_targetColorDescription, nullptr, std::move(pendingAcquireFence), m_uploadManager.get());
}

FileDescriptor ItemRendererVulkan::takeAcquireFence()
{
    FileDescriptor targetFence = m_vulkanTarget ? m_vulkanTarget->takeAcquireFence() : FileDescriptor{};
    if (!m_acquireFence.isValid()) {
        return targetFence;
    }
    if (!targetFence.isValid()) {
        return std::move(m_acquireFence);
    }
    FileDescriptor merged = SyncReleasePoint::mergeSyncFds(targetFence, m_acquireFence);
    m_acquireFence = {};
    return merged;
}

void ItemRendererVulkan::collectItem(Item *item,
                                     const QTransform &parentTransform,
                                     qreal parentOpacity,
                                     qreal brightness,
                                     qreal saturation,
                                     const QRectF &clipRect,
                                     const std::optional<RoundedClip> &parentRoundedClip,
                                     const std::function<bool(Item *)> &filter,
                                     const std::function<bool(Item *)> &holeFilter,
                                     bool isRoot)
{
    bool hole = false;
    if (filter && filter(item)) {
        if (!holeFilter || !holeFilter(item)) {
            return;
        }
        hole = true;
    }
    // renderItem() explicitly requests its root item, so match the OpenGL and
    // QPainter renderers by rendering that root even if it is hidden from the
    // workspace scene. This is required for offscreen rendering of minimized
    // windows. Descendants still honor their explicit visibility.
    if (!isRoot && !item->explicitVisible()) {
        return;
    }

    QTransform itemTransform = item->transform();
    itemTransform *= QTransform::fromTranslate(item->position().x(), item->position().y());
    QTransform transform = itemTransform;
    transform *= parentTransform;
    const bool unmodulatedGroupItem = m_activeBackdropBlurGroup
        && (m_activeBackdropBlurGroup->root == item || m_activeBackdropBlurGroup->surface == item);
    const qreal opacity = parentOpacity * (unmodulatedGroupItem ? 1.0 : item->opacity());
    const QList<Item *> children = item->sortedChildItems();
    for (Item *child : children) {
        if (child->z() >= 0) {
            break;
        }
        collectItem(child, transform, opacity, brightness, saturation, clipRect, parentRoundedClip, filter, holeFilter, false);
    }

    std::optional<RoundedClip> roundedClip;
    if (!item->borderRadius().isNull()) {
        roundedClip = RoundedClip{
            .rect = item->rect(),
            .radii = item->borderRadius().toVector(),
        };
    } else if (parentRoundedClip) {
        bool invertible = false;
        const QTransform parentToItem = itemTransform.inverted(&invertible);
        if (invertible) {
            roundedClip = RoundedClip{
                .rect = parentToItem.mapRect(parentRoundedClip->rect),
                .radii = parentRoundedClip->radii,
            };
        }
    }

    item->preprocess();
    const qsizetype firstOwnLayer = m_layers.size();
    if (auto shadow = qobject_cast<ShadowItem *>(item)) {
        collectShadow(shadow, transform, opacity, brightness, saturation, clipRect);
    } else if (auto surface = qobject_cast<SurfaceItem *>(item)) {
        collectSurface(surface, transform, opacity, brightness, saturation, clipRect, roundedClip);
    } else if (auto decoration = qobject_cast<DecorationItem *>(item)) {
        collectDecoration(decoration, transform, opacity, brightness, saturation, clipRect);
    } else if (auto image = qobject_cast<ImageItem *>(item)) {
        collectImage(image, transform, opacity, brightness, saturation, clipRect);
    } else if (auto border = qobject_cast<OutlinedBorderItem *>(item)) {
        collectOutlinedBorder(border, transform, opacity, brightness, saturation, clipRect);
    }
    const bool drawLayerDebugging = m_layerDebugging
        && qobject_cast<SurfaceItem *>(item)
        && m_layers.size() > firstOwnLayer;
    if (hole) {
        for (qsizetype i = firstOwnLayer; i < m_layers.size(); ++i) {
            m_layers[i].blendMode = VulkanBlendMode::DestinationOut;
        }
    }
    if (drawLayerDebugging) {
        const QRectF rect = item->rect();
        const qreal xScale = std::hypot(transform.m11(), transform.m12());
        const qreal yScale = std::hypot(transform.m21(), transform.m22());
        const qreal deviceScale = std::max(std::min(xScale, yScale), 0.0001);
        const qreal thickness = std::min({10.0 / deviceScale, rect.width() * 0.5, rect.height() * 0.5});
        if (thickness > 0) {
            m_layers.push_back(VulkanCompositorLayer{
                .rect = rect,
                .color = hole ? QColor(0, 50, 0, 50) : QColor(50, 0, 0, 50),
                .transform = transform,
                .clipRect = clipRect,
                .roundedRect = rect.adjusted(thickness, thickness, -thickness, -thickness),
                .outlineThickness = thickness,
                .colorDescription = nullptr,
            });
        }
    }

    for (Item *child : children) {
        if (child->z() < 0) {
            continue;
        }
        collectItem(child, transform, opacity, brightness, saturation, clipRect, roundedClip, filter, holeFilter, false);
    }
}

void ItemRendererVulkan::appendTextureLayer(const QRectF &rect,
                                            TextureVulkan *texture,
                                            const QRectF &sourceRect,
                                            const QTransform &transform,
                                            qreal opacity,
                                            qreal brightness,
                                            qreal saturation,
                                            const QRectF &clipRect,
                                            const std::shared_ptr<ColorDescription> &colorDescription,
                                            RenderingIntent renderingIntent,
                                            const QTransform &textureTransform,
                                            const std::optional<RoundedClip> &roundedClip,
                                            bool opaque)
{
    if (!texture || !texture->nativeTexture()) {
        return;
    }
    if (texture->releasePoint()) {
        m_releasePoints.insert(texture->releasePoint());
    }
    const auto nativeTextures = texture->nativeTextures();
    if (nativeTextures.empty() || nativeTextures.size() > 3) {
        return;
    }
    std::array<VulkanTexture *, 3> texturePlanes{};
    for (size_t i = 0; i < nativeTextures.size(); ++i) {
        texturePlanes[i] = nativeTextures[i].get();
    }
    m_layers.push_back(VulkanCompositorLayer{
        .rect = rect,
        .texture = texture->nativeTexture(),
        .texturePlanes = texturePlanes,
        .texturePlaneCount = uint32_t(nativeTextures.size()),
        .sourceRect = sourceRect,
        .opacity = opacity,
        .transform = transform,
        .clipRect = clipRect,
        .textureTransform = textureTransform,
        .brightness = brightness,
        .saturation = saturation,
        .roundedRect = roundedClip ? std::optional(roundedClip->rect) : std::nullopt,
        .cornerRadii = roundedClip ? roundedClip->radii : QVector4D{},
        .colorDescription = colorDescription,
        .renderingIntent = renderingIntent,
        .opaque = opaque,
    });
    appendFractionalDebugLayer(m_layers.back());
}

void ItemRendererVulkan::appendFractionalDebugLayer(const VulkanCompositorLayer &layer)
{
    if (!m_fractionalDebugging || (!layer.texture && layer.texturePlaneCount == 0)) {
        return;
    }
    VulkanCompositorLayer debugLayer = layer;
    debugLayer.color = Qt::white;
    debugLayer.texture = layer.texturePlaneCount > 0 ? layer.texturePlanes[0] : layer.texture;
    debugLayer.texturePlanes = {};
    debugLayer.texturePlaneCount = 0;
    debugLayer.auxiliaryTexture = nullptr;
    debugLayer.opacity = 1.0;
    debugLayer.brightness = 1.0;
    debugLayer.saturation = 1.0;
    debugLayer.blendMode = VulkanBlendMode::SourceOver;
    debugLayer.colorFilter = VulkanColorFilter::FractionalDebug;
    debugLayer.colorFilterMatrix = {};
    debugLayer.colorFilterParameters = {};
    debugLayer.colorDescription = nullptr;
    debugLayer.opaque = false;
    m_layers.push_back(std::move(debugLayer));
}

void ItemRendererVulkan::collectSurface(SurfaceItem *item,
                                        const QTransform &transform,
                                        qreal opacity,
                                        qreal brightness,
                                        qreal saturation,
                                        const QRectF &clipRect,
                                        const std::optional<RoundedClip> &roundedClip)
{
    auto texture = dynamic_cast<TextureVulkan *>(item->texture());
    if (!texture || item->destinationSize().isEmpty()) {
        return;
    }
    FileDescriptor acquireFence = item->takeAcquireFence();
    if (acquireFence.isValid()) {
        m_acquireFence = m_acquireFence.isValid()
            ? SyncReleasePoint::mergeSyncFds(m_acquireFence, acquireFence)
            : std::move(acquireFence);
    }
    const RectF sourceBox = item->bufferSourceBox();
    const QSizeF destinationSize = item->destinationSize();
    const QTransform textureTransform = outputTransform(item->bufferTransform());
    const RegionF opaqueRegion = item->opaque();
    for (const RectF &rect : item->shape().rects()) {
        const QRectF source(sourceBox.x() + rect.x() * sourceBox.width() / destinationSize.width(),
                            sourceBox.y() + rect.y() * sourceBox.height() / destinationSize.height(),
                            rect.width() * sourceBox.width() / destinationSize.width(),
                            rect.height() * sourceBox.height() / destinationSize.height());
        const bool opaque = opacity >= 1.0 && !roundedClip && opaqueRegion.contains(rect);
        appendTextureLayer(rect, texture, source, transform, opacity, brightness, saturation, clipRect, item->colorDescription(), item->renderingIntent(), textureTransform, roundedClip, opaque);
    }
}

void ItemRendererVulkan::collectImage(ImageItem *item,
                                      const QTransform &transform,
                                      qreal opacity,
                                      qreal brightness,
                                      qreal saturation,
                                      const QRectF &clipRect)
{
    auto texture = dynamic_cast<TextureVulkan *>(item->texture());
    appendTextureLayer(item->rect(), texture, QRectF{}, transform, opacity, brightness, saturation, clipRect, item->colorDescription(), item->renderingIntent());
}

void ItemRendererVulkan::collectDecoration(DecorationItem *item,
                                           const QTransform &transform,
                                           qreal opacity,
                                           qreal brightness,
                                           qreal saturation,
                                           const QRectF &clipRect)
{
    auto atlas = dynamic_cast<AtlasVulkan *>(item->atlas());
    if (!atlas) {
        return;
    }
    RectF left, top, right, bottom;
    item->window()->layoutDecorationRects(left, top, right, bottom);
    const std::array rects{left, top, right, bottom};
    for (uint32_t i = 0; i < rects.size(); ++i) {
        appendTextureLayer(rects[i], atlas->texture(i), QRectF{}, transform, opacity, brightness, saturation, clipRect, item->colorDescription(), item->renderingIntent());
    }
}

void ItemRendererVulkan::collectShadow(ShadowItem *item,
                                       const QTransform &transform,
                                       qreal opacity,
                                       qreal brightness,
                                       qreal saturation,
                                       const QRectF &clipRect)
{
    auto ninePatch = dynamic_cast<NinePatchVulkan *>(item->ninePatch());
    if (!ninePatch || !ninePatch->texture()) {
        return;
    }
    for (const WindowQuad &quad : item->quads()) {
        const QRectF sourceRect(QPointF(quad[0].u(), quad[0].v()),
                                QPointF(quad[2].u(), quad[2].v()));
        appendTextureLayer(quad.bounds(),
                           ninePatch->texture(),
                           sourceRect.normalized(),
                           transform,
                           opacity,
                           brightness,
                           saturation,
                           clipRect,
                           item->colorDescription(),
                           item->renderingIntent());
    }
}

void ItemRendererVulkan::collectOutlinedBorder(OutlinedBorderItem *item,
                                               const QTransform &transform,
                                               qreal opacity,
                                               qreal brightness,
                                               qreal saturation,
                                               const QRectF &clipRect)
{
    const BorderOutline outline = item->outline();
    const qreal thickness = outline.thickness();
    if (thickness <= 0 || item->rect().isEmpty()) {
        return;
    }
    m_layers.push_back(VulkanCompositorLayer{
        .rect = item->rect(),
        .color = outline.color(),
        .opacity = opacity,
        .transform = transform,
        .clipRect = clipRect,
        .brightness = brightness,
        .saturation = saturation,
        .roundedRect = item->rect().adjusted(thickness, thickness, -thickness, -thickness),
        .cornerRadii = outline.radius().toVector(),
        .outlineThickness = thickness,
        .colorDescription = item->colorDescription(),
        .renderingIntent = item->renderingIntent(),
    });
}

} // namespace KWin
