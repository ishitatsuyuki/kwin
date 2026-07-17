/*
    SPDX-FileCopyrightText: 2021 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "effect/offscreeneffect.h"
#include "compositor.h"
#include "core/drmdevice.h"
#include "core/output.h"
#include "core/pixelgrid.h"
#include "core/renderdevice.h"
#include "core/rendertarget.h"
#include "core/renderviewport.h"
#include "core/syncobjtimeline.h"
#include "effect/effecthandler.h"
#include "opengl/eglcontext.h"
#include "opengl/egldisplay.h"
#include "opengl/eglnativefence.h"
#include "opengl/eglswapchain.h"
#include "opengl/gltexture.h"
#include "opengl/glutils.h"
#include "scene/itemrenderer_vulkan.h"
#include "scene/windowitem.h"
#include "scene/workspacescene.h"
#include "vulkan/vulkan_backend.h"
#include "vulkan/vulkan_device.h"
#include "vulkan/vulkan_logging.h"
#include "vulkan/vulkan_rendertarget.h"
#include "vulkan/vulkan_texture.h"

#include <QScopeGuard>

#include <cerrno>
#include <drm_fourcc.h>
#include <poll.h>

namespace KWin
{

struct OffscreenData
{
public:
    virtual ~OffscreenData();
    void setDirty();
    void setShader(GLShader *newShader);
    void setVulkanFilter(VulkanColorFilter filter, const QMatrix4x4 &matrix = {}, const QVector4D &parameters = {});
    void setVertexSnappingMode(RenderGeometry::VertexSnappingMode mode);

    void paint(const RenderTarget &renderTarget, const RenderViewport &viewport, EffectWindow *window, const Region &deviceRegion,
               const WindowPaintData &data, const WindowQuadList &quads);

    void maybeRender(EffectWindow *window);
    bool ensureVulkanShareableSource(VulkanBackend *backend, const QSize &size);
    bool ensureVulkanShaderOutput(VulkanBackend *backend, const QSize &size);
    bool renderVulkanShader(const RenderTarget &renderTarget, const WindowPaintData &data, FileDescriptor &completionFence);
    void resetVulkanShaderOutput();
    void resetVulkanSource();

    std::unique_ptr<GLTexture> m_texture;
    std::unique_ptr<GLFramebuffer> m_fbo;
    std::shared_ptr<EglContext> m_vulkanEglContext;
    std::shared_ptr<EglSwapchain> m_vulkanSourceSwapchain;
    std::shared_ptr<EglSwapchainSlot> m_vulkanSourceSlot;
    std::shared_ptr<EglSwapchain> m_vulkanOutputSwapchain;
    std::shared_ptr<EglSwapchainSlot> m_vulkanOutputSlot;
    std::shared_ptr<VulkanTexture> m_vulkanTexture;
    std::shared_ptr<VulkanTexture> m_vulkanOutputTexture;
    std::unique_ptr<VulkanRenderTarget> m_vulkanTarget;
    std::unique_ptr<ItemRendererVulkan> m_vulkanRenderer;
    bool m_requireShareableSource = false;
    bool m_isDirty = true;
    GLShader *m_shader = nullptr;
    VulkanColorFilter m_vulkanFilter = VulkanColorFilter::None;
    QMatrix4x4 m_vulkanFilterMatrix;
    QVector4D m_vulkanFilterParameters;
    RenderGeometry::VertexSnappingMode m_vertexSnappingMode = RenderGeometry::VertexSnappingMode::Round;
    QMetaObject::Connection m_windowDamagedConnection;
    ItemEffect m_windowEffect;
};

class OffscreenEffectPrivate
{
public:
    std::map<EffectWindow *, std::unique_ptr<OffscreenData>> windows;
    QMetaObject::Connection windowDeletedConnection;
    RenderGeometry::VertexSnappingMode vertexSnappingMode = RenderGeometry::VertexSnappingMode::Round;
};

OffscreenEffect::OffscreenEffect(QObject *parent)
    : Effect(parent)
    , d(std::make_unique<OffscreenEffectPrivate>())
{
}

OffscreenEffect::~OffscreenEffect() = default;

bool OffscreenEffect::supported()
{
    return effects->isOpenGLCompositing()
        || effects->compositingType() == VulkanCompositing;
}

void OffscreenEffect::redirect(EffectWindow *window)
{
    std::unique_ptr<OffscreenData> &offscreenData = d->windows[window];
    if (offscreenData) {
        return;
    }
    offscreenData = std::make_unique<OffscreenData>();
    offscreenData->setVertexSnappingMode(d->vertexSnappingMode);
    offscreenData->m_windowEffect = ItemEffect(window->windowItem());
    offscreenData->m_windowDamagedConnection =
        connect(window, &EffectWindow::windowDamaged, this, &OffscreenEffect::handleWindowDamaged);

    if (d->windows.size() == 1) {
        setupConnections();
    }
}

void OffscreenEffect::unredirect(EffectWindow *window)
{
    auto it = d->windows.find(window);
    if (it == d->windows.end()) {
        return;
    }

    if (effects->isOpenGLCompositing() && !EglContext::currentContext()) {
        effects->openglContext()->makeCurrent();
    }

    d->windows.erase(it);
    if (d->windows.empty()) {
        destroyConnections();
    }
}

void OffscreenEffect::setShader(EffectWindow *window, GLShader *shader)
{
    if (const auto it = d->windows.find(window); it != d->windows.end()) {
        it->second->setShader(shader);
    }
}

void OffscreenEffect::setVulkanInvert(EffectWindow *window)
{
    if (const auto it = d->windows.find(window); it != d->windows.end()) {
        it->second->setVulkanFilter(VulkanColorFilter::Invert);
    }
}

void OffscreenEffect::setVulkanColor(EffectWindow *window, const QColor &color)
{
    if (const auto it = d->windows.find(window); it != d->windows.end()) {
        it->second->setVulkanFilter(VulkanColorFilter::Colorize,
                                    {},
                                    QVector4D(color.redF(), color.greenF(), color.blueF(), color.alphaF()));
    }
}

void OffscreenEffect::setVulkanColorBlindnessCorrection(EffectWindow *window, const QMatrix3x3 &defectMatrix, qreal intensity)
{
    if (const auto it = d->windows.find(window); it != d->windows.end()) {
        const QMatrix4x4 srgbToLms(17.8824f, 43.5161f, 4.11935f, 0.0f,
                                   3.45565f, 27.1554f, 3.86714f, 0.0f,
                                   0.0299566f, 0.184309f, 1.46709f, 0.0f,
                                   0.0f, 0.0f, 0.0f, 1.0f);
        const QMatrix4x4 errorMatrix(0.0809444479f, -0.130504409f, 0.116721066f, 0.0f,
                                     -0.0102485335f, 0.0540193266f, -0.113614708f, 0.0f,
                                     -0.000365296938f, -0.00412161469f, 0.693511405f, 0.0f,
                                     0.0f, 0.0f, 0.0f, 1.0f);
        QMatrix4x4 defect;
        defect.setToIdentity();
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column) {
                defect(row, column) = defectMatrix(row, column);
            }
        }
        QMatrix4x4 difference;
        difference.setToIdentity();
        difference -= errorMatrix * defect * srgbToLms;
        QMatrix4x4 correction;
        correction.setToIdentity();
        correction(1, 0) += 0.7f * intensity * difference(0, 0) + intensity * difference(1, 0);
        correction(1, 1) += 0.7f * intensity * difference(0, 1) + intensity * difference(1, 1);
        correction(1, 2) += 0.7f * intensity * difference(0, 2) + intensity * difference(1, 2);
        correction(2, 0) += 0.7f * intensity * difference(0, 0) + intensity * difference(2, 0);
        correction(2, 1) += 0.7f * intensity * difference(0, 1) + intensity * difference(2, 1);
        correction(2, 2) += 0.7f * intensity * difference(0, 2) + intensity * difference(2, 2);
        it->second->setVulkanFilter(VulkanColorFilter::ColorBlindnessCorrection, correction);
    }
}

void OffscreenEffect::apply(EffectWindow *window, int mask, WindowPaintData &data, WindowQuadList &quads)
{
}

void OffscreenData::resetVulkanShaderOutput()
{
    m_vulkanOutputTexture.reset();
    m_vulkanOutputSlot.reset();
    m_vulkanOutputSwapchain.reset();
}

void OffscreenData::resetVulkanSource()
{
    resetVulkanShaderOutput();
    m_vulkanTarget.reset();
    m_vulkanTexture.reset();
    m_vulkanSourceSlot.reset();
    m_vulkanSourceSwapchain.reset();
}

bool OffscreenData::ensureVulkanShareableSource(VulkanBackend *backend, const QSize &size)
{
    if (m_vulkanSourceSwapchain && m_vulkanSourceSwapchain->size() == size
        && m_vulkanSourceSlot && m_vulkanTexture && m_vulkanTarget) {
        return true;
    }

    resetVulkanSource();
    RenderDevice *renderDevice = backend ? backend->renderDevice() : nullptr;
    VulkanDevice *device = backend ? backend->device() : nullptr;
    if (!renderDevice || !renderDevice->drmDevice() || !renderDevice->eglDisplay() || !device) {
        return false;
    }
    m_vulkanEglContext = renderDevice->eglContext();
    if (!m_vulkanEglContext || m_vulkanEglContext->isFailed()) {
        qCWarning(KWIN_VULKAN) << "Legacy effect shader bridge could not create an EGL context";
        return false;
    }

    // Prefer a floating-point bridge so custom shaders preserve the same range
    // as the native Vulkan offscreen path. Fall back to the format used by the
    // historical OpenGL redirect path when it is not jointly renderable.
    static constexpr std::array formats = {
        DRM_FORMAT_ABGR16161616F,
        DRM_FORMAT_ABGR8888,
    };
    for (const uint32_t format : formats) {
        const ModifierList modifiers = device->computeOutputFormats().value(format).intersected(
            renderDevice->eglDisplay()->nonExternalOnlySupportedDrmFormats().value(format));
        if (modifiers.isEmpty()) {
            continue;
        }
        auto swapchain = EglSwapchain::create(renderDevice->drmDevice()->allocator(),
                                              m_vulkanEglContext.get(),
                                              size,
                                              format,
                                              modifiers);
        if (!swapchain) {
            continue;
        }
        auto slot = swapchain->acquire();
        if (!slot) {
            continue;
        }
        auto texture = device->importBuffer(slot->buffer(),
                                            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        if (!texture) {
            continue;
        }
        m_vulkanSourceSwapchain = std::move(swapchain);
        m_vulkanSourceSlot = std::move(slot);
        m_vulkanTexture = std::move(texture);
        m_vulkanTarget = std::make_unique<VulkanRenderTarget>(m_vulkanTexture.get());
        m_isDirty = true;
        return true;
    }

    qCWarning(KWIN_VULKAN) << "Legacy effect shader bridge has no Vulkan/EGL renderable dma-buf format";
    return false;
}

bool OffscreenData::ensureVulkanShaderOutput(VulkanBackend *backend, const QSize &size)
{
    if (m_vulkanOutputSwapchain && m_vulkanOutputSwapchain->size() == size
        && m_vulkanOutputSlot && m_vulkanOutputTexture) {
        return true;
    }
    resetVulkanShaderOutput();
    RenderDevice *renderDevice = backend ? backend->renderDevice() : nullptr;
    VulkanDevice *device = backend ? backend->device() : nullptr;
    if (!renderDevice || !device || !m_vulkanEglContext || !m_vulkanSourceSwapchain) {
        return false;
    }

    const uint32_t format = m_vulkanSourceSwapchain->format();
    const ModifierList modifiers = device->computeOutputFormats().value(format).intersected(
        renderDevice->eglDisplay()->nonExternalOnlySupportedDrmFormats().value(format));
    auto swapchain = EglSwapchain::create(renderDevice->drmDevice()->allocator(),
                                          m_vulkanEglContext.get(),
                                          size,
                                          format,
                                          modifiers);
    if (!swapchain) {
        qCWarning(KWIN_VULKAN) << "Legacy effect shader bridge failed to allocate its output dma-buf";
        return false;
    }
    auto slot = swapchain->acquire();
    if (!slot) {
        return false;
    }
    auto texture = device->importBuffer(slot->buffer(), VK_IMAGE_USAGE_SAMPLED_BIT);
    if (!texture) {
        qCWarning(KWIN_VULKAN) << "Legacy effect shader bridge failed to import its output into Vulkan";
        return false;
    }
    m_vulkanOutputSwapchain = std::move(swapchain);
    m_vulkanOutputSlot = std::move(slot);
    m_vulkanOutputTexture = std::move(texture);
    return true;
}

bool OffscreenData::renderVulkanShader(const RenderTarget &renderTarget, const WindowPaintData &data, FileDescriptor &completionFence)
{
    const auto backend = qobject_cast<VulkanBackend *>(Compositor::self()->backend());
    if (!m_shader || !m_vulkanTarget || !m_vulkanSourceSlot || !m_vulkanEglContext
        || !ensureVulkanShaderOutput(backend, m_vulkanTexture->size())) {
        return false;
    }

    EglContext *previousContext = EglContext::currentContext();
    if (!m_vulkanEglContext->makeCurrent()) {
        return false;
    }
    const auto restoreContext = qScopeGuard([this, previousContext]() {
        if (previousContext && previousContext != m_vulkanEglContext.get()) {
            previousContext->makeCurrent();
        } else if (!previousContext) {
            m_vulkanEglContext->doneCurrent();
        }
    });

    const auto blockingWait = [](const FileDescriptor &fd) {
        pollfd descriptor{
            .fd = fd.get(),
            .events = POLLIN,
            .revents = 0,
        };
        int result;
        do {
            result = poll(&descriptor, 1, -1);
        } while (result < 0 && errno == EINTR);
        return result > 0;
    };
    const bool useNativeFences = m_vulkanEglContext->displayObject()->supportsNativeFence()
        && !qEnvironmentVariableIsSet("KWIN_VULKAN_LEGACY_SHADER_BLOCKING_SYNC");
    const auto enqueueWait = [this, &blockingWait, useNativeFences](FileDescriptor &&fd) {
        if (!fd.isValid()) {
            return true;
        }
        if (!useNativeFences) {
            return blockingWait(fd);
        }
        const EGLNativeFence fence = EGLNativeFence::importFence(m_vulkanEglContext->displayObject(), std::move(fd));
        return fence.waitSync() || (fd.isValid() && blockingWait(fd));
    };
    if (!enqueueWait(m_vulkanTarget->takeCompletionFence())) {
        qCWarning(KWIN_VULKAN) << "Legacy effect shader bridge failed to wait for its Vulkan source";
        return false;
    }
    if (!enqueueWait(m_vulkanOutputSlot->releaseFd().duplicate())) {
        qCWarning(KWIN_VULKAN) << "Legacy effect shader bridge failed to wait for its previous Vulkan consumer";
        return false;
    }

    m_vulkanEglContext->pushFramebuffer(m_vulkanOutputSlot->framebuffer());
    glClearColor(0.0, 0.0, 0.0, 0.0);
    glClear(GL_COLOR_BUFFER_BIT);
    {
        ShaderBinder binder(m_shader);
        const QSize size = m_vulkanTexture->size();
        QMatrix4x4 projection;
        projection.scale(1, -1);
        projection.ortho(QRectF(QPointF(), QSizeF(size)));

        const qreal rgb = data.brightness() * data.opacity();
        const auto toXYZ = renderTarget.colorDescription()->containerColorimetry().toXYZ();
        m_shader->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, projection);
        m_shader->setUniform(GLShader::Vec4Uniform::ModulationConstant,
                             QVector4D(rgb, rgb, rgb, data.opacity()));
        m_shader->setUniform(GLShader::FloatUniform::Saturation, data.saturation());
        m_shader->setUniform(GLShader::Vec3Uniform::PrimaryBrightness,
                             QVector3D(toXYZ(1, 0), toXYZ(1, 1), toXYZ(1, 2)));
        m_shader->setUniform(GLShader::IntUniform::TextureWidth, size.width());
        m_shader->setUniform(GLShader::IntUniform::TextureHeight, size.height());
        m_shader->setColorspaceUniforms(ColorDescription::sRGB,
                                        renderTarget.colorDescription(),
                                        RenderingIntent::Perceptual);

        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        m_vulkanSourceSlot->texture()->render(QSizeF(size));
        glDisable(GL_BLEND);
    }
    m_vulkanEglContext->popFramebuffer();

    if (!useNativeFences) {
        // Compatibility fallback for EGL implementations without native
        // fences. It is intentionally blocking, but keeps legacy shaders
        // functional without weakening dma-buf ownership rules.
        glFinish();
        completionFence = {};
        return true;
    }

    EGLNativeFence fence(m_vulkanEglContext->displayObject());
    if (!fence.isValid()) {
        qCWarning(KWIN_VULKAN) << "Legacy effect shader bridge could not export an EGL native fence";
        return false;
    }
    // The fence covers the source read and output write. Vulkan waits on it
    // before either rewriting the source or sampling the shader result.
    m_vulkanTarget->setAcquireFence(fence.fileDescriptor().duplicate());
    m_vulkanSourceSlot->releasePoint()->addReleaseFence(fence.fileDescriptor());
    m_vulkanOutputSlot->releasePoint()->addReleaseFence(fence.fileDescriptor());
    completionFence = fence.takeFileDescriptor();
    return true;
}

void OffscreenData::maybeRender(EffectWindow *window)
{
    const qreal scale = window->screen()->scale();
    const RectF logicalGeometry = snapToPixels(window->expandedGeometry(), scale);
    const QSize textureSize = (logicalGeometry.size() * scale).toSize();

    if (textureSize.isEmpty()) {
        m_fbo.reset();
        m_texture.reset();
        resetVulkanSource();
        return;
    }

    if (effects->compositingType() == VulkanCompositing) {
        const auto backend = qobject_cast<VulkanBackend *>(Compositor::self()->backend());
        VulkanDevice *device = backend ? backend->device() : nullptr;
        if (!device) {
            return;
        }
        if (!m_vulkanRenderer || !m_vulkanRenderer->isValid()) {
            resetVulkanSource();
            m_vulkanRenderer = std::make_unique<ItemRendererVulkan>(device);
            if (!m_vulkanRenderer->isValid()) {
                m_vulkanRenderer.reset();
                return;
            }
        }
        if (m_requireShareableSource) {
            if (!ensureVulkanShareableSource(backend, textureSize)) {
                return;
            }
        } else if (!m_vulkanTexture || m_vulkanTexture->size() != textureSize || m_vulkanSourceSlot) {
            resetVulkanSource();
            m_vulkanTexture = VulkanTexture::allocate(device,
                                                      vk::Format::eR16G16B16A16Sfloat,
                                                      textureSize,
                                                      vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
                                                      VulkanQueueRole::Compute);
            if (!m_vulkanTexture) {
                m_vulkanTexture = VulkanTexture::allocate(device,
                                                          vk::Format::eR8G8B8A8Unorm,
                                                          textureSize,
                                                          vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
                                                          VulkanQueueRole::Compute);
            }
            if (!m_vulkanTexture) {
                return;
            }
            m_vulkanTarget = std::make_unique<VulkanRenderTarget>(m_vulkanTexture.get());
            m_isDirty = true;
        }

        if (m_isDirty) {
            // Shareable targets carry an EGL fence after the legacy shader has
            // sampled them. Ordinary Vulkan targets only need their previous
            // completion fence; queue ordering covers the final scene sample.
            if (!m_vulkanSourceSlot) {
                m_vulkanTarget->setAcquireFence(m_vulkanTarget->takeCompletionFence());
            }
            m_vulkanTarget->takeRenderTimeQueries();
            const RenderTarget renderTarget(m_vulkanTarget.get(), ColorDescription::sRGB);
            const RenderViewport viewport(logicalGeometry, scale, renderTarget, QPoint());
            m_vulkanRenderer->beginFrame(renderTarget, viewport);
            m_vulkanRenderer->renderBackground(renderTarget, viewport, Region(Rect(QPoint(), textureSize)));

            WindowPaintData data;
            data.setOpacity(1.0);
            const int mask = Effect::PAINT_WINDOW_TRANSFORMED | Effect::PAINT_WINDOW_TRANSLUCENT;
            m_vulkanRenderer->renderItem(renderTarget,
                                         viewport,
                                         window->windowItem(),
                                         mask,
                                         Region::infinite(),
                                         data,
                                         {},
                                         {});
            m_vulkanRenderer->endFrame();
            if (m_vulkanSourceSlot) {
                m_vulkanSourceSlot->releasePoint()->addReleaseFence(m_vulkanTarget->takeCompletionFence());
                m_vulkanTarget->setCompletionFence(m_vulkanSourceSlot->releaseFd().duplicate());
            }
            m_isDirty = false;
        }
        return;
    }

    if (!m_texture || m_texture->size() != textureSize) {
        m_texture = GLTexture::allocate(GL_RGBA8, textureSize);
        if (!m_texture) {
            return;
        }
        m_texture->setFilter(GL_LINEAR);
        m_texture->setWrapMode(GL_CLAMP_TO_EDGE);
        m_fbo = std::make_unique<GLFramebuffer>(m_texture.get());
        m_isDirty = true;
    }

    if (m_isDirty) {
        RenderTarget renderTarget(m_fbo.get());
        RenderViewport viewport(logicalGeometry, scale, renderTarget, QPoint());
        GLFramebuffer::pushFramebuffer(m_fbo.get());
        glClearColor(0.0, 0.0, 0.0, 0.0);
        glClear(GL_COLOR_BUFFER_BIT);

        WindowPaintData data;
        data.setOpacity(1.0);

        const int mask = Effect::PAINT_WINDOW_TRANSFORMED | Effect::PAINT_WINDOW_TRANSLUCENT;
        effects->drawWindow(renderTarget, viewport, window, mask, Region::infinite(), data);

        GLFramebuffer::popFramebuffer();
        m_isDirty = false;
    }
}

OffscreenData::~OffscreenData()
{
    QObject::disconnect(m_windowDamagedConnection);
    if (m_vulkanEglContext) {
        m_vulkanEglContext->makeCurrent();
    }
    resetVulkanSource();
}

void OffscreenData::setDirty()
{
    m_isDirty = true;
}

void OffscreenData::setShader(GLShader *newShader)
{
    if (m_shader == newShader) {
        return;
    }
    m_shader = newShader;
    resetVulkanShaderOutput();
    if (newShader) {
        m_requireShareableSource = true;
        // OffscreenEffect shaders are normally installed before the first
        // capture. If one is changed later, refresh into a shareable target.
        if (m_vulkanTexture && !m_vulkanSourceSlot) {
            m_vulkanTarget.reset();
            m_vulkanTexture.reset();
            m_isDirty = true;
        }
    }
}

void OffscreenData::setVulkanFilter(VulkanColorFilter filter, const QMatrix4x4 &matrix, const QVector4D &parameters)
{
    m_vulkanFilter = filter;
    m_vulkanFilterMatrix = matrix;
    m_vulkanFilterParameters = parameters;
}

void OffscreenData::setVertexSnappingMode(RenderGeometry::VertexSnappingMode mode)
{
    m_vertexSnappingMode = mode;
}

void OffscreenData::paint(const RenderTarget &renderTarget, const RenderViewport &viewport, EffectWindow *window, const Region &deviceRegion,
                          const WindowPaintData &data, const WindowQuadList &quads)
{
    if (m_vulkanTexture) {
        auto renderer = dynamic_cast<ItemRendererVulkan *>(effects->scene()->renderer());
        if (!renderer) {
            return;
        }
        VulkanTexture *texture = m_vulkanTexture.get();
        FileDescriptor acquireFence;
        std::shared_ptr<SyncReleasePoint> releasePoint = m_vulkanSourceSlot
            ? m_vulkanSourceSlot->releasePoint()
            : nullptr;
        qreal opacity = data.opacity();
        qreal brightness = data.brightness();
        qreal saturation = data.saturation();
        std::shared_ptr<ColorDescription> colorDescription = ColorDescription::sRGB;
        VulkanColorFilter colorFilter = m_vulkanFilter;
        QMatrix4x4 colorFilterMatrix = m_vulkanFilterMatrix;
        QVector4D colorFilterParameters = m_vulkanFilterParameters;
        if (m_shader) {
            if (!renderVulkanShader(renderTarget, data, acquireFence)) {
                return;
            }
            texture = m_vulkanOutputTexture.get();
            releasePoint = m_vulkanOutputSlot->releasePoint();
            // The GL shader has already applied these parameters and the
            // destination colorspace transform, matching the OpenGL path.
            opacity = 1.0;
            brightness = 1.0;
            saturation = 1.0;
            colorDescription = renderTarget.colorDescription();
            colorFilter = VulkanColorFilter::None;
            colorFilterMatrix = {};
            colorFilterParameters = {};
        }
        const qreal scale = viewport.scale();
        QMatrix4x4 windowTransform;
        windowTransform.translate(std::round(window->x() * scale), std::round(window->y() * scale));
        windowTransform *= data.toMatrix(scale);
        const QRectF clipRect = deviceRegion == Region::infinite()
            ? QRectF(QPointF(), QSizeF(renderTarget.size()))
            : QRectF(static_cast<QRect>(renderTarget.transform().map(deviceRegion, renderTarget.transformedSize()).boundingRect()));

        for (const WindowQuad &quad : quads) {
            std::array<QPointF, 4> vertices;
            std::array<QPointF, 4> textureCoordinates;
            for (size_t i = 0; i < vertices.size(); ++i) {
                QPointF local(quad[i].x() * scale, quad[i].y() * scale);
                if (m_vertexSnappingMode == RenderGeometry::VertexSnappingMode::Round) {
                    local = QPointF(std::round(local.x()), std::round(local.y()));
                }
                const QVector4D projected = windowTransform * QVector4D(local.x(), local.y(), 0, 1);
                if (qFuzzyIsNull(projected.w())) {
                    vertices[i] = QPointF(-1e9, -1e9);
                } else {
                    const QPointF logical(projected.x() / projected.w() / scale,
                                          projected.y() / projected.w() / scale);
                    vertices[i] = viewport.mapToRenderTarget(logical);
                }
                textureCoordinates[i] = QPointF(quad[i].u(), quad[i].v());
            }
            renderer->renderTextureQuad(texture,
                                        vertices,
                                        textureCoordinates,
                                        clipRect,
                                        opacity,
                                        brightness,
                                        saturation,
                                        colorDescription,
                                        colorFilter,
                                        colorFilterMatrix,
                                        colorFilterParameters,
                                        nullptr,
                                        acquireFence.duplicate(),
                                        releasePoint);
        }
        return;
    }
    if (!m_texture) {
        return;
    }
    GLShader *shader = m_shader ? m_shader : ShaderManager::instance()->shader(ShaderTrait::MapTexture | ShaderTrait::Modulate | ShaderTrait::AdjustSaturation | ShaderTrait::TransformColorspace);
    ShaderBinder binder(shader);

    const double scale = viewport.scale();

    GLVertexBuffer *vbo = GLVertexBuffer::streamingBuffer();
    vbo->reset();
    vbo->setAttribLayout(std::span(GLVertexBuffer::GLVertex2DLayout), sizeof(GLVertex2D));

    RenderGeometry geometry;
    geometry.setVertexSnappingMode(m_vertexSnappingMode);
    for (auto &quad : quads) {
        geometry.appendWindowQuad(quad, scale);
    }
    geometry.postProcessTextureCoordinates(m_texture->matrix(NormalizedCoordinates));

    const auto map = vbo->map<GLVertex2D>(geometry.size());
    if (!map) {
        return;
    }
    geometry.copy(*map);
    vbo->unmap();

    vbo->bindArrays();

    const qreal rgb = data.brightness() * data.opacity();
    const qreal a = data.opacity();

    QMatrix4x4 mvp = viewport.projectionMatrix();
    mvp.translate(std::round(window->x() * scale), std::round(window->y() * scale));

    const auto toXYZ = renderTarget.colorDescription()->containerColorimetry().toXYZ();
    shader->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, mvp * data.toMatrix(scale));
    shader->setUniform(GLShader::Vec4Uniform::ModulationConstant, QVector4D(rgb, rgb, rgb, a));
    shader->setUniform(GLShader::FloatUniform::Saturation, data.saturation());
    shader->setUniform(GLShader::Vec3Uniform::PrimaryBrightness, QVector3D(toXYZ(1, 0), toXYZ(1, 1), toXYZ(1, 2)));
    shader->setUniform(GLShader::IntUniform::TextureWidth, m_texture->width());
    shader->setUniform(GLShader::IntUniform::TextureHeight, m_texture->height());
    shader->setColorspaceUniforms(ColorDescription::sRGB, renderTarget.colorDescription(), RenderingIntent::Perceptual);

    const bool clipping = deviceRegion != Region::infinite();
    const Region clipRegion = clipping ? viewport.transform().map(deviceRegion, renderTarget.transformedSize()) : Region::infinite();

    if (clipping) {
        glEnable(GL_SCISSOR_TEST);
    }

    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    m_texture->bind();
    vbo->draw(clipRegion, GL_TRIANGLES, 0, geometry.count(), clipping);
    m_texture->unbind();

    glDisable(GL_BLEND);
    if (clipping) {
        glDisable(GL_SCISSOR_TEST);
    }
    vbo->unbindArrays();
}

void OffscreenEffect::drawWindow(const RenderTarget &renderTarget, const RenderViewport &viewport, EffectWindow *window, int mask, const Region &deviceRegion, WindowPaintData &data)
{
    const auto it = d->windows.find(window);
    if (it == d->windows.end()) {
        effects->drawWindow(renderTarget, viewport, window, mask, deviceRegion, data);
        return;
    }
    OffscreenData *offscreenData = it->second.get();

    const RectF expandedGeometry = snapToPixels(window->expandedGeometry(), viewport.scale());
    const RectF frameGeometry = snapToPixels(window->frameGeometry(), viewport.scale());

    RectF visibleRect = expandedGeometry;
    visibleRect.moveTopLeft(expandedGeometry.topLeft() - frameGeometry.topLeft());
    WindowQuad quad;
    quad[0] = WindowVertex(visibleRect.topLeft(), QPointF(0, 0));
    quad[1] = WindowVertex(visibleRect.topRight(), QPointF(1, 0));
    quad[2] = WindowVertex(visibleRect.bottomRight(), QPointF(1, 1));
    quad[3] = WindowVertex(visibleRect.bottomLeft(), QPointF(0, 1));

    WindowQuadList quads;
    quads.append(quad);
    apply(window, mask, data, quads);

    offscreenData->maybeRender(window);
    offscreenData->paint(renderTarget, viewport, window, deviceRegion, data, quads);
}

void OffscreenEffect::handleWindowDamaged(EffectWindow *window)
{
    if (const auto it = d->windows.find(window); it != d->windows.end()) {
        it->second->setDirty();
    }
}

void OffscreenEffect::handleWindowDeleted(EffectWindow *window)
{
    unredirect(window);
}

void OffscreenEffect::setupConnections()
{
    d->windowDeletedConnection =
        connect(effects, &EffectsHandler::windowDeleted, this, &OffscreenEffect::handleWindowDeleted);
}

void OffscreenEffect::destroyConnections()
{
    disconnect(d->windowDeletedConnection);

    d->windowDeletedConnection = {};
}

void OffscreenEffect::setVertexSnappingMode(RenderGeometry::VertexSnappingMode mode)
{
    d->vertexSnappingMode = mode;
    for (auto &window : std::as_const(d->windows)) {
        window.second->setVertexSnappingMode(mode);
    }
}

bool OffscreenEffect::blocksDirectScanout() const
{
    return false;
}

class CrossFadeWindowData : public OffscreenData
{
public:
    RectF frameGeometryAtCapture;
};

class CrossFadeEffectPrivate
{
public:
    std::map<EffectWindow *, std::unique_ptr<CrossFadeWindowData>> windows;
    qreal progress;
};

CrossFadeEffect::CrossFadeEffect(QObject *parent)
    : Effect(parent)
    , d(std::make_unique<CrossFadeEffectPrivate>())
{
}

CrossFadeEffect::~CrossFadeEffect() = default;

void CrossFadeEffect::drawWindow(const RenderTarget &renderTarget, const RenderViewport &viewport, EffectWindow *window, int mask, const Region &deviceRegion, WindowPaintData &data)
{
    const auto it = d->windows.find(window);

    // paint the new window (if applicable) underneath
    if (data.crossFadeProgress() > 0 || it == d->windows.end()) {
        Effect::drawWindow(renderTarget, viewport, window, mask, deviceRegion, data);
    }

    if (it == d->windows.end()) {
        return;
    }
    CrossFadeWindowData *offscreenData = it->second.get();

    // paint old snapshot on top
    WindowPaintData previousWindowData = data;
    previousWindowData.setOpacity((1.0 - data.crossFadeProgress()) * data.opacity());

    const RectF expandedGeometry = snapToPixels(window->expandedGeometry(), viewport.scale());
    const RectF frameGeometry = snapToPixels(window->frameGeometry(), viewport.scale());

    // This is for the case of *non* live effect, when the window buffer we saved has a different size
    // compared to the size the window has now. The "old" window will be rendered scaled to the current
    // window geometry, but everything will be scaled, also the shadow if there is any, making the window
    // frame not line up anymore with window->frameGeometry()
    // to fix that, we consider how much the shadow will have scaled, and use that as margins to the
    // current frame geometry. this causes the scaled window to visually line up perfectly with frameGeometry,
    // having the scaled shadow all outside of it.
    const qreal widthRatio = offscreenData->frameGeometryAtCapture.width() / frameGeometry.width();
    const qreal heightRatio = offscreenData->frameGeometryAtCapture.height() / frameGeometry.height();

    const QMarginsF margins(
        (expandedGeometry.x() - frameGeometry.x()) / widthRatio,
        (expandedGeometry.y() - frameGeometry.y()) / heightRatio,
        (frameGeometry.right() - expandedGeometry.right()) / widthRatio,
        (frameGeometry.bottom() - expandedGeometry.bottom()) / heightRatio);

    RectF visibleRect = RectF(QPointF(0, 0), frameGeometry.size()) - margins;

    WindowQuad quad;
    quad[0] = WindowVertex(visibleRect.topLeft(), QPointF(0, 0));
    quad[1] = WindowVertex(visibleRect.topRight(), QPointF(1, 0));
    quad[2] = WindowVertex(visibleRect.bottomRight(), QPointF(1, 1));
    quad[3] = WindowVertex(visibleRect.bottomLeft(), QPointF(0, 1));

    WindowQuadList quads;
    quads.append(quad);
    offscreenData->paint(renderTarget, viewport, window, deviceRegion, previousWindowData, quads);
}

void CrossFadeEffect::redirect(EffectWindow *window)
{
    if (d->windows.empty()) {
        connect(effects, &EffectsHandler::windowDeleted, this, &CrossFadeEffect::handleWindowDeleted);
    }

    std::unique_ptr<CrossFadeWindowData> &offscreenData = d->windows[window];
    if (offscreenData) {
        return;
    }
    offscreenData = std::make_unique<CrossFadeWindowData>();
    // AnimationEffect can install an arbitrary GL shader after this snapshot
    // has been captured. Preserve the snapshot in an EGL/Vulkan-shareable
    // buffer so that late shader installation does not recapture new content.
    offscreenData->m_requireShareableSource = effects->compositingType() == VulkanCompositing;
    offscreenData->m_windowEffect = ItemEffect(window->windowItem());

    // Avoid including blur and contrast effects. During a normal painting cycle they
    // won't be included, but since we call effects->drawWindow() outside usual compositing
    // cycle, we have to prevent backdrop effects kicking in.
    const QVariant blurRole = window->data(WindowForceBlurRole);
    window->setData(WindowForceBlurRole, QVariant());
    const QVariant contrastRole = window->data(WindowForceBackgroundContrastRole);
    window->setData(WindowForceBackgroundContrastRole, QVariant());

    if (effects->isOpenGLCompositing()) {
        effects->makeOpenGLContextCurrent();
    }
    offscreenData->maybeRender(window);
    offscreenData->frameGeometryAtCapture = window->frameGeometry();

    window->setData(WindowForceBlurRole, blurRole);
    window->setData(WindowForceBackgroundContrastRole, contrastRole);
}

void CrossFadeEffect::unredirect(EffectWindow *window)
{
    auto it = d->windows.find(window);
    if (it == d->windows.end()) {
        return;
    }

    if (effects->isOpenGLCompositing() && !EglContext::currentContext()) {
        effects->openglContext()->makeCurrent();
    }

    d->windows.erase(it);
    if (d->windows.empty()) {
        disconnect(effects, &EffectsHandler::windowDeleted, this, &CrossFadeEffect::handleWindowDeleted);
    }
}

void CrossFadeEffect::handleWindowDeleted(EffectWindow *window)
{
    unredirect(window);
}

void CrossFadeEffect::setShader(EffectWindow *window, GLShader *shader)
{
    if (const auto it = d->windows.find(window); it != d->windows.end()) {
        it->second->setShader(shader);
    }
}

bool CrossFadeEffect::blocksDirectScanout() const
{
    return false;
}

bool CrossFadeEffect::supported()
{
    return effects->isOpenGLCompositing()
        || effects->compositingType() == VulkanCompositing;
}

} // namespace KWin

#include "moc_offscreeneffect.cpp"
