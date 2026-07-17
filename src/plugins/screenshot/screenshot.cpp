/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2010 Martin Gräßlin <mgraesslin@kde.org>
    SPDX-FileCopyrightText: 2010 Nokia Corporation and /or its subsidiary(-ies)
    SPDX-FileCopyrightText: 2021 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "screenshot.h"
#include "screenshotdbusinterface2.h"

#include "compositor.h"
#include "core/output.h"
#include "core/pixelgrid.h"
#include "core/rendertarget.h"
#include "core/renderviewport.h"
#include "effect/effect.h"
#include "opengl/eglbackend.h"
#include "opengl/glplatform.h"
#include "opengl/glutils.h"
#include "scene/decorationitem.h"
#include "scene/item.h"
#include "scene/itemrenderer.h"
#include "scene/shadowitem.h"
#include "scene/surfaceitem.h"
#include "scene/windowitem.h"
#include "scene/workspacescene.h"
#include "screenshotlayer.h"
#include "vulkan/vulkan_backend.h"
#include "vulkan/vulkan_rendertarget.h"
#include "vulkan/vulkan_texture.h"
#include "window.h"
#include "workspace.h"

#include <QPainter>

namespace KWin
{

static bool shouldFilterWindowFromCapture(Window *window, std::optional<pid_t> pidToHide)
{
    return window->excludeFromCapture() || (pidToHide.has_value() && window->pid() == *pidToHide);
}

static void convertFromGLImage(QImage &img, int w, int h, const OutputTransform &renderTargetTransformation)
{
    // from QtOpenGL/qgl.cpp
    // SPDX-FileCopyrightText: 2010 Nokia Corporation and /or its subsidiary(-ies)
    // see https://github.com/qt/qtbase/blob/dev/src/opengl/qgl.cpp
    if (QSysInfo::ByteOrder == QSysInfo::BigEndian) {
        // OpenGL gives RGBA; Qt wants ARGB
        uint *p = reinterpret_cast<uint *>(img.bits());
        uint *end = p + w * h;
        while (p < end) {
            uint a = *p << 24;
            *p = (*p >> 8) | a;
            p++;
        }
    } else {
        // OpenGL gives ABGR (i.e. RGBA backwards); Qt wants ARGB
        for (int y = 0; y < h; y++) {
            uint *q = reinterpret_cast<uint *>(img.scanLine(y));
            for (int x = 0; x < w; ++x) {
                const uint pixel = *q;
                *q = ((pixel << 16) & 0xff0000) | ((pixel >> 16) & 0xff)
                    | (pixel & 0xff00ff00);

                q++;
            }
        }
    }

    QMatrix4x4 matrix;
    // apply render target transformation
    matrix *= renderTargetTransformation.inverted().toMatrix();
    // OpenGL textures are flipped vs QImage
    matrix.scale(1, -1);
    img = img.transformed(matrix.toTransform());
}

class ScreenshotTarget
{
public:
    static std::unique_ptr<ScreenshotTarget> create(const QSize &size)
    {
        auto target = std::make_unique<ScreenshotTarget>();
        if (const auto eglBackend = dynamic_cast<EglBackend *>(Compositor::self()->backend())) {
            const auto context = eglBackend->openglContext();
            if (!context || !context->makeCurrent()) {
                return nullptr;
            }
            target->m_glTexture = GLTexture::allocate(GL_RGBA8, size);
            if (!target->m_glTexture) {
                return nullptr;
            }
            target->m_glTexture->setFilter(GL_LINEAR);
            target->m_glTexture->setWrapMode(GL_CLAMP_TO_EDGE);
            target->m_glTarget = std::make_unique<GLFramebuffer>(target->m_glTexture.get());
            if (!target->m_glTarget->valid()) {
                return nullptr;
            }
            return target;
        }

        if (const auto vulkanBackend = dynamic_cast<VulkanBackend *>(Compositor::self()->backend())) {
            target->m_vulkanTexture = VulkanTexture::allocate(vulkanBackend->device(),
                                                              vk::Format::eR8G8B8A8Unorm,
                                                              size,
                                                              vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferSrc,
                                                              VulkanQueueRole::Compute);
            if (!target->m_vulkanTexture) {
                return nullptr;
            }
            target->m_vulkanTarget = std::make_unique<VulkanRenderTarget>(target->m_vulkanTexture.get());
            return target;
        }

        return nullptr;
    }

    RenderTarget renderTarget()
    {
        if (m_glTarget) {
            return RenderTarget(m_glTarget.get());
        }
        return RenderTarget(m_vulkanTarget.get());
    }

    QSize size() const
    {
        return m_glTexture ? m_glTexture->size() : m_vulkanTexture->size();
    }

    QImage toImage() const
    {
        if (m_vulkanTexture) {
            return m_vulkanTexture->download().convertToFormat(QImage::Format_ARGB32_Premultiplied);
        }

        const auto eglBackend = dynamic_cast<EglBackend *>(Compositor::self()->backend());
        const auto context = eglBackend ? eglBackend->openglContext() : nullptr;
        if (!context || !context->makeCurrent()) {
            return {};
        }
        GLFramebuffer::pushFramebuffer(m_glTarget.get());
        QImage snapshot(m_glTexture->size(), QImage::Format_ARGB32_Premultiplied);
        context->glReadnPixels(0, 0, snapshot.width(), snapshot.height(), GL_RGBA, GL_UNSIGNED_BYTE, snapshot.sizeInBytes(), static_cast<GLvoid *>(snapshot.bits()));
        convertFromGLImage(snapshot, snapshot.width(), snapshot.height(), OutputTransform::Normal);
        GLFramebuffer::popFramebuffer();
        return snapshot;
    }

private:
    std::unique_ptr<GLTexture> m_glTexture;
    std::unique_ptr<GLFramebuffer> m_glTarget;
    std::unique_ptr<VulkanTexture> m_vulkanTexture;
    std::unique_ptr<VulkanRenderTarget> m_vulkanTarget;
};

ScreenShotManager::ScreenShotManager()
    : m_dbusInterface2(new ScreenShotDBusInterface2(this))
{
}

ScreenShotManager::~ScreenShotManager()
{
}

// TODO share code with the screencast plugin?

std::optional<QImage> ScreenShotManager::takeScreenShot(LogicalOutput *screen, ScreenShotFlags flags, std::optional<pid_t> pidToHide)
{
    qreal scale = 1.0;
    if (flags & ScreenShotNativeResolution) {
        scale = screen->scale();
    }
    const QSize nativeSize = (screen->geometryF().size() * scale).toSize();

    const auto target = ScreenshotTarget::create(nativeSize);
    if (!target) {
        return std::nullopt;
    }

    RenderTarget renderTarget = target->renderTarget();
    ScreenshotLayer layer(screen, renderTarget);
    if (!layer.preparePresentationTest()) {
        return std::nullopt;
    }
    const auto beginInfo = layer.beginFrame();
    if (!beginInfo) {
        return std::nullopt;
    }
    SceneView sceneView(kwinApp()->scene(), screen, nullptr, &layer);
    std::unique_ptr<ItemTreeView> cursorView;
    if (!(flags & ScreenShotIncludeCursor)) {
        cursorView = std::make_unique<ItemTreeView>(&sceneView, kwinApp()->scene()->cursorItem(), workspace()->outputs().front(), nullptr, nullptr);
        cursorView->setExclusive(true);
    }
    sceneView.addWindowFilter([pidToHide](Window *window) {
        return shouldFilterWindowFromCapture(window, pidToHide);
    });
    const Rect fullDamage = Rect(QPoint(), target->size());
    sceneView.setViewport(screen->geometryF());
    sceneView.setScale(scale);
    sceneView.prePaint();
    sceneView.paint(beginInfo->renderTarget, QPoint(), fullDamage);
    sceneView.postPaint();
    if (!layer.endFrame(fullDamage, fullDamage, nullptr)) {
        return std::nullopt;
    }

    QImage snapshot = target->toImage();
    if (snapshot.isNull()) {
        return std::nullopt;
    }
    snapshot.setDevicePixelRatio(scale);
    return snapshot;
}

std::optional<QImage> ScreenShotManager::takeScreenShot(const Rect &area, ScreenShotFlags flags, std::optional<pid_t> pidToHide)
{
    qreal scale = 1.0;
    if (flags & ScreenShotNativeResolution) {
        const auto outputs = workspace()->outputs();
        for (LogicalOutput *output : outputs) {
            scale = std::max(scale, output->scale());
        }
    }
    const QSize nativeSize = area.size() * scale;

    const auto target = ScreenshotTarget::create(nativeSize);
    if (!target) {
        return std::nullopt;
    }

    RenderTarget renderTarget = target->renderTarget();
    ScreenshotLayer layer(workspace()->outputs().front(), renderTarget);
    if (!layer.preparePresentationTest()) {
        return std::nullopt;
    }
    const auto beginInfo = layer.beginFrame();
    if (!beginInfo) {
        return std::nullopt;
    }
    SceneView sceneView(kwinApp()->scene(), workspace()->outputs().front(), nullptr, &layer);
    std::unique_ptr<ItemTreeView> cursorView;
    if (!(flags & ScreenShotIncludeCursor)) {
        cursorView = std::make_unique<ItemTreeView>(&sceneView, kwinApp()->scene()->cursorItem(), workspace()->outputs().front(), nullptr, nullptr);
        cursorView->setExclusive(true);
    }
    sceneView.addWindowFilter([pidToHide](Window *window) {
        return shouldFilterWindowFromCapture(window, pidToHide);
    });
    const Rect fullDamage = Rect(QPoint(), target->size());
    sceneView.setViewport(area);
    sceneView.setScale(scale);
    sceneView.prePaint();
    sceneView.paint(beginInfo->renderTarget, QPoint(), fullDamage);
    sceneView.postPaint();
    if (!layer.endFrame(fullDamage, fullDamage, nullptr)) {
        return std::nullopt;
    }

    QImage snapshot = target->toImage();
    if (snapshot.isNull()) {
        return std::nullopt;
    }
    snapshot.setDevicePixelRatio(scale);
    return snapshot;
}

std::optional<QImage> ScreenShotManager::takeScreenShot(Window *window, ScreenShotFlags flags)
{
    if (window->excludeFromCapture()) {
        return std::nullopt;
    }

    const qreal scale = window->targetScale();
    RectF geometry = window->visibleGeometry();
    if (window->windowItem()->decorationItem() && !(flags & ScreenShotIncludeDecoration)) {
        geometry = window->clientGeometry();
    } else if (!(flags & ScreenShotIncludeShadow)) {
        geometry = window->frameGeometry();
    }
    const QSize nativeSize = (geometry.size() * scale).toSize();
    const auto target = ScreenshotTarget::create(nativeSize);
    if (!target) {
        return std::nullopt;
    }

    RenderTarget renderTarget = target->renderTarget();
    RenderViewport viewport(geometry, scale, renderTarget, QPoint());

    WorkspaceScene *scene = kwinApp()->scene();

    scene->renderer()->beginFrame(renderTarget, viewport);
    scene->renderer()->renderBackground(renderTarget, viewport, Region::infinite());
    scene->renderer()->renderItem(renderTarget, viewport, window->windowItem(), Scene::PAINT_WINDOW_TRANSFORMED, Region::infinite(), WindowPaintData{}, [flags, w = window->windowItem()](Item *item) {
        const bool deco = flags & ScreenShotFlag::ScreenShotIncludeDecoration;
        const bool shadow = deco && (flags & ScreenShotFlag::ScreenShotIncludeShadow);
        return (!deco && item == w->decorationItem())
            || (!shadow && item == w->shadowItem());
    }, {});
    if ((flags & ScreenShotFlag::ScreenShotIncludeCursor) && scene->cursorItem()->isVisible()) {
        scene->renderer()->renderItem(renderTarget, viewport, scene->cursorItem(), 0, Region::infinite(), WindowPaintData{}, {}, {});
    }
    scene->renderer()->endFrame();

    QImage snapshot = target->toImage();
    if (snapshot.isNull()) {
        return std::nullopt;
    }
    snapshot.setDevicePixelRatio(scale);
    return snapshot;
}

} // namespace KWin

#include "moc_screenshot.cpp"
